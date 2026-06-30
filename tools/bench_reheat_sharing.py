#!/usr/bin/env python3
"""sa-stress reheat + worker-sharing sweep harness.

Caches stress-inits per (graph, seed) so every reheat/xchg variant is compared
on IDENTICAL inits (removes init RNG as a confound). Mirrors real sa-stress:
warm sakgd from the stress init, p1_frac=0.2, kband=2. The only things that vary
between configs are the solver reheat flags and the cross-worker sharing rounds.
"""
import argparse, json, subprocess, sys, time, threading, shutil, re
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed

import os, tempfile
ROOT   = Path(__file__).resolve().parent.parent
SAKGD  = ROOT / "sakgd"
STRESS = ROOT / "tools" / "stress_init.py"
GDIR   = ROOT / "data/live-2025-contest/live-contest"
SP     = Path(os.environ.get("BENCH_WORKDIR", tempfile.gettempdir())) / "sa_bench"
INITDIR = SP / "inits"; INITDIR.mkdir(parents=True, exist_ok=True)
WORKDIR = SP / "work";  WORKDIR.mkdir(parents=True, exist_ok=True)

P1_FRAC = 0.2          # matches run_contest default
VERIFY_RE = re.compile(r"k=(\d+) totalCrossings=(\d+) vertexEdgeOverlap=(\w+)")

# ---- config sets: each config = dict(name, flags=[solver flags], xchg=int) ----
CONFIG_SETS = {
    "sweep": [
        dict(name="base",  flags=[],                xchg=1),  # current sa-stress
        dict(name="rh2",   flags=["--reheat","2"],  xchg=1),
        dict(name="rh4",   flags=["--reheat","4"],  xchg=1),
        dict(name="rh8",   flags=["--reheat","8"],  xchg=1),
        dict(name="x2",    flags=[],                xchg=2),
        dict(name="x4",    flags=[],                xchg=4),
    ],
}

def verify(path):
    try:
        r = subprocess.run([str(SAKGD), "--verify", str(path)],
                           capture_output=True, text=True, timeout=900)
        m = VERIFY_RE.search(r.stdout)
        if not m: return None
        k, x, ov = int(m.group(1)), int(m.group(2)), m.group(3)
        return (k, x) if ov == "no" else None
    except Exception:
        return None

def make_init(graph, seed, init_sec):
    out = INITDIR / f"g{graph}_s{seed}.json"
    if out.exists() and out.stat().st_size > 0:
        return out
    gp = GDIR / f"Automatic-{graph}.json"
    subprocess.run([sys.executable, str(STRESS), "-i", str(gp), "-o", str(out),
                    "-s", str(seed), "-t", str(init_sec)],
                   capture_output=True, text=True)
    return out

def _sakgd(inp, out, budget_sec, seed, p1_frac, flags):
    cmd = [str(SAKGD), "-i", str(inp), "-o", str(out),
           "-t", f"{budget_sec/60:.5f}", "-p1", f"{budget_sec*p1_frac/60:.5f}",
           "-s", str(seed), "--init", "input", "--kband", "2"] + flags
    subprocess.run(cmd, capture_output=True, text=True)

