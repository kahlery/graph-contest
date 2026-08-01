#!/usr/bin/env bash
# sweep.sh - run several solver configs on one instance and rank them by k.
#
#   tools/sweep.sh <outdir> <instance-path> <minutes> <p1-min> <seed> <spec>...
#
# Each <spec> is "name:args...", e.g. "p16:--fit pnorm --pnorm 16".
# Configs run concurrently (one core each), so keep the count <= core count.
# BIN=bin/sakgd_tgt tools/sweep.sh ...  selects a different solver binary.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

OUT=${1:?usage: sweep.sh outdir instance minutes p1min seed spec...}; shift
G=${1:?}; shift
MIN=${1:?}; shift
P1=${1:?}; shift
SEED=${1:?}; shift
BIN=${BIN:-bin/sakgd}

mkdir -p "$OUT"
for spec in "$@"; do
    name=${spec%%:*}; args=${spec#*:}
    # shellcheck disable=SC2086
    nohup "$ROOT/$BIN" -i "$G" -o "$OUT/$name.json" -t "$MIN" -p1 "$P1" \
        -s "$SEED" $args > "$OUT/$name.log" 2>&1 &
done
echo "launched $# configs on $(basename "$G") (${MIN}min, p1=${P1}min, seed $SEED)"
