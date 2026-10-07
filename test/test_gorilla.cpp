#include <cmath>
#include <cstring>
#include <random>

#include <gtest/gtest.h>

#include "tsdb/gorilla.hpp"

using namespace tsdb;

static std::vector<Sample> roundtrip(const std::vector<Sample>& in) {
  GorillaEncoder enc;
  for (const auto& s : in) enc.append(s.ts, s.value);
  const std::vector<uint8_t> chunk = enc.finish();  // keep alive: decoder is zero-copy
  GorillaDecoder dec(chunk);
  std::vector<Sample> out;
  dec.decode_all(out);
  return out;
}

static void expect_equal(const std::vector<Sample>& a, const std::vector<Sample>& b) {
  ASSERT_EQ(a.size(), b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].ts, b[i].ts) << "ts at " << i;
    EXPECT_EQ(0, std::memcmp(&a[i].value, &b[i].value, 8)) << "value at " << i;
  }
}

TEST(Gorilla, SingleSample) {
  std::vector<Sample> in{{1700000000000LL, 3.14159}};
  expect_equal(in, roundtrip(in));
}

TEST(Gorilla, ConstantInterval) {
  std::vector<Sample> in;
  int64_t ts = 1700000000000LL;
  for (int i = 0; i < 5000; ++i) in.push_back({ts += 1000, 42.0});
  expect_equal(in, roundtrip(in));
}

TEST(Gorilla, RepeatedValuesCompressWell) {
  std::vector<Sample> in;
  int64_t ts = 0;
  for (int i = 0; i < 1000; ++i) in.push_back({ts += 10000, 7.0});
  GorillaEncoder enc;
  for (const auto& s : in) enc.append(s.ts, s.value);
  const auto chunk = enc.finish();
  // 1000 flat points at a constant interval should be far below 16 B/point.
  EXPECT_LT(chunk.size(), in.size() * 2);
}

TEST(Gorilla, SpecialDoubles) {
  std::vector<Sample> in{
      {1, 0.0},
      {2, -0.0},
      {3, std::numeric_limits<double>::infinity()},
      {4, -std::numeric_limits<double>::infinity()},
      {5, 1e308},
      {6, 1e-308},
      {7, 123456.789},
  };
  expect_equal(in, roundtrip(in));
}

TEST(Gorilla, NegativeAndIrregularDeltas) {
  std::vector<Sample> in;
  int64_t ts = 1000;
  std::mt19937_64 rng(1);
  double v = 0;
  for (int i = 0; i < 3000; ++i) {
    ts += 1000 + (int(rng() % 2000) - 1000);  // large jitter, can go backward in delta
    v += (int(rng() % 7) - 3);
    in.push_back({ts, v});
  }
  expect_equal(in, roundtrip(in));
}

TEST(Gorilla, FuzzManyRandomSeries) {
  std::mt19937_64 rng(2024);
  for (int t = 0; t < 500; ++t) {
    std::vector<Sample> in;
    int64_t ts = int64_t(rng());
    double v = int64_t(rng() % 1000) / 3.0;
    const int n = 1 + rng() % 1500;
    std::normal_distribution<double> step(0, 1.0);
    for (int i = 0; i < n; ++i) {
      ts += int64_t(rng() % 100000) - 50000;
      v += step(rng);
      in.push_back({ts, v});
    }
    expect_equal(in, roundtrip(in));
  }
}
