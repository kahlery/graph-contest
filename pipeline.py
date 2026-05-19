#!/usr/bin/env python3
"""
pipeline.py — Overnight pipeline for SAkGD contest solver.

Cycles through all 9 contest graphs (Automatic-1 … 9) running 4 parallel
workers (60 min each), keeps the best result per graph, and warm-starts each
round from the previous best layout. Existing exports are used as the initial
warm-start. Runs until STOP_HOUR (23:00) or Ctrl+C.

Usage:
    python3 pipeline.py
    python3 pipeline.py --until 22:00
    python3 pipeline.py --graphs 1 2 3 --workers 4 --minutes 60
"""
from __future__ import annotations

import argparse
import re
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BIN = ROOT / "sakgd"
CONTEST_DIR = ROOT / "data" / "live-2025-contest" / "live-contest"
EXPORTS_DIR = ROOT / "data" / "live-2025-contest" / "exports"
RESULTS_DIR = ROOT / "results"
RUNS_DIR = RESULTS_DIR / "runs"

# Existing exports to use as warm-start seeds (graph number → export filename)
EXISTING_EXPORTS: dict[int, str] = {
    1: "Automatic-1_exported.json",
    2: "Automatic-2_exported.json",
    3: "Automatic-3_exported.json",
    4: "Automatic-4_exported.json",
    5: "Automatic-5_exported.json",
    6: "Automatic-6_exported.json",
    9: "7.json",  # 7.json is actually Automatic-9's layout (2519 nodes)
}

DEFAULT_GRAPHS = [1, 2, 3, 4, 5, 6, 7, 8, 9]
DEFAULT_WORKERS = 4
DEFAULT_MINUTES = 60.0
DEFAULT_PHASE1 = 10.0
DEFAULT_STOP_HOUR = 23
MIN_REMAINING_SEC = 900  # stop loop if less than 15 min left

_running_procs: list[subprocess.Popen] = []
_shutdown = False


# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

def log(msg: str) -> None:
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {msg}", flush=True)


# ---------------------------------------------------------------------------
# Verify helper
# ---------------------------------------------------------------------------

def verify(path: Path) -> tuple[int | None, int | None]:
    """Run `./sakgd --verify path` and return (k, crossings), or (None, None)."""
    try:
        r = subprocess.run(
            [str(BIN), "--verify", str(path)],
            capture_output=True, text=True, timeout=60,
        )
        for line in reversed(r.stdout.splitlines()):
            m = re.search(r"k=(\d+)\s+totalCrossings=(\d+)", line)
            if m:
                return int(m.group(1)), int(m.group(2))
    except Exception:
        pass
    return None, None


def is_better(k1, x1, k2, x2) -> bool:
    """True if (k1, x1) strictly beats (k2, x2). None counts as infinity."""
    if k1 is None:
        return False
    if k2 is None:
        return True
    return (k1, x1) < (k2, x2)


def fmt(k, x) -> str:
    return f"k={k}, X={x}" if k is not None else "none"


# ---------------------------------------------------------------------------
# Worker runner
# ---------------------------------------------------------------------------

def run_round(
    graph: str,
    input_path: Path,
    round_num: int,
    base_seed: int,
    workers: int,
    minutes: float,
    phase1: float,
) -> tuple[Path | None, int | None, int | None]:
    """
    Launch `workers` parallel sakgd processes with different seeds on `input_path`.
    Wait for all to finish, then return (best_output_path, k, crossings).
    """
    global _running_procs

    run_dir = RUNS_DIR / graph / f"round-{round_num:04d}"
    run_dir.mkdir(parents=True, exist_ok=True)

    launched: list[tuple[subprocess.Popen, Path, int]] = []

    for i in range(workers):
        seed = base_seed + i
        worker_dir = run_dir / f"seed-{seed}"
        worker_dir.mkdir(exist_ok=True)
        output_file = worker_dir / "output.json"
        log_file = worker_dir / "log.txt"

        cmd = [
            str(BIN),
            "-i", str(input_path),
            "-o", str(output_file),
            "-t", str(minutes),
            "-p1", str(phase1),
            "-s", str(seed),
        ]
        log_fh = open(log_file, "w")
        proc = subprocess.Popen(cmd, stdout=log_fh, stderr=log_fh)
        launched.append((proc, output_file, seed))
        _running_procs.append(proc)
        time.sleep(0.1)

    # Wait for all
    for proc, out_path, seed in launched:
        try:
            proc.wait()
        except Exception:
            pass
        try:
            _running_procs.remove(proc)
        except ValueError:
            pass
        if proc.returncode not in (0, None):
            log(f"  worker seed={seed} exited with code {proc.returncode}")

    if _shutdown:
        return None, None, None

    # Pick best result
    best_path: Path | None = None
    best_k: int | None = None
    best_x: int | None = None

    for _, out_path, seed in launched:
        if not out_path.exists():
            log(f"  worker seed={seed} produced no output")
            continue
        k, x = verify(out_path)
        if k is None:
            log(f"  worker seed={seed}: verify failed")
            continue
        log(f"  worker seed={seed}: k={k}, X={x}")
        if is_better(k, x, best_k, best_x):
            best_k, best_x = k, x
            best_path = out_path

    return best_path, best_k, best_x


# ---------------------------------------------------------------------------
# Signal handling
# ---------------------------------------------------------------------------

