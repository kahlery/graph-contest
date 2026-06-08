#!/usr/bin/env python3
"""
GD-2025 k-planarity batch runner.

Methods : sa, staged, ils, staged-adaptive
Workers : W parallel workers per (graph x method) combo, different seeds
Timing  : group-based — small graphs get less budget, large graphs more
Logging : detailed per-combo JSON + HTML report regenerated after every combo

Usage
-----
  python3 run_contest.py                            # default settings
  python3 run_contest.py --minutes-small 5 --minutes-medium 8 --minutes-large 15
  python3 run_contest.py --methods sa,staged --graphs 1-4 --workers 1
  python3 run_contest.py --warm-start
  python3 run_contest.py --report-only
"""

import argparse
import csv
import json
import re
import shutil
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timedelta
from pathlib import Path

ROOT      = Path(__file__).resolve().parent
DATA_DIR  = ROOT / "data" / "live-2025-contest" / "live-contest"
SAKGD     = ROOT / "sakgd"
APPROACH1 = ROOT / "approach1"

# Per-graph time groups (minutes per combo).
GRAPH_GROUPS = {
    "small":  {"graphs": {1, 2, 3, 4}},
    "medium": {"graphs": {5, 6, 7, 9}},
    "large":  {"graphs": {8}},
}

METHOD_COLORS = {
    "sa":              "#4361ee",
    "staged":          "#7209b7",
    "ils":             "#f72585",
    "staged-adaptive": "#f4a261",
}
DEFAULT_COLORS = ["#4361ee", "#7209b7", "#f72585", "#f4a261", "#4cc9f0"]

INITIAL_RE = re.compile(
    r"Initial:\s*k=(\d+)\s+totalX=(\d+)\s+vertexEdgeOverlap=(no|YES)")
FINAL_RE   = re.compile(r"Final best:\s*k=(\d+)\s+totalX=(\d+)")
VERIFY_RE  = re.compile(
    r"k=(\d+)\s+totalCrossings=(\d+)\s+vertexEdgeOverlap=(no|yes)")


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
def parse_graph_spec(spec):
    out = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-", 1)
            out.update(range(int(a), int(b) + 1))
        else:
            out.add(int(part))
    return sorted(out)


def graph_path(idx):
    return DATA_DIR / f"Automatic-{idx}.json"


def graph_size(path):
    with open(path) as f:
        g = json.load(f)
    return len(g.get("nodes", [])), len(g.get("edges", []))


def group_for(idx):
    for name, info in GRAPH_GROUPS.items():
        if idx in info["graphs"]:
            return name
    return "medium"


def budget_for(idx, minutes_map):
    return minutes_map[group_for(idx)]


def ensure_binaries():
    if SAKGD.exists() and APPROACH1.exists():
        return
    print("[build] running make ...", flush=True)
    r = subprocess.run(["make"], cwd=ROOT)
    if r.returncode != 0 or not (SAKGD.exists() and APPROACH1.exists()):
        sys.exit("[build] FAILED — run `make` manually.")


def mins(x):
    return f"{x:.6f}".rstrip("0").rstrip(".")


def parse_log(log_path):
    out = {"baseline_k": None, "baseline_totalX": None,
           "baseline_overlap": False, "k": None, "totalX": None}
    try:
        text = Path(log_path).read_text()
    except OSError:
        return out
    mi = INITIAL_RE.search(text)
    if mi:
        out["baseline_k"]       = int(mi.group(1))
        out["baseline_totalX"]  = int(mi.group(2))
        out["baseline_overlap"] = (mi.group(3) == "YES")
    mf = FINAL_RE.search(text)
    if mf:
        out["k"]      = int(mf.group(1))
        out["totalX"] = int(mf.group(2))
    return out


def verify_output(layout_path):
    if not Path(layout_path).exists():
        return None
    proc = subprocess.run(
        [str(SAKGD), "--verify", str(layout_path)],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    m = VERIFY_RE.search(proc.stdout)
    if not m:
        return None
    return {"k": int(m.group(1)), "totalX": int(m.group(2)),
            "valid": m.group(3) == "no"}


# ---------------------------------------------------------------------------
# single-worker solver launchers  ->  (out_path, rc, wall_sec, first_log, final_log)
# ---------------------------------------------------------------------------
def _run(cmd, log_path):
    t0 = time.time()
    with open(log_path, "w") as log:
        proc = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT)
    return proc.returncode, time.time() - t0


def run_sa(gpath, total_min, p1_frac, seed, out_dir, suffix=""):
    out = out_dir / f"sa{suffix}.json"
    log = out_dir / f"sa{suffix}.log"
    cmd = [str(SAKGD), "-i", str(gpath), "-o", str(out),
           "-t", mins(total_min), "-p1", mins(total_min * p1_frac),
           "-s", str(seed)]
    rc, sec = _run(cmd, log)
    return out, rc, sec, log, log


def run_staged(gpath, total_min, p1_frac, seed, out_dir,
               lns_frac, nh_size, nh_cands, suffix="", adaptive=False):
    lns_min = total_min * lns_frac
    sa_min  = total_min - lns_min
    lns_mode = "lns-adaptive" if adaptive else "lns"

    lns_out = out_dir / f"staged_lns{suffix}.json"
    lns_log = out_dir / f"staged_lns{suffix}.log"
    cmd_lns = [str(APPROACH1), "-i", str(gpath), "-o", str(lns_out),
               "-t", mins(lns_min), "-p1", mins(lns_min * p1_frac),
               "--mode", lns_mode,
               "--nh-size", str(nh_size), "--nh-cands", str(nh_cands),
               "-s", str(seed)]
    rc1, sec1 = _run(cmd_lns, lns_log)

    sa_input = lns_out if lns_out.exists() else gpath
    out     = out_dir / f"staged{suffix}.json"
    sa_log  = out_dir / f"staged_sa{suffix}.log"
    cmd_sa  = [str(SAKGD), "-i", str(sa_input), "-o", str(out),
               "-t", mins(sa_min), "-p1", mins(sa_min * p1_frac),
               "-s", str(seed)]
    rc2, sec2 = _run(cmd_sa, sa_log)
    rc = rc1 if rc1 != 0 else rc2
    return out, rc, sec1 + sec2, lns_log, sa_log


def run_ils(gpath, total_min, p1_frac, seed, out_dir, ils_perturb, suffix=""):
    out = out_dir / f"ils{suffix}.json"
    log = out_dir / f"ils{suffix}.log"
    cmd = [str(APPROACH1), "-i", str(gpath), "-o", str(out),
           "-t", mins(total_min), "-p1", mins(total_min * p1_frac),
           "--mode", "ils", "-s", str(seed)]
    if ils_perturb > 0:
        cmd += ["--ils-perturb", str(ils_perturb)]
    rc, sec = _run(cmd, log)
    return out, rc, sec, log, log


