#!/usr/bin/env bash
# push.sh - one improvement round over a set of instances, keeping a running best.
#
#   tools/push.sh <workdir> <minutes> <p1-min> [solver args...]
#
# Each round runs every selected instance once, warm-starting from that
# instance's current champion (<workdir>/best/<graph>.json) when one exists, and
# promotes the result only if it strictly improves (lower k, ties broken by
# fewer total crossings). Champions therefore never regress, so the script is
# safe to run in a loop for as long as there is time.
#
# Env:
#   ONLY="instance_05 Automatic-6"  restrict to these graphs
#   JOBS=8                          concurrent solvers (default: core count)
#   SEED=...                        base seed (default: derived from the clock)
#   COLD=1                          ignore champions, start from the raw input
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

WORK=${1:?usage: push.sh workdir minutes p1min [args...]}; shift
MIN=${1:?}; shift
P1=${1:?}; shift
ARGS=("$@")

JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}
SEED=${SEED:-$(date +%s)}
ONLY=${ONLY:-}
COLD=${COLD:-0}

mkdir -p "$WORK/best" "$WORK/round"
rm -f "$WORK"/round/*.json "$WORK"/round/*.log 2>/dev/null

# k of a layout, or a huge sentinel when the file is missing/unparsable, so
# plain string compare against a real k always prefers the real one.
kof() {
    [ -f "$1" ] || { echo 999999999; return; }
    local l; l=$("$ROOT/bin/xstat" "$1" 2>/dev/null | sed -n 2p)
    local k; k=$(echo "$l" | grep -oE ' k=[0-9]+' | head -1 | tr -d ' k=')
    [ -n "$k" ] && echo "$k" || echo 999999999
}
xof() {
    [ -f "$1" ] || { echo 999999999; return; }
    local l; l=$("$ROOT/bin/xstat" "$1" 2>/dev/null | sed -n 2p)
    local x; x=$(echo "$l" | grep -oE 'X=[0-9]+' | head -1 | cut -d= -f2)
    [ -n "$x" ] && echo "$x" || echo 999999999
}

running=0
for f in data/input/internal-2026/*.json data/input/intermediate-contest/*.json; do
    g=$(basename "$f" .json)
    if [ -n "$ONLY" ]; then case " $ONLY " in *" $g "*) ;; *) continue ;; esac; fi

    src="$f"; extra=(); p1="$P1"
    if [ "$COLD" != "1" ] && [ -f "$WORK/best/$g.json" ]; then
        src="$WORK/best/$g.json"
        extra=(--init input)   # keep the champion's coordinates, refine them
        # ...and skip phase 1 entirely. Phase 1 is a hot anneal on *total*
        # crossings; run against an already-optimised champion it tears the
        # layout apart, and phase 2 then restarts from the wreckage rather than
        # from the champion. (The champion itself is never lost - the solver
        # writes its best-ever layout - but the round is wasted.)
        p1=0
    fi

    nohup ./bin/sakgd -i "$src" -o "$WORK/round/$g.json" -t "$MIN" -p1 "$p1" \
        -s "$((SEED + RANDOM % 100000))" ${extra[@]+"${extra[@]}"} ${ARGS[@]+"${ARGS[@]}"} \
        > "$WORK/round/$g.log" 2>&1 &

    running=$((running + 1))
    if [ "$running" -ge "$JOBS" ]; then wait -n 2>/dev/null || wait; running=$((running - 1)); fi
done
wait

promoted=0
for j in "$WORK"/round/*.json; do
    [ -f "$j" ] || continue
    g=$(basename "$j" .json)
    case "$g" in *_snap*) continue ;; esac
    nk=$(kof "$j");            bk=$(kof "$WORK/best/$g.json")
    if [ "$nk" -lt "$bk" ] || { [ "$nk" -eq "$bk" ] && [ "$(xof "$j")" -lt "$(xof "$WORK/best/$g.json")" ]; }; then
        cp "$j" "$WORK/best/$g.json"; promoted=$((promoted + 1))
        echo "  promote $g: k $bk -> $nk"
    fi
done
echo "== round done, $promoted champions improved"
python3 tools/score.py "$WORK/best"
