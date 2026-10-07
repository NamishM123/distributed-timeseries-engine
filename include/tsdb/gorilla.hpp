#pragma once
//
// Gorilla time-series compression (Pelkonen et al., VLDB 2015).
//
// Two independent column encoders packed into one chunk:
//   * timestamps  -> delta-of-delta with variable-length buckets
//   * values (f64) -> XOR against the previous value, storing only the
//                     meaningful (non leading/trailing-zero) bit window
//
// Storing the two columns as separate bit streams is what makes the chunk
// "columnar": a scan that only needs timestamps never touches value bytes.
//
#include <cstdint>
#include <cstring>
#include <vector>

#include "tsdb/bitstream.hpp"

namespace tsdb {

struct Sample {
  int64_t ts;
  double value;
};

// Encodes a run of samples for a single series into a compact Gorilla chunk.
class GorillaEncoder {
 public:
  void append(int64_t ts, double value) {
    if (count_ == 0) {
      ts_writer_.write_bits(static_cast<uint64_t>(ts), 64);
      value_writer_.write_bits(double_bits(value), 64);
      prev_ts_ = ts;
      prev_delta_ = 0;  // so the 2nd point's DoD equals its delta
      prev_value_bits_ = double_bits(value);
      prev_leading_ = 0xff;  // force a full value window on first XOR
      prev_trailing_ = 0;
      ++count_;
      return;
    }
    encode_timestamp(ts);
    encode_value(value);
    ++count_;
  }

  // Serialized chunk: [u32 count][u32 ts_len][ts bytes][value bytes].
  std::vector<uint8_t> finish() {
    const auto ts_bytes = ts_writer_.bytes();
    const auto val_bytes = value_writer_.bytes();
    std::vector<uint8_t> out;
    out.reserve(12 + ts_bytes.size() + val_bytes.size());
    put_u32(out, count_);
    put_u32(out, static_cast<uint32_t>(ts_bytes.size()));
    out.insert(out.end(), ts_bytes.begin(), ts_bytes.end());
    out.insert(out.end(), val_bytes.begin(), val_bytes.end());
    return out;
  }

  uint32_t count() const { return count_; }

 private:
  static uint64_t double_bits(double d) {
    uint64_t b;
    std::memcpy(&b, &d, 8);
    return b;
  }
  static void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x & 0xff);
    v.push_back((x >> 8) & 0xff);
    v.push_back((x >> 16) & 0xff);
    v.push_back((x >> 24) & 0xff);
  }

  void encode_timestamp(int64_t ts) {
    const int64_t delta = ts - prev_ts_;
    const int64_t dod = delta - prev_delta_;
    if (dod == 0) {
      ts_writer_.write_bit(0);
    } else if (dod >= -63 && dod <= 64) {
      ts_writer_.write_bits(0b10, 2);
      ts_writer_.write_bits(fit_signed(dod), 7);
    } else if (dod >= -255 && dod <= 256) {
      ts_writer_.write_bits(0b110, 3);
      ts_writer_.write_bits(fit_signed(dod), 9);
    } else if (dod >= -2047 && dod <= 2048) {
      ts_writer_.write_bits(0b1110, 4);
      ts_writer_.write_bits(fit_signed(dod), 12);
    } else {
      ts_writer_.write_bits(0b1111, 4);
      ts_writer_.write_bits(static_cast<uint64_t>(dod), 64);
    }
    prev_ts_ = ts;
    prev_delta_ = delta;
  }

  // Write the low `nbits` of a signed delta-of-delta. The bucket ranges are the
  // asymmetric Gorilla ranges ([-63,64], [-255,256], [-2047,2048]); the decoder
  // interprets the field with a matching threshold (values above 2^(n-1) are
  // negative), so the upper endpoint fits without overflow.
  static uint64_t fit_signed(int64_t v) { return static_cast<uint64_t>(v); }

  void encode_value(double value) {
    const uint64_t bits = double_bits(value);
    const uint64_t x = bits ^ prev_value_bits_;
    prev_value_bits_ = bits;
    if (x == 0) {
      value_writer_.write_bit(0);
      return;
    }
    value_writer_.write_bit(1);
    int leading = __builtin_clzll(x);
    const int trailing = __builtin_ctzll(x);
    if (leading >= 32) leading = 31;  // 5-bit field caps at 31
    if (prev_leading_ != 0xff && leading >= prev_leading_ && trailing >= prev_trailing_) {
      // Reuse the previous bit window.
      value_writer_.write_bit(0);
      const int sig = 64 - prev_leading_ - prev_trailing_;
      value_writer_.write_bits(x >> prev_trailing_, sig);
    } else {
      value_writer_.write_bit(1);
      const int sig = 64 - leading - trailing;
      value_writer_.write_bits(static_cast<uint64_t>(leading), 5);
      value_writer_.write_bits(static_cast<uint64_t>(sig) & 0x3f, 6);  // 64 -> 0
      value_writer_.write_bits(x >> trailing, sig);
      prev_leading_ = leading;
      prev_trailing_ = trailing;
    }
  }

  BitWriter ts_writer_;
  BitWriter value_writer_;
  uint32_t count_ = 0;
  int64_t prev_ts_ = 0;
  int64_t prev_delta_ = 0;
  uint64_t prev_value_bits_ = 0;
  int prev_leading_ = 0xff;
  int prev_trailing_ = 0;
};

