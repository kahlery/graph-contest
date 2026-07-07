# 2026-06-16..18 — Adaptive 1-hour budget orchestrator (`orchestrate.py`)

Branch: `feat/a6-stress-init-methods`. Goal: a scheduler that spends a fixed total
wall-clock budget (~1 hour) across 8–10 graphs to minimise the worst k, with
**convergence-based early termination** ("phase sonlandırma") and **reallocation of
banked time to graphs that keep improving** (A6-like), across different datasets.

## Design provenance
Synthesised by a multi-agent **design workflow** (3 independent designs → judge panel
→ adversarial critique). Scores: economic/quantum 40, round-based 37, live-kill 21.
Winner = **quantum marginal-gain scheduler** (reuse `run_combo`, never kill, early-stop
= stop awarding quanta, reallocate by dk/dt bid, warm-chain continuations). A second
**code-review workflow** (4 dimensions → adversarial verify) confirmed 1 real budget bug.

## Verified facts baked in (empirical, this session)
- **Warm `--init input` is cheap even on A8**: resuming the k=6 / 11040-crossing A8
  layout takes ~3 s total — `computeAllCrossings` cost scales with the layout's
  crossing count, so warm continuations are cheap; only the raw tangled input is
  expensive. ⇒ warm-chaining works for every graph; first lease of a huge graph just
  needs a constructive (non-raw) init.
- **A killed sakgd writes no `-o` layout** (no signal handler) ⇒ never kill a worker
  for scheduling; the per-lease timeout is only a budget backstop (lost quantum keeps
  the previous warm layout).
- sakgd honours `-t` closely in practice (180 s budget → 168–181 s wall observed).

## How it works (`orchestrate.py`)
- **Method per graph from (n,m):** dense/expander (m/n≥8, A6) → `sa` (stress hurts
  dense); sparse/huge (A7/A8/A9) → `sa-stress`; graphviz-missing → `sa`.
- **Quantum leasing** via `run_contest.run_combo` (W-worker multi-start). Qb =
  clamp(B/(4·n_live), 45, 180) s; huge graphs get a longer floor + W/2 workers on the
  cold lease (memory pressure).
- **Explore** one quantum per graph, hardest-first (huge graph pre-pays setup; every
  graph yields a real dk/dt slope). Explore cost bounded to ≤40% of B.
- **Convergence** from the `.trace` best-of-workers envelope (reuses `_read_trace` +
  `_envelope`): drop a graph when slope_k==0 and <0.5% totalX drop over a ≥150 s window,
  ≥2 leases, ≥5 ticks; dense graphs are exempt. Hard-drop on k==0 / temp-floor.
- **Reallocation** = greedy `bid = dk/dt · Q` (+tiny dx tie-break, 1.3× for dense);
  warm-chain via the new `sa-warm` method (single sakgd stage, `--init input`) from the
  graph's verified best layout. Anti-starvation: among bidders within 2× of the top,
  serve the least-serviced. Minimax sink onto the worst-k graph when nobody improves;
  iterative probation of converged graphs if budget remains.
- **Budget safety:** patched `run_contest._run` enforces a per-process timeout clamped
  to a hard deadline (budget − verify reserve) so no lease can run past budget; reserve
  capped to ≤30% of B; never launch with <15 s left. Every graph gets a verified
  submission (falls back to its valid input). Summary JSON + per-graph submission dir.

## Tests
- **`--self-test`** (deterministic, no solver): 22 asserts on classify / slope guards /
  convergence (incl. empty-cum, dense-exempt, setup-only, MIN_LEASES) / bid / quantum
  sizing / anti-starvation — **ALL PASS**.
- **e2e B=220 s, W=2, graphs 1,6,9, q=20, stall=25:** explore A9→A6→A1; A6 (dense) got
  4 leases / 83 s (most budget, never converged); A1/A9 deprioritised after plateau;
  **warm-chain monotone** (A6 1492→746, A1 95→9, A9 2383→11); wall 163 s < 220 s; no
  orphan processes; all 3 submissions verify VALID. Confirmed warm leases use
  `--init input` (no auto-gate line); cold A6 lease correctly picks snake+smooth.
- **review fix re-test B=70 s, W=2, graphs 1,6:** both graphs explored, A1 95→9 VALID
  (was invalid pre-fix — small-budget reserve starved exploration), wall 47 s < 70 s.

## Confirmed review bug (fixed, commit c6075a5)
Per-process kill backstop floored at 60 s and applied per stage, decoupled from
`q=min(q,rem)` ⇒ a ~15 s-budget cold 2-stage lease could hard-run ~120 s. Fix:
deadline-aware clamp on every subprocess timeout. Plus reserve cap + submission
fallback for small-budget robustness.

## Genericisation (2026-06-18) — remove overfit to the Automatic 1-9 set
Audit question: is the scheduler generic or overfit to these 9 graphs? Finding: the
*engine* (budget split, convergence from the live trace, dk/dt reallocation) was
already measurement-driven and graph-agnostic — A6 won the budget because it
*measured* as still-improving, not because the code knows "A6". Only a thin prior
layer was tuned. Removed the two graph-class-specific scheduler rules:
- **`dense` never-converge exemption** in `is_converged` → convergence is now decided
  purely from the measured trace for EVERY graph (still-improving survives on dk>0 /
  totalX-falling; a truly-flat graph drops regardless of label).
- **1.3× dense bid multiplier** in `bid` → reallocation is purely measured dk/dt.
`classify()` thresholds are now SOFT, CLI-overridable defaults (`--dense-density`,
`--big-n`) that bias only the COLD-lease init + a summary label, never the scheduler.
Proof it was redundant: with both knobs removed, a smoke run still gave A6 the most
budget on its measured slope alone (1492→773, 3 leases). self-test updated → ALL PASS.
Commit db527c0.

## Usage
```
python3 orchestrate.py --graphs 1-9 --budget 3600 --workers 8         # the contest
python3 orchestrate.py --graphs final1-10 --budget 3600 --workers 8   # any other dataset
python3 orchestrate.py --graphs 1-9 --budget 3600 --workers 8 --dense-density 6 --big-n 5000
python3 orchestrate.py --graphs '1,6,9' --budget 220 --workers 2 --quantum 20 --stall-floor 25
python3 orchestrate.py --self-test
```
