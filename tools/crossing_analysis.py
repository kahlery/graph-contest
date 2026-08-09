#!/usr/bin/env python3
"""Compute exact per-edge crossing counts for a layout json and report the
bottleneck structure (which edges sit at k, how they cluster by vertex)."""
import json, sys
import numpy as np


def load(path):
    d = json.load(open(path))
    W, H = d['width'], d['height']
    nid = {n['id']: i for i, n in enumerate(d['nodes'])}
    pos = np.zeros((len(d['nodes']), 2), dtype=np.int64)
    for n in d['nodes']:
        pos[nid[n['id']]] = (n['x'], n['y'])
    eu = np.array([nid[e['source']] for e in d['edges']], dtype=np.int64)
    ev = np.array([nid[e['target']] for e in d['edges']], dtype=np.int64)
    return W, H, pos, eu, ev


def sgn(a):
    return np.sign(a).astype(np.int8)


def compute_crossings(pos, eu, ev, chunk=400):
    m = len(eu)
    ax, ay = pos[eu, 0], pos[eu, 1]
    bx, by = pos[ev, 0], pos[ev, 1]
    counts = np.zeros(m, dtype=np.int64)
    pairs = []
    for i0 in range(0, m, chunk):
        i1 = min(m, i0 + chunk)
        # rows i0:i1 vs all columns
        Ax, Ay = ax[i0:i1, None], ay[i0:i1, None]
        Bx, By = bx[i0:i1, None], by[i0:i1, None]
        Cx, Cy = ax[None, :], ay[None, :]
        Dx, Dy = bx[None, :], by[None, :]

        def cross(px, py, qx, qy, rx, ry):
            return (qx - px) * (ry - py) - (qy - py) * (rx - px)

        d1 = sgn(cross(Cx, Cy, Dx, Dy, Ax, Ay))
        d2 = sgn(cross(Cx, Cy, Dx, Dy, Bx, By))
        d3 = sgn(cross(Ax, Ay, Bx, By, Cx, Cy))
        d4 = sgn(cross(Ax, Ay, Bx, By, Dx, Dy))
        cr = (d1 != d2) & (d3 != d4)

        # exclude shared endpoints (by vertex id, not coordinate)
        Eu, Ev = eu[i0:i1, None], ev[i0:i1, None]
        Cu, Cv = eu[None, :], ev[None, :]
        shared = (Eu == Cu) | (Eu == Cv) | (Ev == Cu) | (Ev == Cv)
        cr = cr & ~shared
        # exclude self pair (i==j) explicitly (shared covers it, but be safe)
        idxs = np.arange(i0, i1)[:, None] == np.arange(m)[None, :]
        cr = cr & ~idxs

        rowsum = cr.sum(axis=1)
        counts[i0:i1] += rowsum
        # record pair list only for rows, cols where col>row to avoid dup, cap output size
        rr, cc = np.nonzero(cr)
        for r, c in zip(rr, cc):
            gi, gj = i0 + r, c
            if gi < gj:
                pairs.append((int(gi), int(gj)))
    return counts, pairs


def main():
    path = sys.argv[1]
    W, H, pos, eu, ev = load(path)
    m = len(eu)
    counts, pairs = compute_crossings(pos, eu, ev)
    k = int(counts.max())
    totalX_from_pairs = len(pairs)
    totalX_from_counts = int(counts.sum()) // 2
    print(f"n={len(pos)} m={m} W={W} H={H} k={k} "
          f"totalX(pairs)={totalX_from_pairs} totalX(counts/2)={totalX_from_counts}")
    bottleneck = np.nonzero(counts == k)[0]
    print(f"bottleneck edges (count=={k}): {len(bottleneck)}")
    # cluster bottleneck edges by connected component via shared vertices
    verts = set()
    for ei in bottleneck:
        verts.add(int(eu[ei])); verts.add(int(ev[ei]))
    print(f"distinct vertices touched by bottleneck edges: {len(verts)}")
    # union-find over bottleneck edges by shared vertex
    parent = {v: v for v in verts}
    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb
    for ei in bottleneck:
        union(int(eu[ei]), int(ev[ei]))
    comps = {}
    for ei in bottleneck:
        r = find(int(eu[ei]))
        comps.setdefault(r, []).append(int(ei))
    sizes = sorted((len(v) for v in comps.values()), reverse=True)
    print(f"bottleneck edges grouped into {len(comps)} connected components "
          f"(by shared vertex), sizes={sizes[:20]}")
    for ei in bottleneck[:10]:
        print(f"  edge {int(ei)}: u={int(eu[ei])} v={int(ev[ei])} "
              f"pos_u={tuple(pos[eu[ei]])} pos_v={tuple(pos[ev[ei]])}")


if __name__ == '__main__':
    main()
