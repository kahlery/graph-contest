// xstat - crossing-distribution report for a laid-out contest graph.
//
// Answers the question the plain k/totalX pair cannot: *why* is k what it is.
// Because every crossing charges two edges, sum_e c_e = 2X, so no drawing with
// X crossings can have a bottleneck below ceil(2X/m). That "balance floor" is
// the yardstick this tool prints alongside the achieved k: a layout sitting far
// above its own floor is badly balanced (the fix is redistribution, k can fall
// with X held fixed), while one sitting at the floor can only improve by
// lowering X itself. The two cases call for completely different tactics, and
// the histogram tail tells you how many edges have to be relieved to move down
// one level.
//
//   ./bin/xstat layout.json [more.json ...]
//
#include "../common/json.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace std;
typedef long long ll;

struct Pt { ll x, y; };
struct Edge { int u, v; };

static inline ll crossp(const Pt& a, const Pt& b, const Pt& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}
static inline int sgn(ll x) { return (x > 0) - (x < 0); }

static inline bool bboxOverlap(const Pt& a1, const Pt& a2,
                               const Pt& b1, const Pt& b2) {
    if (max(a1.x, a2.x) < min(b1.x, b2.x)) return false;
    if (max(b1.x, b2.x) < min(a1.x, a2.x)) return false;
    if (max(a1.y, a2.y) < min(b1.y, b2.y)) return false;
    if (max(b1.y, b2.y) < min(a1.y, a2.y)) return false;
    return true;
}
static inline bool onSeg(const Pt& p, const Pt& a, const Pt& b) {
    if (crossp(a, b, p) != 0) return false;
    return min(a.x, b.x) <= p.x && p.x <= max(a.x, b.x) &&
           min(a.y, b.y) <= p.y && p.y <= max(a.y, b.y);
}
// Proper crossing test, matching the contest rule set: shared endpoints do not
// count (callers filter those out), collinear overlap and an endpoint lying on
// the other segment do.
static bool segCross(const Pt& a, const Pt& b, const Pt& c, const Pt& d) {
    int d1 = sgn(crossp(c, d, a)), d2 = sgn(crossp(c, d, b));
    int d3 = sgn(crossp(a, b, c)), d4 = sgn(crossp(a, b, d));
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
        ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) return true;
    if (d1 == 0 && onSeg(a, c, d)) return true;
    if (d2 == 0 && onSeg(b, c, d)) return true;
    if (d3 == 0 && onSeg(c, a, b)) return true;
    if (d4 == 0 && onSeg(d, a, b)) return true;
    return false;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: xstat layout.json [...]\n"); return 2; }

    for (int ai = 1; ai < argc; ai++) {
        const string path = argv[ai];
        mjson::Value j;
        try { j = mjson::parseFile(path); }
        catch (const exception& e) {
            fprintf(stderr, "%s: parse failed: %s\n", path.c_str(), e.what());
            continue;
        }

        // Node ids are arbitrary integers in the contest format; map to dense.
        const auto& jn = j["nodes"].asArray();
        vector<Pt> pos(jn.size());
        vector<int> ids(jn.size());
        for (size_t i = 0; i < jn.size(); i++) {
            ids[i] = jn[i]["id"].asInt();
            pos[i] = { (ll)jn[i]["x"].asDouble(), (ll)jn[i]["y"].asDouble() };
        }
        vector<int> order(ids.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
        sort(order.begin(), order.end(),
             [&](int a, int b) { return ids[a] < ids[b]; });
        auto idx = [&](int id) -> int {
            int lo = 0, hi = (int)order.size() - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                if (ids[order[mid]] == id) return order[mid];
                if (ids[order[mid]] <  id) lo = mid + 1; else hi = mid - 1;
            }
            return -1;
        };

        const auto& je = j["edges"].asArray();
        vector<Edge> edges;
        edges.reserve(je.size());
        for (size_t i = 0; i < je.size(); i++) {
            int u = idx(je[i]["source"].asInt());
            int v = idx(je[i]["target"].asInt());
            if (u < 0 || v < 0) continue;
            edges.push_back({u, v});
        }
        const int n = (int)pos.size(), m = (int)edges.size();
        if (!m) { printf("%s: no edges\n", path.c_str()); continue; }

        // Uniform grid over edge bounding boxes: without it the O(m^2) scan is
        // hopeless on the 20k-edge instances.
        ll mnx = pos[0].x, mxx = pos[0].x, mny = pos[0].y, mxy = pos[0].y;
        for (int i = 1; i < n; i++) {
            mnx = min(mnx, pos[i].x); mxx = max(mxx, pos[i].x);
            mny = min(mny, pos[i].y); mxy = max(mxy, pos[i].y);
        }
        int side = max(1, (int)llround(sqrt((double)m / 2.0)));
        ll spanx = max<ll>(1, mxx - mnx), spany = max<ll>(1, mxy - mny);
        ll cw = max<ll>(1, spanx / side), ch = max<ll>(1, spany / side);
        auto cellx = [&](ll x) { return (int)min<ll>(side - 1, max<ll>(0, (x - mnx) / cw)); };
        auto celly = [&](ll y) { return (int)min<ll>(side - 1, max<ll>(0, (y - mny) / ch)); };

        vector<vector<int>> cells((size_t)side * side);
        for (int e = 0; e < m; e++) {
            const Pt& a = pos[edges[e].u]; const Pt& b = pos[edges[e].v];
            int x0 = cellx(min(a.x, b.x)), x1 = cellx(max(a.x, b.x));
            int y0 = celly(min(a.y, b.y)), y1 = celly(max(a.y, b.y));
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++)
                    cells[(size_t)y * side + x].push_back(e);
        }

        vector<int> cnt(m, 0);
        ll X = 0;
        vector<int> stamp(m, -1);
        for (int e = 0; e < m; e++) {
            const Pt& a = pos[edges[e].u]; const Pt& b = pos[edges[e].v];
            int x0 = cellx(min(a.x, b.x)), x1 = cellx(max(a.x, b.x));
            int y0 = celly(min(a.y, b.y)), y1 = celly(max(a.y, b.y));
            for (int y = y0; y <= y1; y++) {
                for (int x = x0; x <= x1; x++) {
                    for (int f : cells[(size_t)y * side + x]) {
                        if (f <= e || stamp[f] == e) continue;
                        stamp[f] = e;               // dedupe multi-cell overlap
                        const Edge& E = edges[e]; const Edge& F = edges[f];
                        if (E.u == F.u || E.u == F.v ||
                            E.v == F.u || E.v == F.v) continue;
                        const Pt& c = pos[F.u]; const Pt& d = pos[F.v];
                        if (!bboxOverlap(a, b, c, d)) continue;
                        if (segCross(a, b, c, d)) { cnt[e]++; cnt[f]++; X++; }
                    }
                }
            }
        }

        vector<int> sorted = cnt;
        sort(sorted.begin(), sorted.end());
        int k = sorted.back();
        double mean = 2.0 * (double)X / (double)m;
        int floorK = (int)ceil(mean - 1e-9);

        // How many edges sit at each of the top levels: that is exactly the set
        // a level-clearing sweep must relieve to drop k by one.
        printf("%s\n", path.c_str());
        printf("  n=%d m=%d  X=%lld  k=%d  mean=%.2f  floor=ceil(2X/m)=%d"
               "  excess=k-floor=%d  (k/floor=%.2fx)\n",
               n, m, X, k, mean, floorK, k - floorK,
               floorK ? (double)k / floorK : 0.0);
        printf("  percentiles c_e: p50=%d p90=%d p99=%d max=%d\n",
               sorted[(size_t)(0.50 * (m - 1))],
               sorted[(size_t)(0.90 * (m - 1))],
               sorted[(size_t)(0.99 * (m - 1))], k);
        printf("  top-level occupancy:");
        for (int lv = k; lv > k - 6 && lv >= 0; lv--) {
            int c = 0;
            for (int e = 0; e < m; e++) if (cnt[e] == lv) c++;
            printf(" k-%d:%d", k - lv, c);
        }
        printf("\n");
        int above = 0;
        for (int e = 0; e < m; e++) if (cnt[e] > floorK) above++;
        printf("  edges above floor: %d (%.1f%% of m)\n",
               above, 100.0 * above / m);
    }
    return 0;
}
