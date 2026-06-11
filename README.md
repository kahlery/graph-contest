# SAkGD — Graph Drawing Contest k-planarity Solver

Minimises the **k-value** of a straight-line graph drawing — the maximum number of
times any single edge is crossed by other edges.

Based on: Bianchetti & Moalic, *"Winning the GD Challenge for the 4th Time"*,
GD 2025 ([LIPIcs.GD.2025.43](https://drops.dagstuhl.de/entities/document/10.4230/LIPIcs.GD.2025.43)).

Ayrıntılı yaklaşım/metot dokümantasyonu ve makalenin üzerine eklediklerimiz:
[docs/YAKLASIMLAR.md](docs/YAKLASIMLAR.md).

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
| `tools/stress_init.py` | Stress/force-directed initial layout generator (graphviz sfdp/neato + grid snap). Powers the `sa-stress` method — strongest on sparse graphs. |
| `dashboard.py` | Runs multiple solver workers in parallel and serves a live browser dashboard. |
| `run_contest.py` | Batch runner: solves the 9 contest graphs with each method (`sa`, `staged`, `ils`, `lns`), tracks every run in `results/runs/`, maintains best-ever layouts in `results/best/`, and generates `results/report.html`. |
| `dashboard/index.html` | Vanilla-JS frontend for the live dashboard. |
| `data/` | Input graphs (JSON) and example solutions. |
| `data/gda-testing/` | External benchmark suite from [YouSafe/gda-testing](https://github.com/YouSafe/gda-testing) — 219 generated graphs (circulant, kronecker, SBM, random planar, etc.) plus a reference team's k-results (`stats/team-1-*.csv`) for generalization checks beyond Automatic-1..9. |
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

### ILS — Iterated Local Search (`--mode ils`)

Approach 2. Alternates full SA annealing runs with random *kicks* that break
the current layout out of its local optimum:

**Inner SA** — runs the full two-phase SA for an inner budget (`totalTime / 5`).

**Kick** — restores the global best, then randomly relocates `P` nodes
(`--ils-perturb P`, default `n/10`) to fresh canvas positions.

Repeat until the total time budget is exhausted, keeping the global best
across all inner rounds.

This directly addresses the LNS limitation: LNS only ever commits strictly-
improving moves and stalls once it finds a local optimum. ILS restarts from a
genuinely disrupted layout, giving SA a new basin to explore.

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

# ILS (Approach 1) — same timing, default kick size (n/10)
./approach1 -i data/graph.json -o out.json -t 60 -p1 10 --mode ils

# ILS with explicit kick size
./approach1 -i data/graph.json -o out.json -t 60 --mode ils --ils-perturb 20

# LNS (Approach 1)
./approach1 -i data/graph.json -o out.json -t 60 -p1 10 --mode lns

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
| `--mode {sa\|lns\|ils}` | `sa` | Algorithm to run |
| `--nh-size K` | `n/10` | LNS neighbourhood size |
| `--nh-cands R` | `50` | LNS candidate positions per node |
| `--ils-perturb P` | `n/10` | ILS kick size (nodes randomly relocated per kick) |

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

## Batch pipeline (`run_contest.py`)

Solves the 9 contest graphs (`data/live-2025-contest/live-contest/Automatic-*.json`)
with one or more methods and writes everything to `results/`.

```bash
# Smoke test: 30 s per (graph × method), fast graphs
python3 run_contest.py --minutes 0.5 --methods sa,staged,ils --graphs 1-7 --seed 42

# Real run: 10 min per combination, all 9 graphs
python3 run_contest.py --minutes 10 --methods sa,staged,ils --graphs 1-9 --seed 42

# Warm-start from the best layouts found in previous runs
python3 run_contest.py --minutes 10 --methods sa,staged,ils --warm-start

# Just regenerate the HTML report without running the solver
python3 run_contest.py --report-only

# Also run a graph from the gda-testing benchmark suite (data/gda-testing/graphs/);
# entries are ";"-separated so commas in filenames are unambiguous
python3 run_contest.py --methods sa-stress --graphs "8;circulant_graph/100_[1,2,3].json"
```

**Methods**

| Method | What it runs |
|--------|--------------|
| `sa`     | Pure Simulated Annealing (`./sakgd`). |
| `staged` | Contest-style chain: LNS for `--staged-lns-frac` of budget (default 30 %) to descend fast, then SA warm-started on that output for the remaining 70 %. |
| `ils`    | Iterated Local Search: full SA inner run → random kick (relocate `n/10` nodes) → repeat, keeping global best across rounds. Escapes local optima that SA and LNS both get stuck in. |
| `lns`    | Pure LNS (`./approach1 --mode lns`). |

**Output layout**

```
results/
  runs/
    2026-06-03_14-30-52_t10.0m_s42/   # every run is preserved
      Automatic-1/  sa.json sa.log  staged*.json/log  ils.json ils.log
      summary.csv   summary.md
  best/
    Automatic-1/  sa.json  staged.json  ils.json   # best-ever per graph × method
  bests.json       # best k metadata (updated after every run)
  history.json     # full run log
  report.html      # interactive HTML report (bar charts + history table)
```

**Notes / caveats**

- `staged` launches the solver **twice** per graph (LNS leg then SA leg).
- `ils` splits the budget into ~5 inner SA rounds with kicks between them.
- The solver counts all initial crossings *before* the `-t` budget starts.
  On **Automatic-8** (10,466 nodes, ~45 M crossings) this setup is ~6 min per
  launch — give the big graphs a long budget (`--minutes 60` or more).
- **Automatic-8/9** inputs contain vertex-edge overlaps the solver may not be
  able to repair in a short budget; those runs are marked `[INVALID]` / ⚠️.
- Open `results/report.html` in any browser after a run for the visual summary
  (requires internet access for the Chart.js CDN).

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
