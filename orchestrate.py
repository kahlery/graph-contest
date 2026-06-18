#!/usr/bin/env python3
"""
orchestrate.py — adaptive wall-clock budget scheduler for the GD-2025 k-planarity contest.

Spends a fixed total wall-clock budget (e.g. 1 hour) across 8-10 graphs to minimise
the overall / worst k, by:
  - per-graph METHOD SELECTION from (n, m): dense/expander -> plain SA (stress hurts
    dense), sparse / huge -> stress init (force layout wins there);
  - QUANTUM scheduling: work is leased in fixed wall-clock slices (quanta) via the
    existing run_contest.run_combo W-worker multi-start primitive;
  - CONVERGENCE-BASED EARLY TERMINATION ("phase sonlandirma"): a graph whose
    best-k trace has plateaued is dropped, banking its remaining budget;
  - MARGINAL-GAIN REALLOCATION: every freed quantum goes to the graph with the
    steepest recent dk/dt (k-reduction rate), warm-started from its best-so-far
    layout, so always-improving graphs like Automatic-6 soak up the banked time;
  - graceful degradation: per-lease timeout BACKSTOP (budget safety), iterative
    PROBATION of converged graphs if budget remains, ANTI-STARVATION among tied
    improvers, and a MINIMAX SINK onto the worst-k graph when nothing else improves.

Reuses run_contest.py as the single source of truth for methods, datasets, trace
parsing and best-saving, so it tracks the pipeline automatically.

Design provenance: synthesised from a 3-design judge panel + adversarial critique
(see results/exp-2026-06-16-orchestrator.md). Verified facts baked in:
  * warm `--init input` on a GOOD layout is cheap even for A8 (k=6 layout: ~3 s
    total), because computeAllCrossings cost scales with the layout's crossing
    count -- so warm-chaining works for every graph; only the raw tangled input is
    expensive, hence the first lease of a huge graph uses a constructive init.
  * a killed sakgd writes no -o layout, so we NEVER kill a worker for scheduling;
    early termination = "stop awarding quanta". The per-lease timeout fires only as
    a budget backstop (the lost quantum keeps the previous warm layout).

Usage:
  python3 orchestrate.py --graphs 1-9 --budget 3600 --workers 8
  python3 orchestrate.py --graphs '1,6,9' --budget 160 --workers 2 --quantum 20 --stall-floor 25
  python3 orchestrate.py --self-test          # fast, deterministic, no solver
"""
import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

import run_contest as rc
from run_contest import (
    parse_graph_spec, graph_path, graph_name, graph_size,
    run_combo, verify_output, update_best, load_json, save_json,
    METHOD_BY_ID, _read_trace, _envelope, _stage_files, ensure_binaries, ROOT,
)

# --------------------------------------------------------------------------- #
# Tunable constants (CLI can override the budget-shape ones).
# --------------------------------------------------------------------------- #
QMIN, QMAX   = 45.0, 180.0   # quantum bounds (seconds)
BIG_FLOOR    = 180.0         # minimum quantum for a huge graph (amortise setup)
MIN_LEASES   = 2             # never declare convergence on a single noisy lease
STALL_FLOOR  = 150.0         # min trailing window (s) for the plateau test
BID_WIN      = 90.0          # trailing window (s) used to estimate dk/dt for bidding
DENSE_MULT   = 1.3           # bid multiplier for dense/never-converge graphs
REL_X_EPS    = 0.005         # <0.5% total-crossings drop over the window == flat
BID_EPS      = 1e-4          # below this k/s, treat as no real improvement
STARVE_FACTOR = 2.0          # bidders within this factor of the top bid share fairly
OVERRUN      = 1.6           # per-process timeout backstop = OVERRUN * requested
LEASE_FLOOR  = 15.0          # do not launch a lease with less than this much budget

# --------------------------------------------------------------------------- #
# Budget backstop: patch run_contest._run to enforce a per-process wall ceiling.
# sakgd treats -t as a SOFT target (checked between moves); on a pathological move
# it can overrun. A killed process leaves no layout, so this only ever costs the
# current quantum (the previous warm layout is retained) -- a safety net, not the
# normal path. _LEASE_TIMEOUT is set before each lease.
# --------------------------------------------------------------------------- #
_LEASE_TIMEOUT = [None]      # per-lease soft backstop (seconds), set before each lease
_HARD_DEADLINE = [None]      # absolute monotonic time no solver subprocess may run past
_orig_run = rc._run


