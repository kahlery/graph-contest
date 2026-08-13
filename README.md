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

## Project structure

The whole toolchain is native C++ — no Python, no runtime dependency beyond a
C++17 compiler (and `graphviz`'s `sfdp`/`neato` binaries, shelled out to by
`stress_init` for the `sa-stress` init).

```
graph-contest/
├── Makefile                  # make / make debug / make clean -> everything into ./bin/
├── bin/                       # gitignored build output — every binary lands here
│   ├── sakgd
│   ├── approach1
│   ├── stress_init
│   ├── run_contest
│   ├── contest_orchestrate
│   ├── server
│   └── bench_reheat_sharing
├── src/
│   ├── main.cpp              # -> bin/sakgd — Simulated Annealing only
│   ├── approach1_lns.cpp     # -> bin/approach1 — adds Large Neighbourhood Search
│   ├── common/                # shared engine, used by every tool below
│   │   ├── json.hpp           # minimal JSON parser/serializer (contest format)
│   │   ├── subprocess.hpp     # fork/exec helpers: deadlines, combined-log capture
│   │   ├── paths.hpp          # resolves each binary's own directory
│   │   └── engine.hpp         # graph/method registry, run_method/run_combo, bests.json
│   ├── tools/
│   │   ├── stress_init.cpp    # -> bin/stress_init — force-directed init, powers `sa-stress`
│   │   └── bench_reheat_sharing.cpp  # -> bin/bench_reheat_sharing — reheat/xchg sweep harness
│   ├── runner/run_contest.cpp        # -> bin/run_contest — batch runner over graph×method combos
│   ├── orchestrator/contest_orchestrate.cpp  # -> bin/contest_orchestrate — contest-day scheduler
│   └── server/server.cpp             # -> bin/server — web UI (embeds its own HTML/JS)
├── data/
│   ├── input/                 # every subdirectory here is a selectable "input set"
│   │   ├── internal-contest/      # the 9 official contest graphs (Automatic-1..9.json)
│   │   └── 2025-real-contest/     # small example instances + solutions
│   ├── archive/               # older/auxiliary graph suites, not listed or selectable
│   │   ├── gda-testing/           # external benchmark suite (github.com/YouSafe/gda-testing)
│   │   ├── final-graphs/
│   │   └── exports/
│   └── output/                # solver outputs, mirrors data/input/'s per-set layout
│       ├── internal-contest/      # one folder per input set (gitignored per-run heavy files)
│       │   ├── best/                  # best-ever layout per graph × method
│       │   ├── runs/                  # every run, preserved
│       │   ├── submission/            # contest_orchestrate's per-run final layouts
│       │   ├── bests.json             # best k metadata (updated after every run)
│       │   └── report.html            # static summary table, Swiss design matching the web UI
│       ├── 2025-real-contest/
│       ├── exp-*.md, orchestrator-slide.html, sa-stress-best/  # write-ups (not per-set)
└── docs/                     # write-ups, figures, and presentation slides
    ├── YAKLASIMLAR.md
    ├── phase-screenshots/
    ├── algo-doc/
    └── presentation/         # LaTeX slides
```

Every tool in `bin/` resolves its own binary's directory at startup
(`src/common/paths.hpp`) and treats its parent as the repo root, so
`data/input/`, `data/output/`, `./bin/sakgd`/`./bin/approach1`, and each other are
always found regardless of the current working directory — the same
property the old Python scripts got from `Path(__file__).resolve().parent.parent`.

**Input sets.** `run_contest` and `contest_orchestrate` both operate on
"an input set": a directory of `*.json` graphs, selected one of three ways,
in this priority order:
1. `--graphs-dir PATH` — an arbitrary explicit folder (also how `data/archive/`
   stays reachable without being listed).
2. `--input-set NAME` — shorthand for `data/input/NAME`.
3. Neither given — the tool lists `data/input/`'s subdirectories and prompts
   interactively on stdin. (A non-interactive caller, e.g. the web UI's
   spawned child, always passes one of the above; hitting stdin EOF at the
   prompt fails fast with an error instead of hanging.)

