#!/usr/bin/env python3
"""
contest_orchestrate.py — deadline-safe adaptive orchestrator for the internal
GD k-planarity contest.

Given a fixed wall-clock budget (e.g. 40-50 min), it:
  1. ANALYSES every graph (n, m, density) and picks a cold-start method —
     sa-stress for sparse graphs (force-directed init wins), plain sa for very
     dense ones (stress init hurts there). Soft, CLI-overridable prior.
  2. EXPLORES: one cold lease per graph (hardest-first) to establish each k and a
     warm best-so-far layout.
  3. REALLOCATES greedily: each remaining quantum goes to the graph with the best
     recent k-drop (tie-broken toward higher current k = more room), warm-started
     from its best layout. Graphs that stop improving are dropped, banking budget.
  4. Runs every lease with the validated cooperative config: W workers,
     --xchg-rounds with HALF-SHARING (half explore independently, half intensify
     on the shared elite) — no-regret vs a plain portfolio
     (see results/exp-2026-06-30-reheat-sharing.md).
  5. NEVER overruns the budget: a per-subprocess backstop clamps every solver to a
     hard deadline that reserves time for the final per-graph verify + submission
     copy. A killed solver writes no layout, so the previous warm best is retained.

Output: results/submission/<run_id>/<graph>.json (one best VALID layout per graph)
        plus bests.json metadata and an orchestration.json report.

Reuses run_contest.py for methods, datasets, the W-worker run_combo primitive and
best-saving. Unlike orchestrate.py (trace-based scheduler, full-collapse warm
chain), this tracks improvement from run_combo's returned k and uses half-sharing.

Usage:
  python3 contest_orchestrate.py --graphs 1-9 --budget 2700 --workers 8
  python3 contest_orchestrate.py --self-test          # fast, no solver
"""
import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

import run_contest as rc
from run_contest import (
    parse_graph_spec, graph_path, graph_name, graph_size,
    run_combo, verify_output, update_best, load_json, save_json, ensure_binaries,
)

# --- tunables (graphs here are <=600 nodes, so quanta are small) ------------- #
QMIN, QMAX   = 25.0, 120.0   # quantum bounds (seconds)
EXPLORE_CAP  = 0.45          # explore phase may use at most this share of budget
STALL_LEASES = 2             # consecutive no-improvement leases -> converged
MIN_LEASES   = 2             # never converge on a single noisy lease
DENSE_DENS   = 8.0           # m/n above which the cold init prefers plain sa
LEASE_FLOOR  = 12.0          # don't launch a lease with less than this much budget
OVERRUN      = 1.5           # per-process soft timeout = OVERRUN * requested quantum
XCHG_ROUNDS  = 4             # cooperative best-exchange rounds per lease
NH_SIZE, NH_CANDS = 12, 64   # run_combo LNS knobs (unused by sa/sa-stress; passthrough)

# --------------------------------------------------------------------------- #
# Hard-deadline backstop: clamp every solver subprocess so no stage runs past the
# budget. Set _HARD_DEADLINE before the run; _LEASE_TIMEOUT before each lease.
# --------------------------------------------------------------------------- #
_LEASE_TIMEOUT = [None]
_HARD_DEADLINE = [None]


def _run_with_backstop(cmd, log_path):
    t0 = time.time()
    to = _LEASE_TIMEOUT[0]
    if _HARD_DEADLINE[0] is not None:
        left = _HARD_DEADLINE[0] - time.monotonic()
        to = max(1.0, min(to if to else left, left))
    with open(log_path, "w") as log:
        try:
            proc = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=to)
            code = proc.returncode
        except subprocess.TimeoutExpired:
            code = -9
            log.write(f"\n[orch] KILLED by budget backstop after "
                      f"{time.time() - t0:.0f}s (limit {to:.0f}s)\n")
    return code, time.time() - t0


rc._run = _run_with_backstop


def _clamp(x, lo, hi):
    return max(lo, min(hi, x))


