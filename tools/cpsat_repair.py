#!/usr/bin/env python3
"""Exact CP-SAT local repair of the bottleneck (k-valued) edges in a GD-contest
layout. Unlike the SA solver's curated-candidate move classes (pair-move,
cluster-move, joint-repair), this gives every vertex in a bottleneck connected
component a *true* integer-coordinate domain (a window around its current
spot) and encodes the exact proper-segment-intersection test algebraically
(products of coordinate differences via AddMultiplicationEquality), so the
solver provably finds the true joint optimum over that domain rather than a
greedy or capped-candidate approximation.

Usage:
  python3 tools/cpsat_repair.py <layout.json> <out.json> \
      [--window R] [--time-per-component SECONDS] [--max-components N]
"""
import argparse
import json
import sys
import time

import numpy as np
from ortools.sat.python import cp_model


def load(path):
    d = json.load(open(path))
    W, H = d['width'], d['height']
    ids = [n['id'] for n in d['nodes']]
    nid = {i: idx for idx, i in enumerate(ids)}
    pos = np.zeros((len(ids), 2), dtype=np.int64)
    for n in d['nodes']:
        pos[nid[n['id']]] = (n['x'], n['y'])
    eu = np.array([nid[e['source']] for e in d['edges']], dtype=np.int64)
    ev = np.array([nid[e['target']] for e in d['edges']], dtype=np.int64)
    return d, W, H, ids, nid, pos, eu, ev


def sgn(a):
    return np.sign(a).astype(np.int8)


def compute_crossings(pos, eu, ev, chunk=400):
    m = len(eu)
    ax, ay = pos[eu, 0], pos[eu, 1]
    bx, by = pos[ev, 0], pos[ev, 1]
    counts = np.zeros(m, dtype=np.int64)

    def cross(px, py, qx, qy, rx, ry):
        return (qx - px) * (ry - py) - (qy - py) * (rx - px)

    for i0 in range(0, m, chunk):
        i1 = min(m, i0 + chunk)
        Ax, Ay = ax[i0:i1, None], ay[i0:i1, None]
        Bx, By = bx[i0:i1, None], by[i0:i1, None]
        Cx, Cy = ax[None, :], ay[None, :]
        Dx, Dy = bx[None, :], by[None, :]
        d1 = sgn(cross(Cx, Cy, Dx, Dy, Ax, Ay))
        d2 = sgn(cross(Cx, Cy, Dx, Dy, Bx, By))
        d3 = sgn(cross(Ax, Ay, Bx, By, Cx, Cy))
        d4 = sgn(cross(Ax, Ay, Bx, By, Dx, Dy))
        cr = (d1 != d2) & (d3 != d4)
        Eu, Ev = eu[i0:i1, None], ev[i0:i1, None]
        Cu, Cv = eu[None, :], ev[None, :]
        shared = (Eu == Cu) | (Eu == Cv) | (Ev == Cu) | (Ev == Cv)
        cr = cr & ~shared
        idxs = np.arange(i0, i1)[:, None] == np.arange(m)[None, :]
        cr = cr & ~idxs
        counts[i0:i1] += cr.sum(axis=1)
    return counts


def seg_cross_scalar(a, b, c, d):
    if a == c or a == d or b == c or b == d:
        return False

    def cross(p, q, r):
        return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])

    def s(v):
        v = int(v)
        return (v > 0) - (v < 0)

    d1 = s(cross(c, d, a)); d2 = s(cross(c, d, b))
    d3 = s(cross(a, b, c)); d4 = s(cross(a, b, d))
    return d1 != d2 and d3 != d4


