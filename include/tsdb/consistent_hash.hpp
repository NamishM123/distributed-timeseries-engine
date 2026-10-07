#pragma once
//
// Consistent hash ring with virtual nodes and ordered replica selection.
//
// Each physical node is placed at `vnodes` points on a 64-bit ring. A key maps
// to the first vnode clockwise from hash(key); its replica set is the next
// `replicas` *distinct* physical nodes around the ring. Adding or removing a
// node only remaps keys in the arcs adjacent to that node's vnodes, which keeps
// rebalancing proportional to 1/N of the keyspace.
//
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace tsdb {

// FNV-1a 64-bit — small, dependency-free string hash.
inline uint64_t fnv1a(const std::string& s, uint64_t seed = 1469598103934665603ULL) {
  uint64_t h = seed;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  return h;
}

// splitmix64 finalizer. FNV-1a alone leaves the ring positions of near-identical
// vnode keys ("n3#0", "n3#1", ...) correlated, which clusters them and skews
// per-node load. Running the FNV output through splitmix64's avalanche step
// decorrelates them so virtual nodes spread evenly around the ring.
inline uint64_t mix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

// Position of an arbitrary label on the 64-bit ring. Used for both vnode
// placement and key lookup so they share one coordinate space.
inline uint64_t ring_hash(const std::string& s) { return mix64(fnv1a(s)); }

class ConsistentHashRing {
 public:
  explicit ConsistentHashRing(int vnodes = 256) : vnodes_(vnodes) {}

  void add_node(const std::string& node) {
    if (nodes_.count(node)) return;
    nodes_.insert(node);
    for (int i = 0; i < vnodes_; ++i) ring_[vnode_hash(node, i)] = node;
  }

  void remove_node(const std::string& node) {
    if (!nodes_.erase(node)) return;
    for (int i = 0; i < vnodes_; ++i) ring_.erase(vnode_hash(node, i));
  }

  // Primary owner of a key (first vnode clockwise from hash(key)).
  const std::string& owner(const std::string& key) const {
    static const std::string kEmpty;
    if (ring_.empty()) return kEmpty;
    auto it = ring_.lower_bound(ring_hash(key));
    if (it == ring_.end()) it = ring_.begin();
    return it->second;
  }

  // The first `n` distinct physical nodes clockwise from hash(key).
  std::vector<std::string> replica_set(const std::string& key, int n) const {
    std::vector<std::string> out;
    if (ring_.empty()) return out;
    const uint64_t h = ring_hash(key);
    auto it = ring_.lower_bound(h);
    const size_t ring_size = ring_.size();
    for (size_t steps = 0; steps < ring_size && static_cast<int>(out.size()) < n; ++steps) {
      if (it == ring_.end()) it = ring_.begin();
      const std::string& cand = it->second;
      bool seen = false;
      for (const auto& o : out) if (o == cand) { seen = true; break; }
      if (!seen) out.push_back(cand);
      ++it;
    }
    return out;
  }

  size_t node_count() const { return nodes_.size(); }
  size_t vnode_count() const { return ring_.size(); }

 private:
  uint64_t vnode_hash(const std::string& node, int i) const {
    return ring_hash(node + "#" + std::to_string(i));
  }

  int vnodes_;
  std::map<uint64_t, std::string> ring_;
  std::set<std::string> nodes_;
};

}  // namespace tsdb
