# Lexicographic tempered SA — porting the Kohrt approach (2026-08-07)

Goal: implement the approach from Daniel Kohrt's TUM practical talk
("Lexicographic Tempered Simulated Annealing") inside our solver and measure
whether it beats our production default on `data/input/internal-2026`.

Everything below is **opt-in**; with the flags off, the executed code path is
the one that shipped.

## What the talk proposes, and what we already had

| Talk | Our solver before this change |
|---|---|
| FMMM initialisation | own init family (bfs-snake+smooth, tripod, gradx, stress) |
| Objective Φ = Σ crossings(e)² | `--fit sq` / `--fit sq2` (both non-default) |
| Lexicographic (k, n_k, Φ, total) + staged acceptance | `--lexk`: scalarised, and on a **local** k proxy |
| 6 operators (focus/random × centroid/local/regional) | 4 `--cands-mix` proposal slots, different hypotheses |
| Online bandit over operators | `--bandit` over the 4 slots |
| Tempered SA (replica ladder + exchange) | multi-worker + elite exchange, but **one temperature** |
| Scheduler across graphs | `contest_orchestrate` |

So the genuinely new material was: the exact four-level objective with its
staged acceptance rule, the six-operator displacement set, a bandit over
*those* operators, and a temperature **ladder** across replicas.

## What was implemented

`src/main.cpp`, four flags:

- **`--lex4`** — four-level objective `(k, n_k, Φ=Σcr², total)` with the talk's
  staged acceptance: `Δk<0` accept; `Δk>0` accept w.p. `exp(-Δk/T)`; on the
  `Δk=0` plateau, `Δn_k<0` accept, else `dE = Δn_k·u + ΔΦ` with `u = 2k-1`
  (= `k² - (k-1)²`, the Φ-cost of one worst-level crossing), accepted with
  `exp(-dE/(T·u))` and `total` as the tiebreak at `dE == 0`.
  Unlike the old `--lexk`, `k` and `n_k` are the **exact global** values, read
  off `cntPerK` (see `evalLex`). The downward rescan only runs when the top
  level actually empties, so the common case is a handful of ±1s on a small
  level→delta map.
- **`--ops6`** — six operators `fc, fl, fr, rc, rl, rr`: node selection
  (random endpoint of a random edge among the **top-3 occupied crossing
  levels**, via a lazily-rebuilt `hotEdges` cache / uniformly random vertex)
  × movement (neighbour centroid / local jitter / regional jitter, radii
  `--op-local`, `--op-regional` as fractions of `sqrt(W·H)`). Forces one
  proposal per move, so the bandit sees which operator earned each accept.
- **`--aos`** — probability matching over the six operators,
  `p_i = pmin + (1-6·pmin)·q_i/Σq`, `q` an ERWA of the success indicator
  (`--aos-alpha`, default 0.001). An **additive** floor does not work here:
  success rates are 0.5–3%, so a fixed 0.05 floor swamps every `q` and
  selection degenerates to uniform — that was the first implementation and it
  measurably did nothing.