def bottleneck_components(counts, eu, ev):
    k = int(counts.max())
    bidx = np.nonzero(counts == k)[0]
    parent = {}

    def find(x):
        parent.setdefault(x, x)
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb

    for ei in bidx:
        union(int(eu[ei]), int(ev[ei]))
    vcomp = {}
    ecomp = {}
    for ei in bidx:
        r = find(int(eu[ei]))
        vcomp.setdefault(r, set()).update([int(eu[ei]), int(ev[ei])])
        ecomp.setdefault(r, []).append(int(ei))
    keys = list(vcomp.keys())
    return k, [{'vertices': sorted(vcomp[r]), 'edges': sorted(ecomp[r])} for r in keys]


# ---------------- CP-SAT symbolic algebra over coordinate diffs ----------------

class Expr:
    __slots__ = ('e', 'lo', 'hi')

    def __init__(self, e, lo, hi):
        self.e = e
        self.lo = lo
        self.hi = hi


def const_expr(v):
    return Expr(int(v), int(v), int(v))


def var_expr(v, lo, hi):
    return Expr(v, lo, hi)


def sub_expr(a, b):
    return Expr(a.e - b.e, a.lo - b.hi, a.hi - b.lo)


def materialize(model, cache, expr):
    if isinstance(expr.e, cp_model.IntVar):
        return expr.e
    v = model.NewIntVar(expr.lo, expr.hi, f'aux{len(cache)}')
    model.Add(v == expr.e)
    cache.append(v)
    return v


def mul_expr(model, cache, a, b):
    if a.lo == a.hi:
        c = a.lo
        lo, hi = sorted([c * b.lo, c * b.hi])
        return Expr(c * b.e, lo, hi)
    if b.lo == b.hi:
        c = b.lo
        lo, hi = sorted([c * a.lo, c * a.hi])
        return Expr(a.e * c, lo, hi)
    av = materialize(model, cache, a)
    bv = materialize(model, cache, b)
    corners = [a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi]
    lo, hi = min(corners), max(corners)
    prod = model.NewIntVar(lo, hi, f'mul{len(cache)}')
    model.AddMultiplicationEquality(prod, [av, bv])
    return Expr(prod, lo, hi)


def cross_expr(model, cache, P, Q, R):
    dx1 = sub_expr(Q[0], P[0])
    dy1 = sub_expr(R[1], P[1])
    dy2 = sub_expr(Q[1], P[1])
    dx2 = sub_expr(R[0], P[0])
    t1 = mul_expr(model, cache, dx1, dy1)
    t2 = mul_expr(model, cache, dy2, dx2)
    return sub_expr(t1, t2)


def cat_bools(model, expr, tag):
    if expr.lo == expr.hi:
        s = 1 if expr.e > 0 else (-1 if expr.e < 0 else 0)
        return (1 if s > 0 else 0, 1 if s < 0 else 0, 1 if s == 0 else 0)
    is_pos = model.NewBoolVar(f'p{tag}')
    is_neg = model.NewBoolVar(f'n{tag}')
    is_zero = model.NewBoolVar(f'z{tag}')
    model.Add(expr.e > 0).OnlyEnforceIf(is_pos)
    model.Add(expr.e <= 0).OnlyEnforceIf(is_pos.Not())
    model.Add(expr.e < 0).OnlyEnforceIf(is_neg)
    model.Add(expr.e >= 0).OnlyEnforceIf(is_neg.Not())
    model.Add(expr.e == 0).OnlyEnforceIf(is_zero)
    model.Add(expr.e != 0).OnlyEnforceIf(is_zero.Not())
    model.AddExactlyOne([is_pos, is_neg, is_zero])
    return (is_pos, is_neg, is_zero)


def neq_bool(model, cats1, cats2, tag):
    ands = []
    for a, b in zip(cats1, cats2):
        aconst = isinstance(a, int)
        bconst = isinstance(b, int)
        if aconst and bconst:
            if a == 1 and b == 1:
                return 0
            continue
        if aconst:
            if a == 0:
                continue
            ands.append(b)
            continue
        if bconst:
            if b == 0:
                continue
            ands.append(a)
            continue
        andvar = model.NewBoolVar(f'and{tag}_{len(ands)}')
        model.AddMultiplicationEquality(andvar, [a, b])
        ands.append(andvar)
    if not ands:
        return 1
    match = model.NewBoolVar(f'match{tag}')
    model.AddMaxEquality(match, ands)
    neq = model.NewBoolVar(f'neq{tag}')
    model.Add(neq == 1 - match)
    return neq


