#!/usr/bin/env python3
"""score.py - grade a directory of layouts against the live GD-contest leaderboard.

    tools/score.py <dir> [<dir> ...]

For every *.json layout found, shells out to ./bin/xstat for the exact k, total
crossings X and balance floor ceil(2X/m), then prints them beside the current
leader's k for that instance. Two numbers matter per row:

  margin  = leader_k - our_k   (>0 means we are ahead; this is what we must grow)
  excess  = our_k - floor      (how much pure redistribution is still on the table)

The leaderboard ranks by sum over instances of best_k/your_k, so the final line
reports that score under the optimistic assumption that our k becomes the new
best where we lead.
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
XSTAT = os.path.join(ROOT, "bin", "xstat")

# Snapshot of https://mwallinger.algo.cit.tum.de/GD-Contest-Leaderboard/
# manifest.json taken 2026-08-01: best k over all teams, and our own submission.
# Refresh with tools/fetch_leaderboard.py when the leaderboard moves.
LEADER = {
    "instance_01": 0, "instance_02": 2, "instance_03": 9, "instance_04": 33,
    "instance_05": 186, "instance_06": 183, "instance_07": 1, "instance_08": 49,
    "instance_09": 3,
    "Automatic-1": 9, "Automatic-2": 3, "Automatic-3": 30, "Automatic-4": 6,
    "Automatic-5": 73, "Automatic-6": 504, "Automatic-7": 26, "Automatic-8": 6,
    "Automatic-9": 7,
}
OURS = {
    "instance_01": 0, "instance_02": 2, "instance_03": 11, "instance_04": 33,
    "instance_05": 223, "instance_06": 234, "instance_07": 1, "instance_08": 79,
    "instance_09": 3,
    "Automatic-1": 9, "Automatic-2": 3, "Automatic-3": 37, "Automatic-4": 6,
    "Automatic-5": 75, "Automatic-6": 737, "Automatic-7": 28, "Automatic-8": 6,
    "Automatic-9": 10,
}

PAT = re.compile(r"X=(\d+)\s+k=(\d+).*?floor=ceil\(2X/m\)=(\d+)")


def stat(path):
    out = subprocess.run([XSTAT, path], capture_output=True, text=True).stdout
    mt = PAT.search(out)
    if not mt:
        return None
    return int(mt.group(2)), int(mt.group(1)), int(mt.group(3))  # k, X, floor


def main():
    dirs = sys.argv[1:] or ["."]
    rows = {}
    for d in dirs:
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".json"):
                continue
            g = fn[:-5]
            # bench writes <graph>.json; the solver also drops _snapN_* files.
            if "_snap" in g:
                continue
            if g not in LEADER:
                continue
            s = stat(os.path.join(d, fn))
            if s and (g not in rows or s[0] < rows[g][0]):
                rows[g] = s

    if not rows:
        print("no scored layouts found in", dirs)
        return 1

    print("%-14s %6s %6s %8s %7s %7s %8s" %
          ("instance", "k", "leader", "margin", "floor", "excess", "X"))
    print("-" * 62)
    beat = tie = behind = 0
    for g in sorted(LEADER, key=lambda x: (x.startswith("A"), x)):
        if g not in rows:
            continue
        k, X, fl = rows[g]
        lead = LEADER[g]
        margin = lead - k
        flag = "WIN " if margin > 0 else ("=   " if margin == 0 else "LOSE")
        if margin > 0: beat += 1
        elif margin == 0: tie += 1
        else: behind += 1
        print("%-14s %6d %6d %+8d %7d %7d %8d  %s" %
              (g, k, lead, margin, fl, k - fl, X, flag))

    score = 0.0
    for g in LEADER:
        if g in rows:
            best = min(rows[g][0], LEADER[g])
            score += best / max(1e-9, rows[g][0]) if rows[g][0] else 1.0
        else:
            best = min(LEADER[g], OURS[g])
            score += best / max(1e-9, OURS[g]) if OURS[g] else 1.0
    print("-" * 62)
    print("beat=%d tie=%d behind=%d   (scored %d/%d instances)  score~%.3f/18" %
          (beat, tie, behind, len(rows), len(LEADER), score))
    return 0


if __name__ == "__main__":
    sys.exit(main())
