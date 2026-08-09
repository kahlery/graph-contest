#!/usr/bin/env bash
# gap_break.sh - one round of genuinely-different-algorithm attempts against
# the 4 gaps that didn't move under gap_close.sh's search-dynamic variants
# (8 lineages, 8 rounds, zero promotions -> hard local optima for the
# vertex-move SA, not an SA-tuning problem). Exploratory / warm-chained,
# not contest-legal.
#
#   tools/gap_break.sh <workdir> <minutes>
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
WORK=${1:?usage: gap_break.sh workdir minutes}; MIN=${2:?}
BASE=(--cands 4 --cands-ramp 1 --edge-move 5 --cands-mix 1 --bandit 1 --fit pnorm --pnorm 0 --swap 1)
mkdir -p "$WORK/gb"

kof() { [ -f "$1" ] || { echo 999999999; return; }
    local k; k=$("$ROOT/bin/xstat" "$1" 2>/dev/null | sed -n 2p | grep -oE ' k=[0-9]+' | grep -oE '[0-9]+')
    [ -n "$k" ] && echo "$k" || echo 999999999; }

in_for() { case "$1" in instance_08) echo data/input/internal-2026/instance_08.json ;; *) echo "data/input/intermediate-contest/$1.json" ;; esac; }

# tripod: cold structural init + full SA
tripod_run() { local g=$1
    ./bin/tripod_init -i "$(in_for "$g")" -o "$WORK/gb/${g}_tripod.init.json" -s "$RANDOM" \
        > "$WORK/gb/${g}_tripod.init.log" 2>&1
    ./bin/sakgd -i "$WORK/gb/${g}_tripod.init.json" -o "$WORK/gb/${g}_tripod.json" \
        -t "$MIN" -p1 0 --init input -s "$RANDOM" "${BASE[@]}" \
        > "$WORK/gb/${g}_tripod.log" 2>&1
}

# approach1 ILS: warm from current best
ils_run() { local g=$1
    ./bin/approach1 -i "$WORK/best/$g.json" -o "$WORK/gb/${g}_ils.json" \
        -t "$MIN" -p1 0 --mode ils --init input -s "$RANDOM" \
        > "$WORK/gb/${g}_ils.log" 2>&1
}

# approach1 LNS-adaptive: warm from current best
lns_run() { local g=$1
    ./bin/approach1 -i "$WORK/best/$g.json" -o "$WORK/gb/${g}_lns.json" \
        -t "$MIN" -p1 0 --mode lns-adaptive --init input -s "$RANDOM" \
        > "$WORK/gb/${g}_lns.log" 2>&1
}

echo "##### gap_break $(date +%H:%M), budget ${MIN}m per job"
tripod_run instance_08 &
ils_run    instance_08 &
tripod_run Automatic-7 &
ils_run    Automatic-7 &
tripod_run Automatic-9 &
lns_run    Automatic-9 &
tripod_run Automatic-3 &
ils_run    Automatic-3 &
wait

for f in "$WORK"/gb/*.json; do
    case "$f" in *.init.json|*_snap*) continue ;; esac
    b=$(basename "$f" .json)
    g=${b%_tripod}; g=${g%_ils}; g=${g%_lns}
    [ -f "$WORK/best/$g.json" ] || continue
    nk=$(kof "$f"); bk=$(kof "$WORK/best/$g.json")
    tag="LOSE"; [ "$nk" -lt "$bk" ] && tag="BREAKTHROUGH"
    echo "  $b: k=$nk  (current best $g=$bk)  $tag"
    if [ "$nk" -lt "$bk" ]; then
        cp "$f" "$WORK/best/$g.json"
        echo "  promote $g: k $bk -> $nk   [$b]"
    fi
done
python3 tools/score.py "$WORK/best"
