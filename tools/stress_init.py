#!/usr/bin/env python3
"""Stress/force-directed initial layout generator (GD-2025 k-planarity).

Replicates the initialization recipe of the winning SAkGD entry
(Bianchetti & Moalic, LIPIcs.GD.2025.43): a deterministic stress layout
plus repeated stochastic force-directed runs, keeping the candidate with
the lowest sampled crossing density. Uses graphviz engines as the layout
back-ends: neato (stress majorization ~ OGDF StressMinimization) and
sfdp (multilevel force ~ OGDF FMMM).

Continuous coordinates are scaled to fill the contest grid, snapped to
integer points, and position collisions are resolved by a spiral search
for the nearest free cell. Vertex-on-edge overlaps are left to the
solver's existing repair pass.

Output is a contest-format JSON usable as a warm-start input for sakgd.
"""
import argparse
import json
import math
import random
import subprocess
import sys
import time

import numpy as np


def load_graph(path):
    with open(path) as f:
        data = json.load(f)
    nodes = data.get("nodes") or data.get("Nodes")
    edges = data.get("edges") or data.get("Edges")
    W = int(data.get("width", data.get("Width", 1000000)))
    H = int(data.get("height", data.get("Height", 1000000)))
    x0 = int(data.get("x", data.get("X", 0)))
    y0 = int(data.get("y", data.get("Y", 0)))
    ids = []
    for i, nd in enumerate(nodes):
        ids.append(nd.get("id", i))
    idx = {str(v): i for i, v in enumerate(ids)}
    earr = []
    for ev in edges:
        s = ev.get("source", ev.get("from"))
        t = ev.get("target", ev.get("to"))
        earr.append((idx[str(s)], idx[str(t)]))
    return data, ids, earr, W, H, x0, y0


# Logical engine name -> (graphviz binary, extra graph args). "sfdp-spread"
# adds overlap removal + larger node separation/ideal edge length so the
# layout spreads more uniformly across the canvas. After snap_to_grid
# rescales the bounding box to fill WxH, a more uniform distribution means
# lower local density -> fewer crossings on a DENSE graph (Automatic-6),
# where plain force-directed clustering is punished.
SPREAD_ARGS = ["-Goverlap=prism", "-Gsep=+12", "-GK=2.0"]
ENGINE_MAP = {
    "sfdp":        ("sfdp",  []),
    "sfdp-spread": ("sfdp",  SPREAD_ARGS),
    "neato":       ("neato", []),
    "fdp":         ("fdp",   []),
}


