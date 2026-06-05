#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="$SCRIPT_DIR/.env"

# Defaults — all overridden by .env
PORT=8080
WORKERS=15
MIN_SMALL=8
MIN_MEDIUM=12
MIN_LARGE=20
NH_SIZE_CAP=50
NH_CANDS=40
COOLDOWN_SEC=600
POLL_SEC=30

load_env() {
    [[ -f "$ENV_FILE" ]] || return 0
    # shellcheck disable=SC1090
    set -a; source "$ENV_FILE"; set +a
}

SERVER_PID=""
SERVING_PORT=""

start_server() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID"
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    python3 -m http.server "$PORT" --directory "$SCRIPT_DIR/results" \
        >> "$SCRIPT_DIR/results/server.log" 2>&1 &
    SERVER_PID=$!
    SERVING_PORT="$PORT"
    echo "[serve] http://localhost:$PORT/report.html  (pid=$SERVER_PID)"
}

env_mtime() {
    stat -f "%m" "$ENV_FILE" 2>/dev/null \
        || stat -c "%Y" "$ENV_FILE" 2>/dev/null \
        || echo "0"
}

# ── init ──────────────────────────────────────────────────────────────────────
load_env
start_server

while true; do
    echo ""
    echo "══════════════════════════════════════════════"
    echo " $(date '+%Y-%m-%d %H:%M:%S')"
    echo "══════════════════════════════════════════════"

    # 1. Pull & rebuild if upstream has new commits
    echo "[git] fetching..."
    if git -C "$SCRIPT_DIR" fetch origin 2>/dev/null; then
        LOCAL=$(git -C "$SCRIPT_DIR" rev-parse HEAD)
        REMOTE=$(git -C "$SCRIPT_DIR" rev-parse "@{u}" 2>/dev/null || echo "$LOCAL")
        if [[ "$LOCAL" != "$REMOTE" ]]; then
            echo "[git] new commits detected — pulling..."
            git -C "$SCRIPT_DIR" pull --ff-only \
                && (make -C "$SCRIPT_DIR" && echo "[build] OK") \
                || echo "[build] FAILED — continuing with existing binaries"
        else
            echo "[git] up to date"
        fi
    else
        echo "[git] fetch failed — skipping pull"
    fi

    # 2. Reload .env (picks up any changes since last iteration)
    load_env
    echo "[env] PORT=$PORT  WORKERS=$WORKERS  MIN_SMALL=$MIN_SMALL  MIN_MEDIUM=$MIN_MEDIUM  MIN_LARGE=$MIN_LARGE  COOLDOWN_SEC=$COOLDOWN_SEC"

    # Restart server if PORT changed between iterations
    if [[ "$SERVING_PORT" != "$PORT" ]]; then
        echo "[serve] port changed $SERVING_PORT → $PORT, restarting server..."
        start_server
    fi

    # 3. Run contest and generate report.html
    echo "[run] starting run_contest.py..."
    python3 "$SCRIPT_DIR/run_contest.py" \
        --workers        "$WORKERS"     \
        --minutes-small  "$MIN_SMALL"   \
        --minutes-medium "$MIN_MEDIUM"  \
        --minutes-large  "$MIN_LARGE"   \
        --nh-size-cap    "$NH_SIZE_CAP" \
        --nh-cands       "$NH_CANDS"    \
        && echo "[run] done" \
        || echo "[run] exited with non-zero status"

    # 4. Ensure HTTP server is still alive after the (possibly long) run
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "[serve] server died — restarting..."
        start_server
    fi

    # 5. Cooldown: sleep in POLL_SEC increments, reloading .env on change
    echo "[cooldown] ${COOLDOWN_SEC}s cooldown (checking .env every ${POLL_SEC}s)..."
    LAST_MTIME=$(env_mtime)
    elapsed=0

    while [[ $elapsed -lt $COOLDOWN_SEC ]]; do
        sleep "$POLL_SEC"
        elapsed=$((elapsed + POLL_SEC))

        NEW_MTIME=$(env_mtime)
        if [[ "$NEW_MTIME" != "$LAST_MTIME" ]]; then
            PREV_PORT="$PORT"
            load_env
            LAST_MTIME="$NEW_MTIME"
            echo "[env] .env changed — reloaded (PORT=$PORT WORKERS=$WORKERS COOLDOWN_SEC=$COOLDOWN_SEC)"
            if [[ "$PORT" != "$PREV_PORT" ]]; then
                echo "[serve] port changed $PREV_PORT → $PORT, restarting server..."
                start_server
            fi
        fi

        remaining=$((COOLDOWN_SEC - elapsed))
        [[ $remaining -gt 0 ]] && echo "[cooldown] ${remaining}s remaining..."
    done
done
