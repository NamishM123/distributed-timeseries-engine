// Ingestion throughput: single-thread and multi-thread appends/sec.
//
// Pre-generates the workload so the measured region contains only engine work
// (append + Gorilla encode + chunk sealing), then reports points/sec.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "tsdb/engine.hpp"
#include "tsdb/workload.hpp"

using namespace tsdb;
using clk = std::chrono::steady_clock;

int main(int argc, char** argv) {
  WorkloadConfig cfg;
  cfg.series = argc > 1 ? std::atoi(argv[1]) : 2000;
  cfg.samples_per_series = argc > 2 ? std::atoi(argv[2]) : 2000;
  const int threads = argc > 3 ? std::atoi(argv[3]) : static_cast<int>(std::thread::hardware_concurrency());

  // Materialize all samples up front.
  Workload wl(cfg);
  std::vector<std::string> keys(cfg.series);
  std::vector<std::vector<Sample>> data(cfg.series);
  for (int i = 0; i < cfg.series; ++i) {
    keys[i] = wl.series_key(i);
    wl.generate_series(i, &data[i]);
  }
  const uint64_t total_points = uint64_t(cfg.series) * cfg.samples_per_series;

  auto bench = [&](int nthreads) {
    Engine eng;
    std::atomic<int> next{0};
    auto worker = [&] {
      for (;;) {
        const int i = next.fetch_add(1);
        if (i >= cfg.series) break;
        const auto& s = data[i];
        for (const auto& x : s) eng.append(keys[i], x.ts, x.value);
      }
    };
    const auto t0 = clk::now();
    std::vector<std::thread> pool;
    for (int t = 0; t < nthreads; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    const auto t1 = clk::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    return std::pair<double, size_t>{total_points / secs, eng.series_count()};
  };

  printf("workload: %d series x %d samples = %lu points\n", cfg.series, cfg.samples_per_series,
         (unsigned long)total_points);

  auto [single, _s] = bench(1);
  printf("ingest  1 thread : %10.0f points/sec\n", single);

  auto [multi, sc] = bench(threads);
  printf("ingest %2d threads: %10.0f points/sec  (%.2fx, %zu series)\n", threads, multi,
         multi / single, sc);
  return 0;
}
