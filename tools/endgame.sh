#!/usr/bin/env bash
# endgame.sh - split the cores between the two remaining problems.
#
#   tools/endgame.sh <workdir> <minutes> <iterations>
#
# Half the cores run instance_08 under four *different search dynamics*, half
# warm-refine the four 2025-contest gaps.
#
# Why the split: instance_08 has converged to k~61 from every direction tried
# (warm chains, low-X snapshots, gradx, forced-low X), which makes it a local
# optimum of the search rather than of the tuning - so it gets the options that
# change how the walk moves (acceptance rule, whole-canvas argmin, deterministic
# repair, wider best-of-C) rather than another fitness tweak. The 2025 gaps are
# each within a few percent and still improving under plain warm refinement, so
# they just get more of it.
#
# Every run chains from its own previous output, keeping four independent
# instance_08 lineages instead of collapsing them onto one champion.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

WORK=${1:?usage: endgame.sh workdir minutes iterations}
MIN=${2:?}
ITERS=${3:-8}
BASE=(--cands 4 --cands-ramp 1 --edge-move 5 --cands-mix 1 --bandit 1 --fit pnorm --pnorm 0)
mkdir -p "$WORK/best" "$WORK/eg"

kof() {
    [ -f "$1" ] || { echo 999999999; return; }
    local k; k=$("$ROOT/bin/xstat" "$1" 2>/dev/null | sed -n 2p \
                 | grep -oE ' k=[0-9]+' | head -1 | tr -d ' k=')
    [ -n "$k" ] && echo "$k" || echo 999999999
}

# run <tag> <graph> <extra args...>  - chains from its own last output.
run() {
    local tag=$1 g=$2; shift 2
    local src="$WORK/eg/$tag.json"
    [ -f "$src" ] || src="$WORK/best/$g.json"
    cp "$src" "$WORK/eg/$tag.in.json"
    "$ROOT/bin/sakgd" -i "$WORK/eg/$tag.in.json" -o "$WORK/eg/$tag.json" \
        -t "$MIN" -p1 0 --init input -s "$RANDOM" "${BASE[@]}" "$@" \
        > "$WORK/eg/$tag.log" 2>&1
}

for it in $(seq 1 "$ITERS"); do
    echo "##### endgame iter $it $(date +%H:%M)"
    run i08_lahc instance_08 --accept lahc &
    run i08_vm   instance_08 --vm-grid 1 --level-clear 1 &
    run i08_krp  instance_08 --krepair 1 --polish 1 &
    run i08_c8   instance_08 --cands 8 --place smart &
    for g in Automatic-3 Automatic-6 Automatic-7 Automatic-9; do
        run "${g}_w" "$g" &
    done
    wait

    for f in "$WORK"/eg/*.json; do
        case "$f" in *.in.json|*_snap*) continue ;; esac
        b=$(basename "$f" .json)
        case "$b" in
            i08_*) g=instance_08 ;;
            *_w)   g=${b%_w} ;;
            *)     continue ;;
        esac
        nk=$(kof "$f"); bk=$(kof "$WORK/best/$g.json")
        if [ "$nk" -lt "$bk" ]; then
            cp "$f" "$WORK/best/$g.json"
            echo "  promote $g: k $bk -> $nk   [$b]"
        fi
    done
    echo "  i08 lineages: $(for t in lahc vm krp c8; do printf '%s=%s ' "$t" "$(kof "$WORK/eg/i08_$t.json")"; done)"
    python3 tools/score.py "$WORK/best"
done
