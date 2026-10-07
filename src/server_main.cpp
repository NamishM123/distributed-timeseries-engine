// tsdb_node: a single storage node. Serves the TimeSeries gRPC service backed
// by the in-process columnar Engine. Stateless with respect to peers — sharding
// and replication are the router's job; a node just stores what it is given.
//
// Flags (env or positional):
//   TSDB_NODE_ID   logical node id (default: hostname)
//   TSDB_LISTEN    listen address (default 0.0.0.0:50051)
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <unistd.h>

#include <grpcpp/grpcpp.h>

#include "tsdb.grpc.pb.h"
#include "tsdb/engine.hpp"

using namespace tsdb;

namespace {
std::string env_or(const char* k, const std::string& dflt) {
  const char* v = std::getenv(k);
  return v ? std::string(v) : dflt;
}
std::string hostname() {
  char buf[256] = {0};
  gethostname(buf, sizeof(buf) - 1);
  return buf[0] ? std::string(buf) : std::string("node");
}
}  // namespace

class NodeService final : public tsdb::v1::TimeSeries::Service {
 public:
  explicit NodeService(std::string id) : id_(std::move(id)) {}

  grpc::Status Write(grpc::ServerContext*, const tsdb::v1::WriteRequest* req,
                     tsdb::v1::WriteResponse* resp) override {
    uint64_t accepted = 0;
    for (const auto& series : req->series()) {
      for (const auto& s : series.samples()) {
        engine_.append(series.key(), s.timestamp_ms(), s.value());
        ++accepted;
      }
    }
    samples_.fetch_add(accepted, std::memory_order_relaxed);
    resp->set_accepted(accepted);
    resp->set_node(id_);
    return grpc::Status::OK;
  }

  grpc::Status QueryRange(grpc::ServerContext*, const tsdb::v1::QueryRequest* req,
                          tsdb::v1::QueryResponse* resp) override {
    const QueryResult r = engine_.query(req->key(), req->start_ms(), req->end_ms());
    resp->set_node(id_);
    resp->set_scanned(r.size());
    using tsdb::v1::Aggregation;
    if (req->agg() == Aggregation::AGG_NONE) {
      for (size_t i = 0; i < r.size(); ++i) {
        auto* s = resp->add_samples();
        s->set_timestamp_ms(r.timestamps[i]);
        s->set_value(r.values[i]);
      }
    } else {
      const Aggregate a = aggregate_range(r.values.data(), 0, r.values.size());
      double scalar = 0;
      switch (req->agg()) {
        case Aggregation::AGG_SUM: scalar = a.sum; break;
        case Aggregation::AGG_AVG: scalar = a.avg(); break;
        case Aggregation::AGG_MIN: scalar = a.count ? a.min : 0; break;
        case Aggregation::AGG_MAX: scalar = a.count ? a.max : 0; break;
        case Aggregation::AGG_COUNT: scalar = static_cast<double>(a.count); break;
        default: break;
      }
      resp->set_scalar(scalar);
    }
    return grpc::Status::OK;
  }

  grpc::Status Health(grpc::ServerContext*, const tsdb::v1::HealthRequest*,
                      tsdb::v1::HealthResponse* resp) override {
    resp->set_node(id_);
    resp->set_series(engine_.series_count());
    resp->set_samples(samples_.load(std::memory_order_relaxed));
    return grpc::Status::OK;
  }

 private:
  std::string id_;
  Engine engine_;
  std::atomic<uint64_t> samples_{0};
};

int main(int argc, char** argv) {
  const std::string id = env_or("TSDB_NODE_ID", argc > 1 ? argv[1] : hostname());
  const std::string listen = env_or("TSDB_LISTEN", argc > 2 ? argv[2] : "0.0.0.0:50051");

  NodeService service(id);
  // Register the standard grpc.health.v1 service so Kubernetes' native gRPC
  // liveness/readiness probes work out of the box.
  grpc::EnableDefaultHealthCheckService(true);
  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  builder.SetMaxReceiveMessageSize(64 * 1024 * 1024);
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  fprintf(stderr, "tsdb_node id=%s listening on %s\n", id.c_str(), listen.c_str());
  server->Wait();
  return 0;
}