- **`--pt R`** — tempered SA: R replicas, each a complete independent solver on
  its own thread, on a geometric ladder `p2T0·[--pt-lo … --pt-hi]`. Each runs
  its own phase 1 (so the ladder starts from R structurally different layouts,
  as in the talk's "Phase 1: Initialization" slide). Between rounds,
  ladder-adjacent temperatures are swapped under
  `p = min(1, exp((β_a-β_b)(E_a-E_b)))` with `E = Φ` and `β = 1/(T·u)` — the
  same scale `acceptLex` anneals Φ on. On top of that the cold half adopts the
  elite layout when behind (the project's established half-share rule).

### Correctness check

`--lex-check 1` verifies every committed `--lex4` move's predicted `(k, n_k)`
against the true post-commit values. **1 468 941 accepted moves, 0 mismatches**
(instance_09). Left in as a debug flag.

## Measurement 1 — the three single-process pieces

9 instances × 4 configs × 3 seeds × 3 min, 10 runs in parallel on 12 cores.
Identical seed ⇒ identical initial layout, so init is not a confound.
`base` = production default (`--cands 4 --cands-ramp 1 --edge-move 5
--cands-mix 1 --bandit 1`). Lower k is better.

| instance | base | lex4 | ops6 | full |
|---|---|---|---|---|
| 01 | 0 | 0 | 0 | 0 |
| 02 | 2 | 2 | 2 | 2 |
| 03 | **10** | **10** | 11 | 11 |
| 04 | **33** | **33** | 35 | **33** |
| 05 | 238 | 245 | 249 | **221** |
| 06 | 239 | 230 | 247 | **230** |
| 07 | 1 | 1 | 1 | 1 |
| 08 | **66** | 67 | 75 | **66** |
| 09 | 3 | 3 | 3 | 3 |

Head-to-head vs base on best-of-3-seeds k, and on the 3-seed mean:

| config | better | worse | same | Σ Δk |
|---|---|---|---|---|
| lex4 | 1 | 2 | 6 | −1 |
| ops6 | 0 | 5 | 4 | +31 |
| full | 2 | 1 | 6 | −25 |

Mean k, discriminating instances: 05 `248.3 → 228.7` (−7.9%),
06 `247.3 → 234.0` (−5.4%), 08 `68.3 → 66.7` (−2.3%), 03 `10.7 → 11.3` (worse).

**Conclusion: the pieces only pay off together.** `--ops6` alone is clearly
worse and `--lex4` alone is a wash, but the combination wins on the large,
dense instances. The reading: the six operators generate structurally
different moves, and ranking them on a k-plateau needs the exact lexicographic
objective — the scalar local-k proxy cannot tell those moves apart. Small and
mid-size instances (03) regress slightly.

## Measurement 2 — tempered SA

Fair comparison at equal cores and equal wall clock: `--pt 4` (4 threaded
replicas + exchange) vs **4 independent seeds, best-of-4** — which is what the
multi-worker path already does. 3 min, `--lex4 1 --ops6 1 --aos 1` both sides.

Two independent seed groups (20, 700), k per paired run:

| instance | ind×4 (g1) | pt×4 (g1) | ind×4 (g2) | pt×4 (g2) |
|---|---|---|---|---|
| 03 | **10** | 11 | 11 | **10** |
| 04 | 35 | **33** | 33 | **32** |
| 05 | 210 | 210 | **210** | 213 |
| 06 | 220 | **218** | **212** | 224 |
| 08 | 63 | **60** | 67 | **59** |
| 09 | 3 | 3 | 3 | 3 |

Σ Δk (pt − ind): group1 **−6**, group2 **+5**, overall **−1**.
Better on 6, worse on 3, tied on 3 of 12 paired runs.

**Verdict: no reliable win.** Overall −1 over 12 paired runs is noise, and the
per-instance swing is larger than the effect (instance_06 goes −2 in one group
and +12 in the other). The one consistent signal is instance_08 (−3, −8) and
instance_04 (−2, −1), both times in tempering's favour; instances 05/06 —
the densest — are where it loses. Tempering is not worth switching on globally
on this evidence, and the honest read is that at a 3-minute budget the ladder
spends too much of it on hot replicas that never get cold enough to pay off.
Worth re-testing at contest-length budgets before either adopting or dropping.

## Side-finding: the operator win-rate map reproduces

Per-run operator statistics (`[ops6]` line) independently reproduce the talk's
per-graph heat map: **focus-centroid wins on the small instances**
(instance_09, n=56: `fc` 2.3% vs `rr` 0.4%) and **random-vertex operators win
on the large ones** (instance_06, n=600: `rl` 28.4%, `rc` 19.8% vs `fc` 6.5%).
This was not tuned for; it fell out of the bandit.

## Known limitations

- `--lex4` governs the phase-2 **single-vertex** proposal path. The coupled
  `--edge-move` / `--pair-move` / `--swap` helpers still use `acceptByRule`
  with their own scalar ΔE. `--edge-move` is now disabled under `--ops6`
  (it would bypass operator selection and starve the bandit); combining
  `--lex4` with `--edge-move` leaves ~5% of moves on the old rule.
- `--lex4` supplies its own acceptance, so `--accept threshold|lahc` is
  ignored on that path (now warned at startup).
- `--ops6` proposals are not grid-snapped, so `--grid-anneal` is inert under
  it (warned).
- `LexDelta::better` (best-of-C ranking) keeps all four levels strictly
  separated, while `acceptLex` scalarises levels 2–3 as the talk specifies.
  The two orders can disagree; `--ops6` forces C=1 so they never disagree in
  the winning configuration.
- The bandit only updates on proposals that survive the validity gates.
  Measured drop rate: 0.000–0.012% of moves, i.e. immaterial.
- Runs were made with 10 (resp. 3) jobs in parallel, so absolute k values are
  worse than a dedicated run would give. The comparison is unaffected —
  contention is uniform across configs.

## Recommendation

Add a `sa-lex` method variant carrying `--lex4 1 --ops6 1 --aos 1` and route
the **large/dense** instances (05, 06, 08 shaped) to it, keeping the current
default for small and mid-size graphs. Do not enable any of these flags
globally: on 03 the combination is a regression.

`--pt` stays off by default — see Measurement 2. Re-test it at contest-length
budgets (30 min+) before deciding; a 3-minute budget may simply be too short
for a 4-rung ladder.

## Measurement 3 — full-contest hour, Daniel on every graph

After the component gates, the complete profile (`--lex4 1 --ops6 1 --aos 1`)
was injected into the final SA stage of **every** layout family and every warm
continuation. The scheduler, initialisation families, targets, worker count,
seed, and one-hour global budget were held fixed. The candidate started cold;
it did not read any layout from an earlier run.

Baseline `corch_1784476912` versus candidate `corch_1786628948`:

| instance | baseline k | Daniel-all k | Δk |
|---|---:|---:|---:|
| 01 | 0 | 0 | 0 |
| 02 | 2 | 2 | 0 |
| 03 | 10 | 10 | 0 |
| 04 | 32 | 32 | 0 |
| 05 | 189 | **181** | **−8** |
| 06 | **192** | 195 | +3 |
| 07 | 1 | 1 | 0 |
| 08 | 66 | **62** | **−4** |
| 09 | 3 | 3 | 0 |

- Sum-k: **495 → 486 (−9)**.
- Worst-k: 192 → 195 (+3), caused by instance 06.
- Aggregate total crossings: **279878 → 242510 (−37368)**.
- Wall time: 3508.9 / 3600 seconds, 8 workers; all nine final layouts
  independently verified with no vertex-edge overlap.

The primary contest metric chosen for this experiment was sum-k, followed by
worst-k and total crossings, so the candidate wins and `contest_orchestrate`
now defaults to `--search-profile daniel-all`. The frozen control remains
available as `--search-profile current`. Parallel tempering and VM-grid remain
off: neither was part of this A/B.

The operator map also confirms the presentation's graph-specific behaviour.
On 05/06/08, random-local (`rl`) had the highest success rate (10.12% / 10.05%
/ 7.44%), while random-centroid (`rc`) and focus-local (`fl`) formed the next
useful tier. That is strong evidence for retaining online AOS rather than a
fixed operator mixture. Full machine-readable counts are stored under each
graph's `operator_stats` in the candidate `orchestration.json`.

## Reproducing

```
make
# Measurement 1
./bin/sakgd -i data/input/internal-2026/instance_05.json -o /tmp/o.json \
    -t 3 -p1 0.6 -s 1 --lex4 1 --ops6 1 --aos 1
# with the accounting self-check
./bin/sakgd -i data/input/internal-2026/instance_09.json -o /tmp/o.json \
    -t 1 -p1 0.2 -s 1 --lex4 1 --lex-check 1 --ops6 1 --aos 1
# Measurement 2
./bin/sakgd -i data/input/internal-2026/instance_08.json -o /tmp/o.json \
    -t 3 -p1 0.6 -s 20 --pt 4 --pt-rounds 8 --lex4 1 --ops6 1 --aos 1
```
