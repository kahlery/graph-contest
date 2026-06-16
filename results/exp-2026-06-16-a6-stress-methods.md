# 2026-06-16 — Automatic-6 stress-base method search (REJECTED, reverted)

Branch: `feat/a6-stress-init-methods`. Goal: build new methods on the
**sa-stress base** to beat A6 (our one losing graph; record k=624 held by plain
`sa` @8w×15min; competitor 621, GD'25 winner 568 @49min).

## Candidates (all default-off, independent flags)
- **sa-stress-spread** — sfdp `-Goverlap=prism -Gsep=+12 -GK=2.0`, forced `--init input`.
- **sa-stress-exact** — init picked by EXACT crossing count over sfdp/neato/fdp
  (`--exact 1 --engine all`), forced `--init input`.
- **sa-stress-uncross** — SA proposal (placeMode==3) pulls v toward the far
  endpoint of its longest incident edge (`--place uncross`).

## Init-layer evidence (A6, exact total crossings, lower = better)
| init layout                              | totalX  | vs smooth |
|------------------------------------------|---------|-----------|
| snake+barycenter-smooth (auto default)   | 604,208 | —         |
| fdp + exact selection                    | 677,255 | +12%      |
| sfdp-spread                              | 728,584 | +21%      |

The snake+smooth construction dominates every force-directed/stress layout on
this dense graph. Spreading *lengthens* edges and *raises* crossings.

## Screening A/B (same batch, 3 min × 4 workers, seed 42, A6 — best of workers)
| method             | k    | totalX  | vs plain sa |
|--------------------|------|---------|-------------|
| **sa** (control)   | **705** | 526,733 | —        |
| sa-stress (base)   | 737  | 558,967 | +4.5%       |
| sa-stress-exact    | 731  | 561,923 | +3.7% (−0.8% vs base = noise) |
| sa-stress-spread   | 784  | 590,351 | +11.2%      |
| sa-stress-uncross  | 1059 | 673,862 | +50.2%      |

## Mechanism / why they lose
- **Init gate finding:** on A6 the solver's `--init auto` 0.8× gate *keeps the
  inferior stress layout* (≈476 sampled) instead of switching to smooth (≈390),
  because 390 is not < 0.8×476. So `sa-stress` is handicapped vs plain `sa`
  (which gets smooth) — and burns ~8% of budget producing that worse layout.
- **spread:** over-spreads, lengthens edges → +21% init crossings → worst init method.
- **exact:** fdp init (677k) is better than the sfdp layout the base keeps, but
  still worse than smooth (604k); only ±noise vs base, loses to plain `sa`.
- **uncross:** pulling vertices toward far endpoints collapses the dense layout
  → catastrophic (+50%). Worst-edge-pull is wrong for high-density graphs.

## Verdict
**ALL THREE REJECTED.** None beats plain `sa` (705); none meaningfully beats the
stress base. No candidate qualified for the 30-min confirmation. Implementation
commit reverted (kept in history). Re-confirms the standing thesis: **A6's gain
is init quality + budget, and snake+smooth (plain `sa`) is already the best init;
stress base is structurally handicapped on dense A6.** The only real path to 568
is plain `sa` at a larger budget (out of this round's stress-base scope).
