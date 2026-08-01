#!/usr/bin/env bash
# bench.sh - run one solver config across a set of instances and report k
# against the current leaderboard targets.
#
#   tools/bench.sh <outdir> <minutes> <p1-minutes> <seed> [solver args...]
#
# Runs at most $JOBS instances concurrently (default: core count), then prints a
# table of k / floor / excess per instance next to the leader's k, so a config
# is judged by the margin it leaves rather than by raw k.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

OUT=${1:?usage: bench.sh outdir minutes p1min seed [args...]}; shift
MIN=${1:?}; shift
P1=${1:?}; shift
SEED=${1:?}; shift
ARGS=("$@")

JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}
# Restrict to a subset with e.g. ONLY="instance_05 Automatic-6".
ONLY=${ONLY:-}

mkdir -p "$OUT"

specs=()
for f in data/input/internal-2026/*.json data/input/intermediate-contest/*.json; do
    g=$(basename "$f" .json)
    if [ -n "$ONLY" ]; then case " $ONLY " in *" $g "*) ;; *) continue ;; esac; fi
    specs+=("$f")
done

running=0
for f in "${specs[@]}"; do
    g=$(basename "$f" .json)
    nohup ./bin/sakgd -i "$f" -o "$OUT/$g.json" -t "$MIN" -p1 "$P1" -s "$SEED" \
        "${ARGS[@]}" > "$OUT/$g.log" 2>&1 &
    running=$((running+1))
    if [ "$running" -ge "$JOBS" ]; then wait -n 2>/dev/null || wait; running=$((running-1)); fi
done
wait

echo "== config: -t $MIN -p1 $P1 -s $SEED ${ARGS[*]}"
python3 tools/score.py "$OUT"
