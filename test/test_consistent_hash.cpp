#include <set>
#include <string>
#include <unordered_map>

#include <gtest/gtest.h>

#include "tsdb/consistent_hash.hpp"

using namespace tsdb;

static std::vector<std::string> keys(int n) {
  std::vector<std::string> k;
  for (int i = 0; i < n; ++i) k.push_back("series-" + std::to_string(i));
  return k;
}

TEST(ConsistentHash, OwnerIsStable) {
  ConsistentHashRing ring;
  for (int i = 0; i < 5; ++i) ring.add_node("n" + std::to_string(i));
  for (const auto& k : keys(1000)) EXPECT_EQ(ring.owner(k), ring.owner(k));
}

TEST(ConsistentHash, ReplicasAreDistinctAndInRing) {
  ConsistentHashRing ring;
  for (int i = 0; i < 5; ++i) ring.add_node("n" + std::to_string(i));
  for (const auto& k : keys(500)) {
    auto r = ring.replica_set(k, 3);
    ASSERT_EQ(r.size(), 3u);
    std::set<std::string> uniq(r.begin(), r.end());
    EXPECT_EQ(uniq.size(), 3u);  // distinct physical nodes
    EXPECT_EQ(r.front(), ring.owner(k));
  }
}

TEST(ConsistentHash, LoadIsBalanced) {
  ConsistentHashRing ring(200);
  const int N = 8;
  for (int i = 0; i < N; ++i) ring.add_node("n" + std::to_string(i));
  std::unordered_map<std::string, int> load;
  const auto ks = keys(100000);
  for (const auto& k : ks) load[ring.owner(k)]++;
  const double ideal = double(ks.size()) / N;
  int max_load = 0;
  for (auto& [n, c] : load) max_load = std::max(max_load, c);
  // With 200 vnodes/node, peak load should stay within ~15% of ideal.
  EXPECT_LT(max_load, ideal * 1.15);
  EXPECT_EQ(load.size(), size_t(N));
}

TEST(ConsistentHash, MinimalRemapOnNodeAdd) {
  ConsistentHashRing ring(200);
  for (int i = 0; i < 8; ++i) ring.add_node("n" + std::to_string(i));
  const auto ks = keys(100000);
  std::unordered_map<std::string, std::string> before;
  for (const auto& k : ks) before[k] = ring.owner(k);

  ring.add_node("n8");  // 8 -> 9 nodes
  int moved = 0;
  for (const auto& k : ks) if (ring.owner(k) != before[k]) ++moved;

  // Adding the 9th node should move roughly 1/9 of keys; allow generous slack.
  const double frac = double(moved) / ks.size();
  EXPECT_GT(frac, 0.05);
  EXPECT_LT(frac, 0.20);
}

TEST(ConsistentHash, RemoveReroutesOnlyAffectedKeys) {
  ConsistentHashRing ring(200);
  for (int i = 0; i < 6; ++i) ring.add_node("n" + std::to_string(i));
  const auto ks = keys(50000);
  std::unordered_map<std::string, std::string> before;
  for (const auto& k : ks) before[k] = ring.owner(k);

  ring.remove_node("n3");
  int moved = 0;
  for (const auto& k : ks) {
    const std::string now = ring.owner(k);
    EXPECT_NE(now, "n3");
    if (now != before[k]) {
      ++moved;
      EXPECT_EQ(before[k], "n3");  // only keys that lived on n3 move
    }
  }
  EXPECT_GT(moved, 0);
}