// Decodes a chunk produced by GorillaEncoder back into samples.
//
// The decoder is zero-copy: it holds pointers into the chunk buffer rather than
// copying it, so the buffer must outlive the decoder. The rvalue overload is
// deleted to turn the "decode a temporary" mistake into a compile error.
class GorillaDecoder {
 public:
  explicit GorillaDecoder(const std::vector<uint8_t>& chunk) { init(chunk.data(), chunk.size()); }
  explicit GorillaDecoder(std::vector<uint8_t>&&) = delete;  // buffer must outlive decoder
  GorillaDecoder(const uint8_t* data, size_t size) { init(data, size); }

  uint32_t count() const { return count_; }

  // Decode every sample in the chunk into `out`.
  void decode_all(std::vector<Sample>& out) {
    out.clear();
    out.reserve(count_);
    BitReader ts_reader(ts_ptr_, ts_len_);
    BitReader val_reader(val_ptr_, val_len_);
    int64_t ts = 0, prev_delta = 0;
    uint64_t value_bits = 0;
    int leading = 0, trailing = 0;
    for (uint32_t i = 0; i < count_; ++i) {
      if (i == 0) {
        ts = static_cast<int64_t>(ts_reader.read_bits(64));
        value_bits = val_reader.read_bits(64);
        prev_delta = 0;
      } else {
        // --- timestamp ---
        int64_t dod;
        if (ts_reader.read_bit() == 0) {
          dod = 0;
        } else if (ts_reader.read_bit() == 0) {
          dod = decode_dod(ts_reader.read_bits(7), 7);
        } else if (ts_reader.read_bit() == 0) {
          dod = decode_dod(ts_reader.read_bits(9), 9);
        } else if (ts_reader.read_bit() == 0) {
          dod = decode_dod(ts_reader.read_bits(12), 12);
        } else {
          dod = static_cast<int64_t>(ts_reader.read_bits(64));
        }
        const int64_t delta = prev_delta + dod;
        ts += delta;
        prev_delta = delta;
        // --- value ---
        if (val_reader.read_bit() != 0) {
          if (val_reader.read_bit() != 0) {
            leading = static_cast<int>(val_reader.read_bits(5));
            int sig = static_cast<int>(val_reader.read_bits(6));
            if (sig == 0) sig = 64;
            trailing = 64 - leading - sig;
          }
          const int sig = 64 - leading - trailing;
          const uint64_t mantissa = val_reader.read_bits(sig);
          value_bits ^= (mantissa << trailing);
        }
        // XOR == 0 leaves value_bits unchanged.
      }
      double d;
      std::memcpy(&d, &value_bits, 8);
      out.push_back(Sample{ts, d});
    }
  }

 private:
  void init(const uint8_t* data, size_t size) {
    count_ = read_u32(data);
    const uint32_t ts_len = read_u32(data + 4);
    ts_ptr_ = data + 8;
    ts_len_ = ts_len;
    val_ptr_ = data + 8 + ts_len;
    val_len_ = size - 8 - ts_len;
  }
  static uint32_t read_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  }
  // Inverse of fit_signed: an `n`-bit field encodes an asymmetric range where
  // values above 2^(n-1) are negative (v - 2^n). Matches the write side so the
  // upper bucket endpoint (+64/+256/+2048) decodes correctly.
  static int64_t decode_dod(uint64_t bits, int n) {
    const int64_t half = int64_t(1) << (n - 1);
    int64_t v = static_cast<int64_t>(bits);
    if (v > half) v -= (int64_t(1) << n);
    return v;
  }

  uint32_t count_ = 0;
  const uint8_t* ts_ptr_ = nullptr;
  const uint8_t* val_ptr_ = nullptr;
  size_t ts_len_ = 0, val_len_ = 0;
};

}  // namespace tsdb
