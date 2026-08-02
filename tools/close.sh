#!/usr/bin/env bash
# close.sh - endgame allocation against the gaps that are still closing.
#
#   tools/close.sh <workdir> <minutes> <iterations>
#
# Two independent lineages each for Automatic-3/6/7 (the three gaps within a few
# points and still descending), one for Automatic-9, one maintenance lineage for
# instance_08. Two lineages per instance because a single warm chain stalls,
# while independent chains from the same champion diverge and one usually keeps
# moving after the other stops.
#
# instance_08 gets only one: it has converged to k~61 from nine directions, and
# four independent cold multi-starts landed at 69-83, so extra cores there buy
# nothing.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
WORK=${1:?usage: close.sh workdir minutes iterations}; MIN=${2:?}; ITERS=${3:-12}
BASE=(--cands 4 --cands-ramp 1 --edge-move 5 --cands-mix 1 --bandit 1 --fit pnorm --pnorm 0)
mkdir -p "$WORK/best" "$WORK/cl"

kof() { [ -f "$1" ] || { echo 999999999; return; }
    local k; k=$("$ROOT/bin/xstat" "$1" 2>/dev/null | sed -n 2p | grep -oE ' k=[0-9]+' | head -1 | tr -d ' k=')
    [ -n "$k" ] && echo "$k" || echo 999999999; }

run() { local tag=$1 g=$2; shift 2
    local src="$WORK/cl/$tag.json"; [ -f "$src" ] || src="$WORK/best/$g.json"
    cp "$src" "$WORK/cl/$tag.in.json"
    "$ROOT/bin/sakgd" -i "$WORK/cl/$tag.in.json" -o "$WORK/cl/$tag.json" -t "$MIN" \
        -p1 0 --init input -s "$RANDOM" "${BASE[@]}" "$@" > "$WORK/cl/$tag.log" 2>&1; }

for it in $(seq 1 "$ITERS"); do
    echo "##### close iter $it $(date +%H:%M)"
    for g in Automatic-3 Automatic-6 Automatic-7; do
        run "${g}_a" "$g" &
        run "${g}_b" "$g" --place smart &
    done
    run Automatic-9_a Automatic-9 &
    run instance_08_a instance_08 &
    wait
    for f in "$WORK"/cl/*.json; do
        case "$f" in *.in.json|*_snap*) continue ;; esac
        b=$(basename "$f" .json); g=${b%_a}; g=${g%_b}
        [ -f "$WORK/best/$g.json" ] || continue
        nk=$(kof "$f"); bk=$(kof "$WORK/best/$g.json")
        [ "$nk" -lt "$bk" ] && { cp "$f" "$WORK/best/$g.json"; echo "  promote $g: k $bk -> $nk"; }
    done
    python3 tools/score.py "$WORK/best"
done
