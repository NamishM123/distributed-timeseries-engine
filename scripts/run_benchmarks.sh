#!/usr/bin/env bash
# Build in Release and run the full benchmark suite. Numbers printed here are
# the ones quoted in README.md / METRICS.md. Override sizes via the env vars
# below for quick runs.
set -euo pipefail
cd "$(dirname "$0")/.."

: "${SERIES:=3000}"
: "${SAMPLES:=2000}"
: "${QSERIES:=5000}"
: "${QSAMPLES:=4000}"
: "${QUERIES:=20000}"
: "${THREADS:=$(nproc)}"

echo "==> configuring + building (Release)"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc)" >/dev/null

echo; echo "==> unit tests"
( cd build && ctest --output-on-failure )

echo; echo "============================================================"
echo "COMPRESSION  (realistic Prometheus-like workload)"
echo "============================================================"
./build/bench_compression "$SERIES" "$SAMPLES"

echo; echo "============================================================"
echo "INGEST THROUGHPUT"
echo "============================================================"
./build/bench_ingest "$SERIES" "$SAMPLES" "$THREADS"

echo; echo "============================================================"
echo "QUERY LATENCY"
echo "============================================================"
./build/bench_query "$QSERIES" "$QSAMPLES" "$QUERIES" 500

echo; echo "============================================================"
echo "SIMD SCAN SPEEDUP"
echo "============================================================"
./build/bench_simd

echo; echo "============================================================"
echo "CLUSTER SCALING (3 -> 8 nodes, simulated on the real hash ring)"
echo "============================================================"
python3 sim/cluster_sim.py