# ---------------------------------------------------------------------------
# parallel multi-start wrapper
# ---------------------------------------------------------------------------
def run_combo(method, gpath, total_min, p1_frac, base_seed,
              out_dir, n_workers, nh_size, nh_cands, lns_frac, ils_perturb,
              gpath_warm=None):
    """Run n_workers in parallel with different seeds; return list of worker results."""

    def worker(wid):
        seed   = base_seed + wid
        suffix = f"_w{wid}"
        inp    = gpath_warm if (gpath_warm and Path(gpath_warm).exists()) else gpath
        t0     = time.time()
        if method == "sa":
            out, rc, sec, flog, glog = run_sa(
                inp, total_min, p1_frac, seed, out_dir, suffix)
        elif method == "staged":
            out, rc, sec, flog, glog = run_staged(
                inp, total_min, p1_frac, seed, out_dir,
                lns_frac, nh_size, nh_cands, suffix, adaptive=False)
        elif method == "staged-adaptive":
            out, rc, sec, flog, glog = run_staged(
                inp, total_min, p1_frac, seed, out_dir,
                lns_frac, nh_size, nh_cands, suffix, adaptive=True)
        else:  # ils
            out, rc, sec, flog, glog = run_ils(
                inp, total_min, p1_frac, seed, out_dir, ils_perturb, suffix)

        first  = parse_log(flog)
        final  = parse_log(glog)
        wall   = time.time() - t0
        return {
            "worker_id":      wid,
            "seed":           seed,
            "out_path":       str(out),
            "returncode":     rc,
            "wall_clock_sec": round(wall, 1),
            "initial_k":      first["baseline_k"],
            "initial_totalX": first["baseline_totalX"],
            "final_k":        final["k"],
            "final_totalX":   final["totalX"],
            "valid":          rc == 0 and out.exists() and final["k"] is not None,
        }

    wall_start = time.time()
    workers_out = [None] * n_workers
    with ThreadPoolExecutor(max_workers=n_workers) as ex:
        futures = {ex.submit(worker, wid): wid for wid in range(n_workers)}
        for fut in as_completed(futures):
            wid = futures[fut]
            workers_out[wid] = fut.result()
    wall_total = time.time() - wall_start

    # Pick best valid worker result.
    best_w = None
    for w in workers_out:
        if w is None or not w["valid"]:
            continue
        if best_w is None:
            best_w = w
        elif (w["final_k"] < best_w["final_k"] or
              (w["final_k"] == best_w["final_k"] and
               (w["final_totalX"] or 0) < (best_w["final_totalX"] or 0))):
            best_w = w

    return workers_out, best_w, wall_total


# ---------------------------------------------------------------------------
# history / best management
# ---------------------------------------------------------------------------
def load_json(path, default):
    if Path(path).exists():
        try:
            return json.loads(Path(path).read_text())
        except json.JSONDecodeError:
            pass
    return default


def save_json(path, obj):
    Path(path).write_text(json.dumps(obj, indent=2))


def best_key(graph, method):
    return f"{graph}__{method}"


def update_best(out_root, bests, graph, method, k, totalX, layout_path, run_id,
                budget_min=None, n_workers=None, wall_clock_sec=None):
    if not isinstance(k, int):
        return False
    key = best_key(graph, method)
    cur = bests.get(key)
    better = (cur is None
              or k < cur["k"]
              or (k == cur["k"] and totalX is not None
                  and totalX < cur.get("totalX", float("inf"))))
    if not better:
        return False
    best_dir = out_root / "best" / graph
    best_dir.mkdir(parents=True, exist_ok=True)
    dest = best_dir / f"{method}.json"
    if layout_path and Path(layout_path).exists():
        shutil.copy2(layout_path, dest)
    bests[key] = {"k": k, "totalX": totalX,
                  "run_id": run_id, "layout_path": str(dest),
                  "budget_min": budget_min, "n_workers": n_workers,
                  "wall_clock_sec": wall_clock_sec}
    return True


# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Live log parsing (for per-graph in-progress status in report.html)
# ---------------------------------------------------------------------------
_RL_INITIAL  = re.compile(r"Initial:\s*k=(\d+)\s+totalX=(\d+)")
_RL_PH_PROG  = re.compile(r"\[phase (\d+)\]\s+t=(\d+)s\s+bestK=(\d+)\s+bestX=(\d+).*?sT=(\S+)")
_RL_ILS_RND  = re.compile(r"\[ILS phase (\d+)\] round (\d+)/(\d+).*?curBestK=(\d+)")
_RL_ILS_END  = re.compile(r"\[ILS phase (\d+)\] end.*?bestK=(\d+)\s+bestX=(\d+)")
_RL_LNS_PROG = re.compile(r"\[LNS[^\]]*\]\s+t=(\d+)s\s+bestK=(\d+)\s+bestX=(\d+)")
_RL_LNS_END  = re.compile(r"\[LNS[^\]]*phase \d+\] end.*?bestK=(\d+)\s+bestX=(\d+)")
_RL_FINAL    = re.compile(r"Final best:\s*k=(\d+)\s+totalX=(\d+)")


def _parse_live_log(path: Path) -> dict:
    s = dict(status="waiting", initial_k=None, best_k=None, best_x=None,
             t_elapsed=None, temp=None, ils_round=None, ils_total=None, lns_t=None)
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return s
    if not text.strip():
        return s
    s["status"] = "ph1"
    m = _RL_INITIAL.search(text)
    if m:
        s["initial_k"] = int(m.group(1))
        s["best_k"]    = int(m.group(1))
    for m in _RL_PH_PROG.finditer(text):
        ph = int(m.group(1))
        s["t_elapsed"] = int(m.group(2))
        s["best_k"]    = int(m.group(3))
        s["best_x"]    = int(m.group(4))
        s["temp"]      = float(m.group(5))
        s["status"]    = f"ph{ph}"
    for m in _RL_ILS_RND.finditer(text):
        ph = int(m.group(1))
        s["ils_round"] = int(m.group(2))
        s["ils_total"] = int(m.group(3))
        s["best_k"]    = int(m.group(4))
        s["status"]    = f"ils_ph{ph}"
    for m in _RL_ILS_END.finditer(text):
        ph = int(m.group(1))
        s["best_k"] = int(m.group(2))
        s["best_x"] = int(m.group(3))
        s["status"] = f"ils_ph{ph}_done"
    for m in _RL_LNS_PROG.finditer(text):
        s["lns_t"]  = int(m.group(1))
        s["best_k"] = int(m.group(2))
        s["best_x"] = int(m.group(3))
        s["status"] = "lns"
    m = _RL_LNS_END.search(text)
    if m:
        s["best_k"] = int(m.group(1))
        s["best_x"] = int(m.group(2))
        s["status"] = "lns_done"
    m = _RL_FINAL.search(text)
    if m:
        s["best_k"] = int(m.group(1))
        s["best_x"] = int(m.group(2))
        s["status"] = "done"
    return s


