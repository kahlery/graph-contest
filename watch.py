#!/usr/bin/env python3
"""
Live 5-second progress watcher for GD-2025 contest runs.

Usage:
  python3 watch.py              # auto-picks latest run
  python3 watch.py 2026-06-07_13-41-13
"""

import re
import sys
import time
import shutil
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent
RUNS = ROOT / "results" / "runs"

# ── log patterns ──────────────────────────────────────────────────────────────
RE_INITIAL    = re.compile(r"Initial:\s*k=(\d+)\s+totalX=(\d+)")
RE_PH_START   = re.compile(r"\[phase (\d+)\] start.*?budget=(\d+)s")
RE_PH_PROG    = re.compile(r"\[phase (\d+)\]\s+t=(\d+)s\s+bestK=(\d+)\s+bestX=(\d+).*?sT=(\S+)")
RE_PH_END     = re.compile(r"\[phase (\d+)\] end\s+.*?bestK=(\d+)\s+bestX=(\d+)")
RE_ILS_ROUND  = re.compile(r"\[ILS phase (\d+)\] round (\d+)/(\d+).*?curBestK=(\d+)")
RE_ILS_END    = re.compile(r"\[ILS phase (\d+)\] end.*?bestK=(\d+)\s+bestX=(\d+)")
RE_LNS_PROG   = re.compile(r"\[LNS.*?(\d+)\] t=(\d+)s\s+bestK=(\d+)\s+bestX=(\d+)")
RE_LNS_END    = re.compile(r"\[LNS.*?phase (\d+)\] end.*?bestK=(\d+)\s+bestX=(\d+)")
RE_FINAL      = re.compile(r"Final best:\s*k=(\d+)\s+totalX=(\d+)")

# ── ANSI ──────────────────────────────────────────────────────────────────────
R  = "\033[0m"
B  = "\033[1m"
DIM= "\033[2m"
GR = "\033[32m"
YL = "\033[33m"
CY = "\033[36m"
MG = "\033[35m"
RD = "\033[31m"
WH = "\033[97m"
CLEAR = "\033[2J\033[H"


def parse_log(path: Path) -> dict:
    s = dict(status="waiting", initial_k=None, best_k=None, best_x=None,
             phase=0, t_elapsed=None, temp=None,
             ils_round=None, ils_total=None, lns_t=None,
             algo=None)
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return s
    if not text.strip():
        return s

    s["status"] = "ph1"

    m = RE_INITIAL.search(text)
    if m:
        s["initial_k"] = int(m.group(1))
        s["best_k"]    = int(m.group(1))

    # SA / inner-SA progress (phase 1 & 2)
    for m in RE_PH_PROG.finditer(text):
        ph = int(m.group(1))
        s["phase"]     = ph
        s["t_elapsed"] = int(m.group(2))
        s["best_k"]    = int(m.group(3))
        s["best_x"]    = int(m.group(4))
        s["temp"]      = float(m.group(5))
        s["status"]    = f"ph{ph}"
        s["algo"]      = s["algo"] or "sa"

    for m in RE_PH_END.finditer(text):
        ph = int(m.group(1))
        s["best_k"] = int(m.group(2))
        s["best_x"] = int(m.group(3))
        s["status"] = f"ph{ph}_done"

    # ILS rounds
    for m in RE_ILS_ROUND.finditer(text):
        ph = int(m.group(1))
        s["ils_round"] = int(m.group(2))
        s["ils_total"] = int(m.group(3))
        s["best_k"]    = int(m.group(4))
        s["status"]    = f"ils_ph{ph}"
        s["algo"]      = "ils"

    for m in RE_ILS_END.finditer(text):
        ph = int(m.group(1))
        s["best_k"] = int(m.group(2))
        s["best_x"] = int(m.group(3))
        s["status"] = f"ils_ph{ph}_done"
        s["algo"]   = "ils"

    # LNS progress
    for m in RE_LNS_PROG.finditer(text):
        s["lns_t"]  = int(m.group(2))
        s["best_k"] = int(m.group(3))
        s["best_x"] = int(m.group(4))
        s["status"] = "lns"
        s["algo"]   = "lns"

    for m in RE_LNS_END.finditer(text):
        s["best_k"] = int(m.group(2))
        s["best_x"] = int(m.group(3))
        s["status"] = "lns_done"
        s["algo"]   = "lns"

    # Final — always wins
    m = RE_FINAL.search(text)
    if m:
        s["best_k"] = int(m.group(1))
        s["best_x"] = int(m.group(2))
        s["status"] = "done"

    return s