def and_bool(model, a, b, tag):
    if isinstance(a, int) and isinstance(b, int):
        return 1 if (a and b) else 0
    if isinstance(a, int):
        return b if a else 0
    if isinstance(b, int):
        return a if b else 0
    r = model.NewBoolVar(f'andf{tag}')
    model.AddMultiplicationEquality(r, [a, b])
    return r


def bbox(px, py, qx, qy):
    return (min(px, qx), max(px, qx), min(py, qy), max(py, qy))


def bbox_union(b1, b2):
    return (min(b1[0], b2[0]), max(b1[1], b2[1]),
            min(b1[2], b2[2]), max(b1[3], b2[3]))


def bbox_overlap(b1, b2):
    return not (b1[1] < b2[0] or b2[1] < b1[0] or b1[3] < b2[2] or b2[3] < b1[2])


def solve_component(pos, eu, ev, W, H, S, target_eids, window, time_limit, num_workers=8, log=print):
    n = len(pos)
    m = len(eu)
    Sset = set(S)
    model = cp_model.CpModel()
    cache = []

    coord = {}
    for v in S:
        x0, y0 = int(pos[v, 0]), int(pos[v, 1])
        if window is None:
            xlo, xhi, ylo, yhi = 0, W - 1, 0, H - 1
        else:
            xlo, xhi = max(0, x0 - window), min(W - 1, x0 + window)
            ylo, yhi = max(0, y0 - window), min(H - 1, y0 + window)
        xv = model.NewIntVar(xlo, xhi, f'x{v}')
        yv = model.NewIntVar(ylo, yhi, f'y{v}')
        coord[v] = (var_expr(xv, xlo, xhi), var_expr(yv, ylo, yhi))

    def pt(v):
        if v in coord:
            return coord[v]
        x0, y0 = int(pos[v, 0]), int(pos[v, 1])
        return (const_expr(x0), const_expr(y0))

    def reach_bbox(v):
        if v in coord:
            ex, ey = coord[v]
            return (ex.lo, ex.hi, ey.lo, ey.hi)
        x0, y0 = int(pos[v, 0]), int(pos[v, 1])
        return (x0, x0, y0, y0)

    local_eids = list(target_eids)
    local_set = set(local_eids)

    # all-different among S (scalar encoding x*H+y is injective since y<H)
    scal = {}
    for v in S:
        ex, ey = coord[v]
        sc = mul_expr(model, cache, const_expr(H), ex)
        sc = Expr(sc.e + ey.e, sc.lo + ey.lo, sc.hi + ey.hi)
        scal[v] = sc
    for i in range(len(S)):
        for j in range(i + 1, len(S)):
            model.Add(scal[S[i]].e != scal[S[j]].e)
    for v in S:
        rb = reach_bbox(v)
        xlo, xhi, ylo, yhi = rb
        for u in range(n):
            if u in Sset:
                continue
            ux, uy = int(pos[u, 0]), int(pos[u, 1])
            if xlo <= ux <= xhi and ylo <= uy <= yhi:
                model.Add(scal[v].e != ux * H + uy)

    newcount_pairs = {}  # eid -> list of (partner_eid, indicator)

    def add_term(eid, partner, ind):
        newcount_pairs.setdefault(eid, []).append((partner, ind))

    considered_pairs = set()
    for e1 in local_eids:
        u1, v1 = int(eu[e1]), int(ev[e1])
        b1 = bbox_union(reach_bbox(u1), reach_bbox(v1))
        for e2 in range(m):
            if e2 == e1:
                continue
            u2, v2 = int(eu[e2]), int(ev[e2])
            if u2 == u1 or u2 == v1 or v2 == u1 or v2 == v1:
                continue
            key = (min(e1, e2), max(e1, e2))
            if key in considered_pairs:
                continue
            b2 = bbox_union(reach_bbox(u2), reach_bbox(v2))
            if not bbox_overlap(b1, b2):
                continue
            considered_pairs.add(key)
            P, Q = pt(u1), pt(v1)
            C, D = pt(u2), pt(v2)
            tag = f'{e1}_{e2}'
            d1 = cross_expr(model, cache, C, D, P)
            d2 = cross_expr(model, cache, C, D, Q)
            d3 = cross_expr(model, cache, P, Q, C)
            d4 = cross_expr(model, cache, P, Q, D)
            c1 = cat_bools(model, d1, tag + 'a')
            c2 = cat_bools(model, d2, tag + 'b')
            c3 = cat_bools(model, d3, tag + 'c')
            c4 = cat_bools(model, d4, tag + 'd')
            neq12 = neq_bool(model, c1, c2, tag + 'x')
            neq34 = neq_bool(model, c3, c4, tag + 'y')
            ind = and_bool(model, neq12, neq34, tag + 'z')
            add_term(e1, e2, ind)
            add_term(e2, e1, ind)

    touched_edges = set(newcount_pairs.keys())
    if not touched_edges:
        return None  # component has no possibly-affected pairs at all (shouldn't happen)

    counts_full = compute_crossings(pos, eu, ev)

    newcount_expr = {}
    for eid in touched_edges:
        terms = newcount_pairs[eid]
        base = 0
        if eid not in local_set:
            u_e, v_e = tuple(pos[eu[eid]]), tuple(pos[ev[eid]])
            orig_sum = 0
            for partner, _ in terms:
                pu, pv = tuple(pos[eu[partner]]), tuple(pos[ev[partner]])
                if seg_cross_scalar(u_e, v_e, pu, pv):
                    orig_sum += 1
            base = int(counts_full[eid]) - orig_sum
        expr = base
        for _, ind in terms:
            expr = expr + ind
        newcount_expr[eid] = expr

    fixed_floor = int(max(
        (counts_full[e] for e in range(m) if e not in touched_edges),
        default=0))

    UB = fixed_floor + len(local_eids) * 3 + 5
    k_var = model.NewIntVar(0, max(UB, fixed_floor + 1), 'k')
    for eid, expr in newcount_expr.items():
        model.Add(k_var >= expr)
    model.Add(k_var >= fixed_floor)

    total_expr = 0
    for eid in touched_edges:
        total_expr = total_expr + newcount_expr[eid]

    model.Minimize(k_var * 1000000 + total_expr)

    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = time_limit
    solver.parameters.num_workers = num_workers
    status = solver.Solve(model)

    if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        return {'status': 'infeasible', 'k': None,
                'status_name': solver.StatusName(status),
                'nvars': len(model.Proto().variables),
                'nconstr': len(model.Proto().constraints)}

    new_k = solver.Value(k_var)
    result = {'status': 'ok', 'k': new_k, 'fixed_floor': fixed_floor,
              'optimal': status == cp_model.OPTIMAL,
              'positions': {v: (solver.Value(coord[v][0].e), solver.Value(coord[v][1].e))
                            for v in S}}
    return result


