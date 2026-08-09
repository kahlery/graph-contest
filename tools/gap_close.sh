#!/usr/bin/env bash
# gap_close.sh - exploratory (warm-chained) tuning for the last 4 leaderboard
# gaps: instance_08, Automatic-3/7/9. NOT contest-legal (warm-starts across
# rounds) - this is for finding what closes the gap, before reproducing the
# winning config as a single contest_run.sh cold pass. See tools/close.sh /
# tools/endgame.sh for the same pattern on earlier gaps.
#
#   tools/gap_close.sh <workdir> <minutes-per-round> <rounds>
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
WORK=${1:?usage: gap_close.sh workdir minutes rounds}; MIN=${2:?}; ROUNDS=${3:-6}
BASE=(--cands 4 --cands-ramp 1 --edge-move 5 --cands-mix 1 --bandit 1 --fit pnorm --pnorm 0 --swap 1)
mkdir -p "$WORK/best" "$WORK/gc"

kof() { [ -f "$1" ] || { echo 999999999; return; }
    local k; k=$("$ROOT/bin/xstat" "$1" 2>/dev/null | sed -n 2p | grep -oE ' k=[0-9]+' | grep -oE '[0-9]+')
    [ -n "$k" ] && echo "$k" || echo 999999999; }

run() { local tag=$1 g=$2; shift 2
    local src="$WORK/gc/$tag.json"; [ -f "$src" ] || src="$WORK/best/$g.json"
    cp "$src" "$WORK/gc/$tag.in.json"
    "$ROOT/bin/sakgd" -i "$WORK/gc/$tag.in.json" -o "$WORK/gc/$tag.json" -t "$MIN" \
        -p1 0 --init input -s "$RANDOM" "${BASE[@]}" "$@" > "$WORK/gc/$tag.log" 2>&1; }

for it in $(seq 1 "$ROUNDS"); do
    echo "##### gap_close iter $it $(date +%H:%M)"
    run i08_lahc  instance_08  --accept lahc &
    run i08_smart instance_08  --place smart --cands 8 &
    run a3_smart  Automatic-3  --place smart &
    run a3_c8     Automatic-3  --cands 8 &
    run a7_lahc   Automatic-7  --accept lahc &
    run a7_smart  Automatic-7  --place smart &
    run a9_vm     Automatic-9  --vm-grid 1 --level-clear 1 &
    run a9_krp    Automatic-9  --krepair 1 --polish 1 &
    wait

    for f in "$WORK"/gc/*.json; do
        case "$f" in *.in.json|*_snap*) continue ;; esac
        b=$(basename "$f" .json)
        case "$b" in
            i08_*) g=instance_08 ;;
            a3_*)  g=Automatic-3 ;;
            a7_*)  g=Automatic-7 ;;
            a9_*)  g=Automatic-9 ;;
            *) continue ;;
        esac
        nk=$(kof "$f"); bk=$(kof "$WORK/best/$g.json")
        if [ "$nk" -lt "$bk" ]; then
            cp "$f" "$WORK/best/$g.json"
            echo "  promote $g: k $bk -> $nk   [$b]"
        fi
    done
    python3 tools/score.py "$WORK/best"
done