def run_config_on_graph(cfg, graph, seeds, budget_sec, init_sec):
    nw = len(seeds)
    xchg = cfg.get("xchg", 1)
    flags = cfg["flags"]
    tag = f"{cfg['name']}_g{graph}"
    elite = {"file": WORKDIR / f"{tag}_elite.json", "path": None}
    barrier = threading.Barrier(nw) if xchg > 1 else None

    share = cfg.get("share", "all")          # "all" | "half"

    def worker(wid):
        seed = seeds[wid]
        init = make_init(graph, seed, init_sec)
        if xchg <= 1:
            out = WORKDIR / f"{tag}_w{wid}.json"
            _sakgd(init, out, budget_sec, seed, P1_FRAC, flags)
            return out
        # "half": low-half wids stay independent (never adopt elite), preserving
        # exploration for bottleneck graphs; high-half adopt the shared elite.
        independent = (share == "half" and wid < nw // 2)
        rsec = budget_sec / xchg
        cur = init
        for r in range(xchg):
            ro = WORKDIR / f"{tag}_w{wid}_r{r}.json"
            adopt = (r > 0 and elite["path"] and not independent)
            src = elite["path"] if adopt else cur
            # round 0 does the full warm (p1_frac 0.2); later rounds polish (0.02)
            _sakgd(src, ro, rsec, seed + r * 1000, P1_FRAC if r == 0 else 0.02, flags)
            cur = ro
            barrier.wait()
            if wid == 0:
                best = None
                for w in range(nw):
                    res = verify(WORKDIR / f"{tag}_w{w}_r{r}.json")
                    if res and (best is None or res < best[0]):
                        best = (res, WORKDIR / f"{tag}_w{w}_r{r}.json")
                if best:
                    shutil.copyfile(best[1], elite["file"])
                    elite["path"] = elite["file"]
            barrier.wait()
        return cur

    outs = [None] * nw
    with ThreadPoolExecutor(max_workers=nw) as ex:
        futs = {ex.submit(worker, w): w for w in range(nw)}
        for f in as_completed(futs):
            outs[futs[f]] = f.result()
    # best across workers (canonical verify)
    best = None
    for o in outs:
        res = verify(o)
        if res and (best is None or res < best):
            best = res
    return best  # (k, x) or None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--graphs", default="1,4,5,6")
    ap.add_argument("--workers", type=int, default=3)
    ap.add_argument("--budget", type=float, default=75.0, help="SA budget seconds")
    ap.add_argument("--init-sec", type=float, default=8.0)
    ap.add_argument("--base-seed", type=int, default=1)
    ap.add_argument("--configs", default="sweep")
    ap.add_argument("--maxproc", type=int, default=9)
    ap.add_argument("--out", default=str(SP / "results.json"))
    a = ap.parse_args()

    graphs = [g.strip() for g in a.graphs.split(",") if g.strip()]
    seeds = [a.base_seed + i for i in range(a.workers)]
    configs = json.loads(Path(a.configs).read_text()) if a.configs.endswith(".json") \
              else CONFIG_SETS[a.configs]

    # pre-generate inits (parallel) so they don't serialize inside the sweep
    print(f"[init] {len(graphs)} graphs x {a.workers} seeds, {a.init_sec}s box...", flush=True)
    with ThreadPoolExecutor(max_workers=a.maxproc) as ex:
        list(ex.map(lambda gs: make_init(gs[0], gs[1], a.init_sec),
                    [(g, s) for g in graphs for s in seeds]))

    units = [(c, g) for c in configs for g in graphs]
    unit_pool = max(1, a.maxproc // a.workers)
    print(f"[bench] {len(units)} units, {unit_pool} concurrent, "
          f"W={a.workers} budget={a.budget}s seeds={seeds}", flush=True)

    results = {}  # name -> {graph -> [k,x]}
    t0 = time.time()
    def run_unit(u):
        c, g = u
        r = run_config_on_graph(c, g, seeds, a.budget, a.init_sec)
        print(f"  {c['name']:>6} g{g}: {('k=%d x=%d'%r) if r else 'INVALID'}  "
              f"[{time.time()-t0:.0f}s]", flush=True)
        return (c["name"], g, r)
    with ThreadPoolExecutor(max_workers=unit_pool) as ex:
        for name, g, r in ex.map(run_unit, units):
            results.setdefault(name, {})[g] = r

    # table
    print("\n=== RESULTS (k; tiebreak totalX) ===")
    hdr = "config".ljust(8) + "".join(f"  g{g:>4}" for g in graphs) + "   sum_k"
    print(hdr); print("-" * len(hdr))
    rows = []
    for c in configs:
        n = c["name"]; sk = 0; cells = []
        for g in graphs:
            r = results[n].get(g)
            if r: cells.append(f"{r[0]:>6}"); sk += r[0]
            else: cells.append("   INV")
        rows.append((n, sk))
        print(n.ljust(8) + "".join(cells) + f"   {sk:>5}")
    print("-" * len(hdr))
    best = min(rows, key=lambda x: x[1])
    print(f"best sum_k: {best[0]} ({best[1]})")
    Path(a.out).write_text(json.dumps({"params": vars(a), "seeds": seeds,
        "results": results}, indent=2))
    print(f"[saved] {a.out}")

if __name__ == "__main__":
    main()