def _run_with_backstop(cmd, log_path):
    t0 = time.time()
    to = _LEASE_TIMEOUT[0]
    # Clamp to the hard deadline so NO subprocess (any stage) runs past budget, even
    # a cold multi-stage lease launched in the tail: the 60s floor below is decoupled
    # from q=min(q,rem), so without this a ~15s-budget lease could hard-run 60-120s.
    if _HARD_DEADLINE[0] is not None:
        left = _HARD_DEADLINE[0] - time.monotonic()
        to = max(1.0, min(to if to else left, left))
    with open(log_path, "w") as log:
        try:
            proc = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=to)
            code = proc.returncode
        except subprocess.TimeoutExpired:
            code = -9
            log.write(f"\n[orchestrate] KILLED by budget backstop after "
                      f"{time.time() - t0:.0f}s (limit {to:.0f}s)\n")
    return code, time.time() - t0


rc._run = _run_with_backstop


def _clamp(x, lo, hi):
    return max(lo, min(hi, x))


# --------------------------------------------------------------------------- #
# Pure helpers (unit-tested in --self-test; no solver, no machine-load noise).
# --------------------------------------------------------------------------- #
def classify(n, m, no_graphviz=False):
    """Pick a starting method + flags from (n, m). Findings: dense (m/n>=8, A6)
    -> plain SA (stress hurts dense); huge (n>=8000, A8) and sparse (m/n<2.5,
    A7/A9) -> stress init. Trivial (m==0) -> nothing to solve."""
    if m == 0:
        return "sa", {"trivial": True}
    dens = m / max(1, n)
    big = n >= 8000
    if dens >= 8.0:
        return "sa", {"dense": True, "big": big}          # never-converge, exempt
    method = "sa-stress"
    flags = {"big": True} if big else {}
    if no_graphviz:
        method = "sa"          # stress init needs graphviz; fall back to snake init
    return method, flags


def slope(cum, win):
    """(dk_per_s, dx_per_s, x_window_start) over the trailing `win` seconds of the
    cumulative best-of-workers envelope `cum` ([[t,k,x],...]). Best-so-far is
    monotone non-increasing, so slopes are clamped >= 0. Robust to empty / 1-point."""
    if not cum:
        return 0.0, 0.0, 1
    if len(cum) < 2:
        return 0.0, 0.0, max(1, cum[-1][2])
    t_end = cum[-1][0]
    a = cum[0]
    for p in cum:
        if p[0] >= t_end - win:
            a = p
            break
    b = cum[-1]
    dt = max(1.0, b[0] - a[0])
    return (max(0.0, (a[1] - b[1]) / dt),
            max(0.0, (a[2] - b[2]) / dt),
            max(1, a[2]))


def is_converged(g, q, stall_floor):
    """True iff graph g has plateaued (drop it, bank its budget). Dense graphs are
    exempt (empirically never converge). Guards against empty / setup-only traces."""
    if g.get("dense"):
        return False
    if g["leases"] < MIN_LEASES:
        return False
    cum = g["cum"]
    if not cum:
        return False                       # no signal yet -> keep alive
    win = max(2 * q, stall_floor)
    dk, dx, x0 = slope(cum, win)
    t_end = cum[-1][0]
    ticks = sum(1 for p in cum if p[0] >= t_end - win)
    if ticks < 5:
        return False                       # setup-dominated / sparse logger
    rel_x = dx * win / max(1, x0)
    return dk == 0.0 and rel_x < REL_X_EPS


def bid(g, q):
    """Expected next-quantum k-reduction = recent dk/dt * q, with a tiny dx tie-break
    so a 'k-flat but totalX-still-falling' graph (A6) keeps bidding. Dense gets a
    persistence multiplier (its descent extrapolates past the window)."""
    dk, dx, _ = slope(g["cum"], min(q, BID_WIN))
    s = dk * q + 1e-6 * dx * q
    return s * (DENSE_MULT if g.get("dense") else 1.0)


def quantum_base(budget, n_live, override=None):
    if override:
        return float(override)
    return _clamp(budget / (4 * max(1, n_live)), QMIN, QMAX)


