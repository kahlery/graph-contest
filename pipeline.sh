#!/usr/bin/env bash
# GD-2025 pipeline — pull, build, and keep the control server running.
# All run config (methods, budgets, workers, loop, etc.) is set via the web UI.
# Usage: ./pipeline.sh [--port 8080]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT=8080
while [[ $# -gt 0 ]]; do
    case "$1" in --port) PORT="$2"; shift 2;; *) shift;; esac
done

echo "[pipeline] GD-2025 starting — server will be at http://0.0.0.0:$PORT"

# ── initial pull & build ──────────────────────────────────────────────────────
pull_and_build() {
    echo "[git] fetching..."
    if git -C "$SCRIPT_DIR" fetch origin 2>/dev/null; then
        LOCAL=$(git -C "$SCRIPT_DIR" rev-parse HEAD)
        REMOTE=$(git -C "$SCRIPT_DIR" rev-parse "@{u}" 2>/dev/null || echo "$LOCAL")
        if [[ "$LOCAL" != "$REMOTE" ]]; then
            echo "[git] new commits — pulling..."
            git -C "$SCRIPT_DIR" pull --ff-only \
                && make -C "$SCRIPT_DIR" \
                && echo "[build] OK" \
                || echo "[build] FAILED — continuing with existing binaries"
        else
            echo "[git] up to date"
        fi
    else
        echo "[git] fetch failed — skipping pull"
    fi
}

pull_and_build

# ── server loop (restart on crash) ───────────────────────────────────────────
while true; do
    echo "[pipeline] $(date '+%Y-%m-%d %H:%M:%S') — starting server on port $PORT..."
    python3 "$SCRIPT_DIR/server.py" --port "$PORT" --no-browser
    echo "[pipeline] server exited — restarting in 5 s..."
    sleep 5
    pull_and_build
done
