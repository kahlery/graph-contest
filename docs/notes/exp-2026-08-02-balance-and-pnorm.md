# 2026-08-02 — the k-gap is a crossing-balance gap

Starting point: our leaderboard entries were behind the leader (Daniel Kohrt) on
9 of 18 instances. This note records what closed most of that and what did not.

## The framing that mattered

Every crossing charges exactly two edges, so `sum_e c_e = 2X` and therefore

    k >= ceil(2X/m)                    "balance floor"
    k ~= ratio * (2X/m),  ratio >= 1

`bin/xstat` reports k, X, the floor and the top-level occupancy, so a layout can be
judged by its *excess over the floor* rather than by k alone.

The diagnosis that started everything: our submitted `instance_08` layout had
X=43762 and a floor of exactly **49** — and the leader's k *was* 49. He was not
finding drawings with fewer crossings; he was redistributing the same crossings
almost perfectly, while we sat at k=85 with >50% of edges above the floor.

The two factors fight each other. Lowering the bottleneck costs crossings (the
optimiser spreads the drawing out), which raises the floor. Every config must be
compared on **(X, ratio)**, never on k.

## What worked

**`--fit pnorm` (new).** Phase 2 used `dLocalK`, which only sees the true maximum, so
nearly every move scores exactly 0 — a plateau. `sum (c_e/k)^p` weights an edge by
`(c_e/k)^(p-1)`, giving the whole upper tail a gradient. On instance_08 against the
team's tuned baseline (`--cands 4 --cands-ramp 1 --edge-move 5 --cands-mix 1
--bandit 1`), 20 min: `--fit k` 71 → `--fit pnorm --pnorm 32` 65.

**`--pnorm 0` — exponent from the live k (new).** The marginal weight one level below
the bottleneck is `(1-1/k)^(p-1)`; holding it near 0.6 gives `p ~ 0.5k`. A fixed p
cannot serve k=10 and k=520 at once: p=32 leaves that weight at 0.038 on
Automatic-9 (the plateau again) and 0.94 on Automatic-6 (too shallow to see the
tail). `--pnorm` was also clamped at 40, so the large values were unreachable.

**`gradx_init` — the single biggest lever on large canvases.** Gradient descent on the
SigmoidX crossing surrogate reaches a far lower X than any SA phase 1, and a lower X
lowers the ceiling on everything the balancer can then do. instance_05: X 108919 →
88744 (floor 110 → 90), k 240 → 190 → 185.

**Stress (sfdp) init on tight canvases.** `--init auto` has no force-directed member.
A cold run with it lands Automatic-9 at k=22 against the 10 we had submitted from
`sa-stress`. Always seed the tight-canvas instances with `INIT=stress tools/seed.sh`.

**Overlap repair instead of aborting.** A vertex-edge overlap in the final layout used
to abort the run (exit 3, nothing written), discarding an Automatic-7 layout at k=30
while the champion stood at 47. `src/main.cpp` now repairs and recomputes k.

## What did not work (do not retry)

**`--fit tgt`.** With a target at the floor, linear total excess `sum max(0, c-K)` is
algebraically `2X - mK` — just X-minimisation. Quadratic instead over-focuses on a
few extreme edges. Either way the mid-tail carries no signal. k=111 vs pnorm's 64.

**Ramping p (`--pnorm0`).** 4→16 gave 74 against 64 for fixed p: once p rises, X
inflates back and undoes the cheap low-X start.

**Holding X down with `--xcap`.** From instance_08's X=19434 phase-1 snapshot:
w=0.02 (off) → X=25354 k=**62**; w=0.5 → k=77; w=1.0 → k=102; w=2.0 → k=91. Lowering
the bottleneck genuinely costs crossings, so a real constraint strangles the
balancer. Confirmed again on Automatic-9: from X=2029 (champion X=3981) it finished
at X~3800 k=12 against the champion's k=10. The low-X snapshot is a *starting basin*,
not a budget to defend. (The penalty prices the move delta, so the weight is a
constant slope per crossing, not a barrier at the cap.)

## instance_08 is capped at 61 by the algorithm

Nine independent routes converge to 61-62: warm chains, the low-X phase-1 snapshot,
gradx, forced-low X, the adaptive exponent, and four different search dynamics —
LAHC acceptance, VM-grid + level-clear, k-repair + polish, best-of-8 + smart
placement — which returned **exactly 61 each**. Four independent cold multi-starts
then landed at 69/77/81/83, so 61 is the product of accumulated warm refinement and
fresh runs cannot match it. The leader's 49 needs ~20% better balance than this
algorithm family produces; that is a different algorithm, not a tuning gap.

## Results

| instance | submitted | now | board best | |
|---|---|---|---|---|
| instance_06 | 234 | **177** | 183 | win +6 |
| instance_05 | 223 | **185** | 186 | win +1 |
| Automatic-5 | 75 | **72** | 73 | win +1 |
| Automatic-6 | 737 | **504** | 504 | tie |
| instance_03 | 11 | **9** | 9 | tie |
| instance_09 | 3 | 3 | 3 | tie |
| Automatic-3 | 37 | 31 | 30 | -1 |
| Automatic-7 | 28 | 28 | 26 | -2 |
| Automatic-9 | 10 | 10 | 7 | -3 |
| instance_08 | 79 | 61 | 49 | -12 |

`instance_01` (planar, k=0) and `instance_07` (non-planar so k>=1, leader has 1) are
at proven optimum — checked with `networkx.check_planarity`; only instance_01 is
planar. No work is available on either.

Layouts: `data/output/submission_2026-08/` (gitignored per repo convention).
Validate anything before submitting: `tools/validate.py --set <set> <dir>` — an
invalid drawing scores as INVALID, not merely badly.

## Harness

`tools/` — `xstat` (k/X/floor), `score.py` (grade against the live leaderboard),
`validate.py` (pre-submission gate), `seed.sh` (gradx/stress cold seeding),
`push.sh`/`campaign.sh` (never-regressing warm rounds), `collect.sh` (assemble a
validated submission set), `ils.sh` (kick a stalled champion with a hotter restart).
