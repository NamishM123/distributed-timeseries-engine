# distributed-timeseries-engine

A C++20 columnar time-series store with Gorilla compression, consistent-hash
sharding across nodes, and a gRPC serving layer, deployable on Kubernetes.

```
          ┌──────────┐   gRPC    ┌─────────────┐
clients ─▶│  router  │──────────▶│  tsdb_node  │  columnar engine + Gorilla
          │ (hash    │           ├─────────────┤  chunks, SIMD scans
          │  ring,   │──────────▶│  tsdb_node  │
          │  R=2)    │           ├─────────────┤
          └──────────┘──────────▶│  tsdb_node  │
             │  ▲                 └─────────────┘
             │  └ health probes (evict dead nodes → reroute to replicas)
             └── consistent hashing w/ 256 vnodes, R-way replication
```

## What's here

| Layer | Files | Summary |
|-------|-------|---------|
| Bit codec | `include/tsdb/bitstream.hpp` | MSB-first bit reader/writer |
| Compression | `include/tsdb/gorilla.hpp` | Gorilla delta-of-delta timestamps + XOR values (lossless) |
| SIMD scan | `include/tsdb/simd_scan.hpp` | AVX2 aggregation with scalar fallback + runtime CPUID dispatch |
| Storage engine | `include/tsdb/engine.hpp` | Per-series chunk chains, striped locking, range queries |
| Sharding | `include/tsdb/consistent_hash.hpp` | Hash ring, virtual nodes, ordered replica sets |
| Serving | `src/server_main.cpp` | gRPC node (`tsdb_node`) + `grpc.health.v1` |
| Coordinator | `src/router_main.cpp` | gRPC router: shard routing, replication, health-based self-healing |
| Benchmarks | `bench/*.cpp` | ingest / compression / query / SIMD |
| Tests | `test/*.cpp` | GoogleTest unit + fuzz tests |
| Cluster sim | `sim/cluster_sim.py` | 3→8 scaling efficiency on the real hash ring |
| Deploy | `docker/`, `k8s/`, `docker-compose.yml` | container + StatefulSet/Deployment/HPA |

## Build & test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```
Requires g++≥11 (C++20), CMake≥3.16, protobuf + gRPC dev packages
(`protobuf-compiler-grpc libgrpc++-dev libprotobuf-dev`), and GoogleTest for the
unit tests.

## Run the benchmarks

```bash
scripts/run_benchmarks.sh
```
See [METRICS.md](METRICS.md) for the measured results and how each resume
figure is derived.

## Run a local cluster

```bash
# three nodes + a router, via docker compose
docker compose up --build -d
docker compose run --rm loadgen           # writes + queries through the router

# or without containers:
TSDB_NODE_ID=n0 TSDB_LISTEN=0.0.0.0:50051 ./build/tsdb_node &
TSDB_NODE_ID=n1 TSDB_LISTEN=0.0.0.0:50052 ./build/tsdb_node &
TSDB_NODE_ID=n2 TSDB_LISTEN=0.0.0.0:50053 ./build/tsdb_node &
TSDB_NODES="n0=localhost:50051,n1=localhost:50052,n2=localhost:50053" \
  TSDB_REPLICAS=2 ./build/tsdb_router &
./build/loadgen localhost:50050 1000 1000 50
```

## Design notes

- **Columnar Gorilla chunks.** Each series is a chain of immutable chunks plus
  one open chunk. Timestamps and values are *separate* bit streams, so a scan
  that only needs timestamps never touches value bytes. Timestamps use
  delta-of-delta with variable-length buckets; values use XOR-against-previous
  storing only the meaningful bit window. Decoding is zero-copy (the decoder
  borrows the chunk buffer); the rvalue constructor is deleted to prevent
  decode-a-temporary use-after-free.
- **Concurrency.** The engine stripes the series map across 64 shared-mutexes
  and each series guards its own chunk chain, so ingestion across many series
  scales with cores.
- **Sharding & replication.** Keys map to the first vnode clockwise on a
  256-vnode ring; the replica set is the next R distinct physical nodes. Adding
  or removing a node remaps only ~1/N of keys (verified in tests).
- **Self-healing.** The router health-probes every backend; after 2 misses
  (~3–4s) it drops the node from the ring so reads fail over to replicas.
  Kubernetes liveness probes (5s × 3) restart the pod and it rejoins — see
  [METRICS.md](METRICS.md#self-healing) for the ~20s recovery budget.
