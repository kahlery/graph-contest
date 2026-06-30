# sa-stress: reheat + worker-sharing tuning (2026-06-30)

Branch: `feat/sa-stress-reheat-sharing`. Goal: for the **sa-stress** method only,
understand and tune (a) the **reheat** temperature schedule and (b) **cross-worker
best-layout sharing** — how often workers should share — and measure on the live
contest graphs.

## The two mechanisms (as they were)

**Reheat** (`src/main.cpp::runSA`), three layers:
- *Wave restart* — each wave cools `startingTemp *= decTW (0.99)`, restores best.
- *Stagnation reheat* (`--reheat N`, **default OFF**) — if `bestK` doesn't drop for
  N waves, reset wave temp to `initT` (=1 in phase-2, fully hot).
- *Budget-fill reheat* (always on) — when temp hits floor `tLim`, restore best and
  reheat to `reheatCeil = max(tLim*8, ceil*0.5)` (a decaying ceiling).

**Worker sharing** (`run_contest.py::run_combo`, `xchg_rounds`, **default 1 = OFF**)
— budget splits into R rounds; after each round a barrier syncs all workers, the
lowest-k "elite" is elected into a shared file, and every worker warm-starts the
next round from it. It was disabled everywhere (orchestrate.py also hardcodes 1).

## Method

Lean harness (`scratchpad/bench.py`): caches one stress-init per (graph, seed) so
every reheat/sharing variant is compared on **identical inits** (init RNG removed as
a confound); mirrors real sa-stress (warm sakgd, p1_frac=0.2, kband=2). Metric is
best-of-workers **k** (tiebreak totalX), verified with `sakgd --verify` (rejects
vertex-edge overlaps). Graphs chosen by hardness (25s/1w screen): A1=0, A7=1 are
trivial; the discriminating set is **A3=13, A4=36, A8=88, A6=271, A5=278**.

## Results

**Round 1** (W=3, 90s, seeds 1–3, graphs 3,4,5,6,8) — sum_k:
| base | rh2 | rh4 | rh8 | x2 | x4 |
|---|---|---|---|---|---|
| 642 | 642 | 651 | 638 | 644 | **630** |

**Round 2** (W=3, 90s, **fresh** seeds 11–13, graphs 4,5,6,8) — sum_k:
| base | x4 | x6 | x8 | rh8_x6 |
|---|---|---|---|---|
| 636 | **626** | 629 | 627 | 632 |

**Validation** (W=5, 150s, seeds 1–5, graphs 4,5,6,8):
| config | A4 | A5 | A6 | A8 | sum_k |
|---|---|---|---|---|---|
| base | **32** | 249 | 247 | **83** | 611 |
| x4 | 34 | 241 | 241 | 84 | 600 |

**Half-sharing** (W=6, 150s, seeds 21–26, graphs 4,5,6,8):
| config | A4 | A5 | A6 | A8 | sum_k |
|---|---|---|---|---|---|
| base (all independent) | 35 | 246 | 254 | 83 | 618 |
| x4 (all collapse to elite) | 35 | 244 | 255 | 83 | 617 |
| **x4_half (½ indep + ½ share)** | **34** | **236** | **252** | 84 | **606** |

## Verdict

1. **Stagnation reheat (`--reheat`) does not help** — neutral-to-slightly-negative
   across both seed sets, and worse when combined with sharing (rh8_x6 > x6). Leave
   it OFF. (The always-on budget-fill reheat already handles freezing.)
2. **Worker sharing helps, but only on high-crossing graphs far from optimum**
   (A5/A6: workers pool into the best basin and dig). On low-k bottleneck graphs
   (A4/A8) full sharing slightly *hurts* — collapsing all workers to one elite kills
   the diversity those bottlenecks need.
3. **Half-sharing is the fix and the winner** (new `--xchg-half`): keep the low-half
   worker ids independent (explore, never adopt elite) and let the high-half adopt
   the shared elite (intensify). It is **no-regret** — captures the A5/A6 gains
   *and* keeps base's edge on A4/A8 (only A8 +1, noise). It also produced the best
   A5 (236) of any config. Best frequency: **R=4 rounds**; R=6/8 fragment too much.

## How to use (production path)

Wired into `run_contest.py::run_combo` via `--xchg-half` (+ `--xchg-rounds`). The
existing results/best/ + bests.json machinery already keeps one best layout per
graph. Recommended contest invocation:

```bash
python3 run_contest.py --methods sa-stress --graphs 1-9 \
  --workers 6 --xchg-rounds 4 --xchg-half \
  --minutes-small M --minutes-medium M --minutes-large M \
  --out-dir results_internal
# -> results_internal/best/<graph>/sa-stress.json  (one best layout per graph)
#    results_internal/bests.json                   (best-k metadata per graph)
```

Caveat: per-graph budgets are still mapped by **graph index** (1-4 small / 5-7,9
medium / 8 large), tuned for the old 10k-node A8. For the professor's new graphs,
size them explicitly with `--minutes-*` (or set all three equal) so the budget
matches each graph's actual difficulty.
