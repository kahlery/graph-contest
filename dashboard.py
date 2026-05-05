#!/usr/bin/env python3
"""SAkGD dashboard runner.

Launches several SAkGD workers in parallel on the same input graph and serves
a live web dashboard at http://localhost:PORT/ that shows every run, its
current SA phase, temperature, k-value, total crossings, time progress, and a
live SVG of the best layout found so far.

Usage:
    ./dashboard.py data/k6.json -n 6 -t 1 -p1 0.2
    ./dashboard.py data/instance.json -n 8 -t 60 -p1 10 -p 8765
"""
from __future__ import annotations

import argparse
import http.server
import json
import os
import signal
import socket
import socketserver
import subprocess
import sys
import threading
import time
from argparse import Namespace
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DASHBOARD_DIR = ROOT / "dashboard"
DEFAULT_BIN = ROOT / "sakgd"


def is_port_free(port: int) -> bool:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("", port))
            return True
    except OSError:
        return False


def find_free_port(start: int = 8765, attempts: int = 50) -> int:
    for p in range(start, start + attempts):
        if is_port_free(p):
            return p
    return start


def list_input_graphs() -> list[Path]:
    data_dir = ROOT / "data"
    if not data_dir.exists():
        return []
    return sorted(
        p for p in data_dir.glob("*.json")
        if not p.name.endswith(".out.json") and "status" not in p.name
    )


def ask(prompt: str, default=None, parser=str, validate=None, allow_empty=False):
    label = f"{prompt} [{default}]: " if default is not None else f"{prompt}: "
    while True:
        try:
            raw = input(label).strip()
        except EOFError:
            print()
            sys.exit(1)
        if not raw:
            if default is not None:
                return default
            if allow_empty:
                return ""
            print("  -> Boş bırakılamaz.")
            continue
        try:
            value = parser(raw)
        except Exception as e:
            print(f"  -> Geçersiz değer: {e}")
            continue
        if validate is not None:
            err = validate(value)
            if err:
                print(f"  -> {err}")
                continue
        return value


def prompt_args() -> Namespace:
    print("=" * 56)
    print("  SAkGD Dashboard - interaktif mod")
    print("  (Enter = varsayılan değer, Ctrl+C = iptal)")
    print("=" * 56)

    graphs = list_input_graphs()
    if graphs:
        print("\nMevcut graf dosyaları:")
        for i, g in enumerate(graphs, 1):
            try:
                size_kb = g.stat().st_size / 1024
                print(f"  {i}) {g.name}  ({size_kb:.1f} KB)")
            except OSError:
                print(f"  {i}) {g.name}")
        print(f"  0) elle yol gir")
        choice = ask(
            "Girdi grafı seç",
            default="1",
            parser=str,
        )
        if choice == "0":
            input_file = ask(
                "Girdi JSON yolu",
                parser=lambda s: str(Path(s).expanduser()),
                validate=lambda s: None if Path(s).exists() else "dosya bulunamadı",
            )
        else:
            try:
                idx = int(choice) - 1
                input_file = str(graphs[idx])
            except (ValueError, IndexError):
                input_file = ask(
                    "Girdi JSON yolu",
                    parser=lambda s: str(Path(s).expanduser()),
                    validate=lambda s: None if Path(s).exists() else "dosya bulunamadı",
                )
    else:
        print("\n(data/ dizininde graf bulunamadı)")
        input_file = ask(
            "Girdi JSON yolu",
            parser=lambda s: str(Path(s).expanduser()),
            validate=lambda s: None if Path(s).exists() else "dosya bulunamadı",
        )

    print()
    workers = ask(
        "Paralel worker sayısı",
        default=4, parser=int,
        validate=lambda v: None if 1 <= v <= 64 else "1 ile 64 arasında olmalı",
    )
    minutes = ask(
        "Toplam süre (dakika, her worker icin)",
        default=2.0, parser=float,
        validate=lambda v: None if v > 0 else "pozitif olmalı",
    )
    p1_default = round(min(minutes / 3.0, 10.0), 2)
    p1_minutes = ask(
        "Asama 1 (kesisim azaltma) suresi (dakika)",
        default=p1_default, parser=float,
        validate=lambda v: None if 0 < v < minutes else f"0 ile {minutes} arasında olmalı",
    )

    suggested_port = find_free_port(8765)
    port = ask(
        "HTTP port",
        default=suggested_port, parser=int,
        validate=lambda v: None if 1024 <= v <= 65535 else "1024-65535 arasında olmalı",
    )
    if not is_port_free(port):
        alt = find_free_port(port + 1)
        print(f"  -> Port {port} dolu, {alt} kullanılacak.")
        port = alt

    seed = ask("Taban RNG seed", default=42, parser=int)

    open_browser = ask(
        "Tarayıcıyı otomatik aç? (e/h)",
        default="e", parser=str,
        validate=lambda v: None if v.lower() in ("e", "h", "y", "n") else "e veya h",
    )
    no_open = open_browser.lower() in ("h", "n")

    print()
    print("Ayarlar onaylandı, başlatılıyor...")
    print()

    return Namespace(
        input=input_file,
        workers=workers,
        minutes=minutes,
        phase1_minutes=p1_minutes,
        port=port,
        bin=str(DEFAULT_BIN),
        seed=seed,
        out_dir=str(ROOT / "runs"),
        status_interval=1.0,
        no_open=no_open,
    )


