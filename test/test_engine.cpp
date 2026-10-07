#include <thread>

#include <gtest/gtest.h>

#include "tsdb/engine.hpp"
#include "tsdb/simd_scan.hpp"

using namespace tsdb;

TEST(Engine, AppendAndRangeQuery) {
  Engine eng;
  // Sample i has ts=(i+1)*1000 and value i.
  for (int i = 0; i < 10000; ++i) eng.append("s", int64_t(i + 1) * 1000, double(i));
  // Window [3000001, 6000000] selects i = 3000..5999 (3000 samples).
  auto r = eng.query("s", 3'000'001, 6'000'000);
  ASSERT_EQ(r.size(), 3000u);
  EXPECT_EQ(r.timestamps.front(), 3'001'000);
  EXPECT_DOUBLE_EQ(r.values.front(), 3000.0);
  EXPECT_EQ(r.timestamps.back(), 6'000'000);
  EXPECT_DOUBLE_EQ(r.values.back(), 5999.0);
}

TEST(Engine, QueryCrossesSealedAndOpenChunks) {
  Engine eng;
  int64_t ts = 0;
  // More than one chunk (kChunkSamples=1024) plus an open remainder.
  for (int i = 0; i < 2600; ++i) eng.append("k", ts += 10, double(i));
  auto all = eng.query("k", 0, ts);
  EXPECT_EQ(all.size(), 2600u);
  // Values should be returned in timestamp order and monotonically increasing.
  for (size_t i = 1; i < all.values.size(); ++i) EXPECT_LT(all.values[i - 1], all.values[i]);
}

TEST(Engine, UnknownSeriesIsEmpty) {
  Engine eng;
  eng.append("exists", 1, 1.0);
  auto r = eng.query("missing", 0, 100);
  EXPECT_EQ(r.size(), 0u);
}

TEST(Engine, ConcurrentAppendsDistinctSeries) {
  Engine eng;
  const int threads = 8, per = 20000;
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      const std::string key = "series-" + std::to_string(t);
      int64_t ts = 0;
      for (int i = 0; i < per; ++i) eng.append(key, ts += 100, double(i));
    });
  }
  for (auto& th : pool) th.join();
  EXPECT_EQ(eng.series_count(), size_t(threads));
  for (int t = 0; t < threads; ++t) {
    auto r = eng.query("series-" + std::to_string(t), 0, int64_t(per) * 100);
    EXPECT_EQ(r.size(), size_t(per));
  }
}

TEST(SimdScan, MatchesScalar) {
  std::vector<double> v(100000);
  for (size_t i = 0; i < v.size(); ++i) v[i] = double((i * 2654435761u) % 10007) - 5000.0;
  const Aggregate s = aggregate_scalar(v.data(), 0, v.size());
  const Aggregate a = aggregate_range(v.data(), 0, v.size());
  EXPECT_DOUBLE_EQ(s.sum, a.sum);
  EXPECT_DOUBLE_EQ(s.min, a.min);
  EXPECT_DOUBLE_EQ(s.max, a.max);
  EXPECT_EQ(s.count, a.count);
}

TEST(SimdScan, IndexBounds) {
  std::vector<int64_t> ts;
  for (int i = 0; i < 1000; ++i) ts.push_back(i * 10);
  EXPECT_EQ(lower_index(ts.data(), ts.size(), 105), 11u);  // first >= 105 is 110 (idx 11)
  EXPECT_EQ(upper_index(ts.data(), ts.size(), 100), 11u);  // first > 100 is 110 (idx 11)
}
