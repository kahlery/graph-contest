#!/usr/bin/env python3
"""
GD-2025 Run Control Server.

Usage:
  python3 server.py              # port 8080
  python3 server.py --port 9090

Opens http://localhost:<port> — an enhanced report page with:
  - Config form (methods, graphs, budget, workers, seed)
  - Start / Stop / Restart buttons
  - Live status table polling every 5 s via JS (no meta-refresh)
  - Best Results, charts, run history embedded
"""

VERSION = "1.1.1"

import argparse
import json
import re
import subprocess
import sys
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

ROOT    = Path(__file__).resolve().parent
RESULTS = ROOT / "results"
RUNS    = RESULTS / "runs"

# ── subprocess state ──────────────────────────────────────────────────────────
_lock        = threading.Lock()
_proc        = None
_loop_stop   = threading.Event()
_loop_thread = None
_config = {
    "run_name":       "",
    "mode":           "batch",          # "batch" (run_contest) | "orchestrator"
    "methods":        "sa,sa-stress,ils",
    "graphs":         "1-9",
    "minutes_small":  1.0,
    "minutes_medium": 1.5,
    "minutes_large":  8.0,
    "workers":        1,
    "seed":           42,
    "warm_start":     False,
    "loop":           False,
    "cooldown_sec":   60,
    # orchestrator mode
    "orch_budget_min": 45.0,            # total wall-clock budget (minutes)
    "orch_workers":    8,
    "graphs_dir":      "",              # folder of *.json graphs (overrides graphs)
    "orch_out_dir":    "results_internal",
}

_ORCH_LOG  = ROOT / "results_internal" / "orch_server.log"
_orch_logf = None                       # kept open for the child's stdout


def _alive():
    return _proc is not None and _proc.poll() is None


def _looping():
    return _loop_thread is not None and _loop_thread.is_alive()


def _latest_run_dir():
    if not RUNS.exists():
        return None
    dirs = sorted(d for d in RUNS.iterdir() if d.is_dir())
    return dirs[-1] if dirs else None


def _build_cmd():
    if _config.get("mode") == "orchestrator":
        cmd = [
            sys.executable, str(ROOT / "contest_orchestrate.py"),
            "--budget",  str(round(float(_config["orch_budget_min"]) * 60.0, 1)),
            "--workers", str(int(_config["orch_workers"])),
            "--out-dir", str(_config.get("orch_out_dir") or "results_internal"),
            "--seed",    str(int(_config["seed"])),
        ]
        gd = str(_config.get("graphs_dir", "")).strip()
        cmd += ["--graphs-dir", gd] if gd else ["--graphs", str(_config["graphs"])]
        return cmd
    cmd = [
        sys.executable, str(ROOT / "run_contest.py"),
        "--methods",        str(_config["methods"]),
        "--graphs",         str(_config["graphs"]),
        "--minutes-small",  str(_config["minutes_small"]),
        "--minutes-medium", str(_config["minutes_medium"]),
        "--minutes-large",  str(_config["minutes_large"]),
        "--workers",        str(int(_config["workers"])),
        "--seed",           str(int(_config["seed"])),
    ]
    if _config.get("warm_start"):
        cmd.append("--warm-start")
    if str(_config.get("run_name", "")).strip():
        cmd += ["--run-name", str(_config["run_name"]).strip()]
    return cmd


