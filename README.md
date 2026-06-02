# SAkGD — Graph Drawing Contest k-planarity Solver

Minimises the **k-value** of a straight-line graph drawing — the maximum number of
times any single edge is crossed by other edges.

Based on: Bianchetti & Moalic, *"Winning the GD Challenge for the 4th Time"*,
GD 2025 ([LIPIcs.GD.2025.43](https://drops.dagstuhl.de/entities/document/10.4230/LIPIcs.GD.2025.43)).

---

## Problem

Given a graph, place every node at an integer coordinate so that no two nodes
share a point and no node lies on the interior of an edge it does not belong to.
Minimise **k = max crossings on any single edge**, breaking ties by total crossing count.

---

## Files

| File | Description |
|------|-------------|
| `src/main.cpp` | Original solver (`./sakgd`) — Simulated Annealing only. JSON parser and spatial grid included; no external dependencies. |
| `src/approach1_lns.cpp` | Approach 1 solver (`./approach1`) — adds Large Neighbourhood Search on top of the same SA infrastructure. Supports `--mode sa` (identical to `./sakgd`) and `--mode lns`. |
| `dashboard.py` | Runs multiple solver workers in parallel and serves a live browser dashboard. |
| `pipeline.py` | Overnight batch runner: cycles through all 9 contest graphs, warm-starting each round from the previous best output. |
| `dashboard/index.html` | Vanilla-JS frontend for the live dashboard. |
| `data/` | Input graphs (JSON) and example solutions. |
| `results/` | Best outputs per contest graph, plus per-run logs. |
| `Makefile` | `make` / `make debug` / `make clean`. |

---

## Algorithms

Both algorithms share the same two-phase structure and validity checks.

### Phase 1 — Minimise total crossings

Fitness = change in total crossing count `ΔC`.
A worsening move is accepted with probability `exp(−ΔC / T)`.

### Phase 2 — Minimise k-value

Dual-level fitness:
- Primary: change in the local max crossing count among incident edges and their
  crossing partners.
- Tie-breaker: change in total crossing count (normalised).

---

### SA — Simulated Annealing (default, `--mode sa`)

Classic single-node SA following the paper (Algorithm 1):

- **Node selection** — biased toward nodes with many crossings on incident edges
  (weight ≈ `1 + Σ xc[e]`).
- **Position selection** — Gaussian around the current position (`σ ∝ √(T/T₀)`),
  with a 5 % chance of a global random jump in Phase 1.
- **Wave restarts** — temperature is reset each wave; layout is restored from the
  best-known solution at the start of each wave.

Parameters (paper Table 1):

| Phase | T₀ | decT | decTW | tLim |
|-------|-----|------|-------|------|
| 1 (crossings) | 50 | 0.999 | 0.99 | 0.01 |
| 2 (k-value)   | 1  | 0.9999 | 0.99 | 0.01 |

---

### LNS — Large Neighbourhood Search (`--mode lns`)

Approach 1. Moves a connected *group* of nodes per iteration rather than one
at a time, which lets it escape local optima that SA can get stuck in.

**Destroy** — select a BFS-connected neighbourhood of K nodes starting from a
crossing-weight-biased seed.

**Repair** — for each node in the neighbourhood (shuffled), try R random candidate
positions and greedily commit the best strictly-improving one.

**Restart** — every `500/K` iterations, snap back to the best-known layout to
prevent quality drift.

The temperature controls the Gaussian exploration radius (same `selectPlace` as SA)
and decays geometrically from `T₀` to `tLim` over the phase budget.

---

## Build

```bash
make           # builds both ./sakgd and ./approach1  (-O3)
make debug     # debug build with ASAN/UBSAN
make clean
```

Requires only a C++17 compiler (GCC or Clang). No external libraries.

---

## Usage

```bash
# SA — 60 min total, 10 min phase 1
./sakgd -i data/graph.json -o out.json -t 60 -p1 10

# LNS (Approach 1) — same timing
./approach1 -i data/graph.json -o out.json -t 60 -p1 10 --mode lns

# LNS with explicit neighbourhood size and candidates
./approach1 -i data/625-nodes.json -o out.json --mode lns --nh-size 30 --nh-cands 100

# SA via approach1 (identical result to ./sakgd)
./approach1 -i data/graph.json -o out.json --mode sa

# Verify a solution (reports k, total crossings, validity)
./sakgd --verify out.json

# Reproducible run
./sakgd -i data/graph.json -o out.json -s 12345
```

### Shared flags (`./sakgd` and `./approach1`)

| Flag | Default | Description |
|------|---------|-------------|
| `-i PATH` | — | Input JSON |
| `-o PATH` | `<input>.out.json` | Output JSON |
| `-t MINUTES` | 60 | Total time budget |
| `-p1 MINUTES` | 10 | Phase 1 budget |
| `-s SEED` | time-based | RNG seed |
| `--status-file PATH` | — | Write live status JSON (for dashboard) |
| `--status-id STRING` | `run` | Label shown in the dashboard |
| `--status-interval SEC` | `1.0` | Status write interval |
| `--verify` | — | Report metrics and exit |

### `./approach1`-only flags

| Flag | Default | Description |
|------|---------|-------------|
| `--mode {sa\|lns}` | `sa` | Algorithm to run |
| `--nh-size K` | `n/10` | LNS neighbourhood size |
| `--nh-cands R` | `50` | LNS candidate positions per node |

---

## Dashboard (parallel runs + live UI)

```bash
# Interactive — prompts for graph, workers, time
./dashboard.py

# CLI — 4 workers, 60 min each
./dashboard.py data/graph.json -n 4 -t 60 -p1 10
```

Opens a browser tab showing live crossing counts, k-values, temperature progress,
and a mini SVG preview for each worker. The best result across workers is
highlighted.

---

## Pipeline (overnight batch)

```bash
# Run all 9 contest graphs, 4 workers each, until 23:00
python3 pipeline.py --graphs 1-9 --workers 4 --minutes 60 --until 23:00
```

Each round warm-starts from the previous best output stored in `results/`.

---

## JSON format

```json
{
  "width": 1000000, "height": 1000000,
  "nodes": [ { "id": 0, "x": 100, "y": 200 }, ... ],
  "edges": [ { "source": 0, "target": 1 }, ... ]
}
```

`id` may be a string or a number. If `x`/`y` are present they are used as the
initial layout; otherwise nodes are placed randomly. The output preserves the
full input structure, updating only `nodes[*].x` and `nodes[*].y`.

---

## Reference

Bianchetti, J., & Moalic, L. (2025). *Winning the GD Challenge for the 4th Time:
Our Approach.* 33rd International Symposium on Graph Drawing and Network
Visualization, GD 2025. LIPIcs.GD.2025.43.
