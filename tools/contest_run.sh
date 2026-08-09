#!/usr/bin/env bash
# contest_run.sh - one contest-legal pass over an input set.
#
#   tools/contest_run.sh <input-set> <outdir> [minutes-per-graph]
#
# Contest budget is one hour PER GRAPH, with every graph of the set running
# inside the same one-hour wall-clock window - i.e. all graphs in parallel, one
# worker each. Nothing here may carry state between runs: no warm-starting from
# a previous champion, no second pass. Each graph gets exactly one cold run.
#
# (This matters. Champions built by chaining 20-30 minute rounds over several
# hours are not reproducible under this budget and are not legitimate results.)
#
# Per-graph route, chosen by how tightly the canvas is packed
# (nodes per available integer point):
#   dense  >= 0.03  ->  stress_init (graphviz sfdp/neato). gradx optimises in the
#                       unit square and rounding to so few free points wrecks it;
#                       measured, stress wins clearly on Automatic-7/8/9.
#   sparse <  0.03  ->  gradx_init (gradient descent on the SigmoidX crossing
#                       surrogate). Reaches a far lower total crossing count than
#                       any SA phase 1, which lowers the balance floor ceil(2X/m)
#                       and so the whole ceiling on what balancing can reach.
#
# The SA then runs from those coordinates with --p1 0 (phase 1 is a hot anneal on
# *total* crossings and destroys an already-sparse init) and --pnorm 0 (exponent
# from the live k; a fixed one cannot serve k=10 and k=520 in the same set).
#
# ENSEMBLE=dual (env var, off by default): per course-staff guidance
# (2026-07-14 forum: try several independent initializations breadth-first and
# keep whichever gets furthest, rather than trusting one deterministic route)
# runs stress_init AND gradx_init in parallel per graph - same isec budget,
# wall-clock unchanged - and keeps whichever reaches the lower k (ties broken
# by totalX). Doubles per-graph core usage, so only use it when running a
# handful of graphs, not a full 9-graph set on an 8-core box.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SET=${1:?usage: contest_run.sh input-set outdir [minutes]}
OUT=${2:?}
MIN=${3:-60}
INITFRAC=${INITFRAC:-0.12}
ENSEMBLE=${ENSEMBLE:-single}

mkdir -p "$OUT"
BASE=(--cands 4 --cands-ramp 1 --edge-move 5 --cands-mix 1 --bandit 1
      --fit pnorm --pnorm 0 --swap 1)

kx() {  # kx <file> -> "k totalX", 999999999 999999999 if missing/unparseable
    local out k X
    out=$("$ROOT/bin/xstat" "$1" 2>/dev/null | sed -n 2p)
    k=$(grep -oE ' k=[0-9]+' <<<"$out" | grep -oE '[0-9]+')
    X=$(grep -oE 'X=[0-9]+' <<<"$out" | grep -oE '[0-9]+')
    echo "${k:-999999999} ${X:-999999999}"
}

start=$(date +%s)
for f in "data/input/$SET"/*.json; do
    g=$(basename "$f" .json)
    (
        read -r n m dens <<<"$(python3 - "$f" <<'PY'
import json,sys
d=json.load(open(sys.argv[1]))
n=len(d["nodes"]); m=len(d["edges"])
pts=(int(d.get("width",1))+1)*(int(d.get("height",1))+1)
print(n, m, n/max(1,pts))
PY
)"
        isec=$(python3 -c "print(max(5,int($MIN*60*$INITFRAC)))")
        sasec=$(python3 -c "print(max(0.5,$MIN*(1-$INITFRAC)))")

        if [ "$ENSEMBLE" = "dual" ]; then
            ./bin/stress_init -i "$f" -o "$OUT/$g.stress.json" -t "$isec" -s 1 \
                > "$OUT/$g.stress.log" 2>&1 &
            ./bin/gradx_init -i "$f" -o "$OUT/$g.gradx.json" -t "$isec" \
                --threads 1 -s 1 > "$OUT/$g.gradx.log" 2>&1 &
            wait
            read -r sk sX <<<"$(kx "$OUT/$g.stress.json")"
            read -r gk gX <<<"$(kx "$OUT/$g.gradx.json")"
            if [ "$gk" -lt "$sk" ] || { [ "$gk" -eq "$sk" ] && [ "$gX" -lt "$sX" ]; }; then
                route=gradx-dual; cp "$OUT/$g.gradx.json" "$OUT/$g.init.json"
            else
                route=stress-dual; cp "$OUT/$g.stress.json" "$OUT/$g.init.json"
            fi
            echo "dual: stress k=$sk X=$sX | gradx k=$gk X=$gX -> $route" \
                > "$OUT/$g.init.log"
        elif python3 -c "import sys;sys.exit(0 if $dens>=0.03 or $m>6000 else 1)"; then
            route=stress
            ./bin/stress_init -i "$f" -o "$OUT/$g.init.json" -t "$isec" -s 1 \
                > "$OUT/$g.init.log" 2>&1
        else
            route=gradx
            ./bin/gradx_init -i "$f" -o "$OUT/$g.init.json" -t "$isec" \
                --threads 1 -s 1 > "$OUT/$g.init.log" 2>&1
        fi

        if [ -f "$OUT/$g.init.json" ]; then
            src="$OUT/$g.init.json"; p1=0
        else
            # Init failed - fall back to a normal cold run so the graph still
            # produces a layout rather than nothing.
            src="$f"; p1=$(python3 -c "print(round($MIN*0.15,2))"); route="$route(fallback)"
        fi
        echo "$g route=$route n=$n m=$m dens=$dens" > "$OUT/$g.route"
        ./bin/sakgd -i "$src" -o "$OUT/$g.json" -t "$sasec" -p1 "$p1" \
            --init input -s 1 "${BASE[@]}" > "$OUT/$g.log" 2>&1
    ) &
done
wait
end=$(date +%s)

echo "== $SET: wall clock $(( (end-start)/60 ))m $(( (end-start)%60 ))s (budget ${MIN}m)"
for f in "$OUT"/*.json; do
    case "$f" in *.init.json|*.stress.json|*.gradx.json|*_snap*) continue ;; esac
    printf '%-16s %s  [%s]\n' "$(basename "$f" .json)" \
        "$("$ROOT/bin/xstat" "$f" | sed -n 2p | grep -oE 'X=[0-9]+ +k=[0-9]+')" \
        "$(cut -d' ' -f2 < "${f%.json}.route" 2>/dev/null)"
done
python3 tools/score.py "$OUT"