def scan_live_status(run_dir: Path, methods: list) -> dict:
    """Return {(gname, method): state_dict} by reading all log files in run_dir."""
    result = {}
    if not run_dir or not run_dir.exists():
        return result
    for gdir in sorted(run_dir.iterdir(),
                       key=lambda p: int(re.search(r"\d+", p.name).group() or 0)):
        if not gdir.is_dir():
            continue
        gname = gdir.name
        for method in methods:
            if method in ("staged", "staged-adaptive"):
                log = gdir / "staged_sa_w0.log"
                if not log.exists():
                    log = gdir / "staged_lns_w0.log"
            else:
                log = gdir / f"{method}_w0.log"
            result[(gname, method)] = _parse_live_log(log)
    return result


def _live_status_html(live_status: dict, methods: list) -> str:
    """Build the Live Status table HTML from parsed log states."""
    graphs = sorted(
        {g for (g, _) in live_status},
        key=lambda s: int(re.search(r"\d+", s).group() or 0)
    )
    if not graphs:
        return ""
    # Use only methods that actually appear in live_status keys.
    live_methods_set = {m for (_, m) in live_status}
    methods = [m for m in methods if m in live_methods_set]
    if not methods:
        return ""

    def badge(s: dict) -> str:
        st = s.get("status", "waiting")
        bk = s.get("best_k")
        bx = s.get("best_x")
        t  = s.get("t_elapsed")
        T  = s.get("temp")
        if st == "waiting":
            return "<span class='ls-wait'>·· waiting ··</span>"
        if st == "ph1":
            k  = f"k={bk}" if bk is not None else "k=?"
            ts = f" t={t}s"  if t  is not None else ""
            return f"<span class='ls-ph1'>▶ ph1 {k}{ts}</span>"
        if st == "ph2":
            k  = f"k={bk}" if bk is not None else "k=?"
            ts = f" t={t}s"  if t  is not None else ""
            Ts = f" T={T:.3f}" if T is not None else ""
            return f"<span class='ls-ph2'>▶ ph2 {k}{ts}{Ts}</span>"
        if st.startswith("ils_ph"):
            ph  = st.replace("ils_ph", "").replace("_done", "")
            r   = s.get("ils_round")
            tot = s.get("ils_total")
            k   = f"k={bk}" if bk is not None else "k=?"
            rnd = f" r={r}/{tot}" if r else ""
            return f"<span class='ls-ils'>▶ ILS·ph{ph}{rnd} {k}</span>"
        if st in ("lns", "lns_done"):
            k  = f"k={bk}" if bk is not None else "k=?"
            lt = s.get("lns_t")
            ts = f" t={lt}s" if lt else ""
            return f"<span class='ls-lns'>▶ LNS {k}{ts}</span>"
        if st == "done":
            k = f"k={bk}" if bk is not None else "k=?"
            x = f" X={bx}"  if bx is not None else ""
            return f"<span class='ls-done'>✓ {k}{x}</span>"
        return "<span class='ls-wait'>··</span>"

    done_cnt   = sum(1 for s in live_status.values() if s.get("status") == "done")
    active_cnt = sum(1 for s in live_status.values()
                     if s.get("status") not in ("waiting", "done"))
    n_total    = len(live_status)

    mh   = "".join(f"<th>{m.upper()}</th>" for m in methods)
    rows = []
    for g in graphs:
        cells = f"<td><strong>{g}</strong></td>"
        for m in methods:
            s = live_status.get((g, m), {"status": "waiting"})
            cells += f"<td>{badge(s)}</td>"
        rows.append(f"<tr>{cells}</tr>")

    return (
        "<h2>Live Status <small style='font-weight:400;text-transform:none;"
        f"font-size:.75rem;color:#666'>— {done_cnt}/{n_total} done"
        f", {active_cnt} running — auto-refresh 5s</small></h2>"
        "<div class='card'>"
        "<table>"
        f"<thead><tr><th>Graph</th>{mh}</tr></thead>"
        "<tbody>" + "\n".join(rows) + "</tbody>"
        "</table></div>"
    )


# HTML report
# ---------------------------------------------------------------------------
def _js(values):
    return "[" + ", ".join("null" if v is None else str(v) for v in values) + "]"