def build_adjacency(n, eu, ev):
    adj = [[] for _ in range(n)]
    for u, v in zip(eu.tolist(), ev.tolist()):
        adj[u].append(v)
        adj[v].append(u)
    return adj


def expand_hops(S, adj, hops, cap):
    S = list(S)
    frontier = set(S)
    seen = set(S)
    for _ in range(hops):
        nxt = set()
        for v in frontier:
            for u in adj[v]:
                if u not in seen:
                    nxt.add(u)
        for u in nxt:
            if len(seen) >= cap:
                break
            seen.add(u)
            S.append(u)
        frontier = nxt
        if len(seen) >= cap:
            break
    return S


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('layout')
    ap.add_argument('out')
    ap.add_argument('--window', type=int, default=None)
    ap.add_argument('--time-per-component', type=float, default=90.0)
    ap.add_argument('--max-components', type=int, default=999)
    ap.add_argument('--workers', type=int, default=8)
    ap.add_argument('--expand-hops', type=int, default=0,
                     help='also free this many hops of graph neighbors around '
                          'the bottleneck endpoints (lets the solver move the '
                          'crossing PARTNER out of the way too, not just u,v)')
    ap.add_argument('--expand-cap', type=int, default=12,
                     help='max total free vertices per component after expansion')
    ap.add_argument('--only-component', type=int, default=None,
                     help='0-based index into the size-sorted component list; '
                          'solve just this one (for targeted retries)')
    args = ap.parse_args()

    d, W, H, ids, nid, pos, eu, ev = load(args.layout)
    counts = compute_crossings(pos, eu, ev)
    orig_k = int(counts.max())
    orig_X = int(counts.sum()) // 2
    print(f"start: k={orig_k} X={orig_X} n={len(pos)} m={len(eu)} W={W} H={H}")

    k, comps = bottleneck_components(counts, eu, ev)
    comps.sort(key=lambda c: len(c['vertices']))
    print(f"bottleneck k={k}, {len(comps)} connected components, "
          f"sizes={[len(c['vertices']) for c in comps]}")

    adj = build_adjacency(len(pos), eu, ev) if args.expand_hops > 0 else None

    improved_any = False
    ncomp = min(len(comps), args.max_components)
    indices = range(ncomp) if args.only_component is None else [args.only_component]
    for ci in indices:
        base_S = comps[ci]['vertices']
        target_eids = comps[ci]['edges']
        if adj is not None:
            S = expand_hops(base_S, adj, args.expand_hops, args.expand_cap)
        else:
            S = base_S
        t0 = time.time()
        print(f"[component {ci+1}/{ncomp}] base={base_S} expanded_S={S} "
              f"edges={target_eids} solving...", flush=True)
        res = solve_component(pos, eu, ev, W, H, S, target_eids, args.window,
                               args.time_per_component, args.workers)
        dt = time.time() - t0
        if res is None or res['status'] != 'ok':
            extra = ''
            if res is not None:
                extra = f" status={res.get('status_name')} nvars={res.get('nvars')} nconstr={res.get('nconstr')}"
            print(f"  -> infeasible/no-model ({dt:.1f}s){extra}")
            continue
        print(f"  -> solved (optimal={res['optimal']}) local_k={res['k']} "
              f"(vs fixed_floor={res['fixed_floor']}) in {dt:.1f}s")
        if res['k'] < k:
            for v, (x, y) in res['positions'].items():
                pos[v, 0] = x
                pos[v, 1] = y
            newcounts = compute_crossings(pos, eu, ev)
            new_global_k = int(newcounts.max())
            print(f"  APPLIED. recomputed global k={new_global_k} "
                  f"(was {k}) X={int(newcounts.sum())//2}")
            if new_global_k < k:
                improved_any = True
                k = new_global_k
                counts = newcounts
            else:
                print("  (global k did not actually drop - other component still binds)")
        else:
            print(f"  no improvement for this component (local_k={res['k']} >= global k={k})")

    out = {
        'width': int(W), 'height': int(H),
        'nodes': [{'id': ids[i], 'x': int(pos[i, 0]), 'y': int(pos[i, 1])} for i in range(len(ids))],
        'edges': d['edges'],
    }
    json.dump(out, open(args.out, 'w'))
    final_counts = compute_crossings(pos, eu, ev)
    print(f"final: k={int(final_counts.max())} X={int(final_counts.sum())//2} "
          f"improved_any={improved_any}")


if __name__ == '__main__':
    main()
