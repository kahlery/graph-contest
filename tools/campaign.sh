#!/usr/bin/env bash
# campaign.sh - run improvement rounds forever, keeping a never-regressing champion
# per instance.
#
#   tools/campaign.sh <workdir> <minutes-per-round> [solver args...]
#
# Round 1 starts cold from the raw inputs; every later round warm-starts each
# instance from its own champion with a fresh seed, so the search keeps
# diversifying while results can only improve (tools/push.sh promotes on strict
# improvement only). Runs until killed - check progress any time with
#   python3 tools/score.py <workdir>/best
#
# Env: ONLY / JOBS / MAXROUNDS as in push.sh.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

WORK=${1:?usage: campaign.sh workdir minutes [args...]}; shift
MIN=${1:?}; shift
ARGS=("$@")
MAXROUNDS=${MAXROUNDS:-1000}

mkdir -p "$WORK"
round=0
while [ "$round" -lt "$MAXROUNDS" ]; do
    round=$((round + 1))
    echo "===== round $round  ($(date +%H:%M:%S))  ${MIN}min/instance"
    # Round 1 is the only cold one: with no champion yet there is nothing to
    # warm-start from, and push.sh falls back to the raw input anyway.
    SEED=$((RANDOM * 32768 + RANDOM)) tools/push.sh "$WORK" "$MIN" 2 ${ARGS[@]+"${ARGS[@]}"}
done
