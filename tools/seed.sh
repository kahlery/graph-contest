#!/usr/bin/env bash
# seed.sh - cold-start champions from a constructive init, then anneal.
#
#   INIT=gradx|stress tools/seed.sh <workdir> <total-minutes> [solver args...]
#
# Spends GXFRAC of each instance's budget building an initial layout and the
# rest in the SA, which continues from those coordinates (--init input -p1 0:
# both inits are already crossing-sparse and the hot phase-1 total-crossings
# anneal destroys them).
#
#   stress (default) - graphviz sfdp/neato force-directed placement. This is
#     what the submitted Automatic-7=28 / Automatic-9=10 came from; the plain
#     --init auto family (input/bfs-snake/hilbert) has no force-directed member
#     and lands far worse on the tight-canvas intermediate-contest instances.
#   gradx - gradient descent on the SigmoidX crossing surrogate.
#
# Results are promoted into <workdir>/best only when they beat the incumbent, so
# this composes with tools/push.sh warm rounds.
#
# Env: ONLY, JOBS, SEED as in push.sh. GXFRAC (default 0.15).
# gradx_init builds the full non-adjacent edge-pair list, which is O(m^2); at
# m=20288 Automatic-8 alone would need ~1.6 GB, so instances above MAXM are
# skipped here and left to the plain SA path.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

WORK=${1:?usage: seed.sh workdir minutes [args...]}; shift
MIN=${1:?}; shift
ARGS=("$@")

JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}
SEED=${SEED:-$(date +%s)}
ONLY=${ONLY:-}
GXFRAC=${GXFRAC:-0.15}
MAXM=${MAXM:-6000}
INIT=${INIT:-stress}

mkdir -p "$WORK/best" "$WORK/seed"

kof() {
    [ -f "$1" ] || { echo 999999999; return; }
    local k; k=$("$ROOT/bin/xstat" "$1" 2>/dev/null | sed -n 2p | grep -oE ' k=[0-9]+' | head -1 | tr -d ' k=')
    [ -n "$k" ] && echo "$k" || echo 999999999
}

one() {
    local f=$1 g gxsec sasec m
    g=$(basename "$f" .json)
    m=$(python3 -c "import json,sys;print(len(json.load(open(sys.argv[1]))['edges']))" "$f")

    gxsec=$(python3 -c "print(max(5,int($MIN*60*$GXFRAC)))")
    sasec=$(python3 -c "print(max(1.0,$MIN*(1-$GXFRAC)))")

    if [ "$INIT" = "gradx" ]; then
        # gradx_init materialises the full non-adjacent edge-pair list, which is
        # O(m^2) - ~1.6 GB at Automatic-8's m=20288 - so large graphs are left
        # to the stress path.
        [ "$m" -gt "$MAXM" ] && { echo "  skip $g (m=$m > $MAXM for gradx)"; return; }
        ./bin/gradx_init -i "$f" -o "$WORK/seed/$g.gx.json" -t "$gxsec" \
            --threads 1 -s "$((SEED % 100000))" > "$WORK/seed/$g.gx.log" 2>&1
    else
        ./bin/stress_init -i "$f" -o "$WORK/seed/$g.gx.json" -t "$gxsec" \
            -s "$((SEED % 100000))" > "$WORK/seed/$g.gx.log" 2>&1
    fi
    [ -f "$WORK/seed/$g.gx.json" ] || { echo "  $INIT init failed for $g"; return; }

    ./bin/sakgd -i "$WORK/seed/$g.gx.json" -o "$WORK/seed/$g.json" \
        -t "$sasec" -p1 0 --init input -s "$((SEED + 7))" ${ARGS[@]+"${ARGS[@]}"} \
        > "$WORK/seed/$g.log" 2>&1
}

running=0
for f in data/input/internal-2026/*.json data/input/intermediate-contest/*.json; do
    g=$(basename "$f" .json)
    if [ -n "$ONLY" ]; then case " $ONLY " in *" $g "*) ;; *) continue ;; esac; fi
    one "$f" &
    running=$((running + 1))
    if [ "$running" -ge "$JOBS" ]; then wait -n 2>/dev/null || wait; running=$((running - 1)); fi
done
wait

for j in "$WORK"/seed/*.json; do
    case "$j" in *.gx.json) continue ;; esac
    [ -f "$j" ] || continue
    g=$(basename "$j" .json)
    case "$g" in *_snap*) continue ;; esac
    nk=$(kof "$j"); bk=$(kof "$WORK/best/$g.json")
    if [ "$nk" -lt "$bk" ]; then
        cp "$j" "$WORK/best/$g.json"; echo "  promote $g: k $bk -> $nk"
    fi
done
python3 tools/score.py "$WORK/best"
