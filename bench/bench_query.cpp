// Range-query latency distribution over a populated engine.
//
// Loads the workload, then issues many random range queries (each over a
// window of a series) and reports the latency percentiles. p95 is the headline
// number quoted for the serving path.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#include "tsdb/engine.hpp"
#include "tsdb/workload.hpp"

using namespace tsdb;
using clk = std::chrono::steady_clock;

int main(int argc, char** argv) {
  WorkloadConfig cfg;
  cfg.series = argc > 1 ? std::atoi(argv[1]) : 5000;
  cfg.samples_per_series = argc > 2 ? std::atoi(argv[2]) : 4000;
  const int nqueries = argc > 3 ? std::atoi(argv[3]) : 20000;
  const int window = argc > 4 ? std::atoi(argv[4]) : 500;  // samples per query window

  Workload wl(cfg);
  Engine eng;
  std::vector<std::string> keys(cfg.series);
  std::vector<Sample> s;
  for (int i = 0; i < cfg.series; ++i) {
    keys[i] = wl.series_key(i);
    wl.generate_series(i, &s);
    for (const auto& x : s) eng.append(keys[i], x.ts, x.value);
  }
  const uint64_t total = uint64_t(cfg.series) * cfg.samples_per_series;

  std::mt19937_64 rng(99);
  const int64_t span = int64_t(cfg.samples_per_series) * cfg.interval_ms;
  const int64_t wspan = int64_t(window) * cfg.interval_ms;
  std::vector<double> lat;
  lat.reserve(nqueries);
  uint64_t matched = 0;

  for (int q = 0; q < nqueries; ++q) {
    const int i = rng() % cfg.series;
    const int64_t start = cfg.start_ms + static_cast<int64_t>(rng() % std::max<int64_t>(1, span - wspan));
    const int64_t end = start + wspan;
    const auto t0 = clk::now();
    auto r = eng.query(keys[i], start, end);
    const auto t1 = clk::now();
    matched += r.size();
    lat.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
  }

  std::sort(lat.begin(), lat.end());
  auto pct = [&](double p) { return lat[std::min(lat.size() - 1, size_t(p / 100.0 * lat.size()))]; };
  const double thresh_us = 50000.0;  // 50 ms
  size_t under = 0;
  for (double x : lat) if (x <= thresh_us) ++under;

  printf("dataset       : %lu points across %d series\n", (unsigned long)total, cfg.series);
  printf("queries       : %d  (window ~%d samples, avg %.0f matched)\n", nqueries, window,
         double(matched) / nqueries);
  printf("latency (us)  : p50=%.1f  p90=%.1f  p95=%.1f  p99=%.1f  max=%.1f\n", pct(50), pct(90),
         pct(95), pct(99), lat.back());
  printf("under 50 ms   : %.2f%%\n", 100.0 * under / lat.size());
  return 0;
}