**Output layout mirrors input.** Unless `--out-dir` is given explicitly,
output nests under `data/output/<name>`, where `<name>` is whichever input
set (or `--graphs-dir`'s basename) was used — so `data/input/internal-contest/`
and `data/output/internal-contest/` line up, and two different sets never
collide on graph names.

**Scope note:** the batch runner's method registry only carries the methods
this README documents as the production set (`sa`, `sa-warm`, `sa-stress`,
`staged`, `staged-adaptive`, `ils`). The prior Python version also carried
~15 one-off dated research variants (`sa-stress-sq2/pro/cong/swap/lahc/thr`,
`sa-hilbert`, `sa-bary`, `sa-cong`, `sa-swap`, and `-base` A/B comparison
twins) whose own source comments already recorded their verdicts ("lost the
A/B", "kept only for further exploration") — those were dropped rather than
ported. The elaborate Chart.js `report.html` (convergence charts, run
history) was also replaced with a plain static table plus the live-polling
web UI below, per instruction to keep the web UI basic.

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
- **Best-of-C benefit analysis (`--cands C`, production methods use 4)** — each
  move plans C candidate positions exactly (full `planMove` delta) and feeds only
  the best ΔE to the acceptance rule. Trades move volume for move quality; wins
  on every internal-2026 graph. `--cands-ramp 1` (used by cold starts) ramps C
  as 1 → C/2 → C at 30 % / 60 % of the phase budget, since the early descent
  prefers volume. `--place smart` adds neighbour-informed proposals
  (random-subset centroid / near a random neighbour / two-neighbour midpoint);
  it lowers totalX but not k, so it stays opt-in.

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
make           # builds ./bin/sakgd, ./bin/approach1, and the contest tooling into ./bin/
               # (stress_init, run_contest, contest_orchestrate, server,
               # bench_reheat_sharing)  (-O3)
make debug     # debug build of the two solvers with ASAN/UBSAN
make clean
```

Requires only a C++17 compiler (GCC or Clang) and pthreads. No external
libraries — `stress_init`'s `sa-stress` init additionally shells out to
`sfdp`/`neato` (graphviz) if installed, falling back to the input layout
if they aren't.

---

## Usage

```bash
# SA — 60 min total, 10 min phase 1
./bin/sakgd -i data/input/graph.json -o out.json -t 60 -p1 10

# ILS (Approach 1) — same timing, default kick size (n/10)
./bin/approach1 -i data/input/graph.json -o out.json -t 60 -p1 10 --mode ils

# ILS with explicit kick size
./bin/approach1 -i data/input/graph.json -o out.json -t 60 --mode ils --ils-perturb 20

# LNS (Approach 1)
./bin/approach1 -i data/input/graph.json -o out.json -t 60 -p1 10 --mode lns

# SA via approach1 (identical result to ./bin/sakgd)
./bin/approach1 -i data/input/graph.json -o out.json --mode sa

# Verify a solution (reports k, total crossings, validity)
./bin/sakgd --verify out.json

# Reproducible run
./bin/sakgd -i data/input/graph.json -o out.json -s 12345
```

### Shared flags (`./bin/sakgd` and `./bin/approach1`)

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

### Lexicographic tempered SA (`./bin/sakgd`, all opt-in)

Ported from Daniel Kohrt's TUM practical talk; measured in
[docs/notes/exp-2026-08-07-lexicographic-tempered.md](docs/notes/exp-2026-08-07-lexicographic-tempered.md).
The first three only pay off **together** (mean k −7.9% / −5.4% / −2.3% on
internal-2026 05 / 06 / 08), and they regress the small graphs — hence the
`sa-lex` method rather than a new default. `--pt` showed no reliable win at a
3-minute budget.

| Flag | Default | Description |
|------|---------|-------------|
| `--lex4 0\|1` | `0` | Four-level objective `(k, n_k, Φ=Σcr², total)` with staged acceptance; exact global k and n_k |
| `--ops6 0\|1` | `0` | Six displacement operators: (focus worst edges \| random vertex) × (centroid \| local \| regional) |
| `--aos 0\|1` | `0` | Online bandit (probability matching) over those six operators |
| `--op-local F` | `0.01` | Local move radius, fraction of `sqrt(W·H)` |
| `--op-regional F` | `0.10` | Regional move radius, fraction of `sqrt(W·H)` |
| `--aos-alpha F` | `0.001` | Bandit learning rate |
| `--pt R` | `0` | Tempered SA: R replicas on a temperature ladder, one thread each, periodic exchange |
| `--pt-rounds N` | `8` | Exchange rounds in phase 2 |
| `--pt-lo F` / `--pt-hi F` | `0.25` / `4` | Ladder ends, as multiples of `--p2-t0` |
| `--lex-check 0\|1` | `0` | Debug: verify each committed `--lex4` move's predicted `(k, n_k)` against the truth |

```bash
# the measured-best combination (method id: sa-lex)
./bin/sakgd -i graph.json -o out.json -t 3 -p1 0.6 --lex4 1 --ops6 1 --aos 1
```

### `./bin/approach1`-only flags

| Flag | Default | Description |
|------|---------|-------------|
| `--mode {sa\|lns\|ils}` | `sa` | Algorithm to run |
| `--nh-size K` | `n/10` | LNS neighbourhood size |
| `--nh-cands R` | `50` | LNS candidate positions per node |
| `--ils-perturb P` | `n/10` | ILS kick size (nodes randomly relocated per kick) |

---

## Web UI (`./bin/server`)

```bash
./bin/server --port 8080
```

A deliberately basic, light-mode control panel (embedded HTML/CSS/JS, no
external assets) that polls `/api/status` every 3 seconds. Start/stop either
the batch runner or the contest orchestrator from the browser, watch the
live combined log tail, and see `bests.json` (best k/totalX per graph ×
method) update as runs complete. Only one run is active at a time.

---

## Batch pipeline (`./bin/run_contest`)

Solves every graph in a chosen input set (see **Input sets** above) with one
or more methods and writes everything to `data/output/<input-set-name>/`.

```bash
# Prompts: which input set? (lists data/input/*)
./bin/run_contest --minutes-small 0.5 --minutes-medium 0.5 --methods sa,staged,ils --seed 42

# Explicit input set, no prompt
./bin/run_contest --input-set internal-contest --minutes-small 10 --minutes-medium 10 --minutes-large 10 \
                   --methods sa,staged,ils --seed 42

# Warm-start from the best layouts found in previous runs
./bin/run_contest --input-set internal-contest --methods sa,staged,ils --warm-start

# Just regenerate report.html from data/output/internal-contest/ without running the solver
./bin/run_contest --input-set internal-contest --report-only

# Explicit folder (e.g. the archived gda-testing benchmark suite, not a listed input set)
./bin/run_contest --graphs-dir data/archive/gda-testing/graphs/circulant_graph --methods sa-stress
```

Budget group (`small`/`medium`/`large`) is picked from each graph's own
`n + m` (node + edge count), not a hardcoded per-graph table — any input set
gets sensible budget scaling automatically, calibrated so the 9 official
contest graphs land in the same groups as before.

**Methods**

| Method | What it runs |
|--------|--------------|
| `sa`               | Pure Simulated Annealing (`./bin/sakgd`). |
| `sa-warm`          | Single `sakgd` stage forced to `--init input`; used by the orchestrator to resume from a best-so-far layout. |
| `sa-stress`        | `./bin/stress_init` (force-directed layout) then SA warm-started on it — strongest on sparse graphs. |
| `staged`           | LNS for `--staged-lns-frac` of budget (default 30 %) to descend fast, then SA warm-started on that output for the remaining 70 %. |
| `staged-adaptive`  | Same as `staged` but with `approach1 --mode lns-adaptive`. |
| `ils`              | Iterated Local Search: full SA inner run → random kick (relocate `n/10` nodes) → repeat, keeping global best across rounds. |

**Cooperative worker sharing** (`--xchg-rounds R`, `--xchg-half`)

By default the W workers are an independent portfolio. `--xchg-rounds R` (R>1)
splits the budget into R rounds; after each round the lowest-k "elite" layout is
shared and workers warm-start the next round from it. `--xchg-half` keeps the
low-half worker ids independent (exploration) and lets the high-half adopt the
elite (intensification) — *no-regret*: it captures sharing's gains on high-k
graphs without losing the independent edge on low-k bottleneck graphs. See
[data/output/exp-2026-06-30-reheat-sharing.md](data/output/exp-2026-06-30-reheat-sharing.md).
Recommended for `sa-stress`: `--workers 6 --xchg-rounds 4 --xchg-half`
(stagnation `--reheat` was tested and does **not** help — leave it off).

**Output layout** (per input set — see **Output layout mirrors input** above)

```
data/output/<input-set-name>/
  runs/
    2026-06-03_14-30-52_t10.0m_s42/   # every run is preserved
      Automatic-1/  sa.json sa.log  staged*.json/log  ils.json ils.log
      summary.csv   summary.md
  best/
    Automatic-1/  sa.json  staged.json  ils.json   # best-ever per graph × method
  bests.json       # best k metadata (updated after every run)
  history.json     # full run log
  report.html      # plain static summary table (best k/totalX per graph × method)
```

**Notes / caveats**

- `staged` launches the solver **twice** per graph (LNS leg then SA leg).
- `ils` splits the budget into ~5 inner SA rounds with kicks between them.
- The solver counts all initial crossings *before* the `-t` budget starts.
  On **Automatic-8** (10,466 nodes, ~45 M crossings) this setup is ~6 min per
  launch — give the big graphs a long budget (`--minutes-large 60` or more).
- **Automatic-8/9** inputs contain vertex-edge overlaps the solver may not be
  able to repair in a short budget; those runs are marked `[INVALID]` / ⚠️.
- `report.html` is a plain, no-JS static table — open it in any browser, no
  internet connection needed. For a live view while a run is in progress,
  use `./bin/server` instead (see below).

---

## Contest orchestrator (`./bin/contest_orchestrate`)

Deadline-safe adaptive scheduler for a single wall-clock budget (e.g. 40–50 min).
Analyses each graph (n, m, density), picks the cold method per graph, gives every
graph one explore lease, then **reallocates the rest of the budget to the hardest
graphs** (priority `current_k × improving_boost ÷ leases`, so the worst graphs get
the most time while least-serviced peers rotate in). Every lease runs the validated
half-sharing config (`--xchg-rounds 4`, half independent / half shared). A
per-subprocess backstop guarantees it **never overruns** the budget, reserving time
for the final per-graph verify + submission copy.

```bash
./bin/contest_orchestrate --input-set internal-contest --budget 2700 --workers 8
#   -> data/output/internal-contest/submission/<run_id>/<graph>.json   (one best VALID layout per graph)
#      data/output/internal-contest/bests.json                          (best-k metadata)
./bin/contest_orchestrate --self-test         # pure-logic checks, no solver

# Full-contest Daniel-profile A/B against a stored one-hour baseline.
# Every graph starts cold; warm layouts exist only inside this run's leases.
caffeinate -dims ./bin/contest_orchestrate \
  --input-set internal-2026 --budget 3600 --workers 8 --seed 19 \
  --search-profile daniel-all --baseline-run corch_1784476912
```

`--search-profile daniel-all` is the default after the full-hour seed-19 A/B
improved sum-k `495 → 486`; pass `--search-profile current` for the frozen
control. `daniel-all` leaves each graph's
stress/tripod/gradx/staged initialisation and the scheduler unchanged, but
replaces every subsequent SA phase-2 search (including warm continuations)
with `--lex4 1 --ops6 1 --aos 1`. The run report records seed, profile, source
revision, and aggregated operator statistics. When `--baseline-run` is set,
`comparison.json`/`comparison.md` are written next to `orchestration.json` and
copied into the per-run submission folder.

Validated: on a 420 s budget it finishes in 381 s with a valid layout for all 9
graphs (see [data/output/exp-2026-06-30-reheat-sharing.md](data/output/exp-2026-06-30-reheat-sharing.md)).
Use a fresh `--out-dir` per contest so `bests.json` starts clean.

Point it at any folder of graphs, e.g. one not listed under `data/input/`
(submissions keep the original file names):

```bash
./bin/contest_orchestrate --graphs-dir /path/to/graphs --budget 2700 --workers 8
```

**In the web UI** (`./bin/server`): pick the Orchestrator tab, pick an input
set from the dropdown (or an explicit graphs folder), set budget and workers,
then Start; the log tail and `bests.json` table update on every 3 s poll.

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