def run_graphviz(engine, n, edges, seed, timeout):
    """Run a graphviz engine, return float coords array [n,2] or None."""
    binary, gv_extra = ENGINE_MAP.get(engine, (engine, []))
    lines = ["graph G {", 'node [shape=point];']
    lines.extend(f"{i};" for i in range(n))
    lines.extend(f"{a}--{b};" for a, b in edges)
    lines.append("}")
    dot = "\n".join(lines)
    cmd = [binary, "-Tplain", f"-Gstart={seed}"] + gv_extra
    if binary == "neato" and n > 1000:
        # subset model: sparse stress terms, scales past a few thousand nodes
        cmd.append("-Gmodel=subset")
    try:
        proc = subprocess.run(cmd, input=dot, capture_output=True,
                              text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None
    if proc.returncode != 0:
        return None
    pos = np.zeros((n, 2))
    seen = 0
    for line in proc.stdout.splitlines():
        if line.startswith("node "):
            parts = line.split()
            i = int(parts[1])
            pos[i, 0] = float(parts[2])
            pos[i, 1] = float(parts[3])
            seen += 1
    return pos if seen == n else None


def snap_to_grid(pos, W, H, x0, y0, rng):
    """Scale continuous coords to fill the grid, snap to distinct points."""
    n = len(pos)
    lo = pos.min(axis=0)
    hi = pos.max(axis=0)
    span = np.maximum(hi - lo, 1e-9)
    sx = (pos[:, 0] - lo[0]) / span[0] * W
    sy = (pos[:, 1] - lo[1]) / span[1] * H
    ix = np.clip(np.rint(sx).astype(np.int64), 0, W)
    iy = np.clip(np.rint(sy).astype(np.int64), 0, H)

    occupied = {}
    out = np.zeros((n, 2), dtype=np.int64)
    order = list(range(n))
    rng.shuffle(order)
    for i in order:
        x, y = int(ix[i]), int(iy[i])
        if (x, y) not in occupied:
            occupied[(x, y)] = i
            out[i] = (x, y)
            continue
        placed = False
        for r in range(1, max(W, H) + 1):
            for dx in range(-r, r + 1):
                for dy in (-r, r) if abs(dx) < r else range(-r, r + 1):
                    cx, cy = x + dx, y + dy
                    if 0 <= cx <= W and 0 <= cy <= H and (cx, cy) not in occupied:
                        occupied[(cx, cy)] = i
                        out[i] = (cx, cy)
                        placed = True
                        break
                if placed:
                    break
            if placed:
                break
        if not placed:
            raise RuntimeError("grid full: cannot place all nodes")
    out[:, 0] += x0
    out[:, 1] += y0
    return out


def sampled_avg_crossings(coords, edges_np, rng, samples=200000):
    """Estimate average crossings per edge by sampling edge pairs."""
    m = len(edges_np)
    total_pairs = m * (m - 1) // 2
    if total_pairs == 0:
        return 0.0
    s = min(samples, total_pairs)
    a = rng.integers(0, m, size=s)
    b = rng.integers(0, m, size=s)
    keep = a != b
    a, b = a[keep], b[keep]
    ea, eb = edges_np[a], edges_np[b]
    # exclude pairs sharing an endpoint
    share = ((ea[:, 0:1] == eb) | (ea[:, 1:2] == eb)).any(axis=1)
    a, b = a[~share], b[~share]
    if len(a) == 0:
        return 0.0
    p1 = coords[edges_np[a, 0]].astype(np.float64)
    p2 = coords[edges_np[a, 1]].astype(np.float64)
    q1 = coords[edges_np[b, 0]].astype(np.float64)
    q2 = coords[edges_np[b, 1]].astype(np.float64)

    def cross(o, u, v):
        return (u[:, 0] - o[:, 0]) * (v[:, 1] - o[:, 1]) - \
               (u[:, 1] - o[:, 1]) * (v[:, 0] - o[:, 0])

    d1 = cross(p1, p2, q1)
    d2 = cross(p1, p2, q2)
    d3 = cross(q1, q2, p1)
    d4 = cross(q1, q2, p2)
    inter = (d1 * d2 < 0) & (d3 * d4 < 0)
    rate = inter.mean()
    est_total = rate * total_pairs
    return 2.0 * est_total / m


def exact_avg_crossings(coords, edges_np):
    """EXACT average crossings per edge (full pairwise, chunked over the
    first edge). The sampled estimate above is noisy on dense graphs
    (Automatic-6: ~4.5M edge pairs) and can pick a worse candidate layout;
    an exact count over all pairs removes that selection noise. Cheap for
    small m (O(m^2) but vectorised over the second edge, <1s at m=3000)."""
    m = len(edges_np)
    if m < 2:
        return 0.0
    coords = coords.astype(np.float64)
    P1 = coords[edges_np[:, 0]]
    P2 = coords[edges_np[:, 1]]
    e0, e1 = edges_np[:, 0], edges_np[:, 1]
    total = 0
    for i in range(m - 1):
        j = i + 1
        a1, a2 = P1[i], P2[i]
        q1, q2 = P1[j:], P2[j:]
        share = ((e0[j:] == e0[i]) | (e0[j:] == e1[i]) |
                 (e1[j:] == e0[i]) | (e1[j:] == e1[i]))
        # orientation tests (strict crossing == both straddle)
        d1 = (a2[0] - a1[0]) * (q1[:, 1] - a1[1]) - (a2[1] - a1[1]) * (q1[:, 0] - a1[0])
        d2 = (a2[0] - a1[0]) * (q2[:, 1] - a1[1]) - (a2[1] - a1[1]) * (q2[:, 0] - a1[0])
        d3 = (q2[:, 0] - q1[:, 0]) * (a1[1] - q1[:, 1]) - (q2[:, 1] - q1[:, 1]) * (a1[0] - q1[:, 0])
        d4 = (q2[:, 0] - q1[:, 0]) * (a2[1] - q1[:, 1]) - (q2[:, 1] - q1[:, 1]) * (a2[0] - q1[:, 0])
        inter = (d1 * d2 < 0) & (d3 * d4 < 0) & (~share)
        total += int(inter.sum())
    return 2.0 * total / m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-i", "--input", required=True)
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("-s", "--seed", type=int, default=1)
    ap.add_argument("-t", "--time-budget", type=float, default=60.0,
                    help="seconds for repeated layout attempts")
    ap.add_argument("--engine", default="auto",
                    choices=["auto", "neato", "sfdp", "both",
                             "sfdp-spread", "fdp", "all"])
    ap.add_argument("--max-attempts", type=int, default=8)
    ap.add_argument("--exact", type=int, default=0,
                    help="1 = select candidate by EXACT crossing count "
                         "(removes sampling noise; use on small/dense graphs)")
    args = ap.parse_args()

    t0 = time.time()
    data, ids, edges, W, H, x0, y0 = load_graph(args.input)
    n = len(ids)
    edges_np = np.array(edges, dtype=np.int64)
    rng_np = np.random.default_rng(args.seed)
    rng_py = random.Random(args.seed)

    if args.engine == "auto":
        # sfdp first: much faster and empirically lower crossing density;
        # neato (stress) only as a secondary candidate on smaller graphs
        engines = ["sfdp"] if n > 3000 else ["sfdp", "neato"]
    elif args.engine == "both":
        engines = ["neato", "sfdp"]
    elif args.engine == "all":
        engines = ["sfdp", "neato", "fdp"]
    else:
        engines = [args.engine]

    best_coords, best_score, best_tag = None, math.inf, ""
    attempt = 0
    while attempt < args.max_attempts:
        remain = args.time_budget - (time.time() - t0)
        if attempt > 0 and remain <= 1.0:
            break
        engine = engines[attempt % len(engines)]
        gv_seed = args.seed * 1000 + attempt
        # always give the first attempt a generous timeout so at least one
        # candidate layout exists even when the stage budget is tiny
        pos = run_graphviz(engine, n, edges, gv_seed,
                           timeout=max(remain, 30.0))
        attempt += 1
        if pos is None:
            print(f"[stress-init] attempt {attempt} {engine}: failed/timeout",
                  file=sys.stderr)
            continue
        # random rotation diversifies grid snapping across workers/attempts
        ang = rng_py.uniform(0, 2 * math.pi)
        c, s_ = math.cos(ang), math.sin(ang)
        pos = pos @ np.array([[c, -s_], [s_, c]])
        try:
            coords = snap_to_grid(pos, W, H, x0, y0, rng_py)
        except RuntimeError as e:
            print(f"[stress-init] attempt {attempt} {engine}: {e}",
                  file=sys.stderr)
            continue
        score = (exact_avg_crossings(coords, edges_np) if args.exact
                 else sampled_avg_crossings(coords, edges_np, rng_np))
        print(f"[stress-init] attempt {attempt} {engine}: "
              f"{'exact' if args.exact else 'est'} avg crossings/edge = "
              f"{score:.2f}", file=sys.stderr)
        if score < best_score:
            best_coords, best_score, best_tag = coords, score, engine

    if best_coords is None:
        print("[stress-init] all attempts failed; copying input layout",
              file=sys.stderr)
        with open(args.input) as f:
            raw = f.read()
        with open(args.output, "w") as f:
            f.write(raw)
        return 0

    nodes_key = "nodes" if "nodes" in data else "Nodes"
    for i, nd in enumerate(data[nodes_key]):
        nd["x"] = int(best_coords[i, 0])
        nd["y"] = int(best_coords[i, 1])
    with open(args.output, "w") as f:
        json.dump(data, f)
    print(f"[stress-init] done: engine={best_tag} "
          f"est avg crossings/edge={best_score:.2f} "
          f"elapsed={time.time() - t0:.1f}s", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
