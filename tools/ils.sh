#!/usr/bin/env bash
# ils.sh - iterated local search: perturb a stalled champion, then re-cool.
#
#   tools/ils.sh <workdir> <minutes> <iterations>
#
# Every warm lineage has plateaued (Automatic-3 stuck at 31, Automatic-6 at
# 504/505, Automatic-7 at 28, instance_08 at 61) and cold restarts land far
# worse, so neither more refinement nor more diversity helps. This instead
# raises the phase-2 starting temperature so the walk is kicked out of the
# basin and allowed to re-descend - the champion is never at risk because the
# solver only ever writes its best-ever layout, and promotion is on strict
# improvement.
#
# Two kick strengths per instance: --p2-t0 defaults to 1.0, so 2 and 4 are a
# mild and a hard shove.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
WORK=${1:?usage: ils.sh workdir minutes iterations}; MIN=${2:?}; ITERS=${3:-12}
BASE=(--cands 4 --cands-ramp 1 --edge-move 5 --cands-mix 1 --bandit 1 --fit pnorm --pnorm 0)
mkdir -p "$WORK/best" "$WORK/ils"

kof() { [ -f "$1" ] || { echo 999999999; return; }
    local k; k=$("$ROOT/bin/xstat" "$1" 2>/dev/null | sed -n 2p | grep -oE ' k=[0-9]+' | head -1 | tr -d ' k=')
    [ -n "$k" ] && echo "$k" || echo 999999999; }

# Always kick from the current champion, not from the previous kick's output:
# a chain of hot restarts just random-walks away from the good layout.
run() { local tag=$1 g=$2 t0=$3
    cp "$WORK/best/$g.json" "$WORK/ils/$tag.in.json"
    "$ROOT/bin/sakgd" -i "$WORK/ils/$tag.in.json" -o "$WORK/ils/$tag.json" -t "$MIN" \
        -p1 0 --init input -s "$RANDOM" "${BASE[@]}" --p2-t0 "$t0" \
        > "$WORK/ils/$tag.log" 2>&1; }

for it in $(seq 1 "$ITERS"); do
    echo "##### ils iter $it $(date +%H:%M)"
    for g in Automatic-3 Automatic-6 Automatic-7; do
        run "${g}_k2" "$g" 2 &
        run "${g}_k4" "$g" 4 &
    done
    run Automatic-9_k3 Automatic-9 3 &
    run instance_08_k3 instance_08 3 &
    wait
    for f in "$WORK"/ils/*.json; do
        case "$f" in *.in.json|*_snap*) continue ;; esac
        b=$(basename "$f" .json); g=${b%_k[0-9]}
        [ -f "$WORK/best/$g.json" ] || continue
        nk=$(kof "$f"); bk=$(kof "$WORK/best/$g.json")
        [ "$nk" -lt "$bk" ] && { cp "$f" "$WORK/best/$g.json"; echo "  promote $g: k $bk -> $nk  [$b]"; }
    done
    python3 tools/score.py "$WORK/best"
done