def is_done(s: dict) -> bool:
    return s.get("status") == "done"


def cell(s: dict, w: int) -> str:
    st = s.get("status", "waiting")

    if st == "waiting":
        txt = f"{DIM}  ·· waiting ··{R}"

    elif st in ("ph1", "ph1_done"):
        k = f"k={s['best_k']}" if s["best_k"] is not None else "k=?"
        t = f" t={s['t_elapsed']}s" if s["t_elapsed"] is not None else ""
        txt = f"{YL}  ▶ ph1 {k}{t}{R}"

    elif st in ("ph2", "ph2_done"):
        k = f"k={s['best_k']}" if s["best_k"] is not None else "k=?"
        t = f" t={s['t_elapsed']}s" if s["t_elapsed"] is not None else ""
        T = f" T={s['temp']:.3f}" if s["temp"] is not None else ""
        txt = f"{CY}  ▶ ph2 {k}{t}{T}{R}"

    elif st.startswith("ils_ph"):
        ph   = st.replace("ils_ph", "").replace("_done", "")
        r    = s["ils_round"]
        tot  = s["ils_total"]
        k    = f"k={s['best_k']}" if s["best_k"] is not None else "k=?"
        rnd  = f" r={r}/{tot}" if r is not None else ""
        txt  = f"{MG}  ▶ ILS·ph{ph}{rnd} {k}{R}"

    elif st in ("lns", "lns_done"):
        k   = f"k={s['best_k']}" if s["best_k"] is not None else "k=?"
        t   = f" t={s['lns_t']}s" if s["lns_t"] is not None else ""
        txt = f"{WH}  ▶ LNS {k}{t}{R}"

    elif st == "done":
        k = f"k={s['best_k']}" if s["best_k"] is not None else "k=?"
        x = f" X={s['best_x']}" if s["best_x"] is not None else ""
        txt = f"{GR}  ✓ {k}{x}{R}"

    else:
        txt = f"{DIM}  ··{R}"

    # pad to w (ANSI codes are invisible, so measure visible len)
    visible = re.sub(r"\033\[[0-9;]*m", "", txt)
    pad = max(0, w - len(visible))
    return txt + " " * pad


def fmt_dur(sec: float) -> str:
    sec = int(sec)
    h, rem = divmod(sec, 3600)
    m, s   = divmod(rem, 60)
    if h:
        return f"{h}h{m:02d}m{s:02d}s"
    return f"{m}m{s:02d}s"


