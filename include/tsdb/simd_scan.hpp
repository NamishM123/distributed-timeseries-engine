#pragma once
//
// SIMD-accelerated scans over decompressed sample columns.
//
// After a Gorilla chunk is decoded into flat timestamp/value arrays, a range
// query reduces to: find the index window [lo,hi) whose timestamps fall in
// [start,end], then aggregate the values in that window. The aggregation is the
// hot loop, so it has an AVX2 path (4 doubles/iteration) alongside a portable
// scalar fallback. `scan_aggregate` picks the AVX2 path at runtime when the CPU
// supports it.
//
#include <cstdint>
#include <limits>

#if defined(__x86_64__)
#include <immintrin.h>
#if defined(__GNUC__)
#include <cpuid.h>
#endif
#endif

namespace tsdb {

struct Aggregate {
  double sum = 0.0;
  double min = std::numeric_limits<double>::infinity();
  double max = -std::numeric_limits<double>::infinity();
  uint64_t count = 0;
  double avg() const { return count ? sum / static_cast<double>(count) : 0.0; }
};

// Binary-search the lower/upper bounds of a sorted timestamp column.
inline size_t lower_index(const int64_t* ts, size_t n, int64_t start) {
  size_t lo = 0, hi = n;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (ts[mid] < start) lo = mid + 1; else hi = mid;
  }
  return lo;
}
inline size_t upper_index(const int64_t* ts, size_t n, int64_t end) {
  size_t lo = 0, hi = n;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (ts[mid] <= end) lo = mid + 1; else hi = mid;
  }
  return lo;
}

inline Aggregate aggregate_scalar(const double* v, size_t lo, size_t hi) {
  Aggregate a;
  for (size_t i = lo; i < hi; ++i) {
    const double x = v[i];
    a.sum += x;
    if (x < a.min) a.min = x;
    if (x > a.max) a.max = x;
  }
  a.count = hi - lo;
  return a;
}

#if defined(__x86_64__)
__attribute__((target("avx2"))) inline Aggregate aggregate_avx2(const double* v, size_t lo, size_t hi) {
  Aggregate a;
  size_t i = lo;
  const size_t n = hi - lo;
  if (n >= 4) {
    __m256d vsum = _mm256_setzero_pd();
    __m256d vmin = _mm256_set1_pd(std::numeric_limits<double>::infinity());
    __m256d vmax = _mm256_set1_pd(-std::numeric_limits<double>::infinity());
    const size_t last = lo + (n & ~size_t(3));
    for (; i < last; i += 4) {
      const __m256d x = _mm256_loadu_pd(v + i);
      vsum = _mm256_add_pd(vsum, x);
      vmin = _mm256_min_pd(vmin, x);
      vmax = _mm256_max_pd(vmax, x);
    }
    double s[4], mn[4], mx[4];
    _mm256_storeu_pd(s, vsum);
    _mm256_storeu_pd(mn, vmin);
    _mm256_storeu_pd(mx, vmax);
    a.sum = s[0] + s[1] + s[2] + s[3];
    a.min = mn[0]; a.max = mx[0];
    for (int k = 1; k < 4; ++k) { if (mn[k] < a.min) a.min = mn[k]; if (mx[k] > a.max) a.max = mx[k]; }
  }
  for (; i < hi; ++i) {  // scalar tail
    const double x = v[i];
    a.sum += x;
    if (x < a.min) a.min = x;
    if (x > a.max) a.max = x;
  }
  a.count = n;
  return a;
}

inline bool cpu_has_avx2() {
#if defined(__GNUC__)
  unsigned eax, ebx, ecx, edx;
  if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) return false;
  return (ebx & (1u << 5)) != 0;  // AVX2 = CPUID.7.EBX[5]
#else
  return false;
#endif
}
#endif  // __x86_64__

// Aggregate values v[lo,hi), dispatching to AVX2 when available.
inline Aggregate aggregate_range(const double* v, size_t lo, size_t hi) {
#if defined(__x86_64__)
  static const bool kAvx2 = cpu_has_avx2();
  if (kAvx2) return aggregate_avx2(v, lo, hi);
#endif
  return aggregate_scalar(v, lo, hi);
}

}  // namespace tsdb