def _shutdown_handler(*_):
    global _shutdown
    _shutdown = True
    log("Interrupt received — terminating workers...")
    for proc in list(_running_procs):
        try:
            proc.terminate()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Overnight SAkGD pipeline")
    p.add_argument(
        "--graphs", nargs="+", type=int, default=DEFAULT_GRAPHS,
        metavar="N",
        help=f"graph numbers to process (default: {DEFAULT_GRAPHS})",
    )
    p.add_argument(
        "--workers", type=int, default=DEFAULT_WORKERS,
        help=f"parallel workers per graph (default: {DEFAULT_WORKERS})",
    )
    p.add_argument(
        "--minutes", type=float, default=DEFAULT_MINUTES,
        help=f"SA budget per run in minutes (default: {DEFAULT_MINUTES})",
    )
    p.add_argument(
        "--phase1", type=float, default=DEFAULT_PHASE1,
        help=f"phase-1 (crossing reduction) budget in minutes (default: {DEFAULT_PHASE1})",
    )
    p.add_argument(
        "--until", default=f"{DEFAULT_STOP_HOUR}:00",
        metavar="HH:MM",
        help=f"stop time today (default: {DEFAULT_STOP_HOUR}:00)",
    )
    return p.parse_args()


def main():
    global _shutdown

    args = parse_args()

    if not BIN.exists():
        sys.exit(f"Solver binary not found: {BIN}\nRun `make` first.")

    RESULTS_DIR.mkdir(exist_ok=True)
    RUNS_DIR.mkdir(parents=True, exist_ok=True)

    signal.signal(signal.SIGINT, _shutdown_handler)
    signal.signal(signal.SIGTERM, _shutdown_handler)

    # Parse stop time
    try:
        stop_h, stop_m = map(int, args.until.split(":"))
        stop_time = datetime.now().replace(
            hour=stop_h, minute=stop_m, second=0, microsecond=0
        )
    except Exception:
        sys.exit(f"Invalid --until value: {args.until}  (expected HH:MM)")

    if datetime.now() >= stop_time:
        sys.exit(f"Stop time {args.until} is already in the past.")

    graph_names = [f"Automatic-{n}" for n in sorted(set(args.graphs))]

    # Verify all input graphs exist
    for name in graph_names:
        p = CONTEST_DIR / f"{name}.json"
        if not p.exists():
            sys.exit(f"Input graph not found: {p}")

    log(f"Pipeline starting — will run until {stop_time.strftime('%H:%M')}")
    log(f"Graphs   : {', '.join(graph_names)}")
    log(f"Workers  : {args.workers} × {args.minutes:.0f} min (phase1={args.phase1:.0f} min)")
    log(f"Results  : {RESULTS_DIR}")
    log("-" * 60)

    # Seed results/ from existing exports (only if no pipeline result yet)
    for num, fname in EXISTING_EXPORTS.items():
        name = f"Automatic-{num}"
        if name not in graph_names:
            continue
        src = EXPORTS_DIR / fname
        dst = RESULTS_DIR / f"{name}_best.json"
        if src.exists() and not dst.exists():
            shutil.copy2(str(src), str(dst))
            log(f"Seeded {name} from existing export: {fname}")

    # Load best metrics per graph (from results/ which may now include seeded exports)
    best_metrics: dict[str, tuple[int | None, int | None]] = {}
    for name in graph_names:
        bp = RESULTS_DIR / f"{name}_best.json"
        k, x = verify(bp) if bp.exists() else (None, None)
        best_metrics[name] = (k, x)
        if k is not None:
            log(f"Starting best for {name}: {fmt(k, x)}")

    round_num = 0

    while not _shutdown:
        remaining = (stop_time - datetime.now()).total_seconds()
        if remaining < MIN_REMAINING_SEC:
            log(f"Less than {MIN_REMAINING_SEC // 60} min remaining — stopping pipeline.")
            break

        round_num += 1
        base_seed = round_num * 100
        log(f"=== Round {round_num}  (seeds {base_seed}–{base_seed + args.workers - 1}) ===")

        for name in graph_names:
            if _shutdown:
                break

            remaining = (stop_time - datetime.now()).total_seconds()
            if remaining < MIN_REMAINING_SEC:
                log("Time limit reached inside round — stopping.")
                _shutdown = True
                break

            best_path = RESULTS_DIR / f"{name}_best.json"
            original = CONTEST_DIR / f"{name}.json"
            input_path = best_path if best_path.exists() else original

            old_k, old_x = best_metrics[name]
            log(f"[{name}] Round {round_num} starting  (best so far: {fmt(old_k, old_x)})")
            log(f"[{name}] Input: {input_path.name}")

            out_path, new_k, new_x = run_round(
                name, input_path, round_num, base_seed,
                args.workers, args.minutes, args.phase1,
            )

            if out_path is None:
                log(f"[{name}] No valid output from round {round_num}.")
                continue

            log(f"[{name}] Round {round_num} best: {fmt(new_k, new_x)}")

            if is_better(new_k, new_x, old_k, old_x):
                shutil.copy2(str(out_path), str(best_path))
                best_metrics[name] = (new_k, new_x)
                improvement = (
                    f"k {old_k}→{new_k}" if old_k is not None else "first result"
                )
                log(f"[{name}] *** NEW BEST ({improvement}, X={new_x}) saved ***")
            else:
                log(f"[{name}] No improvement (best stays {fmt(old_k, old_x)})")

        log("")

    # Final summary
    log("=" * 60)
    log("Pipeline finished. Final results:")
    for name in graph_names:
        k, x = best_metrics[name]
        if k is not None:
            log(f"  {name}: {fmt(k, x)}  →  results/{name}_best.json")
        else:
            log(f"  {name}: no result produced")
    log("=" * 60)


if __name__ == "__main__":
    main()
