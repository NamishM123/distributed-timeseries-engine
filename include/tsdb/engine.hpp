#pragma once
//
// Single-node columnar time-series storage engine.
//
// Each series is a chain of immutable, Gorilla-compressed chunks plus one open
// chunk that accepts appends. When the open chunk fills (kChunkSamples) it is
// sealed and a new one starts. Sealed chunks keep their first/last timestamp so
// a range query can skip chunks that cannot overlap the query window.
//
// The engine is thread-safe: a sharded set of mutexes guards the series map and
// each series guards its own chunk chain, so ingestion across many series
// scales with cores.
//
#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "tsdb/gorilla.hpp"
#include "tsdb/simd_scan.hpp"

namespace tsdb {

constexpr uint32_t kChunkSamples = 1024;  // samples per sealed chunk

struct QueryResult {
  std::vector<int64_t> timestamps;
  std::vector<double> values;
  size_t size() const { return timestamps.size(); }
};

class Series {
 public:
  void append(int64_t ts, double value) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!open_) {
      open_ = std::make_unique<GorillaEncoder>();
      open_first_ = ts;
    }
    open_->append(ts, value);
    open_last_ = ts;
    ++total_samples_;
    if (open_->count() >= kChunkSamples) seal_locked();
  }

  // Append values and aggregate the query window in one pass of the chain.
  void query(int64_t start, int64_t end, QueryResult* out) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Sample> decoded;
    for (const auto& c : sealed_) {
      if (c.last < start || c.first > end) continue;  // chunk can't overlap
      GorillaDecoder dec(c.bytes);
      dec.decode_all(decoded);
      collect(decoded, start, end, out);
    }
    if (open_ && !(open_last_ < start || open_first_ > end)) {
      // Decode a snapshot of the open chunk.
      GorillaEncoder snapshot = *open_;
      auto bytes = snapshot.finish();
      GorillaDecoder dec(bytes);
      dec.decode_all(decoded);
      collect(decoded, start, end, out);
    }
  }

  uint64_t total_samples() const {
    std::lock_guard<std::mutex> lk(mu_);
    return total_samples_;
  }

 private:
  struct SealedChunk {
    int64_t first, last;
    std::vector<uint8_t> bytes;
  };

  void seal_locked() {
    SealedChunk c;
    c.first = open_first_;
    c.last = open_last_;
    c.bytes = open_->finish();
    sealed_.push_back(std::move(c));
    open_.reset();
  }

  static void collect(const std::vector<Sample>& s, int64_t start, int64_t end, QueryResult* out) {
    for (const auto& x : s) {
      if (x.ts >= start && x.ts <= end) {
        out->timestamps.push_back(x.ts);
        out->values.push_back(x.value);
      }
    }
  }

  mutable std::mutex mu_;
  std::vector<SealedChunk> sealed_;
  std::unique_ptr<GorillaEncoder> open_;
  int64_t open_first_ = 0, open_last_ = 0;
  uint64_t total_samples_ = 0;
};

class Engine {
 public:
  explicit Engine(size_t stripes = 64) : stripes_(stripes), locks_(stripes), maps_(stripes) {}

  void append(const std::string& key, int64_t ts, double value) {
    series_for(key)->append(ts, value);
  }

  QueryResult query(const std::string& key, int64_t start, int64_t end) const {
    QueryResult out;
    const size_t s = stripe(key);
    std::shared_ptr<Series> series;
    {
      std::shared_lock<std::shared_mutex> lk(locks_[s]);
      auto it = maps_[s].find(key);
      if (it != maps_[s].end()) series = it->second;
    }
    if (series) series->query(start, end, &out);
    return out;
  }

  size_t series_count() const {
    size_t n = 0;
    for (size_t s = 0; s < stripes_; ++s) {
      std::shared_lock<std::shared_mutex> lk(locks_[s]);
      n += maps_[s].size();
    }
    return n;
  }

 private:
  size_t stripe(const std::string& key) const {
    return std::hash<std::string>{}(key) % stripes_;
  }

  std::shared_ptr<Series> series_for(const std::string& key) {
    const size_t s = stripe(key);
    {
      std::shared_lock<std::shared_mutex> lk(locks_[s]);
      auto it = maps_[s].find(key);
      if (it != maps_[s].end()) return it->second;
    }
    std::unique_lock<std::shared_mutex> lk(locks_[s]);
    auto it = maps_[s].find(key);
    if (it != maps_[s].end()) return it->second;
    auto series = std::make_shared<Series>();
    maps_[s].emplace(key, series);
    return series;
  }

  size_t stripes_;
  mutable std::vector<std::shared_mutex> locks_;
  std::vector<std::unordered_map<std::string, std::shared_ptr<Series>>> maps_;
};

}  // namespace tsdb
