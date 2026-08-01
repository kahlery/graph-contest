#!/usr/bin/env python3
"""validate.py - check a layout obeys every GD-contest rule before submitting.

    tools/validate.py <input-graph.json> <layout.json> [...]
    tools/validate.py --set internal-2026 <layout-dir>

A layout that breaks any rule scores as invalid, not merely badly - the
leaderboard shows one team with three INV entries - so this is worth running on
anything we are about to submit. Checks, in order:

  1. every node has integer coordinates inside [0,width] x [0,height]
  2. no two nodes share a point
  3. the node set and edge set match the input exactly
  4. no node lies in the interior (or on an endpoint) of an edge it is not
     incident to - crossings on such an edge would be ill-defined
  5. reports k and total crossings

Exit status is non-zero if any layout fails.
"""
import json
import os
import sys
from collections import defaultdict


def seg_orient(o, p, q):
    r = (p[0] - o[0]) * (q[1] - o[1]) - (p[1] - o[1]) * (q[0] - o[0])
    return (r > 0) - (r < 0)


def on_seg(p, a, b):
    """p collinear with ab and inside its bounding box."""
    if seg_orient(a, b, p) != 0:
        return False
    return (min(a[0], b[0]) <= p[0] <= max(a[0], b[0]) and
            min(a[1], b[1]) <= p[1] <= max(a[1], b[1]))


def crosses(a, b, c, d):
    d1, d2 = seg_orient(c, d, a), seg_orient(c, d, b)
    d3, d4 = seg_orient(a, b, c), seg_orient(a, b, d)
    if ((d1 > 0 > d2) or (d1 < 0 < d2)) and ((d3 > 0 > d4) or (d3 < 0 < d4)):
        return True
    return ((d1 == 0 and on_seg(a, c, d)) or (d2 == 0 and on_seg(b, c, d)) or
            (d3 == 0 and on_seg(c, a, b)) or (d4 == 0 and on_seg(d, a, b)))


def check(inp_path, lay_path):
    inp = json.load(open(inp_path))
    lay = json.load(open(lay_path))
    errs = []

    W, H = inp.get("width"), inp.get("height")
    pos, seen = {}, {}
    for nd in lay["nodes"]:
        i = nd["id"]
        x, y = nd["x"], nd["y"]
        if int(x) != x or int(y) != y:
            errs.append("node %s has non-integer coords (%r,%r)" % (i, x, y))
        x, y = int(x), int(y)
        if W is not None and not (0 <= x <= W and 0 <= y <= H):
            errs.append("node %s outside canvas: (%d,%d) vs %sx%s" % (i, x, y, W, H))
        if (x, y) in seen:
            errs.append("nodes %s and %s share point (%d,%d)" % (seen[(x, y)], i, x, y))
        seen[(x, y)] = i
        pos[i] = (x, y)

    if set(pos) != {nd["id"] for nd in inp["nodes"]}:
        errs.append("node id set differs from the input")
    ein = {frozenset((e["source"], e["target"])) for e in inp["edges"]}
    elay = {frozenset((e["source"], e["target"])) for e in lay["edges"]}
    if ein != elay:
        errs.append("edge set differs from the input (%d vs %d edges)"
                    % (len(ein), len(elay)))

    E = [(e["source"], e["target"]) for e in lay["edges"]]
    if errs:  # geometry checks below assume a well-formed layout
        return errs, None, None

    # Bucket edges into a uniform grid so the pairwise scans stay near-linear
    # on the 20k-edge instances.
    xs = [p[0] for p in pos.values()]; ys = [p[1] for p in pos.values()]
    mnx, mxx, mny, mxy = min(xs), max(xs), min(ys), max(ys)
    side = max(1, int((len(E) / 2.0) ** 0.5))
    cw = max(1, (mxx - mnx) // side + 1); ch = max(1, (mxy - mny) // side + 1)
    cells = defaultdict(list)
    for idx, (u, v) in enumerate(E):
        a, b = pos[u], pos[v]
        for cx in range((min(a[0], b[0]) - mnx) // cw, (max(a[0], b[0]) - mnx) // cw + 1):
            for cy in range((min(a[1], b[1]) - mny) // ch, (max(a[1], b[1]) - mny) // ch + 1):
                cells[(cx, cy)].append(idx)

    for i, p in pos.items():
        cx, cy = (p[0] - mnx) // cw, (p[1] - mny) // ch
        for idx in cells.get((cx, cy), ()):
            u, v = E[idx]
            if i in (u, v):
                continue
            if on_seg(p, pos[u], pos[v]):
                errs.append("node %s lies on edge (%s,%s)" % (i, u, v))
                break

    cnt = [0] * len(E)
    total = 0
    # A pair of edges appears together in every grid cell their bounding boxes
    # share, so it must be counted only the first time it is seen. Keying on
    # crossing pairs alone keeps this bounded by the number of crossings
    # (k*m/2) rather than by the far larger number of candidate pairs.
    crossed = set()
    for cell in cells.values():
        for ii in range(len(cell)):
            for jj in range(ii + 1, len(cell)):
                i, j = cell[ii], cell[jj]
                if i > j:
                    i, j = j, i
                if (i, j) in crossed:
                    continue
                ui, vi = E[i]; uj, vj = E[j]
                if ui in (uj, vj) or vi in (uj, vj):
                    continue
                if crosses(pos[ui], pos[vi], pos[uj], pos[vj]):
                    crossed.add((i, j))
                    cnt[i] += 1; cnt[j] += 1; total += 1
    return errs, max(cnt) if cnt else 0, total


def main():
    args = sys.argv[1:]
    pairs = []
    if args and args[0] == "--set":
        iset, d = args[1], args[2]
        for fn in sorted(os.listdir(d)):
            if fn.endswith(".json") and "_snap" not in fn:
                ip = os.path.join("data/input", iset, fn)
                if os.path.exists(ip):
                    pairs.append((ip, os.path.join(d, fn)))
    else:
        pairs = [(args[0], p) for p in args[1:]]

    bad = 0
    for ip, lp in pairs:
        errs, k, total = check(ip, lp)
        name = os.path.basename(lp)
        if errs:
            bad += 1
            print("INVALID %-16s %s" % (name, errs[0]))
            for e in errs[1:4]:
                print("        %-16s %s" % ("", e))
        else:
            print("ok      %-16s k=%d (grid-counted)" % (name, k))
    print("%d/%d valid" % (len(pairs) - bad, len(pairs)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