def render(run_dir: Path, run_start: datetime):
    tw = shutil.get_terminal_size((140, 40)).columns

    # --- collect graph dirs sorted numerically ---
    graph_dirs = sorted(
        [d for d in run_dir.iterdir() if d.is_dir()],
        key=lambda p: int(re.search(r"\d+", p.name).group() or 0)
    )

    # --- detect methods from log files ---
    method_set = set()
    for gd in graph_dirs:
        for f in gd.glob("*.log"):
            n = f.stem  # e.g. sa_w0, ils_w0, staged_lns_w0, staged_sa_w0
            if n.startswith("sa_"):             method_set.add("sa")
            if n.startswith("ils_"):            method_set.add("ils")
            if n.startswith("staged_lns"):      method_set.add("staged")
            if n.startswith("staged-adaptive"): method_set.add("staged-adaptive")
    methods = [m for m in ["sa", "staged", "staged-adaptive", "ils"] if m in method_set]
    if not methods:
        methods = ["sa", "ils"]

    # --- parse every log ---
    # staged: use staged_sa_w0.log if it exists, else staged_lns_w0.log
    grid = {}
    for gd in graph_dirs:
        for method in methods:
            if method in ("staged", "staged-adaptive"):
                log = gd / "staged_sa_w0.log"
                if not log.exists():
                    log = gd / "staged_lns_w0.log"
            else:
                log = gd / f"{method}_w0.log"
            grid[(gd.name, method)] = parse_log(log)

    n_combos  = len(graph_dirs) * len(methods)
    done_cnt  = sum(1 for s in grid.values() if is_done(s))
    active_cnt= sum(1 for s in grid.values() if s["status"] not in ("waiting", "done"))

    elapsed   = (datetime.now() - run_start).total_seconds()
    pct       = done_cnt / n_combos if n_combos else 0
    bar_w     = 36
    filled    = int(bar_w * pct)
    bar       = f"{GR}" + "█" * filled + f"{DIM}" + "░" * (bar_w - filled) + R

    # estimate remaining
    if done_cnt > 0:
        per_combo   = elapsed / done_cnt
        est_rem_sec = per_combo * (n_combos - done_cnt)
        est_rem     = fmt_dur(est_rem_sec)
    else:
        est_rem = "?"

    # --- column widths ---
    g_w   = max((len(gd.name) for gd in graph_dirs), default=12) + 2
    avail = max(tw - g_w - 4, 40)
    m_w   = max(26, avail // len(methods)) if methods else 26

    lines = []
    sep   = "─"

    # header
    now_str = datetime.now().strftime("%H:%M:%S")
    lines.append(
        f"{B}{WH}  GD-2025 Live Progress{R}  "
        f"{DIM}run: {run_dir.name}   "
        f"elapsed: {fmt_dur(elapsed)}   "
        f"now: {now_str}{R}"
    )
    lines.append(
        f"  Overall  [{bar}]  "
        f"{B}{done_cnt}/{n_combos}{R} done  "
        f"{YL}{active_cnt}{R} running  "
        f"est. remaining: {CY}{est_rem}{R}"
    )
    lines.append("")

    # column header
    hdr  = "  " + "Graph".ljust(g_w)
    div  = "  " + sep * g_w
    for m in methods:
        hdr += f"{B}{m.center(m_w)}{R}"
        div += sep * m_w
    lines.append(hdr)
    lines.append(div)

    # per-graph rows
    for gd in graph_dirs:
        row = "  " + f"{B}{gd.name}{R}".ljust(g_w + 8)  # +8 for ANSI bold codes
        visible_g = gd.name
        row = "  " + f"{B}{visible_g}{R}" + " " * (g_w - len(visible_g))
        for m in methods:
            s   = grid.get((gd.name, m), {"status": "waiting"})
            row += cell(s, m_w)
        lines.append(row)

    lines.append(div)
    lines.append(f"  {DIM}Refreshes every 5s — Ctrl+C to stop{R}")

    print(CLEAR + "\n".join(lines), end="", flush=True)


def find_latest_run() -> Path | None:
    if not RUNS.exists():
        return None
    dirs = sorted(d for d in RUNS.iterdir() if d.is_dir())
    return dirs[-1] if dirs else None


def main():
    if len(sys.argv) > 1:
        run_dir = RUNS / sys.argv[1]
        if not run_dir.is_dir():
            sys.exit(f"Run not found: {run_dir}")
    else:
        run_dir = find_latest_run()
        if not run_dir:
            sys.exit("No runs found in results/runs/")

    try:
        run_start = datetime.strptime(run_dir.name, "%Y-%m-%d_%H-%M-%S")
    except ValueError:
        run_start = datetime.now()

    print(f"Watching {run_dir.name} ...", flush=True)

    try:
        while True:
            render(run_dir, run_start)
            time.sleep(5)
    except KeyboardInterrupt:
        print("\n\nStopped.")


if __name__ == "__main__":
    main()