# --------------------------------------------------------------------------- #
# Pure helpers (unit-tested in --self-test; no solver, no machine-load noise).
# --------------------------------------------------------------------------- #
def cold_method(n, m, no_graphviz=False, dense_dens=DENSE_DENS):
    """Cold-lease method from (n, m): plain sa for dense/no-graphviz, else sa-stress.
    Warm leases always use sa-warm. The greedy loop self-corrects, so a
    misclassified graph costs at most one explore lease."""
    if m == 0:
        return "sa"
    if (m / max(1, n)) >= dense_dens or no_graphviz:
        return "sa"
    return "sa-stress"


def quantum_base(budget, n_live, override=None):
    if override:
        return float(override)
    return _clamp(budget / (4 * max(1, n_live)), QMIN, QMAX)


def bid(g):
    """Priority of a live graph for the next quantum. Unexplored graphs run first.
    Otherwise: ROOM (current k = headroom toward the minimax/sum-k objective) scaled
    by a mild boost if the last lease was still improving, divided by leases served
    so the budget SPREADS across all hard graphs (least-serviced rotates in) instead
    of one descent monopolising it. Graphs that stop improving are dropped by
    converged(), so a high-k graph that is genuinely stuck won't hog forever."""
    if g["leases"] == 0:
        return float("inf")
    if g["done"]:
        return -1.0
    boost = 1.5 if (g["last_dk"] > 0 or g["last_dxf"] > 0) else 1.0
    return (g["best_k"] or 0) * boost / g["leases"]


def converged(g):
    """Drop a graph once it has had a fair trial and stopped improving."""
    return g["leases"] >= MIN_LEASES and g["stalls"] >= STALL_LEASES


