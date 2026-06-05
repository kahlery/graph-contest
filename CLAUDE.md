# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

```bash
make              # builds ./sakgd (SA only) and ./approach1 (SA + LNS + ILS)
make debug        # -O0 with ASAN/UBSAN
make clean
```

Requires only C++17, no external libraries. Source files: `src/main.cpp` → `./sakgd`, `src/approach1_lns.cpp` → `./approach1`.

## Running the solvers

```bash
# SA — 60 min total, 10 min phase 1
./sakgd -i data/graph.json -o out.json -t 60 -p1 10

# ILS or LNS via approach1
./approach1 -i data/graph.json -o out.json -t 60 --mode ils
./approach1 -i data/graph.json -o out.json -t 60 --mode lns

# Verify a solution
./sakgd --verify out.json
```

## Batch pipeline

```bash
# Quick smoke test (30 s per graph×method, fast graphs only)
python3 run_contest.py --minutes 0.5 --methods sa,staged,ils --graphs 1-7 --seed 42

# Real run
python3 run_contest.py --minutes 10 --methods sa,staged,ils --graphs 1-9

# Warm-start from best previous outputs
python3 run_contest.py --minutes 10 --methods sa,staged,ils --warm-start

# Regenerate report.html without re-running solvers
python3 run_contest.py --report-only
```

`pipeline.sh` is a continuous loop: git-pull → rebuild → run_contest → cooldown. Configured via `.env` (WORKERS, MIN_SMALL, MIN_MEDIUM, MIN_LARGE, COOLDOWN_SEC, NH_SIZE_CAP, NH_CANDS).

## Architecture

**Problem**: place graph nodes at integer coordinates to minimise *k* = max crossings on any single edge.

**Two-phase structure** (shared by all algorithms):
- Phase 1 — minimise total crossing count (fitness = ΔC, SA acceptance)
- Phase 2 — minimise k-value (dual-level fitness: primary = Δlocal-max, tiebreak = ΔtotalX)

**Algorithms** (`--mode` flag on `./approach1`):
- `sa` — pure Simulated Annealing; moves one node per step, biased selection toward high-crossing nodes, Gaussian position sampling
- `lns` — Large Neighbourhood Search; BFS-connected group of K nodes destroyed then greedily repaired; only strictly-improving commits (can stall at local optima)
- `ils` — Iterated Local Search; inner SA for `budget/5` → kick (relocate n/10 nodes randomly) → repeat; best practical escaper from local optima
- `staged` — LNS for 30% of budget (fast descent) then SA warm-started on result for remaining 70%

**Key caveat — Automatic-8**: 10,466 nodes, ~45 M crossings. Initial crossing setup takes ~6 min per solver launch before the `-t` budget starts. Always give this graph ≥60 min.

**Output layout**:
```
results/
  runs/<timestamp>/   # every run preserved; per-graph per-method .json + .log
  best/               # best-ever layout per graph × method
  bests.json          # best k metadata
  history.json        # full run log
  report.html         # interactive HTML report (Chart.js from CDN)
```

**Dashboard** (`dashboard.py`): parallel workers with a live browser UI showing k-values, temperature, and SVG previews. Run interactively or as `./dashboard.py data/graph.json -n 4 -t 60`.

## JSON format

```json
{"width": 1000000, "height": 1000000,
 "nodes": [{"id": 0, "x": 100, "y": 200}, ...],
 "edges": [{"source": 0, "target": 1}, ...]}
```

Output preserves all input fields, updating only `nodes[*].x` / `nodes[*].y`.