def generate_report(out_root, history, bests, methods_order=None, live_status=None):
    best_results = {}
    for key, val in bests.items():
        if "__" in key:
            g, m = key.split("__", 1)
            best_results[(g, m)] = val

    # combo_lookup lets us fill in config fields missing from older bests.json entries
    combo_lookup = {}
    for _run in history.get("runs", []):
        _rid = _run.get("id", "")
        for _c in _run.get("combos", []):
            combo_lookup[(_c["graph"], _c["method"], _rid)] = _c

    all_graphs, all_methods = set(), set()
    baseline_by_graph, graph_meta = {}, {}
    for run in history.get("runs", []):
        for r in run.get("combos", []):
            all_graphs.add(r["graph"])
            all_methods.add(r["method"])
            if isinstance(r.get("baseline_k"), int):
                baseline_by_graph[r["graph"]] = r["baseline_k"]
            if r.get("nodes"):
                graph_meta[r["graph"]] = {
                    "nodes": r["nodes"], "edges": r["edges"]}

    if methods_order:
        methods = [m for m in methods_order if m in all_methods]
        methods += [m for m in sorted(all_methods) if m not in methods]
    else:
        methods = sorted(all_methods)

    graphs = sorted(all_graphs, key=lambda s: (len(s), s))

    baseline_data = [baseline_by_graph.get(g) for g in graphs]
    method_data   = {m: [best_results.get((g, m), {}).get("k") for g in graphs]
                     for m in methods}
    improv_data   = {}
    for m in methods:
        row = []
        for g, bk in zip(graphs, baseline_data):
            mk = best_results.get((g, m), {}).get("k")
            if isinstance(bk, int) and isinstance(mk, int) and bk > 0:
                row.append(round((bk - mk) / bk * 100, 1))
            else:
                row.append(None)
        improv_data[m] = row

    graph_best_k = {}
    for g in graphs:
        ks = [best_results.get((g, m), {}).get("k") for m in methods]
        vk = [k for k in ks if isinstance(k, int)]
        graph_best_k[g] = min(vk) if vk else None

    datasets_js_parts = [f"""{{
            label: 'Baseline',
            data: {_js(baseline_data)},
            backgroundColor: 'rgba(160,160,160,0.4)',
            borderColor: '#aaaaaa', borderWidth: 1,
        }}"""]
    improv_parts = []
    for i, m in enumerate(methods):
        c = METHOD_COLORS.get(m, DEFAULT_COLORS[i % len(DEFAULT_COLORS)])
        datasets_js_parts.append(f"""{{
            label: '{m.upper()}',
            data: {_js(method_data[m])},
            backgroundColor: '{c}99', borderColor: '{c}', borderWidth: 2,
        }}""")
        improv_parts.append(f"""{{
            label: '{m.upper()}',
            data: {_js(improv_data[m])},
            backgroundColor: '{c}99', borderColor: '{c}', borderWidth: 2,
        }}""")

    _fallback = "#333"
    mh = "".join(
        f"<th colspan='2' style='color:{METHOD_COLORS.get(m, _fallback)}'>{m.upper()}</th>"
        for m in methods)
    ms = "<th>k</th><th>totalX</th>" * len(methods)

    tbody = []
    for r in [{"graph": g, **graph_meta.get(g, {"nodes": "?", "edges": "?"})}
               for g in graphs]:
        g  = r["graph"]
        bk = baseline_by_graph.get(g, "?")
        bk_best = graph_best_k.get(g)
        cells = f"<td>{g}</td><td>{r['nodes']}</td><td>{r['edges']}</td><td>{bk}</td>"
        for m in methods:
            br  = best_results.get((g, m), {})
            mk  = br.get("k")
            tx  = br.get("totalX", "—")
            if mk is None:
                cells += "<td>—</td><td>—</td>"
            else:
                is_best = (mk == bk_best)
                kstr    = f"<strong>{mk}</strong>" if is_best else str(mk)
                td_cls  = " class='best-cell'" if is_best else ""
                run_id  = br.get("run_id", "")
                cl      = combo_lookup.get((g, m, run_id), {})
                bud     = br.get("budget_min")    or cl.get("budget_min")
                nw      = br.get("n_workers")     or cl.get("n_workers")
                wc      = br.get("wall_clock_sec") or cl.get("wall_clock_sec")
                seeds   = cl.get("seeds", [])
                cfg = []
                if bud is not None: cfg.append(f"{bud}m budget")
                if nw  is not None: cfg.append(f"{nw}w")
                if seeds:           cfg.append(f"seed {seeds[0]}")
                if wc  is not None: cfg.append(f"⏱ {wc}s")
                if run_id:          cfg.append(f"run {run_id}")
                cfg_html = (
                    f"<br><small class='cfg'>{' &nbsp;·&nbsp; '.join(cfg)}</small>"
                    if cfg else "")
                cells += f"<td{td_cls}>{kstr}{cfg_html}</td><td>{tx or '—'}</td>"
        tbody.append(f"<tr>{cells}</tr>")

    # Per-method runtime & config summary — aggregated across all runs.
    method_stats = {m: {"combos": 0, "wall_sum": 0.0, "budgets": set(),
                        "workers": set(), "graphs": set()} for m in methods}
    for run in history.get("runs", []):
        for c in run.get("combos", []):
            m = c.get("method")
            if m not in method_stats:
                continue
            st = method_stats[m]
            st["combos"] += 1
            wc = c.get("wall_clock_sec")
            if isinstance(wc, (int, float)):
                st["wall_sum"] += wc
            if c.get("budget_min") is not None:
                st["budgets"].add(c["budget_min"])
            if c.get("n_workers") is not None:
                st["workers"].add(c["n_workers"])
            st["graphs"].add(c.get("graph"))

    def _fmt_set(s, suffix=""):
        if not s:
            return "—"
        vals = sorted(s)
        if len(vals) == 1:
            return f"{vals[0]}{suffix}"
        return f"{vals[0]}–{vals[-1]}{suffix}"

    def _fmt_dur(sec):
        sec = int(sec)
        h, rem = divmod(sec, 3600)
        m_, s_ = divmod(rem, 60)
        if h:
            return f"{h}h {m_}m"
        if m_:
            return f"{m_}m {s_}s"
        return f"{s_}s"

    method_summary_rows = []
    for m in methods:
        st = method_stats[m]
        if st["combos"] == 0:
            continue
        avg_wall = st["wall_sum"] / st["combos"] if st["combos"] else 0
        color = METHOD_COLORS.get(m, "#333")
        method_summary_rows.append(
            f"<tr>"
            f"<td style='color:{color};font-weight:700'>{m.upper()}</td>"
            f"<td>{_fmt_set(st['budgets'], ' min')}</td>"
            f"<td>{_fmt_set(st['workers'])}</td>"
            f"<td>{st['combos']}</td>"
            f"<td>{len(st['graphs'])}</td>"
            f"<td>{_fmt_dur(avg_wall)}</td>"
            f"<td>{_fmt_dur(st['wall_sum'])}</td>"
            f"</tr>")
    if method_summary_rows:
        method_summary_html = (
            "<h2>Method Runtime &amp; Config</h2>\n"
            "<div class='card'><table>"
            "<thead><tr><th>Method</th><th>Budget / combo</th><th>Workers</th>"
            "<th>Combos run</th><th>Graphs</th><th>Avg wall / combo</th>"
            "<th>Total wall</th></tr></thead><tbody>"
            + "\n".join(method_summary_rows)
            + "</tbody></table></div>")
    else:
        method_summary_html = ""

    # Run history section — detailed combo log.
    history_html_parts = []
    for run in reversed(history.get("runs", [])):
        inner_rows = []
        combos_by_graph = {}
        for c in run.get("combos", []):
            combos_by_graph.setdefault(c["graph"], {})[c["method"]] = c
        for g in sorted(combos_by_graph, key=lambda s: (len(s), s)):
            cells = f"<td>{g}</td>"
            for m in methods:
                c = combos_by_graph.get(g, {}).get(m)
                if c and isinstance(c.get("best_k"), int):
                    flag   = "" if c.get("best_valid") else " ⚠"
                    imp    = c.get("improvement_pct")
                    imp_s  = f" <small>({imp:+.1f}%)</small>" if imp is not None else ""
                    wk_s   = f"<br><small>⏱ {c.get('wall_clock_sec','?')}s | {c.get('n_workers','?')}w</small>"
                    cells += f"<td>{c['best_k']}{flag}{imp_s}{wk_s}</td>"
                else:
                    cells += "<td>—</td>"
            inner_rows.append(f"<tr>{cells}</tr>")

        inner = (f"<table class='inner'><thead><tr><th>Graph</th>"
                 + "".join(f"<th>{m.upper()}</th>" for m in methods)
                 + "</tr></thead><tbody>"
                 + "\n".join(inner_rows) + "</tbody></table>")
        n_combos  = len(run.get("combos", []))
        n_workers = run.get("workers", 1)
        mm        = run.get("minutes_map", {})
        mm_parts  = ([f"S={mm['small']}m"]  if "small"  in mm else []) + \
                    ([f"M={mm['medium']}m"] if "medium" in mm else []) + \
                    ([f"L={mm['large']}m"]  if "large"  in mm else [])
        mm_s      = f" &nbsp;·&nbsp; budget [{', '.join(mm_parts)}]" if mm_parts else ""
        rc_seeds  = run["combos"][0].get("seeds", []) if run.get("combos") else []
        seed_s    = f" &nbsp;·&nbsp; seed {rc_seeds[0]}" if rc_seeds else ""
        history_html_parts.append(f"""<details>
          <summary>Run {run.get('id','?')} &nbsp;|&nbsp; {run.get('timestamp','?')}
            &nbsp;|&nbsp; workers={n_workers} &nbsp;|&nbsp;
            methods=[{', '.join(run.get('methods',[]))}]{mm_s}{seed_s}
            &nbsp;|&nbsp; {n_combos} combos</summary>
          {inner}
        </details>""")

    history_html = "\n".join(history_html_parts)

    legend = (
        '<span><span class="dot" style="background:#999"></span>Baseline</span>'
        + "".join(
            f'<span><span class="dot" style="background:{METHOD_COLORS.get(m, "#555")}"></span>{m.upper()}</span>'
            for m in methods))

    # --- progress section ---
    progress_html = ""
    start_ts_ms   = 0
    report_gen_ms = 0
    remaining_min = 0
    if history.get("runs"):
        latest      = history["runs"][-1]
        p_graphs    = latest.get("graphs", [])
        p_methods   = latest.get("methods", [])
        total       = len(p_graphs) * len(p_methods)
        done_combos = latest.get("combos", [])
        done        = len(done_combos)
        minutes_map = latest.get("minutes_map", {})

        graph_budget = {c["graph"]: c["budget_min"] for c in done_combos}
        done_set     = {(c["graph"], c["method"]) for c in done_combos}

        def _budget(g):
            if g in graph_budget:
                return graph_budget[g]
            try:
                idx = int(g.split("-")[-1])
                return minutes_map.get(group_for(idx), 10)
            except Exception:
                return 10

        remaining_min = sum(
            _budget(g) for g in p_graphs for m in p_methods
            if (g, m) not in done_set
        )
        next_combo = next(
            ((g, m) for g in p_graphs for m in p_methods if (g, m) not in done_set),
            None
        )

        try:
            start_dt    = datetime.strptime(latest["id"], "%Y-%m-%d_%H-%M-%S")
            elapsed_sec = (datetime.now() - start_dt).total_seconds()
            h, rem      = divmod(int(elapsed_sec), 3600)
            m_val       = rem // 60
            elapsed_str = (f"{h}h {m_val}m" if h else f"{m_val}m {rem % 60}s")
            eta_dt      = datetime.now() + timedelta(seconds=remaining_min * 60)
            eta_str     = eta_dt.strftime("%H:%M")
            start_ts_ms    = int(start_dt.timestamp() * 1000)
            report_gen_ms  = int(datetime.now().timestamp() * 1000)
        except Exception:
            elapsed_str   = "?"
            eta_str       = "?"
            start_ts_ms   = 0
            report_gen_ms = 0

        pct      = done / total * 100 if total else 0
        next_str = (f"{next_combo[0]} &nbsp;/&nbsp; <em>{next_combo[1].upper()}</em>"
                    if next_combo else "all combos complete")

        try:
            run_started = datetime.fromisoformat(latest["timestamp"]).strftime("%Y-%m-%d %H:%M:%S")
        except Exception:
            run_started = latest.get("timestamp", "?")
        run_workers  = latest.get("workers", "?")
        run_methods  = ", ".join(m.upper() for m in latest.get("methods", []))
        run_graphs   = ", ".join(g.split("-")[-1] for g in p_graphs)

        last_combo = done_combos[-1] if done_combos else None
        if last_combo:
            lk   = last_combo.get("best_k", "—")
            ltx  = last_combo.get("best_totalX", "—")
            limp = last_combo.get("improvement_pct")
            limp_s = f" <span class='imp'>({limp:+.1f}%)</span>" if limp is not None else ""
            lflag  = " <span class='warn'>⚠ invalid</span>" if not last_combo.get("best_valid") else ""
            last_str = (f"{last_combo['graph']} &nbsp;/&nbsp;"
                        f" <em>{last_combo['method'].upper()}</em>"
                        f" &nbsp;→&nbsp; k={lk}, totalX={ltx}{limp_s}{lflag}")
        else:
            last_str = "—"

        progress_html = f"""
<h2>Run Progress</h2>
<div class="card prog-card">
  <div class="prog-header">
    <div>
      <span class="prog-run-id">Run {latest.get("id", "?")}</span>
      <span class="prog-started">started {run_started}</span>
    </div>
    <div class="prog-meta">{run_workers} workers &nbsp;·&nbsp; graphs [{run_graphs}] &nbsp;·&nbsp; {run_methods}</div>
  </div>
  <div class="prog-stats">
    <div><span class="prog-stat-label">Combos Done</span><span class="prog-stat-value">{done} / {total}</span></div>
    <div><span class="prog-stat-label">Progress</span><span class="prog-stat-value">{pct:.1f}%</span></div>
    <div><span class="prog-stat-label">Elapsed</span><span class="prog-stat-value" id="elapsed">{elapsed_str}</span></div>
    <div><span class="prog-stat-label">Remaining</span><span class="prog-stat-value" id="remaining">~{remaining_min} min</span></div>
    <div><span class="prog-stat-label">ETA</span><span class="prog-stat-value eta" id="eta">{eta_str}</span></div>
  </div>
  <div class="progress-bar-outer">
    <div class="progress-bar-inner" style="width:{pct:.1f}%">
      <span class="progress-bar-label">{pct:.1f}%</span>
    </div>
  </div>
  <div class="prog-current">
    <div class="prog-row"><span class="prog-row-label">Last completed</span><span class="prog-row-val">{last_str}</span></div>
    <div class="prog-row"><span class="prog-row-label">Currently running</span><span class="prog-row-val running">{next_str}</span></div>
  </div>
</div>"""

    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    n_runs = len(history.get("runs", []))

    refresh_sec = 5 if live_status is not None else 30
    live_methods = methods if live_status is not None else []
    live_html = _live_status_html(live_status, live_methods) if live_status else ""

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta http-equiv="refresh" content="{refresh_sec}">
<title>GD-2025 k-planarity Results</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4/dist/chart.umd.min.js"></script>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700;800&display=swap" rel="stylesheet">
<style>
  :root {{
    --bg:#f1f3f6; --card:#ffffff; --border:#e2e6ec;
    --text:#0f172a; --muted:#64748b; --th-bg:#f8fafc;
    --row-alt:#fafbfc; --row-hover:#eef2ff;
    --best:#15803d; --accent:#4f46e5;
  }}
  * {{ box-sizing:border-box; margin:0; padding:0; }}
  body {{
    background:var(--bg); color:var(--text);
    font-family:'Inter',system-ui,-apple-system,sans-serif;
    padding:28px 36px; max-width:1380px; margin:0 auto; line-height:1.5;
  }}

  /* ── page header ── */
  .page-header {{ margin-bottom:20px; padding-bottom:14px;
                  border-bottom:2px solid var(--border); }}
  .page-header h1 {{ font-size:1.55rem; font-weight:800; color:var(--text);
                     letter-spacing:-.025em; display:flex; align-items:center; gap:10px; }}
  .page-header h1 .badge {{
    display:inline-block; background:var(--accent); color:#fff;
    font-size:.6rem; font-weight:700; letter-spacing:.1em; text-transform:uppercase;
    padding:3px 8px; border-radius:4px; vertical-align:middle; margin-top:-2px;
  }}
  .sub {{ color:var(--muted); font-size:.8rem; margin-top:5px; display:flex;
          gap:12px; flex-wrap:wrap; align-items:center; }}
  .sub-dot {{ color:var(--border); }}

  /* ── section headings ── */
  h2 {{
    font-size:.67rem; font-weight:800; letter-spacing:.13em; text-transform:uppercase;
    color:var(--muted); padding-bottom:7px; margin:36px 0 14px;
    border-bottom:2px solid var(--border); display:flex; align-items:center; gap:8px;
  }}
  h2::before {{ content:''; display:inline-block; width:3px; height:.9em;
                background:var(--accent); border-radius:2px; flex-shrink:0; }}

  /* ── cards ── */
  .card {{
    background:var(--card); border:1px solid var(--border);
    border-radius:10px; padding:20px; margin-bottom:16px;
    box-shadow:0 1px 3px rgba(15,23,42,.06),0 1px 2px rgba(15,23,42,.04);
  }}

  /* ── charts ── */
  .charts {{ display:grid; grid-template-columns:1fr 1fr; gap:16px; }}
  @media(max-width:900px){{ .charts{{ grid-template-columns:1fr; }} }}
  canvas {{ max-height:300px; }}

  /* ── tables ── */
  table {{ width:100%; border-collapse:collapse; font-size:.82rem; }}
  th,td {{ padding:9px 14px; text-align:right; border-bottom:1px solid var(--border); }}
  th {{
    background:var(--th-bg); color:var(--muted); text-align:center;
    font-size:.65rem; font-weight:700; letter-spacing:.09em; text-transform:uppercase;
    border-bottom:2px solid var(--border); white-space:nowrap;
  }}
  td:first-child,th:first-child {{ text-align:left; }}
  tbody tr:nth-child(even) td {{ background:var(--row-alt); }}
  tbody tr:hover td {{ background:var(--row-hover); }}
  strong {{ color:var(--best); font-weight:700; }}

  /* ── run history details ── */
  details {{
    background:var(--card); border:1px solid var(--border);
    border-radius:8px; padding:12px 16px; margin-bottom:6px;
    box-shadow:0 1px 2px rgba(15,23,42,.04);
    transition:box-shadow .15s;
  }}
  details[open] {{ box-shadow:0 2px 8px rgba(15,23,42,.08); }}
  summary {{
    cursor:pointer; font-weight:600; color:var(--text);
    font-size:.85rem; user-select:none;
    display:flex; align-items:center; gap:8px;
  }}
  summary::marker {{ color:var(--muted); }}
  summary:hover {{ color:var(--accent); }}
  .inner {{ margin-top:10px; font-size:.77rem; }}
  .inner td {{ padding:4px 10px; }}

  /* ── legend ── */
  .legend {{ display:flex; gap:12px; flex-wrap:wrap; margin-bottom:18px;
             font-size:.8rem; color:var(--muted); align-items:center; }}
  .dot {{ width:10px; height:10px; border-radius:3px; display:inline-block; margin-right:4px; }}

  /* ── progress bar ── */
  .progress-bar-outer {{ background:#dde1e9; height:18px; margin:14px 0;
                          border-radius:99px; overflow:hidden; }}
  .progress-bar-inner {{ height:100%; background:linear-gradient(90deg,var(--accent),#a855f7);
                          border-radius:99px; position:relative; min-width:4px; transition:width .5s; }}
  .progress-bar-label {{ position:absolute; right:8px; top:50%; transform:translateY(-50%);
                          font-size:.68rem; font-weight:700; color:#fff;
                          text-shadow:0 1px 2px rgba(0,0,0,.35); white-space:nowrap; }}

  /* ── prog card legacy classes (used by JS) ── */
  .prog-card {{ border-left:4px solid var(--accent); }}
  .prog-header {{ display:flex; justify-content:space-between; align-items:baseline;
                  flex-wrap:wrap; gap:6px; padding-bottom:12px; margin-bottom:14px;
                  border-bottom:1px solid var(--border); }}
  .prog-run-id {{ font-weight:700; font-size:.95rem; color:var(--text); margin-right:10px; }}
  .prog-started {{ font-size:.8rem; color:var(--muted); }}
  .prog-meta    {{ font-size:.78rem; color:var(--muted); }}
  .prog-stats   {{ display:flex; gap:28px; flex-wrap:wrap; margin-bottom:4px; }}
  .prog-stat-label {{ color:var(--muted); font-size:.68rem; text-transform:uppercase;
                      letter-spacing:.07em; font-weight:700; display:block; margin-bottom:3px; }}
  .prog-stat-value {{ font-size:.92rem; font-weight:600; color:var(--text); }}
  .prog-stat-value.eta {{ color:var(--accent); }}
  .prog-current {{ margin-top:6px; display:flex; flex-direction:column; gap:5px; }}
  .prog-row {{ display:flex; align-items:baseline; gap:8px; font-size:.82rem; }}
  .prog-row-label {{ color:var(--muted); font-size:.68rem; text-transform:uppercase;
                     letter-spacing:.06em; font-weight:700; min-width:130px; flex-shrink:0; }}
  .prog-row-val {{ color:#334155; }}
  .prog-row-val.running {{ color:var(--accent); font-weight:600; }}

  /* ── misc ── */
  .imp  {{ color:#16a34a; font-size:.78rem; }}
  .warn {{ color:#dc2626; font-size:.78rem; }}
  .cfg  {{ color:#94a3b8; font-size:.68rem; font-weight:400; font-style:normal; }}
  .best-cell {{ background:#f0fdf4 !important; }}
  .ls-wait {{ color:#cbd5e1; font-size:.77rem; }}
  .ls-ph1  {{ color:#d97706; font-weight:600; font-size:.77rem; }}
  .ls-ph2  {{ color:#2563eb; font-weight:600; font-size:.77rem; }}
  .ls-ils  {{ color:#7c3aed; font-weight:600; font-size:.77rem; }}
  .ls-lns  {{ color:#0891b2; font-weight:600; font-size:.77rem; }}
  .ls-done {{ color:#16a34a; font-weight:700; font-size:.77rem; }}
</style>
</head>
<body>
<div class="page-header">
  <h1>GD-2025 k-planarity <span class="badge">Results</span></h1>
  <div class="sub">
    <span>Generated {now}</span>
    <span class="sub-dot">·</span>
    <span>{len(graphs)} graphs</span>
    <span class="sub-dot">·</span>
    <span>{n_runs} runs</span>
  </div>
</div>

<div class="legend">{legend}</div>
<!-- PROGRESS_START -->
{progress_html}
<!-- PROGRESS_END -->
<!-- LIVE_STATUS_START -->
{live_html}
<!-- LIVE_STATUS_END -->
<h2>Best Results (All Runs)</h2>
<div class="card">
<table>
  <thead>
    <tr><th>Graph</th><th>Nodes</th><th>Edges</th><th>Baseline k</th>{mh}</tr>
    <tr><th colspan="4"></th>{ms}</tr>
  </thead>
  <tbody>{"".join(tbody)}</tbody>
</table>
</div>

{method_summary_html}

<h2>k-value Comparison</h2>
<div class="charts">
  <div class="card"><canvas id="kChart"></canvas></div>
  <div class="card"><canvas id="improvChart"></canvas></div>
</div>

<h2>Run History</h2>
{history_html}

<script>
const GL = {json.dumps(graphs)};
new Chart(document.getElementById('kChart'), {{
  type:'bar',
  data:{{ labels:GL, datasets:[{",".join(datasets_js_parts)}] }},
  options:{{ responsive:true,
    plugins:{{ legend:{{labels:{{color:'#333'}}}},
              title:{{display:true,text:'k-value (lower is better)',color:'#444'}} }},
    scales:{{
      x:{{ticks:{{color:'#444'}},grid:{{color:'#e0e0e0'}}}},
      y:{{type:'logarithmic',ticks:{{color:'#444'}},grid:{{color:'#e0e0e0'}},
          title:{{display:true,text:'k (log scale)',color:'#444'}}}}
    }}
  }}
}});
new Chart(document.getElementById('improvChart'), {{
  type:'bar',
  data:{{ labels:GL, datasets:[{",".join(improv_parts)}] }},
  options:{{ responsive:true,
    plugins:{{ legend:{{labels:{{color:'#333'}}}},
              title:{{display:true,text:'Reduction from baseline (%)',color:'#444'}} }},
    scales:{{
      x:{{ticks:{{color:'#444'}},grid:{{color:'#e0e0e0'}}}},
      y:{{ticks:{{color:'#444',callback:v=>v+'%'}},grid:{{color:'#e0e0e0'}},
          title:{{display:true,text:'% improvement',color:'#444'}}}}
    }}
  }}
}});
(function(){{
  const START_MS      = {start_ts_ms};
  const REPORT_MS     = {report_gen_ms};
  const REMAIN_AT_GEN = {remaining_min} * 60;
  if (!START_MS) return;
  function fmt(sec) {{
    sec = Math.max(0, Math.floor(sec));
    const h = Math.floor(sec / 3600), r = sec % 3600;
    const m = Math.floor(r / 60), s = r % 60;
    if (h > 0) return h + 'h ' + m + 'm';
    return m + 'm ' + String(s).padStart(2,'0') + 's';
  }}
  function tick() {{
    const now     = Date.now();
    const elapsed = (now - START_MS) / 1000;
    const left    = Math.max(0, REMAIN_AT_GEN - (now - REPORT_MS) / 1000);
    const etaDt   = new Date(now + left * 1000);
    const etaStr  = String(etaDt.getHours()).padStart(2,'0') + ':' + String(etaDt.getMinutes()).padStart(2,'0');
    const el = document.getElementById('elapsed');
    const rm = document.getElementById('remaining');
    const et = document.getElementById('eta');
    if (el) el.textContent = fmt(elapsed);
    if (rm) rm.textContent = '~' + Math.ceil(left / 60) + ' min';
    if (et) et.textContent = etaStr;
  }}
  tick();
  setInterval(tick, 1000);
}})();
</script>
</body>
</html>"""

    (out_root / "report.html").write_text(html)
    print(f"[report] {out_root / 'report.html'}", flush=True)


# ---------------------------------------------------------------------------
# CSV / Markdown summary
# ---------------------------------------------------------------------------
def write_csv(combos, path):
    cols = ["graph", "nodes", "edges", "baseline_k", "method",
            "n_workers", "budget_min", "wall_clock_sec",
            "best_k", "best_totalX", "best_valid", "improvement_pct",
            "new_best_ever", "seeds"]
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for c in combos:
            w.writerow({col: c.get(col, "") for col in cols})


def write_md(combos, path, methods):
    by_graph = {}
    for c in combos:
        by_graph.setdefault(c["graph"], {})
        by_graph[c["graph"]][c["method"]] = c

    lines = ["# Contest Results\n",
             "Lower **k** is better; ties broken by totalX.\n"]
    hdr = "| Graph | Nodes | Edges | Baseline k |"
    sep = "|---|---|---|---|"
    for m in methods:
        hdr += f" {m} k | {m} totalX | workers | time(s) |"
        sep += "---|---|---|---|"
    lines += [hdr, sep]

    for g in sorted(by_graph, key=lambda s: (len(s), s)):
        meta = next(iter(by_graph[g].values()), {})
        bk_best = min(
            (c["best_k"] for c in by_graph[g].values()
             if isinstance(c.get("best_k"), int)), default=None)
        row = f"| {g} | {meta.get('nodes','?')} | {meta.get('edges','?')} | {meta.get('baseline_k','?')} |"
        for m in methods:
            c = by_graph[g].get(m)
            if not c or not isinstance(c.get("best_k"), int):
                row += " — | — | — | — |"
                continue
            k = c["best_k"]
            kstr = f"**{k}**" if k == bk_best else str(k)
            inv  = "" if c.get("best_valid") else " ⚠️"
            row += (f" {kstr}{inv} | {c.get('best_totalX','—')} |"
                    f" {c.get('n_workers','?')} | {c.get('wall_clock_sec','?')} |")
        lines.append(row)

    path.write_text("\n".join(lines))


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--methods", default="sa,staged,ils,staged-adaptive")
    ap.add_argument("--graphs",  default="1-9")
    ap.add_argument("--workers", type=int, default=2,
                    help="parallel workers per combo (default 2)")
    ap.add_argument("--minutes-small",  type=float, default=5.0,
                    help="budget for graphs 1-4 (default 5 min)")
    ap.add_argument("--minutes-medium", type=float, default=8.0,
                    help="budget for graphs 5,6,7,9 (default 8 min)")
    ap.add_argument("--minutes-large",  type=float, default=15.0,
                    help="budget for graph 8 (default 15 min)")
    ap.add_argument("--staged-lns-frac", type=float, default=0.3)
    ap.add_argument("--p1-frac",         type=float, default=0.2)
    ap.add_argument("--nh-size-cap",     type=int,   default=40)
    ap.add_argument("--nh-cands",        type=int,   default=30)
    ap.add_argument("--ils-perturb",     type=int,   default=0)
    ap.add_argument("--seed",            type=int,   default=42)
    ap.add_argument("--out-dir",         default="results")
    ap.add_argument("--warm-start",      action="store_true")
    ap.add_argument("--report-only",     action="store_true")
    args = ap.parse_args()

    out_root = ROOT / args.out_dir
    out_root.mkdir(parents=True, exist_ok=True)

    minutes_map = {
        "small":  args.minutes_small,
        "medium": args.minutes_medium,
        "large":  args.minutes_large,
    }

    history = load_json(out_root / "history.json", {"runs": []})
    bests   = load_json(out_root / "bests.json",   {})

    if args.report_only:
        generate_report(out_root, history, bests)
        return

    methods = [m.strip() for m in args.methods.split(",") if m.strip()]
    valid_m = {"sa", "staged", "ils", "staged-adaptive"}
    for m in methods:
        if m not in valid_m:
            sys.exit(f"Unknown method '{m}'. Choose from: {', '.join(sorted(valid_m))}")

    indices = parse_graph_spec(args.graphs)
    ensure_binaries()
    generate_report(out_root, history, bests)

    run_id    = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    run_dir   = out_root / "runs" / run_id
    run_dir.mkdir(parents=True, exist_ok=True)

    # Background thread: regenerate report every 5 s with live log data.
    _stop_bg = threading.Event()

    def _bg_report():
        while not _stop_bg.wait(5):
            try:
                live = scan_live_status(run_dir, methods)
                _h   = load_json(out_root / "history.json", {"runs": []})
                _b   = load_json(out_root / "bests.json",   {})
                generate_report(out_root, _h, _b,
                                methods_order=methods, live_status=live)
            except Exception:
                pass

    _bg = threading.Thread(target=_bg_report, daemon=True, name="bg-report")
    _bg.start()

    # Estimate total wall-clock.
    total_est = sum(budget_for(i, minutes_map) for i in indices) * len(methods)
    print(f"[run] id={run_id}")
    print(f"[run] graphs={indices}  methods={methods}  workers={args.workers}")
    print(f"[run] budgets: small={minutes_map['small']}m  "
          f"medium={minutes_map['medium']}m  large={minutes_map['large']}m")
    print(f"[run] estimated wall-clock: {total_est:.0f} min ({total_est/60:.1f}h)\n")

    all_combos = []

    for idx in indices:
        gpath_orig = graph_path(idx)
        if not gpath_orig.exists():
            print(f"[skip] {gpath_orig} not found")
            continue
        gname   = gpath_orig.stem
        n, m_e  = graph_size(gpath_orig)
        budget  = budget_for(idx, minutes_map)
        nh_size = max(3, min(n // 10, args.nh_size_cap))
        out_dir = run_dir / gname
        out_dir.mkdir(parents=True, exist_ok=True)

        if n > 3000:
            print(f"[note] {gname} has {n} nodes — initial setup is heavy; "
                  f"giving {budget} min per combo.")

        print(f"=== {gname}  (n={n}, m={m_e}, budget={budget}min) ===")

        baseline_k = None

        for method in methods:
            # Warm-start: use best-ever layout for this method as input.
            warm_path = None
            if args.warm_start:
                bk_entry = bests.get(best_key(gname, method), {})
                lp = bk_entry.get("layout_path")
                if lp and Path(lp).exists():
                    warm_path = lp
                    print(f"  [{method}] warm-starting from {lp}")

            t_wall_start = time.time()
            workers_out, best_w, wall_total = run_combo(
                method, gpath_orig, budget, args.p1_frac, args.seed,
                out_dir, args.workers, nh_size, args.nh_cands,
                args.staged_lns_frac, args.ils_perturb,
                gpath_warm=warm_path,
            )
            t_wall = time.time() - t_wall_start

            # Collect baseline_k from any worker that parsed it.
            for w in workers_out:
                if w and isinstance(w.get("initial_k"), int) and baseline_k is None:
                    baseline_k = w["initial_k"]

            best_k  = best_w["final_k"]   if best_w else None
            best_tx = best_w["final_totalX"] if best_w else None
            valid   = best_w["valid"]      if best_w else False
            best_out = best_w["out_path"]  if best_w else None

            improv = None
            if isinstance(best_k, int) and isinstance(baseline_k, int) and baseline_k > 0:
                improv = round((baseline_k - best_k) / baseline_k * 100, 1)

            new_best = update_best(
                out_root, bests, gname, method, best_k, best_tx, best_out, run_id,
                budget_min=budget, n_workers=args.workers,
                wall_clock_sec=round(wall_total, 1))

            seeds = [w["seed"] for w in workers_out if w]
            flag  = "" if valid else "  [INVALID]"
            imp_s = f"  ({improv:+.1f}% vs baseline)" if improv is not None else ""
            print(f"  {method:16s}  k={best_k}  totalX={best_tx}"
                  f"  wall={wall_total:.1f}s  workers={len(workers_out)}"
                  f"  seeds={seeds}{flag}{imp_s}"
                  + ("  *** NEW BEST ***" if new_best else ""))

            combo_record = {
                "graph":           gname,
                "nodes":           n,
                "edges":           m_e,
                "method":          method,
                "budget_min":      budget,
                "n_workers":       args.workers,
                "seeds":           seeds,
                "wall_clock_sec":  round(wall_total, 1),
                "baseline_k":      baseline_k,
                "best_k":          best_k,
                "best_totalX":     best_tx,
                "best_valid":      valid,
                "improvement_pct": improv,
                "new_best_ever":   new_best,
                "workers_detail":  workers_out,
            }
            all_combos.append(combo_record)

            # Persist after every combo so report stays live.
            save_json(out_root / "bests.json", bests)
            _h = load_json(out_root / "history.json", {"runs": []})
            found = False
            for entry in _h["runs"]:
                if entry["id"] == run_id:
                    entry["combos"] = all_combos
                    found = True
                    break
            if not found:
                _h["runs"].append({
                    "id":          run_id,
                    "timestamp":   datetime.now().isoformat(timespec="seconds"),
                    "methods":     methods,
                    "workers":     args.workers,
                    "graphs":      [f"Automatic-{i}" for i in indices],
                    "minutes_map": minutes_map,
                    "combos":      all_combos,
                })
            save_json(out_root / "history.json", _h)
            _live = scan_live_status(run_dir, methods)
            generate_report(out_root, _h, bests,
                            methods_order=methods, live_status=_live)

        print()

    # Final summaries.
    write_csv(all_combos, run_dir / "summary.csv")
    write_md(all_combos, run_dir / "summary.md", methods)

    # Detailed JSON log.
    detail = {
        "run_id":    run_id,
        "timestamp": datetime.now().isoformat(timespec="seconds"),
        "config": {
            "methods":       methods,
            "workers":       args.workers,
            "minutes_small": minutes_map["small"],
            "minutes_medium": minutes_map["medium"],
            "minutes_large": minutes_map["large"],
            "seed":          args.seed,
            "warm_start":    args.warm_start,
        },
        "combos": all_combos,
    }
    save_json(run_dir / "detailed.json", detail)

    _stop_bg.set()
    _bg.join(timeout=8)

    print(f"[done] {run_dir}")
    print(f"[done] open {out_root / 'report.html'} in a browser")


if __name__ == "__main__":
    main()
