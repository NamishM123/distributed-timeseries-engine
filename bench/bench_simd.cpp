// SIMD scan speedup: scalar vs AVX2 aggregation over a decompressed value
// column. Reports the throughput ratio, which is the "Nx SIMD-accelerated
// scans" figure.
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#include "tsdb/simd_scan.hpp"

using namespace tsdb;
using clk = std::chrono::steady_clock;

template <class F>
double time_it(F f, int iters, volatile double* sink) {
  const auto t0 = clk::now();
  for (int i = 0; i < iters; ++i) {
    Aggregate a = f();
    *sink += a.sum + a.min + a.max;
  }
  const auto t1 = clk::now();
  return std::chrono::duration<double>(t1 - t0).count();
}

int main(int argc, char** argv) {
  const size_t n = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 4'000'000;
  const int iters = argc > 2 ? std::atoi(argv[2]) : 200;

  std::vector<double> v(n);
  std::mt19937_64 rng(5);
  std::uniform_real_distribution<double> d(0, 1000);
  for (auto& x : v) x = d(rng);

  volatile double sink = 0;
#if defined(__x86_64__)
  printf("CPU AVX2      : %s\n", cpu_has_avx2() ? "yes" : "no");
#endif
  printf("elements      : %zu  (iters=%d)\n", n, iters);

  const double ts = time_it([&] { return aggregate_scalar(v.data(), 0, n); }, iters, &sink);
#if defined(__x86_64__)
  const double ta = time_it([&] { return aggregate_avx2(v.data(), 0, n); }, iters, &sink);
#else
  const double ta = ts;
#endif

  const double gps_scalar = double(n) * iters / ts / 1e9;
  const double gps_avx2 = double(n) * iters / ta / 1e9;
  printf("scalar        : %.3f s  (%.2f Gelem/s)\n", ts, gps_scalar);
  printf("avx2          : %.3f s  (%.2f Gelem/s)\n", ta, gps_avx2);
  printf("speedup       : %.2fx\n", ts / ta);
  printf("(sink=%.1f)\n", (double)sink);
  return 0;
}
