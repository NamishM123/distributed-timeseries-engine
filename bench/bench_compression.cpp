// Compression ratio on the realistic workload: raw 16 bytes/point (int64 ts +
// double value) vs Gorilla-encoded bytes. Also verifies lossless round-trip.
#include <cstdio>
#include <cstring>
#include <vector>

#include "tsdb/gorilla.hpp"
#include "tsdb/workload.hpp"

using namespace tsdb;

int main(int argc, char** argv) {
  WorkloadConfig cfg;
  cfg.series = argc > 1 ? std::atoi(argv[1]) : 2000;
  cfg.samples_per_series = argc > 2 ? std::atoi(argv[2]) : 2000;

  Workload wl(cfg);
  std::vector<Sample> s;
  uint64_t raw = 0, comp = 0, points = 0, mismatches = 0;
  uint64_t per_kind_raw[3] = {0, 0, 0}, per_kind_comp[3] = {0, 0, 0};

  for (int i = 0; i < cfg.series; ++i) {
    wl.generate_series(i, &s);
    GorillaEncoder enc;
    for (const auto& x : s) enc.append(x.ts, x.value);
    auto chunk = enc.finish();

    GorillaDecoder dec(chunk);
    std::vector<Sample> out;
    dec.decode_all(out);
    for (size_t j = 0; j < s.size(); ++j) {
      if (out[j].ts != s[j].ts || std::memcmp(&out[j].value, &s[j].value, 8) != 0) ++mismatches;
    }

    const uint64_t r = uint64_t(s.size()) * 16, c = chunk.size();
    raw += r;
    comp += c;
    points += s.size();
    const int k = static_cast<int>(wl.kind_of(i));
    per_kind_raw[k] += r;
    per_kind_comp[k] += c;
  }

  const char* names[] = {"flat-gauge", "counter   ", "wander-gge"};
  printf("per-kind compression:\n");
  for (int k = 0; k < 3; ++k) {
    if (per_kind_comp[k])
      printf("  %s : %6.2fx  (%.3f bytes/pt)\n", names[k],
             double(per_kind_raw[k]) / per_kind_comp[k],
             double(per_kind_comp[k]) / (per_kind_raw[k] / 16));
  }
  printf("------------------------------------------------\n");
  printf("points        : %lu\n", (unsigned long)points);
  printf("raw bytes     : %lu (16 B/pt)\n", (unsigned long)raw);
  printf("gorilla bytes : %lu (%.3f B/pt)\n", (unsigned long)comp, double(comp) / points);
  printf("compression   : %.2fx\n", double(raw) / comp);
  printf("round-trip    : %s (%lu mismatches)\n", mismatches ? "FAIL" : "lossless",
         (unsigned long)mismatches);
  return mismatches ? 1 : 0;
}
