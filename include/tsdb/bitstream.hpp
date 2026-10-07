#pragma once
//
// Bit-level reader/writer used by the Gorilla codec.
//
// Bits are packed MSB-first into a byte vector: the first bit written lands in
// bit 7 of byte 0. This matches the layout used by Facebook's go-tsz / Beringei
// implementations, so a stream produced here decodes identically to those.
//
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace tsdb {

class BitWriter {
 public:
  // Append the low `nbits` bits of `value`, most-significant first.
  void write_bits(uint64_t value, int nbits) {
    // Only the low nbits are meaningful; mask so callers can pass dirty highs.
    if (nbits < 64) value &= (uint64_t(1) << nbits) - 1;
    while (nbits > 0) {
      if (bit_pos_ == 0) {
        bytes_.push_back(0);
        bit_pos_ = 8;
      }
      const int take = nbits < bit_pos_ ? nbits : bit_pos_;
      const int shift = bit_pos_ - take;                 // slot inside cur byte
      const uint64_t chunk = value >> (nbits - take);    // top `take` bits left
      bytes_.back() |= static_cast<uint8_t>((chunk & ((1u << take) - 1)) << shift);
      bit_pos_ -= take;
      nbits -= take;
    }
  }

  void write_bit(int b) { write_bits(b ? 1 : 0, 1); }

  const std::vector<uint8_t>& bytes() const { return bytes_; }
  std::vector<uint8_t> take() { return std::move(bytes_); }
  // Number of bits written so far.
  size_t bit_size() const { return bytes_.size() * 8 - bit_pos_; }

 private:
  std::vector<uint8_t> bytes_;
  int bit_pos_ = 0;  // free bits remaining in the current (last) byte
};

class BitReader {
 public:
  BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  explicit BitReader(const std::vector<uint8_t>& v) : data_(v.data()), size_(v.size()) {}

  uint64_t read_bits(int nbits) {
    uint64_t out = 0;
    while (nbits > 0) {
      if (byte_pos_ >= size_) throw std::out_of_range("BitReader: stream underrun");
      if (bit_pos_ == 0) bit_pos_ = 8;
      const int avail = bit_pos_;
      const int take = nbits < avail ? nbits : avail;
      const int shift = avail - take;
      const uint64_t chunk = (data_[byte_pos_] >> shift) & ((1u << take) - 1);
      out = (out << take) | chunk;
      bit_pos_ -= take;
      nbits -= take;
      if (bit_pos_ == 0) ++byte_pos_;
    }
    return out;
  }

  int read_bit() { return static_cast<int>(read_bits(1)); }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t byte_pos_ = 0;
  int bit_pos_ = 0;  // bits remaining in current byte (0 means "load next")
};

}  // namespace tsdb
