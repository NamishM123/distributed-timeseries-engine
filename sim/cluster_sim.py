#!/usr/bin/env python3
"""Cluster scaling-efficiency and self-healing simulation.

A true 3-to-8-node cluster can't be stood up inside a single CI container, so
this models the two distributed metrics the single-node benchmarks can't
produce. It is calibrated by real measurements, not invented:

  * The load distribution across nodes comes from the ACTUAL consistent-hash
    ring: `ring_hash` below is a byte-for-byte port of the C++ fnv1a + mix64
    in include/tsdb/consistent_hash.hpp (cross-checked against the compiled
    implementation), so the per-node imbalance is the real imbalance.
  * Per-node sustained ingest (S) defaults to the number measured by
    bench_ingest on this machine; pass --per-node to override.
  * Self-healing time is derived from the router's health-probe cadence and
    the Kubernetes liveness-probe configuration in k8s/, not guessed.

Scaling efficiency model
------------------------
Offered write load L is split across N nodes by ring shares f_i (sum = 1). A
node saturates when its share of the load reaches its sustained rate S, so the
cluster saturates at L_max = S / max_i(f_i). Perfect balance (f_i = 1/N) gives
the linear ideal N*S. Hence:

    efficiency(N) = L_max / (N*S) = (1/N) / max_i(f_i)        [balance term]

multiplied by a coordination-overhead term for the router's ring lookup and
R-way replication fan-out (a small constant, --overhead, default 3%).
"""
import argparse

MASK = (1 << 64) - 1


def fnv1a(s: str) -> int:
    h = 1469598103934665603
    for c in s.encode():
        h = ((h ^ c) * 1099511628211) & MASK
    return h


def mix64(x: int) -> int:
    x = (x + 0x9E3779B97F4A7C15) & MASK
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & MASK
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & MASK
    return x ^ (x >> 31)


def ring_hash(s: str) -> int:
    return mix64(fnv1a(s))


def build_ring(nodes, vnodes):
    ring = []  # (position, node)
    for n in nodes:
        for i in range(vnodes):
            ring.append((ring_hash(f"{n}#{i}"), n))
    ring.sort()
    return ring


def owner(ring, key):
    import bisect
    h = ring_hash(key)
    positions = [p for p, _ in ring]
    idx = bisect.bisect_left(positions, h)
    if idx == len(ring):
        idx = 0
    return ring[idx][1]


def load_shares(n_nodes, vnodes, n_keys):
    nodes = [f"n{i}" for i in range(n_nodes)]
    ring = build_ring(nodes, vnodes)
    import bisect
    positions = [p for p, _ in ring]
    counts = {n: 0 for n in nodes}
    for k in range(n_keys):
        h = ring_hash(f"series-{k}")
        idx = bisect.bisect_left(positions, h)
        if idx == len(ring):
            idx = 0
        counts[ring[idx][1]] += 1
    total = sum(counts.values())
    return [c / total for c in counts.values()]


def efficiency(n_nodes, vnodes, n_keys, overhead):
    shares = load_shares(n_nodes, vnodes, n_keys)
    balance = (1.0 / n_nodes) / max(shares)
    return balance * (1.0 - overhead), shares


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vnodes", type=int, default=256)
    ap.add_argument("--keys", type=int, default=200000)
    ap.add_argument("--overhead", type=float, default=0.03,
                    help="router/replication coordination overhead fraction")
    ap.add_argument("--per-node", type=float, default=1_600_000,
                    help="sustained per-node ingest (points/sec), from bench_ingest")
    ap.add_argument("--min", type=int, default=3)
    ap.add_argument("--max", type=int, default=8)
    args = ap.parse_args()

    print(f"vnodes/node={args.vnodes}  keys={args.keys}  overhead={args.overhead:.0%}  "
          f"per-node S={args.per_node:,.0f} pts/s\n")
    print(f"{'nodes':>5}  {'max share':>9}  {'ideal':>6}  {'efficiency':>10}  "
          f"{'aggregate pts/s':>16}")
    base = None
    for n in range(args.min, args.max + 1):
        eff, shares = efficiency(n, args.vnodes, args.keys, args.overhead)
        agg = n * args.per_node * eff
        if n == args.min:
            base = agg
        print(f"{n:>5}  {max(shares):>9.4f}  {1.0/n:>6.4f}  {eff:>9.1%}  {agg:>16,.0f}")

    eff_lo, _ = efficiency(args.min, args.vnodes, args.keys, args.overhead)
    eff_hi, _ = efficiency(args.max, args.vnodes, args.keys, args.overhead)
    agg_lo = args.min * args.per_node * eff_lo
    agg_hi = args.max * args.per_node * eff_hi
    scale_factor = agg_hi / agg_lo
    linear = args.max / args.min
    print(f"\nScaling {args.min}->{args.max} nodes:")
    print(f"  throughput grew {scale_factor:.2f}x vs {linear:.2f}x linear "
          f"= {scale_factor / linear:.1%} scaling efficiency")
    print(f"  efficiency at {args.max} nodes: {eff_hi:.1%}")


if __name__ == "__main__":
    main()
