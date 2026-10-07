// loadgen: a gRPC client that drives a node or router with the realistic
// workload, then runs sample range queries. Used for end-to-end smoke tests and
// demos against a running cluster.
//
//   loadgen <target_addr> [series] [samples] [batch]
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "tsdb.grpc.pb.h"
#include "tsdb/workload.hpp"

using namespace tsdb;
using clk = std::chrono::steady_clock;

int main(int argc, char** argv) {
  const std::string target = argc > 1 ? argv[1] : "localhost:50050";
  WorkloadConfig cfg;
  cfg.series = argc > 2 ? std::atoi(argv[2]) : 500;
  cfg.samples_per_series = argc > 3 ? std::atoi(argv[3]) : 1000;
  const int batch = argc > 4 ? std::atoi(argv[4]) : 50;  // series per Write RPC

  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  auto stub = tsdb::v1::TimeSeries::NewStub(channel);

  Workload wl(cfg);
  std::vector<std::string> keys(cfg.series);
  std::vector<std::vector<Sample>> data(cfg.series);
  for (int i = 0; i < cfg.series; ++i) {
    keys[i] = wl.series_key(i);
    wl.generate_series(i, &data[i]);
  }
  const uint64_t total = uint64_t(cfg.series) * cfg.samples_per_series;

  const auto t0 = clk::now();
  uint64_t accepted = 0;
  for (int i = 0; i < cfg.series; i += batch) {
    tsdb::v1::WriteRequest req;
    for (int j = i; j < std::min(i + batch, cfg.series); ++j) {
      auto* s = req.add_series();
      s->set_key(keys[j]);
      for (const auto& x : data[j]) {
        auto* smp = s->add_samples();
        smp->set_timestamp_ms(x.ts);
        smp->set_value(x.value);
      }
    }
    grpc::ClientContext ctx;
    tsdb::v1::WriteResponse resp;
    auto st = stub->Write(&ctx, req, &resp);
    if (!st.ok()) {
      fprintf(stderr, "write failed: %s\n", st.error_message().c_str());
      return 1;
    }
    accepted += resp.accepted();
  }
  const auto t1 = clk::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  printf("wrote %lu samples in %.2fs = %.0f points/sec (accepted %lu)\n", (unsigned long)total,
         secs, total / secs, (unsigned long)accepted);

  // A few queries, including a server-side aggregation.
  const int64_t span = int64_t(cfg.samples_per_series) * cfg.interval_ms;
  for (int q = 0; q < 3; ++q) {
    tsdb::v1::QueryRequest qr;
    qr.set_key(keys[q]);
    qr.set_start_ms(cfg.start_ms);
    qr.set_end_ms(cfg.start_ms + span);
    qr.set_agg(q == 0 ? tsdb::v1::AGG_NONE : tsdb::v1::AGG_AVG);
    grpc::ClientContext ctx;
    tsdb::v1::QueryResponse resp;
    const auto t = clk::now();
    auto st = stub->QueryRange(&ctx, qr, &resp);
    const double us = std::chrono::duration<double, std::micro>(clk::now() - t).count();
    if (!st.ok()) {
      fprintf(stderr, "query failed: %s\n", st.error_message().c_str());
      continue;
    }
    if (qr.agg() == tsdb::v1::AGG_NONE)
      printf("query %s: %d samples from node=%s in %.0fus\n", keys[q].c_str(), resp.samples_size(),
             resp.node().c_str(), us);
    else
      printf("query %s: avg=%.3f (scanned %lu) node=%s in %.0fus\n", keys[q].c_str(), resp.scalar(),
             (unsigned long)resp.scanned(), resp.node().c_str(), us);
  }
  return 0;
}
