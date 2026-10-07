#pragma once
//
// Synthetic-but-realistic monitoring workload generator.
//
// Gorilla's compression and query numbers only mean anything on data shaped
// like real metrics, so benchmarks and the load generator share this model
// instead of hand-rolling ad-hoc loops. The mix mirrors a Prometheus
// node_exporter scrape: mostly flat/rarely-changing gauges, constant-rate
// counters, and a minority of low-precision wandering gauges, scraped at a
// near-constant interval with occasional jitter.
//
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "tsdb/gorilla.hpp"

namespace tsdb {

enum class MetricKind { kFlatGauge, kCounter, kWanderGauge };

struct WorkloadConfig {
  int series = 1000;
  int samples_per_series = 2000;
  int64_t start_ms = 1700000000000LL;
  int64_t interval_ms = 15000;  // 15s scrape
  uint64_t seed = 1234;
  // Mix proportions (should sum to ~1.0): flat, counter, wander.
  double flat_frac = 0.5;
  double counter_frac = 0.3;
  // remainder is wandering gauges
};

class Workload {
 public:
  explicit Workload(WorkloadConfig cfg) : cfg_(cfg), rng_(cfg.seed) {}

  std::string series_key(int i) const {
    static const char* metrics[] = {"cpu.usage", "mem.used", "net.rx", "disk.io", "http.reqs"};
    return std::string(metrics[i % 5]) + "{host=h" + std::to_string(i / 5 % 256) +
           ",inst=" + std::to_string(i) + "}";
  }

  MetricKind kind_of(int i) const {
    const double r = (i % 100) / 100.0;
    if (r < cfg_.flat_frac) return MetricKind::kFlatGauge;
    if (r < cfg_.flat_frac + cfg_.counter_frac) return MetricKind::kCounter;
    return MetricKind::kWanderGauge;
  }

  // Fill `out` with one series' samples; advances the internal RNG.
  void generate_series(int i, std::vector<Sample>* out) {
    out->clear();
    out->reserve(cfg_.samples_per_series);
    const MetricKind k = kind_of(i);
    int64_t ts = cfg_.start_ms;
    double v = initial_value(k);
    for (int s = 0; s < cfg_.samples_per_series; ++s) {
      // Near-constant cadence with rare ±1ms jitter (clock skew / scrape drift).
      ts += cfg_.interval_ms + ((rng_() % 200 == 0) ? (static_cast<int>(rng_() % 3) - 1) : 0);
      v = next_value(k, v);
      out->push_back(Sample{ts, v});
    }
  }

 private:
  double initial_value(MetricKind k) {
    switch (k) {
      case MetricKind::kFlatGauge: return static_cast<double>(rng_() % 100);
      case MetricKind::kCounter: return 1000.0;
      case MetricKind::kWanderGauge: return 0.75;
    }
    return 0.0;
  }

  double next_value(MetricKind k, double v) {
    switch (k) {
      case MetricKind::kFlatGauge:
        return (rng_() % 100 < 85) ? v : static_cast<double>(rng_() % 100);  // 85% flat
      case MetricKind::kCounter:
        return v + 2.0;  // constant rate
      case MetricKind::kWanderGauge: {
        double nv = v + (static_cast<int>(rng_() % 3) - 1) * 0.01;
        if (nv < 0) nv = 0;
        return std::round(nv * 100) / 100;  // 2 decimal places
      }
    }
    return v;
  }

  WorkloadConfig cfg_;
  std::mt19937_64 rng_;
};

}  // namespace tsdb
