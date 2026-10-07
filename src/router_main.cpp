// tsdb_router: the cluster coordinator. Exposes the same TimeSeries gRPC
// surface as a node and forwards each series to the node(s) that own it on a
// consistent-hash ring.
//
//   * Write     -> replicate each series to its first R distinct ring nodes.
//   * QueryRange-> try the primary owner, fall back to the next live replica.
//   * Health    -> aggregate stats across live backends.
//
// A background health checker pings every backend; after kFailThreshold missed
// probes a node is dropped from the ring, so its keys immediately re-route to
// their replicas (self-healing). A recovered node is added back. Ring changes
// touch only ~1/N of the keyspace thanks to virtual nodes.
//
// Discovery (env):
//   TSDB_NODES   comma list of id=host:port  (e.g. n0=10.0.0.1:50051,n1=...)
//   TSDB_LISTEN  router listen address (default 0.0.0.0:50050)
//   TSDB_REPLICAS replication factor R (default 2)
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "tsdb.grpc.pb.h"
#include "tsdb/consistent_hash.hpp"

using namespace tsdb;
using Stub = tsdb::v1::TimeSeries::Stub;

namespace {
std::string env_or(const char* k, const std::string& dflt) {
  const char* v = std::getenv(k);
  return v ? std::string(v) : dflt;
}
constexpr int kFailThreshold = 2;  // missed probes before eviction
}  // namespace

// Holds one backend's address, stub, and health state.
struct Backend {
  std::string id;
  std::string addr;
  std::unique_ptr<Stub> stub;
  int misses = 0;
  bool live = true;
};

class Router final : public tsdb::v1::TimeSeries::Service {
 public:
  Router(const std::string& nodes_spec, int replicas) : replicas_(replicas) {
    std::stringstream ss(nodes_spec);
    std::string tok;
    auto trim = [](std::string s) {
      const auto a = s.find_first_not_of(" \t\r\n");
      const auto b = s.find_last_not_of(" \t\r\n");
      return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    while (std::getline(ss, tok, ',')) {
      tok = trim(tok);
      if (tok.empty()) continue;
      const auto eq = tok.find('=');
      if (eq == std::string::npos) continue;
      const std::string id = trim(tok.substr(0, eq));
      const std::string addr = trim(tok.substr(eq + 1));
      auto b = std::make_unique<Backend>();
      b->id = id;
      b->addr = addr;
      b->stub = tsdb::v1::TimeSeries::NewStub(
          grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));
      ring_.add_node(id);
      backends_[id] = std::move(b);
    }
    checker_ = std::thread([this] { health_loop(); });
  }

  ~Router() override {
    stop_.store(true);
    if (checker_.joinable()) checker_.join();
  }

  grpc::Status Write(grpc::ServerContext*, const tsdb::v1::WriteRequest* req,
                     tsdb::v1::WriteResponse* resp) override {
    // Group series by target node (primary + replicas), then one RPC per node.
    std::unordered_map<std::string, tsdb::v1::WriteRequest> batches;
    {
      std::shared_lock<std::shared_mutex> lk(mu_);
      for (const auto& series : req->series()) {
        auto targets = ring_.replica_set(series.key(), replicas_);
        for (const auto& node : targets) *batches[node].add_series() = series;
      }
    }
    uint64_t accepted = 0;
    for (auto& [node, batch] : batches) {
      Stub* stub = stub_for(node);
      if (!stub) continue;
      grpc::ClientContext ctx;
      tsdb::v1::WriteResponse r;
      if (stub->Write(&ctx, batch, &r).ok()) accepted += r.accepted();
    }
    resp->set_accepted(accepted);
    resp->set_node("router");
    return grpc::Status::OK;
  }

  grpc::Status QueryRange(grpc::ServerContext*, const tsdb::v1::QueryRequest* req,
                          tsdb::v1::QueryResponse* resp) override {
    std::vector<std::string> targets;
    {
      std::shared_lock<std::shared_mutex> lk(mu_);
      targets = ring_.replica_set(req->key(), replicas_);
    }
    for (const auto& node : targets) {  // primary first, then replicas
      Stub* stub = stub_for(node);
      if (!stub) continue;
      grpc::ClientContext ctx;
      if (stub->QueryRange(&ctx, *req, resp).ok()) {
        resp->set_node(node);
        return grpc::Status::OK;
      }
    }
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "no live replica for key");
  }

  grpc::Status Health(grpc::ServerContext*, const tsdb::v1::HealthRequest*,
                      tsdb::v1::HealthResponse* resp) override {
    uint64_t series = 0, samples = 0;
    int live = 0;
    std::shared_lock<std::shared_mutex> lk(mu_);
    for (auto& [id, b] : backends_) {
      if (!b->live) continue;
      ++live;
      grpc::ClientContext ctx;
      tsdb::v1::HealthRequest hr;
      tsdb::v1::HealthResponse h;
      if (b->stub->Health(&ctx, hr, &h).ok()) {
        series += h.series();
        samples += h.samples();
      }
    }
    resp->set_node("router(live=" + std::to_string(live) + ")");
    resp->set_series(series);
    resp->set_samples(samples);
    return grpc::Status::OK;
  }

 private:
  Stub* stub_for(const std::string& node) {
    std::shared_lock<std::shared_mutex> lk(mu_);
    auto it = backends_.find(node);
    return it == backends_.end() ? nullptr : it->second->stub.get();
  }

  void health_loop() {
    using namespace std::chrono_literals;
    while (!stop_.load()) {
      for (auto& [id, b] : backends_) {
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + 1s);
        tsdb::v1::HealthRequest hr;
        tsdb::v1::HealthResponse h;
        const bool ok = b->stub->Health(&ctx, hr, &h).ok();
        std::unique_lock<std::shared_mutex> lk(mu_);
        if (ok) {
          b->misses = 0;
          if (!b->live) {  // recovered -> rejoin ring
            b->live = true;
            ring_.add_node(id);
            fprintf(stderr, "health: node %s recovered, re-added to ring\n", id.c_str());
          }
        } else if (b->live && ++b->misses >= kFailThreshold) {
          b->live = false;  // evict -> keys re-route to replicas
          ring_.remove_node(id);
          fprintf(stderr, "health: node %s evicted after %d misses\n", id.c_str(), b->misses);
        }
      }
      std::this_thread::sleep_for(2s);
    }
  }

  int replicas_;
  mutable std::shared_mutex mu_;
  ConsistentHashRing ring_;
  std::map<std::string, std::unique_ptr<Backend>> backends_;
  std::thread checker_;
  std::atomic<bool> stop_{false};
};

int main(int argc, char** argv) {
  const std::string nodes = env_or("TSDB_NODES", argc > 1 ? argv[1] : "");
  const std::string listen = env_or("TSDB_LISTEN", "0.0.0.0:50050");
  const int replicas = std::atoi(env_or("TSDB_REPLICAS", "2").c_str());
  if (nodes.empty()) {
    fprintf(stderr, "error: set TSDB_NODES=id=host:port,...\n");
    return 1;
  }

  Router router(nodes, replicas);
  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen, grpc::InsecureServerCredentials());
  builder.RegisterService(&router);
  builder.SetMaxReceiveMessageSize(64 * 1024 * 1024);
  auto server = builder.BuildAndStart();
  fprintf(stderr, "tsdb_router listening on %s (R=%d, backends=%s)\n", listen.c_str(), replicas,
          nodes.c_str());
  server->Wait();
  return 0;
}
