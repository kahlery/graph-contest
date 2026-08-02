#!/usr/bin/env bash
# final_push.sh - per-instance endgame runs against the remaining leaderboard gaps.
#
#   tools/final_push.sh <workdir> <minutes> <iterations>
#
# Unlike push.sh (one uniform config over every instance) this hard-codes the
# route each remaining instance actually needs, because they are no longer alike:
#
#   Automatic-7 / Automatic-9  balance from the cold stress+long-phase-1 snapshot,
#                              whose X is far below the champion's (A9 2029 vs
#                              3981, A7 7334 vs 9378). Low X is the whole gap on
#                              these two.
#   everything else            warm-refine the champion.
#
# All runs use --pnorm 0 (exponent from the live k); A9 and A7 additionally get a
# fixed-exponent sibling so auto can be checked against a hand-picked value.
# Champions are promoted only on strict improvement.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

WORK=${1:?usage: final_push.sh workdir minutes iterations}
MIN=${2:?}
ITERS=${3:-6}
SNAP=${SNAP:-}          # dir holding <graph>_snap1_phase1.json seeds

BASE=(--cands 4 --cands-ramp 1 --edge-move 5 --cands-mix 1 --bandit 1 --fit pnorm)
mkdir -p "$WORK/best" "$WORK/fp"

kof() {
    [ -f "$1" ] || { echo 999999999; return; }
    local k; k=$("$ROOT/bin/xstat" "$1" 2>/dev/null | sed -n 2p \
                 | grep -oE ' k=[0-9]+' | head -1 | tr -d ' k=')
    [ -n "$k" ] && echo "$k" || echo 999999999
}

run() {  # run <tag> <src> <extra-args...>
    local tag=$1 src=$2; shift 2
    "$ROOT/bin/sakgd" -i "$src" -o "$WORK/fp/$tag.json" -t "$MIN" -p1 0 \
        --init input -s "$((RANDOM))" "${BASE[@]}" "$@" \
        > "$WORK/fp/$tag.log" 2>&1
}

for it in $(seq 1 "$ITERS"); do
    echo "##### final_push iter $it $(date +%H:%M)"

    for g in Automatic-9 Automatic-7; do
        # Prefer this instance's own best result so far; fall back to the
        # low-X phase-1 snapshot on the first pass.
        src="$WORK/fp/${g}_auto.json"
        [ -f "$src" ] || src="$SNAP/${g}_snap1_phase1.json"
        [ -f "$src" ] || src="$WORK/best/$g.json"
        run "${g}_auto" "$src" --pnorm 0 &
    done
    run "Automatic-9_fix" "${SNAP:-$WORK/best}/Automatic-9_snap1_phase1.json" --pnorm 6 &
    run "Automatic-7_fix" "${SNAP:-$WORK/best}/Automatic-7_snap1_phase1.json" --pnorm 16 &
    for g in Automatic-3 Automatic-6 instance_08 instance_06; do
        run "${g}_auto" "$WORK/best/$g.json" --pnorm 0 &
    done
    wait

    for f in "$WORK"/fp/*.json; do
        case "$f" in *_snap*) continue ;; esac
        b=$(basename "$f" .json); g=${b%_auto}; g=${g%_fix}
        [ -f "$WORK/best/$g.json" ] || continue
        nk=$(kof "$f"); bk=$(kof "$WORK/best/$g.json")
        if [ "$nk" -lt "$bk" ]; then
            cp "$f" "$WORK/best/$g.json"
            echo "  promote $g: k $bk -> $nk"
        fi
    done
    python3 tools/score.py "$WORK/best"
done
