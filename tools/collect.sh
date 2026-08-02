#!/usr/bin/env bash
# collect.sh - assemble the best valid layout per instance into a submission set.
#
#   tools/collect.sh <outdir> <searchdir> [<searchdir> ...]
#
# Scans every *.json under the search directories, identifies which instance
# each belongs to by its (node count, edge count), keeps the lowest k (ties
# broken by fewer total crossings), and copies the winners into
# <outdir>/<set>/<graph>.json.
#
# Every candidate is validated before it can win, so a layout that breaks a
# contest rule can never reach the submission set - an invalid drawing scores
# as INVALID, not merely badly. The final report lists any instance with no
# valid candidate at all.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

OUT=${1:?usage: collect.sh outdir searchdir...}; shift
[ "$#" -ge 1 ] || { echo "need at least one search dir" >&2; exit 2; }

python3 - "$OUT" "$@" <<'PYEOF'
import json, glob, os, re, shutil, subprocess, sys

out, dirs = sys.argv[1], sys.argv[2:]

# (n, m) identifies the instance; layouts are copies of the input graph.
ref = {}
for f in glob.glob("data/input/*/*.json"):
    d = json.load(open(f))
    iset = os.path.basename(os.path.dirname(f))
    ref[(len(d["nodes"]), len(d["edges"]))] = (iset, os.path.basename(f)[:-5], f)

pat = re.compile(r"X=(\d+)\s+k=(\d+)")
best = {}
seen = 0
for d in dirs:
    for f in glob.glob(os.path.join(d, "**", "*.json"), recursive=True):
        if "_snap" in f or f.endswith(".gx.json"):
            continue
        try:
            g = json.load(open(f))
            key = ref.get((len(g["nodes"]), len(g["edges"])))
            if not key:
                continue
            o = subprocess.run([os.path.join("bin", "xstat"), f],
                               capture_output=True, text=True).stdout
            mt = pat.search(o)
            if not mt:
                continue
            seen += 1
            X, k = int(mt.group(1)), int(mt.group(2))
            name = key[1]
            if name not in best or (k, X) < best[name][:2]:
                best[name] = (k, X, f, key)
        except Exception:
            pass

print("scanned %d parsable layouts across %d dirs" % (seen, len(dirs)))
ok = 0
for name, (k, X, f, (iset, _, inp)) in sorted(best.items()):
    dst_dir = os.path.join(out, iset)
    os.makedirs(dst_dir, exist_ok=True)
    dst = os.path.join(dst_dir, name + ".json")
    v = subprocess.run(["python3", "tools/validate.py", inp, f],
                       capture_output=True, text=True).stdout
    if "INVALID" in v:
        print("  SKIP  %-14s k=%-6d invalid: %s" % (name, k, v.strip().splitlines()[0]))
        continue
    shutil.copy(f, dst)
    ok += 1
    print("  ok    %-14s k=%-6d X=%-8d <- %s" % (name, k, X, f))

missing = sorted({v[1] for v in ref.values()} - set(best))
print("\n%d instances collected; %d missing: %s" % (ok, len(missing), ", ".join(missing) or "none"))
PYEOF