def parse_args() -> Namespace:
    p = argparse.ArgumentParser(
        description="Run multiple SAkGD workers in parallel with a live web dashboard.",
    )
    p.add_argument("input", nargs="?", help="input graph JSON (omit for interactive)")
    p.add_argument("-n", "--workers", type=int, default=4,
                   help="number of parallel SA workers (default: 4)")
    p.add_argument("-t", "--minutes", type=float, default=60.0,
                   help="total time budget per worker in minutes (default: 60)")
    p.add_argument("-p1", "--phase1-minutes", type=float, default=10.0,
                   help="phase-1 time budget in minutes (default: 10)")
    p.add_argument("-p", "--port", type=int, default=8765,
                   help="HTTP port for the dashboard (default: 8765)")
    p.add_argument("--bin", default=str(DEFAULT_BIN),
                   help=f"path to the sakgd binary (default: {DEFAULT_BIN})")
    p.add_argument("--seed", type=int, default=42,
                   help="base RNG seed; worker N uses seed+N")
    p.add_argument("--out-dir", default=str(ROOT / "runs"),
                   help="directory for per-worker outputs (default: ./runs)")
    p.add_argument("--status-interval", type=float, default=1.0,
                   help="seconds between status dumps (default: 1.0)")
    p.add_argument("--no-open", action="store_true",
                   help="don't try to open the browser automatically")
    p.add_argument("-I", "--interactive", action="store_true",
                   help="prompt for every value interactively")
    return p.parse_args()


class Worker:
    def __init__(self, idx, seed, run_dir, status_file, output_file, log_file, proc):
        self.idx = idx
        self.seed = seed
        self.id = f"seed-{seed:05d}"
        self.run_dir = run_dir
        self.status_file = status_file
        self.output_file = output_file
        self.log_file = log_file
        self.proc = proc
        self.start_time = time.time()


def launch_workers(args) -> list[Worker]:
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    workers: list[Worker] = []
    for i in range(args.workers):
        seed = args.seed + i
        run_dir = out_dir / f"seed-{seed:05d}"
        run_dir.mkdir(parents=True, exist_ok=True)
        status_file = run_dir / "status.json"
        output_file = run_dir / "output.json"
        log_file = run_dir / "log.txt"
        if status_file.exists():
            status_file.unlink()

        cmd = [
            str(args.bin),
            "-i", str(args.input),
            "-o", str(output_file),
            "-t", str(args.minutes),
            "-p1", str(args.phase1_minutes),
            "-s", str(seed),
            "--status-file", str(status_file),
            "--status-id", f"seed-{seed:05d}",
            "--status-interval", str(args.status_interval),
        ]
        log = open(log_file, "w")
        proc = subprocess.Popen(cmd, stdout=log, stderr=log)
        workers.append(Worker(i, seed, run_dir, status_file, output_file, log_file, proc))
        time.sleep(0.05)
    return workers


def read_json_safely(path: Path):
    try:
        with open(path, "r") as f:
            return json.load(f)
    except Exception:
        return None


class DashboardHandler(http.server.SimpleHTTPRequestHandler):
    workers: list[Worker] = []
    started_at: float = 0.0
    input_name: str = ""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(DASHBOARD_DIR), **kwargs)

    def log_message(self, *a):
        return

    def do_GET(self):
        if self.path == "/api/runs":
            return self._api_runs()
        if self.path == "/api/info":
            return self._api_info()
        if self.path in ("/", ""):
            self.path = "/index.html"
        return super().do_GET()

    def _send_json(self, payload, status=200):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _api_info(self):
        self._send_json({
            "input": DashboardHandler.input_name,
            "started_at": DashboardHandler.started_at,
            "now": time.time(),
            "workers": len(DashboardHandler.workers),
        })

    def _api_runs(self):
        out = []
        for w in DashboardHandler.workers:
            data = read_json_safely(w.status_file)
            ret = w.proc.poll()
            out.append({
                "id": w.id,
                "seed": w.seed,
                "alive": ret is None,
                "exit_code": ret,
                "wall_seconds": time.time() - w.start_time,
                "data": data,
            })
        self._send_json(out)


class ThreadingHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    if len(sys.argv) == 1:
        args = prompt_args()
    else:
        args = parse_args()
        if args.interactive or not args.input:
            args = prompt_args()

    if not Path(args.bin).exists():
        sys.exit(f"Solver binary not found: {args.bin}\nDid you run `make`?")
    if not Path(args.input).exists():
        sys.exit(f"Input not found: {args.input}")
    if not DASHBOARD_DIR.exists():
        sys.exit(f"Dashboard directory missing: {DASHBOARD_DIR}")

    print(f"Launching {args.workers} workers ({args.minutes} min each, "
          f"phase1={args.phase1_minutes} min)")
    workers = launch_workers(args)
    DashboardHandler.workers = workers
    DashboardHandler.started_at = time.time()
    DashboardHandler.input_name = os.path.basename(args.input)

    server = ThreadingHTTPServer(("", args.port), DashboardHandler)
    url = f"http://localhost:{args.port}/"
    print(f"Dashboard ready: {url}")

    if not args.no_open:
        try:
            import webbrowser
            threading.Timer(0.5, lambda: webbrowser.open(url)).start()
        except Exception:
            pass

    stopping = threading.Event()

    def shutdown(*_):
        if stopping.is_set():
            return
        stopping.set()
        print("\nShutting down workers...")
        for w in workers:
            try:
                w.proc.terminate()
            except Exception:
                pass
        server.shutdown()

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()

    try:
        while not stopping.is_set():
            if all(w.proc.poll() is not None for w in workers):
                # All workers finished -> keep the server up so the user can
                # still inspect the final state. Wait for Ctrl+C.
                pass
            time.sleep(0.5)
    finally:
        shutdown()
        for w in workers:
            try:
                w.proc.wait(timeout=2.0)
            except Exception:
                try:
                    w.proc.kill()
                except Exception:
                    pass
        server.server_close()


if __name__ == "__main__":
    main()
