#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="$SCRIPT_DIR/.env"

# Defaults — all overridden by .env
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

env_mtime() {
    stat -f "%m" "$ENV_FILE" 2>/dev/null \
        || stat -c "%Y" "$ENV_FILE" 2>/dev/null \
        || echo "0"
}

# ── init ──────────────────────────────────────────────────────────────────────
load_env

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
    echo "[env] WORKERS=$WORKERS  MIN_SMALL=$MIN_SMALL  MIN_MEDIUM=$MIN_MEDIUM  MIN_LARGE=$MIN_LARGE  COOLDOWN_SEC=$COOLDOWN_SEC"

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

    # 4. Cooldown: sleep in POLL_SEC increments, reloading .env on change
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