# --------------------------------------------------------------------------- #
class ContestOrchestrator:
    def __init__(self, graphs_spec, budget, workers, out_dir, seed=1,
                 xchg_rounds=XCHG_ROUNDS, half_share=True, dense_dens=DENSE_DENS,
                 quantum_override=None, verbose=True, graphs_dir=None):
        self.budget = float(budget)
        self.W = int(workers)
        self.seed = int(seed)
        self.xchg = int(xchg_rounds)
        self.half = bool(half_share)
        self.dense_dens = float(dense_dens)
        self.q_override = quantum_override
        self.verbose = verbose
        self.out_root = Path(out_dir)
        self.run_id = f"corch_{int(time.time())}"
        self.run_dir = self.out_root / "runs" / self.run_id
        self.run_dir.mkdir(parents=True, exist_ok=True)
        self.bests = load_json(self.out_root / "bests.json", {})
        self.no_graphviz = shutil.which("sfdp") is None and shutil.which("neato") is None
        self.t0 = None
        self.max_overrun = 0.0
        self.lease_log = []
        self.G = self._build(graphs_spec, graphs_dir)

    def log(self, msg):
        if self.verbose:
            print(f"[corch {self.now():6.1f}s] {msg}", flush=True)

    def now(self):
        return time.monotonic() - self.t0 if self.t0 else 0.0

    @staticmethod
    def scan_dir(graphs_dir):
        """Every *.json in a folder that parses as a graph (has nodes+edges),
        as (name=file stem, absolute path). Used by --graphs-dir and the server's
        folder picker so submissions keep the ORIGINAL file names."""
        entries = []
        d = Path(graphs_dir).expanduser()
        for f in sorted(d.glob("*.json")):
            try:
                n, m = graph_size(f)
            except Exception:
                continue
            if n > 0:
                entries.append((f.stem, f.resolve(), n, m))
        return entries

    def _build(self, spec, graphs_dir=None):
        G = {}
        if graphs_dir:
            items = [(nm, p) for nm, p, _n, _m in self.scan_dir(graphs_dir)]
        else:
            items = [(graph_name(tok, graph_path(tok)), graph_path(tok))
                     for tok in parse_graph_spec(spec)]
        for nm, p in items:
            try:
                n, m = graph_size(p)
            except Exception as e:
                print(f"[corch] WARN skip {nm}: {e}", file=sys.stderr)
                continue
            G[nm] = dict(tok=nm, path=str(p), n=n, m=m,
                         method0=cold_method(n, m, self.no_graphviz, self.dense_dens),
                         best_k=None, best_x=None, warm=None,
                         leases=0, stalls=0, last_dk=0.0, last_dxf=0.0,
                         spent=0.0, done=(m == 0))
            if m == 0:
                G[nm].update(best_k=0, warm=G[nm]["path"])
        return G

    # --- budget accounting -------------------------------------------------- #
    def _verify_reserve(self):
        return sum(2.0 + g["n"] / 4000.0 for g in self.G.values())

    def reserve(self):
        return min(max(20.0, 0.02 * self.budget) + self._verify_reserve(),
                   0.30 * self.budget) + self.max_overrun

    def remaining(self):
        return self.budget - self.now() - self.reserve()

    def _n_live(self):
        return max(1, sum(1 for g in self.G.values() if not g["done"]))

    # --- one lease ---------------------------------------------------------- #
    def lease(self, nm, g):
        rem = self.remaining()
        if rem < LEASE_FLOOR:
            return False
        qb = quantum_base(self.budget, self._n_live(), self.q_override)
        q = min(qb, rem)
        cold = g["warm"] is None
        method = g["method0"] if cold else "sa-warm"
        warm = None if cold else g["warm"]
        # short quanta fragment poorly under many rounds; scale rounds to the slice
        xchg = max(1, min(self.xchg, int(q // 18)))
        qdir = self.run_dir / nm / f"q{g['leases']}"
        qdir.mkdir(parents=True, exist_ok=True)
        _LEASE_TIMEOUT[0] = max(45.0, q * OVERRUN)
        self.log(f"lease {nm:<14} {method:<9} warm={'Y' if warm else 'N'} "
                 f"q={q:.0f}s xchg={xchg}{'h' if (xchg>1 and self.half) else ''} "
                 f"(k={g['best_k']}, lease#{g['leases']})")
        try:
            _, best_w, wall = run_combo(
                method, g["path"], q / 60.0, 0.2 if cold else 0.05,
                self.seed + g["leases"] * 100, qdir, self.W, NH_SIZE, NH_CANDS,
                0.5, 0, gpath_warm=warm, xchg_rounds=xchg,
                half_share=(self.half and xchg > 1))
        except Exception as e:
            self.log(f"  run_combo ERROR on {nm}: {e}")
            best_w, wall = None, 0.0
        g["spent"] += wall
        g["leases"] += 1
        self.max_overrun = max(self.max_overrun, wall - q)
        self.lease_log.append((nm, method, warm is not None, round(q, 1), round(wall, 1)))
        self._absorb(nm, g, best_w, method, wall, q)
        return True

    def _absorb(self, nm, g, best_w, method, wall, q):
        """Fold a lease result into g's state: update best (verified), measure the
        k / totalX improvement that drives the next bid, and detect convergence."""
        def _cold_fallback():
            # A graph must always end with a VALID submission: if the cold lease
            # failed (e.g. stress init produced an invalid layout) and we have no
            # good layout yet, drop to plain sa for the next attempt.
            if g["warm"] is None and g["method0"] != "sa":
                g["method0"] = "sa"
                self.log(f"  {nm}: cold {method} invalid -> fall back to plain sa")

        if not (best_w and best_w.get("valid")):
            g["stalls"] += 1
            _cold_fallback()
            return
        nk, nx = best_w["final_k"], best_w["final_totalX"]
        v = verify_output(best_w["out_path"])           # re-verify before trusting
        if not (v and v["valid"] and v["k"] == nk):
            g["stalls"] += 1
            _cold_fallback()
            return
        ok = g["best_k"]
        ox = g["best_x"] if g["best_x"] is not None else float("inf")
        improved_k = ok is None or nk < ok
        improved_x = (ok is not None and nk == ok and nx < ox)
        if improved_k or improved_x:
            g["last_dk"] = (ok - nk) if (ok is not None and nk < ok) else 0.0
            g["last_dxf"] = ((ox - nx) / ox) if (ox not in (0, float("inf")) and nx < ox) else 0.0
            g["best_k"], g["best_x"], g["warm"] = nk, nx, best_w["out_path"]
            g["stalls"] = 0
            update_best(self.out_root, self.bests, nm, method, nk, nx,
                        best_w["out_path"], self.run_id, n_workers=self.W,
                        wall_clock_sec=round(wall, 1))
        else:
            g["last_dk"], g["last_dxf"] = 0.0, 0.0
            g["stalls"] += 1
        # self-terminated very early (optimal / temp floor) and not budget-clamped
        if nk == 0 or (wall < 0.4 * q and q >= QMIN):
            g["done"] = True

    # --- scheduler ---------------------------------------------------------- #
    def run(self):
        self.t0 = time.monotonic()
        _HARD_DEADLINE[0] = self.t0 + self.budget - min(self._verify_reserve(),
                                                        0.20 * self.budget)
        try:
            self._explore()
            self._greedy()
        except Exception as e:
            print(f"[corch] scheduler aborted: {e}", file=sys.stderr)
        return self._finalize()

    def _explore(self):
        todo = [(nm, g) for nm, g in self.G.items() if not g["done"]]
        todo.sort(key=lambda kv: -(kv[1]["m"] + kv[1]["n"]))   # hardest first
        if not todo:
            return
        qb = quantum_base(self.budget, len(todo), self.q_override)
        if qb * len(todo) > EXPLORE_CAP * self.budget and not self.q_override:
            self.q_override = max(QMIN / 2, EXPLORE_CAP * self.budget / len(todo))
            self.log(f"explore shrink: quantum -> {self.q_override:.0f}s")
        self.log(f"=== EXPLORE {len(todo)} graphs (hardest first) ===")
        for nm, g in todo:
            if self.remaining() < LEASE_FLOOR:
                break
            self.lease(nm, g)

    def _greedy(self):
        self.log("=== GREEDY marginal-gain reallocation ===")
        while self.remaining() >= LEASE_FLOOR:
            for g in self.G.values():
                if not g["done"] and converged(g):
                    g["done"] = True
                    self.log(f"converged: bank budget (k={g['best_k']})")
            live = [(bid(g), nm, g) for nm, g in self.G.items() if not g["done"]]
            if not live:
                self.log("all graphs converged -> stop early, budget banked")
                break
            live.sort(key=lambda c: c[0], reverse=True)
            _, nm, g = live[0]
            if not self.lease(nm, g):
                break

    # --- output ------------------------------------------------------------- #
    def _finalize(self):
        save_json(self.out_root / "bests.json", self.bests)
        sub_dir = self.out_root / "submission" / self.run_id
        sub_dir.mkdir(parents=True, exist_ok=True)
        summary = {}
        for nm, g in self.G.items():
            src = g["warm"] if (g["warm"] and Path(g["warm"]).exists()) else g["path"]
            sub = sub_dir / f"{nm}.json"
            v = None
            if src and Path(src).exists():
                shutil.copy2(src, sub)
                v = verify_output(sub) if g["n"] < 20000 else None
            summary[nm] = dict(k=g["best_k"], totalX=g["best_x"],
                               seconds=round(g["spent"], 1), leases=g["leases"],
                               converged=g["done"], method0=g["method0"],
                               n=g["n"], m=g["m"], submission=str(sub),
                               verified_k=(v or {}).get("k"),
                               valid=(v or {}).get("valid") if v else (g["m"] == 0))
        report = dict(run_id=self.run_id, budget_sec=self.budget, workers=self.W,
                      xchg_rounds=self.xchg, half_share=self.half,
                      wall_sec=round(self.now(), 1), reserve_sec=round(self.reserve(), 1),
                      max_overrun_sec=round(self.max_overrun, 1), summary=summary)
        save_json(self.run_dir / "orchestration.json", report)
        self._print(report)
        return report

    def _print(self, report):
        print("\n" + "=" * 74)
        print(f"CONTEST ORCHESTRATION  run={report['run_id']}  "
              f"budget={report['budget_sec']:.0f}s  wall={report['wall_sec']:.0f}s  "
              f"reserve={report['reserve_sec']:.0f}s")
        print("-" * 74)
        print(f"{'graph':<16}{'k':>6}{'totalX':>11}{'sec':>7}{'lease':>6}"
              f"{'conv':>6}{'valid':>7}  method")
        for nm, s in sorted(report["summary"].items()):
            print(f"{nm:<16}{str(s['k']):>6}{str(s['totalX']):>11}{s['seconds']:>7.0f}"
                  f"{s['leases']:>6}{'Y' if s['converged'] else 'n':>6}"
                  f"{('Y' if s['valid'] else 'N'):>7}  {s['method0']}")
        ks = [s["k"] for s in report["summary"].values() if s["k"] is not None]
        print("-" * 74)
        print(f"worst-k={max(ks) if ks else None}  sum-k={sum(ks) if ks else None}  "
              f"wall={report['wall_sec']:.0f}/{report['budget_sec']:.0f}s  "
              f"submissions in {self.out_root}/submission/{report['run_id']}/")
        print("=" * 74)


# --------------------------------------------------------------------------- #
def self_test():
    ok = True

    def check(name, cond):
        nonlocal ok
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}")
        ok = ok and cond

    print("cold_method() prior")
    check("sparse -> sa-stress", cold_method(500, 1984) == "sa-stress")
    check("dense (m/n>=8) -> sa", cold_method(100, 900) == "sa")
    check("empty -> sa", cold_method(10, 0) == "sa")
    check("no graphviz -> sa", cold_method(500, 1984, no_graphviz=True) == "sa")
    check("dense threshold configurable", cold_method(100, 900, dense_dens=20) == "sa-stress")

    print("quantum_base() bounds")
    check("clamped low -> QMIN", quantum_base(100, 9) == QMIN)
    check("mid", abs(quantum_base(2700, 9) - 75.0) < 1e-9)
    check("clamped high -> QMAX", quantum_base(100000, 1) == QMAX)
    check("override respected", quantum_base(2700, 9, override=20) == 20.0)

    print("bid() ordering (room/leases spread, improving boost)")
    check("unexplored bids infinite", bid({"leases": 0}) == float("inf"))
    check("done graph bids negative", bid({"leases": 3, "done": True}) == -1.0)
    hi = {"leases": 1, "done": False, "last_dk": 0.0, "last_dxf": 0.0, "best_k": 250}
    lo = {"leases": 1, "done": False, "last_dk": 0.0, "last_dxf": 0.0, "best_k": 4}
    check("worst (high-k) graph prioritised over trivial", bid(hi) > bid(lo))
    served = {**hi, "leases": 6}
    fresh = {"leases": 1, "done": False, "last_dk": 0.0, "last_dxf": 0.0, "best_k": 85}
    check("least-serviced rotates in (heavily-served high-k yields to fresh peer)",
          bid(fresh) > bid(served))
    impr = {**hi, "last_dk": 4.0}
    check("improving boost favours an active descent at equal room/leases",
          bid(impr) > bid(hi))

    print("converged()")
    check("2 stalls after fair trial -> converged",
          converged({"leases": 3, "stalls": 2}) is True)
    check("1 stall not converged", converged({"leases": 3, "stalls": 1}) is False)
    check("single lease never converged", converged({"leases": 1, "stalls": 5}) is False)

    print("\n" + ("ALL PASS" if ok else "SOME FAILED"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--graphs", default="1-9", help="graph set (run_contest syntax)")
    ap.add_argument("--graphs-dir", default=None,
                    help="run on every *.json graph in this folder (submissions keep "
                         "the original file names); overrides --graphs")
    ap.add_argument("--budget", type=float, default=2700, help="total wall-clock seconds")
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--out-dir", default="results")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--xchg-rounds", type=int, default=XCHG_ROUNDS)
    ap.add_argument("--no-half", action="store_true", help="disable half-sharing")
    ap.add_argument("--dense-density", type=float, default=DENSE_DENS)
    ap.add_argument("--quantum", type=float, default=None, help="override quantum sec (testing)")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    ensure_binaries()
    ContestOrchestrator(args.graphs, args.budget, args.workers, args.out_dir,
                        seed=args.seed, xchg_rounds=args.xchg_rounds,
                        half_share=not args.no_half, dense_dens=args.dense_density,
                        quantum_override=args.quantum, graphs_dir=args.graphs_dir).run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
