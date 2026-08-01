#!/usr/bin/env bash
# rank.sh - rank the layouts a sweep produced, best k first.
#   tools/rank.sh <sweepdir>
# Falls back to the running log's bestK when a config has not written its
# output file yet, so it is also useful mid-sweep.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
D=${1:?usage: rank.sh sweepdir}

for f in "$D"/*.log; do
    name=$(basename "$f" .log)
    j="$D/$name.json"
    if [ -f "$j" ]; then
        line=$("$ROOT/bin/xstat" "$j" | sed -n 2p)
        k=$(echo "$line"   | grep -oE 'k=[0-9]+'      | head -1 | cut -d= -f2)
        X=$(echo "$line"   | grep -oE 'X=[0-9]+'      | head -1 | cut -d= -f2)
        fl=$(echo "$line"  | grep -oE 'floor=ceil\(2X/m\)=[0-9]+' | cut -d= -f3)
        printf '%6s %8s %10s %8s  %s\n' "$k" "$fl" "$X" "done" "$name"
    else
        p=$(grep -oE 'bestK=[0-9]+ bestX=[0-9]+' "$f" | tail -1)
        k=$(echo "$p" | grep -oE 'bestK=[0-9]+' | cut -d= -f2)
        X=$(echo "$p" | grep -oE 'bestX=[0-9]+' | cut -d= -f2)
        printf '%6s %8s %10s %8s  %s\n' "${k:--}" "-" "${X:--}" "running" "$name"
    fi
done | sort -n | awk 'BEGIN{printf "%6s %8s %10s %8s  %s\n","k","floor","X","state","config"}{print}'
