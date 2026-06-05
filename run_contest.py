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


def update_best(out_root, bests, graph, method, k, totalX, layout_path, run_id):
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
                  "run_id": run_id, "layout_path": str(dest)}
    return True


# ---------------------------------------------------------------------------
# HTML report
# ---------------------------------------------------------------------------
def _js(values):
    return "[" + ", ".join("null" if v is None else str(v) for v in values) + "]"


def generate_report(out_root, history, bests, methods_order=None):
    best_results = {}
    for key, val in bests.items():
        if "__" in key:
            g, m = key.split("__", 1)
            best_results[(g, m)] = val

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
    if not graphs:
        (out_root / "report.html").write_text(
            "<html><body>No results yet.</body></html>")
        return

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
            br = best_results.get((g, m), {})
            mk = br.get("k")
            tx = br.get("totalX", "—")
            if mk is None:
                cells += "<td>—</td><td>—</td>"
            else:
                kstr = f"<strong>{mk}</strong>" if mk == bk_best else str(mk)
                cells += f"<td>{kstr}</td><td>{tx or '—'}</td>"
        tbody.append(f"<tr>{cells}</tr>")

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
        history_html_parts.append(f"""<details>
          <summary>Run {run.get('id','?')} &nbsp;|&nbsp; {run.get('timestamp','?')}
            &nbsp;|&nbsp; workers={n_workers} &nbsp;|&nbsp;
            methods=[{', '.join(run.get('methods',[]))}]
            &nbsp;|&nbsp; {n_combos} combos</summary>
          {inner}
        </details>""")

    history_html = "\n".join(history_html_parts) or "<p>No runs yet.</p>"

    legend = (
        '<span><span class="dot" style="background:#999"></span>Baseline</span>'
        + "".join(
            f'<span><span class="dot" style="background:{METHOD_COLORS.get(m, "#555")}"></span>{m.upper()}</span>'
            for m in methods))

    # --- progress section ---
    progress_html = ""
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
            start_dt    = datetime.fromisoformat(latest["timestamp"])
            elapsed_sec = (datetime.now() - start_dt).total_seconds()
            h, rem      = divmod(int(elapsed_sec), 3600)
            elapsed_str = f"{h}h {rem // 60}m"
            eta_dt      = datetime.now() + timedelta(seconds=remaining_min * 60)
            eta_str     = eta_dt.strftime("%H:%M")
        except Exception:
            elapsed_str = "?"
            eta_str     = "?"

        pct      = done / total * 100 if total else 0
        next_str = (f"{next_combo[0]} &nbsp;/&nbsp; <em>{next_combo[1]}</em>"
                    if next_combo else "all combos complete")

        progress_html = f"""
<h2>Run Progress</h2>
<div class="card">
  <div class="prog-stats">
    <div><span class="prog-stat-label">Combos Done</span><span class="prog-stat-value">{done} / {total}</span></div>
    <div><span class="prog-stat-label">Progress</span><span class="prog-stat-value">{pct:.1f}%</span></div>
    <div><span class="prog-stat-label">Elapsed</span><span class="prog-stat-value">{elapsed_str}</span></div>
    <div><span class="prog-stat-label">Remaining</span><span class="prog-stat-value">~{remaining_min} min</span></div>
    <div><span class="prog-stat-label">ETA</span><span class="prog-stat-value eta">{eta_str}</span></div>
  </div>
  <div class="progress-bar-outer">
    <div class="progress-bar-inner" style="width:{pct:.1f}%"></div>
  </div>
  <div class="prog-next">Currently running: <strong>{next_str}</strong></div>
</div>"""

    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    n_runs = len(history.get("runs", []))

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta http-equiv="refresh" content="30">
<title>GD-2025 k-planarity Results</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4/dist/chart.umd.min.js"></script>
<style>
  :root {{
    --bg:#f0f0f0; --card:#ffffff; --border:#c4c4c4;
    --text:#111111; --muted:#666666; --th-bg:#e4e4e4;
    --row-alt:#f7f7f7; --row-hover:#eef2ff;
    --best:#1a6e2e; --accent:#4361ee;
  }}
  * {{ box-sizing:border-box; margin:0; padding:0; }}
  body {{ background:var(--bg); color:var(--text);
          font-family:'Segoe UI',system-ui,sans-serif;
          padding:32px; max-width:1400px; margin:0 auto; }}
  h1 {{ font-size:1.6rem; font-weight:700; padding-bottom:10px;
        border-bottom:3px solid var(--accent); margin-bottom:6px; }}
  .sub {{ color:var(--muted); font-size:.85rem; margin-bottom:24px; margin-top:4px; }}
  h2 {{ font-size:.76rem; font-weight:700; letter-spacing:.09em; text-transform:uppercase;
        color:var(--muted); border-bottom:1px solid var(--border);
        padding-bottom:4px; margin:24px 0 10px; }}
  .card {{ background:var(--card); border:1px solid var(--border); padding:20px; margin-bottom:16px; }}
  .charts {{ display:grid; grid-template-columns:1fr 1fr; gap:16px; }}
  @media(max-width:900px){{ .charts{{ grid-template-columns:1fr; }} }}
  canvas {{ max-height:300px; }}
  table {{ width:100%; border-collapse:collapse; font-size:.82rem; }}
  th,td {{ padding:6px 10px; border:1px solid var(--border); text-align:right; }}
  th {{ background:var(--th-bg); color:#333; text-align:center; font-weight:700;
        font-size:.74rem; letter-spacing:.05em; text-transform:uppercase; }}
  td:first-child,th:first-child {{ text-align:left; }}
  tr:nth-child(even) {{ background:var(--row-alt); }}
  tr:hover {{ background:var(--row-hover); }}
  strong {{ color:var(--best); font-weight:700; }}
  details {{ background:var(--card); border:1px solid var(--border); padding:10px 14px; margin-bottom:6px; }}
  summary {{ cursor:pointer; font-weight:600; color:#333; font-size:.85rem; }}
  summary:hover {{ color:var(--accent); }}
  .inner {{ margin-top:8px; font-size:.77rem; }}
  .inner td {{ padding:3px 8px; }}
  .legend {{ display:flex; gap:16px; flex-wrap:wrap; margin-bottom:12px; font-size:.82rem; }}
  .dot {{ width:10px; height:10px; display:inline-block; margin-right:5px; }}
  .progress-bar-outer {{ background:#d8d8d8; height:20px; margin:10px 0; }}
  .progress-bar-inner {{ height:100%; background:linear-gradient(90deg,#4361ee,#f72585); }}
  .prog-stats {{ display:flex; gap:32px; flex-wrap:wrap; margin-bottom:8px; }}
  .prog-stat-label {{ color:var(--muted); font-size:.7rem; text-transform:uppercase;
                      letter-spacing:.07em; font-weight:700; display:block; margin-bottom:2px; }}
  .prog-stat-value {{ font-size:.92rem; font-weight:600; color:var(--text); }}
  .prog-stat-value.eta {{ color:var(--accent); }}
  .prog-next {{ font-size:.82rem; color:var(--muted); margin-top:6px; }}
  .prog-next strong {{ color:#333; font-weight:600; }}
</style>
</head>
<body>
<h1>GD-2025 k-planarity Results</h1>
<div class="sub">Generated {now} &nbsp;|&nbsp; {len(graphs)} graphs &nbsp;|&nbsp; {n_runs} runs</div>

<div class="legend">{legend}</div>
{progress_html}
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

    run_id    = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    run_dir   = out_root / "runs" / run_id
    run_dir.mkdir(parents=True, exist_ok=True)

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
                out_root, bests, gname, method, best_k, best_tx, best_out, run_id)

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
            generate_report(out_root, _h, bests, methods_order=methods)

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

    print(f"[done] {run_dir}")
    print(f"[done] open {out_root / 'report.html'} in a browser")


if __name__ == "__main__":
    main()