def _spawn():
    """Launch the configured run. Orchestrator stdout is captured to _ORCH_LOG so
    the UI can stream progress; batch runs stay silent (their live status comes
    from run_contest's own status files)."""
    global _orch_logf
    if _config.get("mode") == "orchestrator":
        _ORCH_LOG.parent.mkdir(parents=True, exist_ok=True)
        _orch_logf = open(_ORCH_LOG, "w")
        return subprocess.Popen(_build_cmd(), cwd=ROOT,
                                stdout=_orch_logf, stderr=subprocess.STDOUT)
    return subprocess.Popen(_build_cmd(), cwd=ROOT,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _loop_worker():
    """Background thread: wait for current run to finish, cooldown, repeat."""
    global _proc
    while not _loop_stop.is_set():
        # Wait for the current process to finish.
        proc = _proc
        if proc is not None:
            proc.wait()
        if _loop_stop.is_set():
            break
        # Cooldown between loops.
        cooldown = int(_config.get("cooldown_sec") or 0)
        if cooldown > 0 and _loop_stop.wait(timeout=cooldown):
            break
        if _loop_stop.is_set():
            break
        # Start the next run.
        with _lock:
            if _loop_stop.is_set():
                break
            _proc = _spawn()


def api_status():
    rd       = _latest_run_dir()
    start_ms = 0
    if rd:
        try:
            from datetime import datetime as _dt
            start_ms = int(_dt.strptime(rd.name, "%Y-%m-%d_%H-%M-%S").timestamp() * 1000)
        except ValueError:
            pass
    return {
        "running":      _alive(),
        "looping":      _looping(),
        "pid":          _proc.pid if _alive() else None,
        "run_dir":      rd.name if rd else None,
        "run_start_ms": start_ms,
        "config":       _config,
    }


def api_methods():
    """Method registry for the control panel — derived from run_contest.METHODS.
    Adding a method there makes it appear here (and as a checkbox) automatically."""
    from run_contest import METHODS
    default_ids = {m.strip() for m in _config["methods"].split(",") if m.strip()}
    return [{"id": m["id"], "label": m["label"], "default": m["id"] in default_ids}
            for m in METHODS]


def api_gda_graphs():
    """List the gda-testing benchmark graphs (grouped by category) for the
    extra-graphs picker. Each entry's value is a --graphs token usable
    directly (relative path under data/gda-testing/graphs)."""
    from run_contest import GDA_DIR, graph_size
    out = {}
    if not GDA_DIR.exists():
        return out
    for f in sorted(GDA_DIR.rglob("*.json")):
        rel = f.relative_to(GDA_DIR)
        cat = rel.parts[0]
        try:
            n, m = graph_size(f)
        except Exception:
            continue
        out.setdefault(cat, []).append({
            "value": str(rel), "label": rel.name, "n": n, "m": m,
        })
    return out


def api_live():
    rd = _latest_run_dir()
    if not rd:
        return {}
    methods = [m.strip() for m in _config["methods"].split(",") if m.strip()]
    from run_contest import scan_live_status
    raw = scan_live_status(rd, methods)
    return {f"{g}__{m}": v for (g, m), v in raw.items()}


def api_start(body: dict):
    global _proc, _config, _loop_thread
    with _lock:
        if _alive():
            return {"ok": False, "error": "A run is already active — stop it first."}
        _config.update({k: v for k, v in body.items() if k in _config})
        _proc = _spawn()
        pid = _proc.pid
        if _config.get("loop") and _config.get("mode") != "orchestrator":
            _loop_stop.clear()
            _loop_thread = threading.Thread(target=_loop_worker, daemon=True, name="loop")
            _loop_thread.start()
        return {"ok": True, "pid": pid}


def api_stop():
    global _proc
    _loop_stop.set()  # cancel loop first
    with _lock:
        if _alive():
            _proc.terminate()
            try:
                _proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                _proc.kill()
            _proc = None
    return {"ok": True}


def api_scan_folder(body: dict):
    """Preview a folder for the orchestrator: list every *.json that parses as a
    graph (has nodes+edges), so the UI can confirm before auto-running."""
    path = str(body.get("path", "")).strip()
    if not path:
        return {"ok": False, "error": "Enter a folder path."}
    p = Path(path).expanduser()
    if not p.exists():
        return {"ok": False, "error": f"Not found: {p}"}
    if not p.is_dir():
        return {"ok": False, "error": f"Not a folder: {p}"}
    from contest_orchestrate import ContestOrchestrator
    try:
        entries = ContestOrchestrator.scan_dir(str(p))
    except Exception as e:
        return {"ok": False, "error": f"Scan failed: {e}"}
    if not entries:
        return {"ok": False, "error": f"No graph .json files in {p}"}
    return {"ok": True, "path": str(p.resolve()), "count": len(entries),
            "graphs": [{"name": nm, "n": n, "m": m} for nm, _pp, n, m in entries]}


def api_orch_status():
    """Orchestrator progress: tail of its stdout log + the latest run's summary."""
    running = _alive() and _config.get("mode") == "orchestrator"
    out = {"running": running, "log": "", "summary": None}
    if _ORCH_LOG.exists():
        try:
            out["log"] = "\n".join(
                _ORCH_LOG.read_text(errors="replace").splitlines()[-60:])
        except Exception:
            pass
    runs = Path(_config.get("orch_out_dir") or "results_internal") / "runs"
    if runs.exists():
        for d in sorted((x for x in runs.iterdir() if x.is_dir()), reverse=True):
            f = d / "orchestration.json"
            if f.exists():
                try:
                    out["summary"] = json.loads(f.read_text())
                except Exception:
                    pass
                break
    return out


# ── injected HTML / CSS / JS ──────────────────────────────────────────────────
_CTRL_CSS = """
  /* ── page nav ── */
  .page-nav { display:flex; gap:4px; flex-wrap:wrap; margin-bottom:28px;
              padding:8px 0; border-bottom:2px solid var(--border,#e2e6ec);
              position:sticky; top:0; z-index:200;
              background:var(--bg,#f1f3f6); box-shadow:0 2px 8px rgba(15,23,42,.07); }
  .page-nav strong { font-size:.78rem; color:var(--text,#0f172a); margin-right:4px;
                     letter-spacing:-.01em; align-self:center; }
  .page-nav a { color:var(--muted,#64748b); text-decoration:none; font-size:.71rem;
                font-weight:700; text-transform:uppercase; letter-spacing:.08em;
                padding:4px 10px; border-radius:5px;
                transition:background .15s,color .15s; }
  .page-nav a:hover { background:var(--row-hover,#eef2ff); color:var(--accent,#4f46e5); }

  /* ── two-column top ── */
  .dash-top { display:grid; grid-template-columns:270px 1fr; gap:16px;
              align-items:start; margin-bottom:20px; }
  @media(max-width:780px){ .dash-top{ grid-template-columns:1fr; } }

  /* ── run control sidebar ── */
  .ctrl-card { background:var(--card); border:1px solid var(--border);
               border-radius:10px; border-left:4px solid var(--accent);
               padding:16px; position:sticky; top:60px;
               box-shadow:0 1px 3px rgba(15,23,42,.06); }
  .ctrl-hdr  { display:flex; justify-content:space-between; align-items:center;
               margin-bottom:14px; padding-bottom:12px; border-bottom:1px solid var(--border); }
  .ctrl-hdr-title { font-size:.9rem; font-weight:700; color:#222; }
  .status-pill { font-size:.72rem; font-weight:700; padding:3px 9px; border-radius:10px; }
  .s-idle     { background:#f0f0f0; color:#777; }
  .s-running  { background:#d1fae5; color:#065f46; }
  .s-stopped  { background:#fee2e2; color:#991b1b; }
  .s-done     { background:#d1fae5; color:#065f46; }

  .cfg-section { margin-bottom:12px; }
  .cfg-label   { font-size:.62rem; font-weight:700; text-transform:uppercase;
                 letter-spacing:.07em; color:#777; margin-bottom:5px; }
  .method-grid { display:grid; grid-template-columns:1fr 1fr; gap:4px 8px; }
  .method-chk  { display:flex; align-items:center; gap:5px; font-size:.81rem;
                 color:#333; cursor:pointer; user-select:none; }
  .method-chk input { accent-color:var(--accent); width:14px; height:14px; cursor:pointer; }
  .cfg-input   { width:100%; padding:5px 8px; border:1px solid #d0d0d0; border-radius:4px;
                 font-size:.83rem; font-family:inherit; color:#222; background:#fff; box-sizing:border-box; }
  .cfg-input:focus { outline:2px solid var(--accent); border-color:var(--accent); }
  .cfg-num  { width:60px; }
  .cfg-row  { display:flex; gap:8px; align-items:center; }
  .cfg-row-lbl { font-size:.75rem; color:#555; font-weight:600; min-width:18px; }
  .cfg-warm-lbl { display:flex; align-items:center; gap:6px; font-size:.82rem;
                  color:#333; cursor:pointer; }
  .cfg-warm-lbl input { accent-color:var(--accent); }

  .ctrl-btns { display:flex; flex-direction:column; gap:7px; margin-top:14px;
               padding-top:12px; border-top:1px solid var(--border); }
  .btn { width:100%; padding:8px 0; border:none; border-radius:5px; cursor:pointer;
         font-size:.84rem; font-weight:700; font-family:inherit; text-align:center;
         transition:filter .15s; }
  .btn:hover:not(:disabled) { filter:brightness(1.1); }
  .btn:disabled { opacity:.4; cursor:not-allowed; }
  .btn-start   { background:#4361ee; color:#fff; }
  .btn-stop    { background:#ef4444; color:#fff; }
  .btn-restart { background:#f59e0b; color:#fff; }
  .ctrl-msg { margin-top:8px; font-size:.78rem; min-height:1.3em;
              padding:3px 6px; border-radius:4px; }
  .ctrl-msg.ok  { background:#d1fae5; color:#065f46; }
  .ctrl-msg.err { background:#fee2e2; color:#991b1b; }

  /* ── progress section ── */
  .prog-stat-row { display:flex; gap:28px; flex-wrap:wrap; margin:14px 0 16px; }
  .prog-stat { display:flex; flex-direction:column; gap:3px; }
  .prog-stat-lbl { font-size:.6rem; font-weight:700; text-transform:uppercase;
                   letter-spacing:.1em; color:var(--muted,#64748b); }
  .prog-stat-val { font-size:1.15rem; font-weight:700; color:var(--text,#0f172a); line-height:1; }
  .prog-stat.hi .prog-stat-val { color:var(--accent,#4f46e5); }
  .prog-info-row { display:flex; align-items:baseline; gap:10px;
                   font-size:.83rem; padding:5px 0; border-top:1px solid var(--border,#e2e6ec); }
  .prog-info-lbl { font-size:.6rem; font-weight:700; text-transform:uppercase;
                   letter-spacing:.09em; color:var(--muted,#64748b); min-width:78px; flex-shrink:0; }
  .prog-inactive { opacity:.55; }

  /* ── live status table ── */
  .ls-cell { padding:6px 10px !important; }
  .ls-wait { color:#bbb; font-size:.77rem; }
  .ls-ph1  { color:#b45309; font-weight:600; font-size:.77rem; }
  .ls-ph2  { color:#1d4ed8; font-weight:600; font-size:.77rem; }
  .ls-ils  { color:#6d28d9; font-weight:600; font-size:.77rem; }
  .ls-lns  { color:#0e7490; font-weight:600; font-size:.77rem; }
  .ls-done { color:#15803d; font-weight:700; font-size:.77rem; }
  .ls-done-cell { background:#f0fdf4 !important; }
  .ls-active-cell { background:#fffbeb !important; }

  /* ── mode toggle (Batch / Orchestrator) ── */
  .mode-toggle { display:flex; gap:0; border:1px solid var(--border,#d0d0d0);
                 border-radius:6px; overflow:hidden; }
  .mode-btn { flex:1; padding:6px 8px; font-size:.78rem; font-weight:700; border:0;
              background:#fff; color:var(--muted,#64748b); cursor:pointer;
              transition:background .15s,color .15s; }
  .mode-btn + .mode-btn { border-left:1px solid var(--border,#d0d0d0); }
  .mode-btn.active { background:var(--accent,#4f46e5); color:#fff; }
  #orch-log { font-family:ui-monospace,SFMono-Regular,Menlo,monospace; font-size:.7rem;
              white-space:pre-wrap; background:#0f172a; color:#cbd5e1; padding:10px;
              border-radius:6px; max-height:260px; overflow:auto; line-height:1.45; }
"""

_CTRL_PANEL = """
<aside class="ctrl-card" id="ctrl-panel">
  <div class="ctrl-hdr">
    <span class="ctrl-hdr-title">Run Control</span>
    <span style="font-size:.72rem;color:#888;font-weight:500;margin-left:6px">v1.1.1</span>
    <span id="ctrl-status" class="status-pill s-idle">○ Idle</span>
  </div>

  <div class="cfg-section">
    <div class="cfg-label">Run name <span style="font-weight:400;color:#888">(optional)</span></div>
    <input id="cfg-run-name" type="text" placeholder="e.g. kband A/B, baseline sweep…"
           style="width:100%;box-sizing:border-box">
  </div>

  <div class="cfg-section">
    <div class="cfg-label">Mode</div>
    <div class="mode-toggle">
      <button type="button" id="mode-batch" class="mode-btn active" onclick="setMode('batch')">Batch</button>
      <button type="button" id="mode-orch"  class="mode-btn"        onclick="setMode('orchestrator')">Orchestrator</button>
    </div>
  </div>

  <!-- ORCHESTRATOR MODE: pick a folder of graphs, auto-run within a wall-clock budget -->
  <div id="orch-fields" style="display:none">
    <div class="cfg-section">
      <div class="cfg-label">Graphs folder</div>
      <input id="cfg-graphs-dir" type="text" class="cfg-input"
             placeholder="/path/to/folder-with-Automatic-*.json"
             value="data/live-2025-contest/live-contest">
      <div style="display:flex;gap:5px;margin-top:5px">
        <button type="button" class="btn" style="flex:1;padding:5px;font-size:.78rem"
                onclick="orchScan()">🔍 Scan</button>
      </div>
      <div id="orch-scan-result" style="font-size:.72rem;color:var(--muted);margin-top:5px"></div>
    </div>
    <div class="cfg-section" style="display:flex;gap:12px;flex-wrap:wrap">
      <div style="flex:1;min-width:90px">
        <div class="cfg-label">Budget (min)</div>
        <input id="cfg-orch-budget" type="number" class="cfg-input cfg-num" value="45" min="1" step="1">
      </div>
      <div style="flex:1;min-width:70px">
        <div class="cfg-label">Workers</div>
        <input id="cfg-orch-workers" type="number" class="cfg-input cfg-num" value="8" min="1" max="32">
      </div>
    </div>
    <div class="cfg-section">
      <button type="button" class="btn btn-start" style="width:100%"
              onclick="orchScanAndRun()">▶ Scan &amp; Run Orchestrator</button>
      <div style="font-size:.7rem;color:var(--muted);margin-top:4px">
        Analyses each graph, spends the budget adaptively (half-sharing), writes one
        best layout per graph to <code>&lt;out&gt;/submission/</code>. Never overruns.
      </div>
    </div>
  </div>

  <!-- BATCH MODE (run_contest.py): explicit methods + per-size budgets -->
  <div id="batch-fields">
  <div class="cfg-section">
    <div class="cfg-label">Methods</div>
    <!-- Checkboxes are rendered from /api/methods (run_contest.METHODS). -->
    <div class="method-grid" id="method-boxes"></div>
  </div>

  <div class="cfg-section">
    <div class="cfg-label">Graphs</div>
    <input id="cfg-graphs" type="text" class="cfg-input" value="1-9" placeholder="1-9 or 1,3,5">
    <div style="display:flex;gap:5px;margin-top:5px">
      <select id="gda-cat" class="cfg-input" style="flex:1"></select>
      <select id="gda-graph" class="cfg-input" style="flex:2"></select>
      <button type="button" class="btn" style="padding:4px 10px;font-size:.78rem"
              onclick="gdaAddGraph()">+ Add</button>
    </div>
    <div style="font-size:.7rem;color:var(--muted);margin-top:3px">
      gda-testing benchmark suite — picks append to the field above (";"-separated)
    </div>
  </div>

  <div class="cfg-section">
    <div class="cfg-label">Budget (min per combo)</div>
    <div style="display:flex;flex-direction:column;gap:5px">
      <div class="cfg-row"><span class="cfg-row-lbl">S</span><input id="cfg-small"  type="number" class="cfg-input cfg-num" value="1"   step="0.5" min="0.1"><span class="cfg-row-lbl" style="color:#aaa;font-size:.7rem">small graphs</span></div>
      <div class="cfg-row"><span class="cfg-row-lbl">M</span><input id="cfg-medium" type="number" class="cfg-input cfg-num" value="1.5" step="0.5" min="0.1"><span class="cfg-row-lbl" style="color:#aaa;font-size:.7rem">medium graphs</span></div>
      <div class="cfg-row"><span class="cfg-row-lbl">L</span><input id="cfg-large"  type="number" class="cfg-input cfg-num" value="8"   step="1"   min="1"  ><span class="cfg-row-lbl" style="color:#aaa;font-size:.7rem">large graphs</span></div>
    </div>
  </div>

  <div class="cfg-section" style="display:flex;gap:12px;flex-wrap:wrap">
    <div style="flex:1;min-width:70px">
      <div class="cfg-label">Workers</div>
      <input id="cfg-workers" type="number" class="cfg-input cfg-num" value="1" min="1" max="32">
    </div>
    <div style="flex:1;min-width:70px">
      <div class="cfg-label">Seed</div>
      <input id="cfg-seed" type="number" class="cfg-input cfg-num" value="42" min="0">
    </div>
  </div>

  <div class="cfg-section" style="display:flex;gap:14px;flex-wrap:wrap;align-items:flex-start">
    <label class="cfg-warm-lbl"><input id="cfg-warm" type="checkbox"> Warm-start</label>
    <label class="cfg-warm-lbl"><input id="cfg-loop" type="checkbox"> Loop</label>
  </div>

  <div class="cfg-section" id="cfg-cooldown-wrap" style="display:none">
    <div class="cfg-label">Cooldown between loops</div>
    <div class="cfg-row">
      <input id="cfg-cooldown" type="number" class="cfg-input cfg-num" value="60" min="0">
      <span style="font-size:.72rem;color:var(--muted)">seconds</span>
    </div>
  </div>
  </div><!-- /batch-fields -->

  <div class="ctrl-btns">
    <button id="btn-start"   class="btn btn-start"   onclick="ctrlStart()">▶ Start Run</button>
    <button id="btn-stop"    class="btn btn-stop"    onclick="ctrlStop()"    disabled>■ Stop Run</button>
    <button id="btn-restart" class="btn btn-restart" onclick="ctrlRestart()" disabled>↺ Restart</button>
  </div>
  <div id="ctrl-msg" class="ctrl-msg"></div>
</aside>
"""

_LIVE_SECTION = """
<div id="live-status-section" style="margin-bottom:20px">
  <h2 id="sec-live">Live Status
    <small id="live-meta" style="font-weight:400;text-transform:none;font-size:.74rem;color:#888"></small>
  </h2>
  <div class="card" id="live-table-wrap" style="overflow-x:auto;padding:12px">
    <em style="color:#aaa">Connecting…</em>
  </div>
  <h2 style="margin-top:14px">Live Best per Graph
    <small style="font-weight:400;text-transform:none;font-size:.74rem;color:#888">— best of all methods &amp; workers, right now</small>
  </h2>
  <div class="card" id="live-best-wrap" style="overflow-x:auto;padding:12px">
    <em style="color:#aaa">Connecting…</em>
  </div>
</div>
"""

_CTRL_JS = r"""
(function () {
  const POLL_MS = 5000;
  let pollTimer, _firstPoll = true;

  // Populated from /api/methods on load (source of truth: run_contest.METHODS).
  let METHOD_IDS = [];

  function renderMethodBoxes(list) {
    METHOD_IDS = list.map(m => m.id);
    const box = document.getElementById('method-boxes');
    if (!box) return;
    box.innerHTML = list.map(m =>
      `<label class="method-chk"><input type="checkbox" id="m-${m.id}"`
      + `${m.default ? ' checked' : ''}><span>${m.label}</span></label>`
    ).join('');
  }

  async function loadMethods() {
    try {
      const r = await fetch('/api/methods');
      renderMethodBoxes(await r.json());
    } catch (e) { console.warn('methods load failed', e); }
  }

  // ── gda-testing benchmark graph picker ──────────────────────────────────
  let GDA_GRAPHS = {};

  function renderGdaGraph() {
    const cat = document.getElementById('gda-cat')?.value;
    const sel = document.getElementById('gda-graph');
    if (!sel || !cat) return;
    sel.innerHTML = (GDA_GRAPHS[cat] || []).map(g =>
      `<option value="${g.value}">${g.label} (n=${g.n}, m=${g.m})</option>`
    ).join('');
  }

  async function loadGdaGraphs() {
    try {
      const r = await fetch('/api/gda-graphs');
      GDA_GRAPHS = await r.json();
      const catSel = document.getElementById('gda-cat');
      if (!catSel) return;
      catSel.innerHTML = Object.keys(GDA_GRAPHS).sort().map(c =>
        `<option value="${c}">${c}</option>`
      ).join('');
      catSel.onchange = renderGdaGraph;
      renderGdaGraph();
    } catch (e) { console.warn('gda-graphs load failed', e); }
  }

  window.gdaAddGraph = function () {
    const val = document.getElementById('gda-graph')?.value;
    if (!val) return;
    const field = document.getElementById('cfg-graphs');
    const cur   = field.value.trim();
    field.value = cur ? `${cur};${val}` : val;
  };

  // ── config helpers ─────────────────────────────────────────────────────────
  function readCfg() {
    const methods = METHOD_IDS.filter(m =>
      document.getElementById('m-' + m)?.checked
    ).join(',');
    return {
      methods,
      mode:            CUR_MODE,
      run_name:       document.getElementById('cfg-run-name').value.trim(),
      graphs:         document.getElementById('cfg-graphs').value.trim(),
      minutes_small:  parseFloat(document.getElementById('cfg-small').value),
      minutes_medium: parseFloat(document.getElementById('cfg-medium').value),
      minutes_large:  parseFloat(document.getElementById('cfg-large').value),
      workers:        parseInt(document.getElementById('cfg-workers').value),
      seed:           parseInt(document.getElementById('cfg-seed').value),
      warm_start:     document.getElementById('cfg-warm').checked,
      loop:           document.getElementById('cfg-loop').checked,
      cooldown_sec:   parseInt(document.getElementById('cfg-cooldown').value) || 60,
      graphs_dir:      document.getElementById('cfg-graphs-dir').value.trim(),
      orch_budget_min: parseFloat(document.getElementById('cfg-orch-budget').value),
      orch_workers:    parseInt(document.getElementById('cfg-orch-workers').value),
    };
  }

  // ── mode toggle (Batch vs Orchestrator) ─────────────────────────────────────
  let CUR_MODE = 'batch';
  window.setMode = function (mode) {
    CUR_MODE = mode;
    document.getElementById('batch-fields').style.display = mode === 'batch' ? '' : 'none';
    document.getElementById('orch-fields').style.display  = mode === 'batch' ? 'none' : '';
    document.getElementById('mode-batch').classList.toggle('active', mode === 'batch');
    document.getElementById('mode-orch').classList.toggle('active', mode !== 'batch');
    // the shared Start button drives batch; the orchestrator has its own Scan & Run
    document.getElementById('btn-start').style.display = mode === 'batch' ? '' : 'none';
  };

  async function orchScan() {
    const path = document.getElementById('cfg-graphs-dir').value.trim();
    const el = document.getElementById('orch-scan-result');
    el.style.color = 'var(--muted)';
    el.textContent = 'Scanning…';
    try {
      const r = await fetch('/api/scan-folder', {
        method: 'POST', headers: {'Content-Type':'application/json'},
        body: JSON.stringify({ path }),
      });
      const d = await r.json();
      if (!d.ok) { el.style.color = '#b91c1c'; el.textContent = '✕ ' + d.error; return null; }
      el.style.color = '#15803d';
      const names = d.graphs.map(g => `${g.name} (n=${g.n},m=${g.m})`).join(', ');
      el.textContent = `✓ ${d.count} graph${d.count>1?'s':''}: ${names}`;
      return d;
    } catch (e) {
      el.style.color = '#b91c1c'; el.textContent = '✕ scan failed'; return null;
    }
  }
  window.orchScan = orchScan;

  window.orchScanAndRun = async function () {
    const d = await orchScan();
    if (!d) { setMsg('Fix the folder path first.', true); return; }
    setMode('orchestrator');
    setMsg(`Starting orchestrator on ${d.count} graphs…`, false);
    const res = await fetch('/api/start', {
      method: 'POST', headers: {'Content-Type':'application/json'},
      body: JSON.stringify(readCfg()),
    });
    const data = await res.json();
    if (!data.ok) { setMsg(data.error || 'Error', true); return; }
    const bud = document.getElementById('cfg-orch-budget').value;
    setMsg(`Orchestrator running (PID ${data.pid}) — budget ${bud} min`, false);
    clearTimeout(pollTimer); poll();
  };

  function syncCfg(c) {
    if (!c) return;
    if (c.run_name != null) document.getElementById('cfg-run-name').value = c.run_name;
    if (c.methods != null) {
      const active = c.methods.split(',').map(s => s.trim());
      METHOD_IDS.forEach(m => {
        const el = document.getElementById('m-' + m);
        if (el) el.checked = active.includes(m);
      });
    }
    if (c.graphs         != null) document.getElementById('cfg-graphs').value    = c.graphs;
    if (c.minutes_small  != null) document.getElementById('cfg-small').value     = c.minutes_small;
    if (c.minutes_medium != null) document.getElementById('cfg-medium').value    = c.minutes_medium;
    if (c.minutes_large  != null) document.getElementById('cfg-large').value     = c.minutes_large;
    if (c.workers        != null) document.getElementById('cfg-workers').value   = c.workers;
    if (c.seed           != null) document.getElementById('cfg-seed').value      = c.seed;
    if (c.warm_start     != null) document.getElementById('cfg-warm').checked    = c.warm_start;
    if (c.loop           != null) { document.getElementById('cfg-loop').checked  = c.loop; toggleCooldown(); }
    if (c.cooldown_sec   != null) document.getElementById('cfg-cooldown').value  = c.cooldown_sec;
    if (c.graphs_dir     != null && c.graphs_dir) document.getElementById('cfg-graphs-dir').value = c.graphs_dir;
    if (c.orch_budget_min!= null) document.getElementById('cfg-orch-budget').value  = c.orch_budget_min;
    if (c.orch_workers   != null) document.getElementById('cfg-orch-workers').value = c.orch_workers;
    if (c.mode           != null) setMode(c.mode);
  }

  function escapeHtml(s) {
    return (s || '').replace(/[&<>]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
  }

  // ── orchestrator progress (log tail + per-graph summary) ────────────────────
  async function renderOrch(status) {
    const el = document.getElementById('run-progress-section');
    if (!el) return;
    let d = {};
    try { d = await (await fetch('/api/orch-status')).json(); } catch (e) {}
    const running = status.running;
    const pill = running
      ? '<span class="status-pill s-running">● Running</span>'
      : (d.summary ? '<span class="status-pill s-done">✓ Completed</span>'
                   : '<span class="status-pill s-idle">○ Idle</span>');
    let body = '';
    if (d.summary && d.summary.summary) {
      const s = d.summary.summary;
      const ks = Object.values(s).map(x => x.k).filter(x => x != null);
      body += `<div style="font-size:.82rem;margin-bottom:8px">worst-k=<b>${ks.length?Math.max(...ks):'—'}</b>`
            + ` &middot; sum-k=<b>${ks.length?ks.reduce((a,b)=>a+b,0):'—'}</b>`
            + ` &middot; wall ${Math.round(d.summary.wall_sec)}/${Math.round(d.summary.budget_sec)}s</div>`;
      body += '<table style="width:100%;border-collapse:collapse;font-size:.78rem">'
            + '<tr style="text-align:left;color:#888"><th>graph</th><th>k</th><th>totalX</th><th>leases</th><th>valid</th></tr>';
      for (const [g, v] of Object.entries(s).sort()) {
        body += `<tr><td style="font-weight:600">${g}</td><td>${v.k}</td>`
              + `<td>${v.totalX ?? '—'}</td><td>${v.leases}</td>`
              + `<td>${v.valid ? '✓' : '<span style="color:#b91c1c">✕</span>'}</td></tr>`;
      }
      body += '</table>';
    }
    el.innerHTML = `<h2 id="sec-progress">Orchestrator &nbsp;${pill}</h2>`
      + `<div class="card">${body || '<em style="color:#aaa">Waiting for the first lease…</em>'}`
      + `<div style="margin-top:10px"><div class="cfg-label">Live log</div>`
      + `<div id="orch-log">${escapeHtml(d.log || '(no output yet)')}</div></div></div>`;
    const log = document.getElementById('orch-log');
    if (log) log.scrollTop = log.scrollHeight;
  }

  function toggleCooldown() {
    const on = document.getElementById('cfg-loop').checked;
    document.getElementById('cfg-cooldown-wrap').style.display = on ? '' : 'none';
  }
  document.getElementById('cfg-loop').addEventListener('change', toggleCooldown);

  function setMsg(msg, isErr) {
    const el = document.getElementById('ctrl-msg');
    el.className = 'ctrl-msg' + (msg ? (isErr ? ' err' : ' ok') : '');
    el.textContent = msg;
  }

  // ── sidebar status pill ────────────────────────────────────────────────────
  function setCtrlStatus(running, looping) {
    const el = document.getElementById('ctrl-status');
    if (running && looping) {
      el.className = 'status-pill s-running'; el.textContent = '↻ Loop · Running';
    } else if (running) {
      el.className = 'status-pill s-running'; el.textContent = '● Running';
    } else if (looping) {
      el.className = 'status-pill s-running'; el.textContent = '↻ Cooldown…';
    } else {
      el.className = 'status-pill s-idle';    el.textContent = '○ Idle';
    }
    const busy = running || looping;
    document.getElementById('btn-start').disabled   =  busy;
    document.getElementById('btn-stop').disabled    = !busy;
    document.getElementById('btn-restart').disabled = !busy;
  }

  // ── time helpers ───────────────────────────────────────────────────────────
  function fmtDur(sec) {
    sec = Math.max(0, Math.floor(sec));
    const h = Math.floor(sec / 3600), r = sec % 3600;
    const m = Math.floor(r / 60),     s = r % 60;
    if (h) return `${h}h ${m}m`;
    return `${m}m ${String(s).padStart(2,'0')}s`;
  }
  function fmtTime(ms) {
    const d = new Date(ms);
    return String(d.getHours()).padStart(2,'0') + ':' + String(d.getMinutes()).padStart(2,'0');
  }

  // ── 1-second ticker ────────────────────────────────────────────────────────
  let _startMs = 0, _remainAtPoll = 0, _pollAt = 0, _isRunning = false;

  function tickElapsed() {
    const elEl = document.getElementById('prog-elapsed');
    const rmEl = document.getElementById('prog-remain');
    const etEl = document.getElementById('prog-eta');
    if (!_startMs || !elEl) return;
    const now     = Date.now();
    const elapsed = (now - _startMs) / 1000;
    const remain  = _isRunning ? Math.max(0, _remainAtPoll - (now - _pollAt) / 1000) : 0;
    elEl.textContent = fmtDur(elapsed);
    if (rmEl) rmEl.textContent = _isRunning && remain > 0 ? fmtDur(remain) : '—';
    if (etEl) etEl.textContent = _isRunning && remain > 0 ? fmtTime(now + remain * 1000) : '—';
  }
  setInterval(tickElapsed, 1000);

  // ── progress section ───────────────────────────────────────────────────────
  function renderProgress(status, live) {
    const el = document.getElementById('run-progress-section');
    if (!el) return;

    const running = status.running;
    const runDir  = status.run_dir || '';
    const startMs = status.run_start_ms || 0;
    const cfg     = status.config || {};
    const now     = Date.now();

    const keys    = Object.keys(live);
    const total   = keys.length;
    const done    = keys.filter(k => live[k].status === 'done').length;
    const actives = keys.filter(k => !['waiting','done'].includes(live[k].status));
    const pct     = total > 0 ? done / total * 100 : 0;

    const elapsedSec = startMs ? (now - startMs) / 1000 : 0;
    const remainSec  = (running && done > 0 && startMs)
      ? Math.max(0, (elapsedSec / done) * (total - done)) : 0;

    _startMs = startMs; _remainAtPoll = remainSec;
    _pollAt  = now;     _isRunning    = running;

    const looping = status.looping;
    let statusPill;
    if (running && looping)               statusPill = `<span class="status-pill s-running">↻ Loop · Running</span>`;
    else if (running)                     statusPill = `<span class="status-pill s-running">● Running</span>`;
    else if (looping)                     statusPill = `<span class="status-pill s-running">↻ Cooldown…</span>`;
    else if (total > 0 && done === total) statusPill = `<span class="status-pill s-done">✓ Completed</span>`;
    else if (total > 0)                   statusPill = `<span class="status-pill s-stopped">○ Stopped</span>`;
    else                                  statusPill = `<span class="status-pill s-idle">○ Idle</span>`;

    // Idle state — no data yet
    if (total === 0) {
      el.innerHTML = `
<h2 id="sec-progress">Run Progress</h2>
<div class="card" style="display:flex;justify-content:space-between;align-items:center">
  <span style="color:var(--muted);font-size:.87rem">No active run — configure settings and click <strong>Start Run</strong>.</span>
  ${statusPill}
</div>`;
      return;
    }

    const doneKeys  = keys.filter(k => live[k].status === 'done');
    const lastKey   = doneKeys.length ? doneKeys[doneKeys.length - 1] : null;

    const activeHtml = actives.length
      ? actives.map(k => {
          const [g, m] = k.split('__');
          return `${g} / <em>${m.toUpperCase()}</em> ${badge(live[k])}`;
        }).join(' &emsp; ')
      : '<span style="color:var(--muted)">—</span>';

    const lastHtml = lastKey
      ? (() => {
          const [g, m] = lastKey.split('__');
          const s = live[lastKey];
          return `${g} / <em>${m.toUpperCase()}</em> &nbsp;✓ k=${s.best_k ?? '?'}${s.best_x != null ? '  X='+s.best_x : ''}`;
        })()
      : '<span style="color:var(--muted)">—</span>';

    const cfgSummary = cfg.methods
      ? `<span style="font-size:.78rem;color:var(--muted)">[${cfg.methods}] · graphs ${cfg.graphs||'?'} · ${cfg.workers||'?'}w · seed ${cfg.seed??'?'}</span>`
      : '';

    el.innerHTML = `
<h2 id="sec-progress">Run Progress</h2>
<div class="card ${!running && total > 0 ? 'prog-inactive' : ''}">
  <div style="display:flex;justify-content:space-between;align-items:center;
              padding-bottom:10px;margin-bottom:4px;border-bottom:1px solid var(--border)">
    <div style="display:flex;align-items:center;gap:12px;flex-wrap:wrap">
      <strong style="font-size:.9rem;font-family:monospace;letter-spacing:-.01em">${runDir}</strong>
      ${startMs ? `<span style="font-size:.75rem;color:var(--muted)">started ${fmtTime(startMs)}</span>` : ''}
      ${cfgSummary}
    </div>
    ${statusPill}
  </div>
  <div class="prog-stat-row">
    <div class="prog-stat">
      <span class="prog-stat-lbl">Done</span>
      <span class="prog-stat-val">${done} <small style="font-size:.7em;font-weight:500;color:var(--muted)">/ ${total}</small></span>
    </div>
    <div class="prog-stat">
      <span class="prog-stat-lbl">Elapsed</span>
      <span class="prog-stat-val" id="prog-elapsed">${startMs ? fmtDur(elapsedSec) : '—'}</span>
    </div>
    <div class="prog-stat">
      <span class="prog-stat-lbl">Remaining</span>
      <span class="prog-stat-val" id="prog-remain">${running && remainSec > 0 ? fmtDur(remainSec) : '—'}</span>
    </div>
    <div class="prog-stat hi">
      <span class="prog-stat-lbl">ETA</span>
      <span class="prog-stat-val" id="prog-eta">${running && remainSec > 0 ? fmtTime(now + remainSec*1000) : '—'}</span>
    </div>
  </div>
  <div class="prog-info-row">
    <span class="prog-info-lbl">Active now</span>
    <span style="font-size:.83rem">${activeHtml}</span>
  </div>
  <div class="prog-info-row">
    <span class="prog-info-lbl">Last done</span>
    <span style="font-size:.83rem">${lastHtml}</span>
  </div>
</div>`;
  }

  // ── live status badge ──────────────────────────────────────────────────────
  function badge(s) {
    const st = s.status || 'waiting';
    const bk = s.best_k  != null ? 'k=' + s.best_k  : 'k=?';
    const bx = s.best_x  != null ? ' X=' + s.best_x  : '';
    const t  = s.t_elapsed != null ? ' t=' + s.t_elapsed + 's' : '';
    const T  = s.temp      != null ? ' T=' + s.temp.toFixed(3) : '';
    if (st === 'waiting')              return `<span class="ls-wait">waiting</span>`;
    if (st === 'ph1')                  return `<span class="ls-ph1">▶ ph1 ${bk}${t}</span>`;
    if (st === 'ph2')                  return `<span class="ls-ph2">▶ ph2 ${bk}${t}${T}</span>`;
    if (st.startsWith('ils_ph')) {
      const ph = st.replace('ils_ph','').replace('_done','');
      const r  = s.ils_round != null ? ` r=${s.ils_round}/${s.ils_total}` : '';
      return `<span class="ls-ils">▶ ILS·ph${ph}${r} ${bk}</span>`;
    }
    if (st === 'lns' || st === 'lns_done') {
      const lt = s.lns_t != null ? ` t=${s.lns_t}s` : '';
      return `<span class="ls-lns">▶ LNS ${bk}${lt}</span>`;
    }
    if (st === 'done') return `<span class="ls-done">✓ ${bk}${bx}</span>`;
    return `<span class="ls-wait">—</span>`;
  }

  // ── live status table ──────────────────────────────────────────────────────
  function renderLive(data) {
    const wrap = document.getElementById('live-table-wrap');
    const meta = document.getElementById('live-meta');
    const keys = Object.keys(data);
    if (!keys.length) {
      wrap.innerHTML = '<em style="color:#aaa;font-size:.85rem">No live data — start a run or check back later.</em>';
      if (meta) meta.textContent = '';
      return;
    }
    const graphs  = [...new Set(keys.map(k => k.split('__')[0]))]
      .sort((a,b) => parseInt(a.match(/\d+/)||0) - parseInt(b.match(/\d+/)||0));
    const methods = [...new Set(keys.map(k => k.split('__')[1]))];
    const done    = keys.filter(k => data[k].status === 'done').length;
    const active  = keys.filter(k => !['waiting','done'].includes(data[k].status)).length;

    if (meta) meta.textContent = `${done}/${keys.length} done · ${active} active · polls every 5s`;

    let t = `<table>
  <thead><tr>
    <th style="text-align:left;padding:8px 12px">Graph</th>
    ${methods.map(m => `<th style="padding:8px 12px">${m.toUpperCase()}</th>`).join('')}
  </tr></thead><tbody>`;

    for (const g of graphs) {
      t += `<tr><td class="ls-cell" style="font-weight:600">${g.replace('Automatic-','Auto-')}</td>`;
      for (const m of methods) {
        const s = data[g+'__'+m] || {status:'waiting'};
        const isDone   = s.status === 'done';
        const isActive = !['waiting','done'].includes(s.status);
        const cls = isDone ? ' ls-done-cell' : isActive ? ' ls-active-cell' : '';
        t += `<td class="ls-cell${cls}">${badge(s)}</td>`;
      }
      t += '</tr>';
    }
    t += '</tbody></table>';
    wrap.innerHTML = t;
  }

  // ── live best-per-graph table ───────────────────────────────────────────────
  function renderGraphBest(data) {
    const wrap = document.getElementById('live-best-wrap');
    const keys = Object.keys(data);
    if (!keys.length) {
      wrap.innerHTML = '<em style="color:#aaa;font-size:.85rem">No live data — start a run or check back later.</em>';
      return;
    }
    const graphs = [...new Set(keys.map(k => k.split('__')[0]))]
      .sort((a,b) => parseInt(a.match(/\d+/)||0) - parseInt(b.match(/\d+/)||0));

    let t = `<table>
  <thead><tr>
    <th style="text-align:left;padding:8px 12px">Graph</th>
    <th style="padding:8px 12px">Best k</th>
    <th style="text-align:left;padding:8px 12px">Config</th>
  </tr></thead><tbody>`;

    for (const g of graphs) {
      let best = null;
      for (const k of keys) {
        if (!k.startsWith(g + '__')) continue;
        const s = data[k];
        if (s.best_k == null) continue;
        if (best === null
            || s.best_k < best.s.best_k
            || (s.best_k === best.s.best_k && (s.best_x ?? Infinity) < (best.s.best_x ?? Infinity))) {
          best = {method: k.split('__')[1], s};
        }
      }
      t += `<tr><td class="ls-cell" style="font-weight:600">${g.replace('Automatic-','Auto-')}</td>`;
      if (best) {
        const x = best.s.best_x != null ? `X=${best.s.best_x}` : 'X=?';
        const w = best.s.worker != null ? `w${best.s.worker}` : '';
        t += `<td class="ls-cell" style="font-weight:700">${best.s.best_k}</td>`;
        t += `<td class="ls-cell"><small style="color:#888">${best.method.toUpperCase()} · ${w} · ${x}</small></td>`;
      } else {
        t += `<td class="ls-cell">—</td><td class="ls-cell">—</td>`;
      }
      t += '</tr>';
    }
    t += '</tbody></table>';
    wrap.innerHTML = t;
  }

  // ── poll loop ──────────────────────────────────────────────────────────────
  async function poll() {
    try {
      const status = await (await fetch('/api/status')).json();
      if (_firstPoll) { syncCfg(status.config); _firstPoll = false; }
      setCtrlStatus(status.running, status.looping);
      // while a run is active, follow its actual mode; when idle, follow the toggle
      const mode = status.running ? ((status.config && status.config.mode) || CUR_MODE)
                                  : CUR_MODE;
      if (mode === 'orchestrator') {
        await renderOrch(status);
      } else {
        const live = await (await fetch('/api/live')).json();
        renderProgress(status, live);
        renderLive(live);
        renderGraphBest(live);
      }
    } catch (e) { console.warn('poll error', e); }
    pollTimer = setTimeout(poll, POLL_MS);
  }

  // ── button handlers ────────────────────────────────────────────────────────
  window.ctrlStart = async function () {
    setMsg('Starting…', false);
    const res  = await fetch('/api/start', {
      method: 'POST', headers: {'Content-Type':'application/json'},
      body: JSON.stringify(readCfg()),
    });
    const data = await res.json();
    if (!data.ok) { setMsg(data.error || 'Error', true); return; }
    setMsg('Run started — PID ' + data.pid, false);
    clearTimeout(pollTimer); poll();
  };

  window.ctrlStop = async function () {
    setMsg('Stopping…', false);
    const res  = await fetch('/api/stop', { method: 'POST' });
    const data = await res.json();
    if (!data.ok) { setMsg(data.error || 'Error', true); return; }
    setMsg('Run stopped.', false);
    clearTimeout(pollTimer); poll();
  };

  window.ctrlRestart = async function () {
    setMsg('Stopping…', false);
    await fetch('/api/stop', { method: 'POST' });
    await new Promise(r => setTimeout(r, 800));
    setMsg('Restarting…', false);
    const res  = await fetch('/api/start', {
      method: 'POST', headers: {'Content-Type':'application/json'},
      body: JSON.stringify(readCfg()),
    });
    const data = await res.json();
    if (!data.ok) { setMsg(data.error || 'Error', true); return; }
    setMsg('Restarted — PID ' + data.pid, false);
    clearTimeout(pollTimer); poll();
  };

  // Render method checkboxes first (so the first syncCfg can tick them), then poll.
  (async () => { await loadMethods(); await loadGdaGraphs(); poll(); })();
})();
"""


# ── HTTP handler ──────────────────────────────────────────────────────────────
class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # quiet

    def _json(self, data, code=200):
        body = json.dumps(data).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _html(self, html: str):
        body = html.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path).path
        if path in ("/", "/index.html"):
            self._serve_page()
        elif path == "/api/status":
            self._json(api_status())
        elif path == "/api/methods":
            self._json(api_methods())
        elif path == "/api/gda-graphs":
            self._json(api_gda_graphs())
        elif path == "/api/live":
            self._json(api_live())
        elif path == "/api/orch-status":
            self._json(api_orch_status())
        else:
            self.send_error(404)

    def do_POST(self):
        path = urlparse(self.path).path
        length = int(self.headers.get("Content-Length", 0))
        body   = json.loads(self.rfile.read(length) or b"{}") if length else {}
        if path == "/api/start":
            self._json(api_start(body))
        elif path == "/api/stop":
            self._json(api_stop())
        elif path == "/api/scan-folder":
            self._json(api_scan_folder(body))
        else:
            self.send_error(404)

    def _serve_page(self):
        from run_contest import generate_report, load_json
        history = load_json(RESULTS / "history.json", {"runs": []})
        bests   = load_json(RESULTS / "bests.json",   {})
        generate_report(RESULTS, history, bests)
        html = (RESULTS / "report.html").read_text()

        # 1. Remove meta-refresh.
        html = re.sub(r'<meta http-equiv="refresh"[^>]*>', '', html)

        # 2. Inject CSS.
        html = html.replace("</style>", _CTRL_CSS + "</style>", 1)

        # 3. Inject sticky nav bar right after <body>.
        nav = """<nav class="page-nav">
  <strong style="margin-right:8px;color:#222;font-size:.8rem;letter-spacing:.05em">GD-2025</strong>
  <a href="#sec-progress">Progress</a>
  <a href="#sec-live">Live Status</a>
  <a href="#sec-best">Best Results</a>
  <a href="#sec-charts">Charts</a>
  <a href="#sec-charts2">k-Value</a>
  <a href="#sec-history">History</a>
</nav>\n"""
        html = html.replace("<body>", "<body>\n" + nav, 1)

        # 4. Replace PROGRESS block with two-column dash-top:
        #    left = ctrl sidebar, right = JS-rendered progress.
        html = re.sub(
            r'<!-- PROGRESS_START -->.*?<!-- PROGRESS_END -->',
            '<div class="dash-top">\n' + _CTRL_PANEL + '\n<div id="run-progress-section"></div>\n</div>',
            html, flags=re.DOTALL
        )

        # 5. Replace LIVE_STATUS block with JS-driven live table.
        html = re.sub(
            r'<!-- LIVE_STATUS_START -->.*?<!-- LIVE_STATUS_END -->',
            _LIVE_SECTION,
            html, flags=re.DOTALL
        )

        # 6. Add section IDs to key headings for nav anchors.
        html = re.sub(r'<h2>Best Results',        '<h2 id="sec-best">Best Results',     html, count=1)
        html = re.sub(r'<h2>Method Runtime',      '<h2 id="sec-charts">Method Runtime', html, count=1)
        html = re.sub(r'<h2>k-value Comparison',  '<h2 id="sec-charts2">k-value Comparison', html, count=1)
        html = re.sub(r'<h2>Run History',         '<h2 id="sec-history">Run History',   html, count=1)

        # 7. Inject JS before </body>.
        html = html.replace("</body>", f"<script>{_CTRL_JS}</script>\n</body>", 1)

        self._html(html)


# ── main ──────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--no-browser", action="store_true")
    args = ap.parse_args()

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    url    = f"http://localhost:{args.port}"
    print(f"[server] GD-2025 Control Server running at {url}")
    print(f"[server] Ctrl+C to stop")

    if not args.no_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[server] Stopping…")
        if _alive():
            api_stop()
        server.shutdown()


if __name__ == "__main__":
    main()