def quantum_for(g, qb):
    return max(2 * qb, BIG_FLOOR) if g.get("big") else qb


# --------------------------------------------------------------------------- #
# Orchestrator
# --------------------------------------------------------------------------- #
class Orchestrator:
    def __init__(self, graphs_spec, budget, workers, out_dir, seed=1,
                 quantum_override=None, stall_floor=STALL_FLOOR, p1_cold=0.2,
                 p1_warm=0.05, verbose=True):
        self.budget = float(budget)
        self.W = int(workers)
        self.seed = int(seed)
        self.q_override = quantum_override
        self.stall_floor = float(stall_floor)
        self.p1_cold = p1_cold
        self.p1_warm = p1_warm
        self.verbose = verbose
        self.out_root = Path(out_dir)
        self.run_id = f"orch_{int(time.time())}"
        self.run_dir = self.out_root / "runs" / self.run_id
        self.run_dir.mkdir(parents=True, exist_ok=True)
        self.bests = load_json(self.out_root / "bests.json", {})
        self.no_graphviz = shutil.which("sfdp") is None and shutil.which("neato") is None
        self.G = self._build_graphs(graphs_spec)
        self.t0 = None
        self.deadline = None
        self.max_overrun = 0.0           # learned slack for the reserve
        self.skipped_launches = 0        # budget-gate skips (tested)
        self.lease_log = []              # (name, method, warm?, nw, q_req, wall) per lease

    def log(self, msg):
        if self.verbose:
            print(f"[orch {self.now():6.1f}s] {msg}", flush=True)

    def now(self):
        return time.monotonic() - self.t0 if self.t0 else 0.0

    def _build_graphs(self, spec):
        G = {}
        for tok in parse_graph_spec(spec):
            try:
                p = graph_path(tok)
                n, m = graph_size(p)
            except Exception as e:                    # unreadable/huge JSON: skip
                print(f"[orch] WARN skipping {tok}: {e}", file=sys.stderr)
                continue
            method, flags = classify(n, m, self.no_graphviz)
            nm = graph_name(tok, p)
            # Baseline-k probe. SKIP it for large graphs: verify_output runs
            # `sakgd --verify` (forced --init input) which does computeAllCrossings on
            # the RAW input -- on a tangled huge graph (A8: ~45M crossings) that is
            # MINUTES and would hang startup before the budget clock even starts. The
            # first lease establishes the real k instead.
            if not m:
                v = {"k": 0, "valid": True}
            elif n >= 5000:
                v = None
            else:
                v = verify_output(p)
            done = bool(flags.get("trivial")) or bool(v and v.get("k") == 0)
            G[nm] = dict(tok=tok, path=str(p), n=n, m=m, method0=method,
                         dense=flags.get("dense", False), big=flags.get("big", False),
                         trivial=flags.get("trivial", False),
                         cum=[], spent=0.0, leases=0, post_leases=0,
                         best_k=(v or {}).get("k"), best_x=None,
                         warm=None, done=done, probation_failed=False, fallback=False,
                         spent_before_q=0.0)
        return G

    # --- budget accounting -------------------------------------------------- #
    def _verify_reserve(self):
        """Wall held back for the end-of-run verify_output of every banked best
        (each spawns sakgd --verify; cost grows with n)."""
        return sum(8.0 + g["n"] / 2000.0 for g in self.G.values())

    def reserve(self):
        """Time held back for end-of-run verifies + observed solver overrun, CAPPED to
        a fraction of the budget so a small budget still leaves room to explore every
        graph (binds only when the budget is tiny; ~5% and irrelevant at 3600s)."""
        full = max(30.0, 0.03 * self.budget) + self._verify_reserve()
        return min(full, 0.30 * self.budget) + self.max_overrun

    def remaining(self):
        return self.deadline - self.now() - self.reserve()

    # --- one lease ---------------------------------------------------------- #
    def lease(self, nm, g, probation=False):
        rem = self.remaining()
        if rem < LEASE_FLOOR:
            self.skipped_launches += 1
            return False
        qb = quantum_base(self.budget, self._n_live(), self.q_override)
        q = quantum_for(g, qb)
        cold = g["warm"] is None
        # First lease of a huge graph: fewer workers (memory pressure), longer slice.
        nw = max(1, self.W // 2) if (g["big"] and cold) else self.W
        q = min(q, rem)
        method = g["method0"] if cold else "sa-warm"
        warm = None if cold else g["warm"]
        p1 = self.p1_cold if cold else self.p1_warm
        qdir = self.run_dir / nm / f"q{g['leases']}"
        qdir.mkdir(parents=True, exist_ok=True)
        g["spent_before_q"] = g["spent"]
        _LEASE_TIMEOUT[0] = max(60.0, q * OVERRUN)
        self.log(f"lease {nm:<14} method={method:<9} warm={'Y' if warm else 'N'} "
                 f"nw={nw} q={q:.0f}s  (k so far={g['best_k']}, leases={g['leases']})")
        try:
            _, best_w, wall = run_combo(
                method, g["path"], q / 60.0, p1, self.seed + g["leases"] * 100,
                qdir, nw, 12, 64, 0.5, 0, gpath_warm=warm, xchg_rounds=1)
        except Exception as e:
            self.log(f"  run_combo ERROR on {nm}: {e}")
            best_w, wall = None, 0.0
        g["spent"] += wall
        g["leases"] += 1
        if not cold:
            g["post_leases"] += 1
        self.max_overrun = max(self.max_overrun, wall - q)
        self._ingest_trace(g, qdir, method, nw)
        self.lease_log.append((nm, method, warm is not None, nw, round(q, 1), round(wall, 1)))

        if best_w and best_w.get("valid"):
            nk, nx = best_w["final_k"], best_w["final_totalX"]
            improved = (g["best_k"] is None or nk < g["best_k"]
                        or (nk == g["best_k"] and (nx or 0) < (g["best_x"] or float("inf"))))
            if improved and self._verify_layout(best_w["out_path"], nk):
                g["best_k"], g["best_x"], g["warm"] = nk, nx, best_w["out_path"]
                update_best(self.out_root, self.bests, nm, method, nk, nx,
                            best_w["out_path"], self.run_id, n_workers=nw,
                            wall_clock_sec=round(wall, 1))
            # Self-terminated early (optimal, or temp floor) -> converged. Exclude a
            # deliberately budget-clamped short final quantum from temp-floor logic.
            clamped = q < quantum_for(g, qb) - 1.0
            if nk == 0 or (not clamped and wall < 0.4 * q):
                g["done"] = True
        else:
            if cold and not g["fallback"]:
                g["method0"], g["fallback"] = "sa", True     # graphviz/stress failed
                self.log(f"  {nm}: invalid/failed cold lease -> fall back to plain 'sa'")
            elif g["leases"] >= MIN_LEASES:
                g["done"] = True
        if probation and not (best_w and best_w.get("valid")):
            g["probation_failed"] = True
        return True

    def _verify_layout(self, path, expected_k):
        """Guard the warm chain: only advance on a re-verified valid layout whose k
        matches the log (catches a 'finished but geometrically degenerate' layout)."""
        v = verify_output(path)
        return bool(v and v.get("valid") and v.get("k") == expected_k)

    def _ingest_trace(self, g, qdir, method, nw):
        """Append this quantum's best-of-workers envelope to g['cum'], offset onto
        g's own cumulative time axis (chained at the seconds already spent)."""
        spec = METHOD_BY_ID[method]
        last = len(spec["stages"]) - 1
        sers = []
        for w in range(nw):
            tr = _stage_files(spec, last, qdir, f"_w{w}")[1].with_suffix(".trace")
            pts, _ = _read_trace(tr, base=g["spent_before_q"])
            if pts:
                sers.append(pts)
        env = _envelope(sers)
        if env:
            g["cum"].extend(env)

    # --- scheduler ---------------------------------------------------------- #
    def _n_live(self):
        return max(1, sum(1 for g in self.G.values() if not g["done"]))

    def run(self):
        self.t0 = time.monotonic()
        self.deadline = self.budget          # now()/remaining() work in seconds-since-start
        # Absolute ceiling for solver subprocesses: leave the verify reserve so the
        # end-of-run verifies still fit inside the budget. Backstop clamps to this.
        _HARD_DEADLINE[0] = self.t0 + self.budget - min(self._verify_reserve(),
                                                        0.20 * self.budget)
        try:
            self._explore()
            self._greedy()
        except Exception as e:                       # never lose banked results
            print(f"[orch] scheduler aborted: {e}", file=sys.stderr)
        return self._finalize()

    def _explore(self):
        """One quantum to every non-trivial graph, hardest-first so a huge graph
        pre-pays its setup early and every graph yields a real slope before bidding.
        Explore cost is bounded to <=40% of budget by shrinking the per-graph slice."""
        todo = [(nm, g) for nm, g in self.G.items() if not g["done"]]
        todo.sort(key=lambda kv: -(kv[1]["m"] + kv[1]["n"]))
        if not todo:
            return
        qb = quantum_base(self.budget, len(todo), self.q_override)
        want = sum(quantum_for(g, qb) for _, g in todo)
        cap = 0.4 * self.budget
        if want > cap and not self.q_override:
            # shrink the explore quantum so exploration leaves room for reallocation
            self.q_override = max(QMIN / 2, cap / len(todo))
            self.log(f"explore would cost {want:.0f}s (>{cap:.0f}); "
                     f"shrinking quantum to {self.q_override:.0f}s")
        self.log(f"=== EXPLORE {len(todo)} graphs (hardest first) ===")
        for nm, g in todo:
            if self.remaining() < LEASE_FLOOR:
                break
            if not g["done"]:
                self.lease(nm, g)

    def _greedy(self):
        self.log("=== GREEDY marginal-gain reallocation ===")
        while self.remaining() >= LEASE_FLOOR:
            # 1) drop freshly-converged graphs
            for nm, g in self.G.items():
                if g["done"]:
                    continue
                try:
                    qb = quantum_base(self.budget, self._n_live(), self.q_override)
                    if is_converged(g, quantum_for(g, qb), self.stall_floor):
                        g["done"] = True
                        self.log(f"converged: {nm} (k={g['best_k']}, "
                                 f"spent={g['spent']:.0f}s) -> banking budget")
                except Exception as e:                # one bad graph never aborts run
                    print(f"[orch] convergence check failed for {nm}: {e}",
                          file=sys.stderr)
            # 2) score live graphs
            cands = []
            for nm, g in self.G.items():
                if g["done"]:
                    continue
                try:
                    qb = quantum_base(self.budget, self._n_live(), self.q_override)
                    cands.append((bid(g, quantum_for(g, qb)), nm, g))
                except Exception as e:
                    print(f"[orch] bid failed for {nm}: {e}", file=sys.stderr)
                    cands.append((0.0, nm, g))
            if cands:
                cands.sort(key=lambda c: c[0], reverse=True)
                top = cands[0][0]
                if top < BID_EPS:
                    # nobody improving (but live, e.g. dense early) -> minimax sink
                    nm, g = self._worst_k_live(cands)
                    self.lease(nm, g)
                    continue
                # anti-starvation: among bidders within STARVE_FACTOR of top, serve
                # the least-serviced (fewest post-explore leases) to avoid one graph
                # hogging the bank from an equally-improvable peer.
                near = [c for c in cands if c[0] >= top / STARVE_FACTOR and c[0] >= BID_EPS]
                near.sort(key=lambda c: (c[2]["post_leases"], -c[0]))
                self.lease(near[0][1], near[0][2])
                continue
            # 3) everyone converged but budget remains -> iterative probation
            if not self._probation():
                self.log("all graphs converged / probation exhausted -> idle to deadline")
                break

    def _worst_k_live(self, cands):
        live = [(nm, g) for _, nm, g in cands]
        return max(live, key=lambda kv: (kv[1]["best_k"] or 0))

    def _probation(self):
        """Reopen converged graphs (worst-k first) for one warm quantum each. A truly
        converged graph re-converges in one quantum at ~zero cost; a falsely-dropped
        one resumes improving. Returns True if any graph was reopened."""
        pool = [(nm, g) for nm, g in self.G.items()
                if g["done"] and not g["probation_failed"] and g["best_k"] is not None
                and g["warm"] is not None]
        if not pool:
            return False
        pool.sort(key=lambda kv: -(kv[1]["best_k"] or 0))
        nm, g = pool[0]
        before = (g["best_k"], g["best_x"])
        self.log(f"probation: reopening {nm} (k={g['best_k']})")
        g["done"] = False
        self.lease(nm, g, probation=True)
        after = (g["best_k"], g["best_x"])
        if after >= before:                  # no improvement -> stays converged
            g["done"] = True
            g["probation_failed"] = True
        return True

    # --- output ------------------------------------------------------------- #
    def _finalize(self):
        save_json(self.out_root / "bests.json", self.bests)
        sub_dir = self.out_root / "submission" / self.run_id
        sub_dir.mkdir(parents=True, exist_ok=True)
        summary = {}
        for nm, g in self.G.items():
            sub_path = None
            verified = None
            # Submit the best layout; for a graph that never got an improving lease
            # fall back to its (valid) original input so EVERY graph has a submission.
            warm_ok = bool(g["warm"] and Path(g["warm"]).exists())
            src = g["warm"] if warm_ok else g["path"]
            if src and Path(src).exists():
                sub_path = sub_dir / f"{nm}.json"
                shutil.copy2(src, sub_path)
                # A solved warm layout verifies fast at any size; a raw-input
                # fallback on a huge graph would re-pay computeAllCrossings (~min)
                # -> skip that verify and trust the m==0/size signal.
                if warm_ok or g["n"] < 5000:
                    verified = verify_output(sub_path)
            summary[nm] = dict(
                k=g["best_k"], totalX=g["best_x"], seconds=round(g["spent"], 1),
                leases=g["leases"], converged=g["done"],
                method0=g["method0"], dense=g["dense"], big=g["big"],
                n=g["n"], m=g["m"],
                submission=str(sub_path) if sub_path else None,
                verified_k=(verified or {}).get("k") if verified else None,
                valid=(verified or {}).get("valid") if verified else (g["m"] == 0))
        report = dict(run_id=self.run_id, budget_sec=self.budget, workers=self.W,
                      wall_sec=round(self.now(), 1), reserve_sec=round(self.reserve(), 1),
                      max_overrun_sec=round(self.max_overrun, 1),
                      skipped_launches=self.skipped_launches, summary=summary)
        save_json(self.run_dir / "orchestration.json", report)
        self._print_summary(report)
        return report

    def _print_summary(self, report):
        print("\n" + "=" * 78)
        print(f"ORCHESTRATION SUMMARY  run={report['run_id']}  "
              f"budget={report['budget_sec']:.0f}s  wall={report['wall_sec']:.0f}s  "
              f"reserve={report['reserve_sec']:.0f}s  max_overrun={report['max_overrun_sec']:.0f}s")
        print("-" * 78)
        print(f"{'graph':<16}{'k':>6}{'totalX':>12}{'sec':>8}{'lease':>6}"
              f"{'conv':>6}{'valid':>7}  method")
        for nm, s in sorted(report["summary"].items()):
            print(f"{nm:<16}{str(s['k']):>6}{str(s['totalX']):>12}{s['seconds']:>8.0f}"
                  f"{s['leases']:>6}{'Y' if s['converged'] else 'n':>6}"
                  f"{('Y' if s['valid'] else 'N'):>7}  {s['method0']}"
                  f"{' [dense]' if s['dense'] else ''}{' [big]' if s['big'] else ''}")
        worst = max((s["k"] for s in report["summary"].values() if s["k"] is not None),
                    default=None)
        print("-" * 78)
        print(f"worst-k (minimax objective) = {worst}    "
              f"total wall = {report['wall_sec']:.0f}s / {report['budget_sec']:.0f}s budget")
        print("=" * 78)


# --------------------------------------------------------------------------- #
# Deterministic self-test of the pure scheduling logic (no solver).
# --------------------------------------------------------------------------- #
def self_test():
    ok = True

    def check(name, cond):
        nonlocal ok
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}")
        ok = ok and cond

    print("self-test: classify()")
    check("dense A6 (200,3000) -> sa+dense", classify(200, 3000) == ("sa", {"dense": True, "big": False}))
    check("huge A8 (10466,20288) -> sa-stress+big", classify(10466, 20288) == ("sa-stress", {"big": True}))
    check("sparse A9 (2519,4938) -> sa-stress", classify(2519, 4938) == ("sa-stress", {}))
    check("trivial m=0 -> trivial", classify(10, 0) == ("sa", {"trivial": True}))
    check("no graphviz -> sa", classify(2519, 4938, no_graphviz=True)[0] == "sa")

    print("self-test: slope() guards + monotone descent")
    check("empty cum -> zeros", slope([], 100) == (0.0, 0.0, 1))
    check("single point -> zeros", slope([[5, 9, 100]], 100) == (0.0, 0.0, 100))
    dk, dx, x0 = slope([[0, 20, 1000], [10, 10, 500]], 100)
    check("descending k slope = 1.0/s", abs(dk - 1.0) < 1e-9 and abs(dx - 50.0) < 1e-9)
    dk2, _, _ = slope([[0, 9, 100], [10, 9, 100]], 100)
    check("flat k slope = 0", dk2 == 0.0)

    print("self-test: is_converged()")
    easy = dict(dense=False, leases=2, cum=[[0, 9, 100], [5, 9, 100], [10, 9, 100],
                                            [15, 9, 100], [20, 9, 100], [25, 9, 100]])
    check("flat easy graph (>=5 ticks, 2 leases) converges", is_converged(easy, 20, 25) is True)
    check("same graph, only 1 lease -> not converged",
          is_converged({**easy, "leases": 1}, 20, 25) is False)
    a6 = dict(dense=True, leases=5, cum=[[t, 800 - t, 9000 - 5 * t] for t in range(0, 60, 5)])
    check("dense graph never converges", is_converged(a6, 20, 25) is False)
    setup = dict(dense=False, leases=2, cum=[[0, 50, 9999], [120, 50, 9999]])  # 2 ticks
    check("setup-only trace (<5 ticks) not converged", is_converged(setup, 60, 150) is False)
    check("empty cum not converged", is_converged(dict(dense=False, leases=2, cum=[]), 20, 25) is False)

    print("self-test: bid() + dense multiplier")
    g_imp = dict(dense=False, cum=[[0, 20, 1000], [10, 10, 500]])
    g_den = dict(dense=True, cum=[[0, 20, 1000], [10, 10, 500]])
    b1, b2 = bid(g_imp, 60), bid(g_den, 60)
    check("improving graph has positive bid", b1 > 0)
    check("dense multiplier raises bid 1.3x", abs(b2 - b1 * DENSE_MULT) < 1e-6)
    check("flat graph bid ~ 0", bid(dict(dense=False, cum=[[0, 9, 100], [9, 9, 100]]), 60) < BID_EPS)

    print("self-test: quantum sizing")
    check("Qb clamped low->QMIN", quantum_base(160, 8) == QMIN)
    check("Qb mid", abs(quantum_base(3600, 9) - 100.0) < 1e-9)
    check("Qb clamped high->QMAX", quantum_base(100000, 1) == QMAX)
    check("big graph quantum >= BIG_FLOOR", quantum_for({"big": True}, 50) >= BIG_FLOOR)
    check("override respected", quantum_base(160, 8, override=20) == 20.0)

    print("self-test: anti-starvation tie-break (least-serviced among near-top)")
    cands = [(1.0, "a6_1", {"post_leases": 5}), (0.9, "a6_2", {"post_leases": 0})]
    top = cands[0][0]
    near = [c for c in cands if c[0] >= top / STARVE_FACTOR]
    near.sort(key=lambda c: (c[2]["post_leases"], -c[0]))
    check("least-serviced peer wins despite slightly lower bid", near[0][1] == "a6_2")

    print("\n" + ("ALL PASS" if ok else "SOME FAILED"))
    return 0 if ok else 1


# --------------------------------------------------------------------------- #
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--graphs", default="1-9", help="graph set (run_contest syntax)")
    ap.add_argument("--budget", type=float, default=3600, help="total wall-clock seconds")
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--out-dir", default="results")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--quantum", type=float, default=None,
                    help="override base quantum seconds (testing)")
    ap.add_argument("--stall-floor", type=float, default=STALL_FLOOR,
                    help="min plateau window seconds (lower for short tests)")
    ap.add_argument("--self-test", action="store_true",
                    help="run the deterministic logic self-test and exit")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    ensure_binaries()
    orch = Orchestrator(args.graphs, args.budget, args.workers, args.out_dir,
                        seed=args.seed, quantum_override=args.quantum,
                        stall_floor=args.stall_floor)
    orch.run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
