// tripod_init - "tripod" structural initial layout (GD k-planarity).
//
// Exploits the layout family observed in the strongest internal-2026
// submissions (and rediscovered by our SA when it wins): nodes packed along
// three thick arms radiating from a dense core, cross-cluster edges bundled
// into the three fans between arm pairs. Building that skeleton directly
// hands SA a far better basin than force-directed init on the dense mid-size
// graphs: 10-min cold runs on instance_05 dropped k 234 -> 210, and warm
// chains from tripod layouts set the 05/06 records (196 / 202).
//
// Recipe (port of scratchpad tripod_init.py, spectral variant — the BFS
// partition variant lost the A/B: 212/229/74 vs 210/210/68):
//   1. spectral 3-partition: 2 smallest nontrivial eigenvectors of the
//      normalized Laplacian (subspace iteration on M = I + D^-1/2 A D^-1/2
//      with the trivial eigenvector deflated), Lloyd k-means in that plane;
//   2. arms at 120 degrees; within an arm, nodes with many cross-cluster
//      edges sit near the core, internal ones toward the tip;
//   3. wedge-shaped arms (perpendicular spread grows with radius) + jitter,
//      snapped to distinct integer coordinates.
//
// Usage: tripod_init -i input.json -o output.json [-s seed] [--arms N]
//        (-t accepted and ignored — the tool runs in milliseconds; the
//         runner passes a stage budget to every init stage)

#include "../common/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using ll = long long;
using namespace std;

static string idKey(const mjson::Value& iv, int fallbackIndex) {
    if (iv.isStr()) return iv.asString();
    if (iv.isNum()) return to_string(iv.asLL());
    return to_string(fallbackIndex);
}

int main(int argc, char** argv) {
    string inPath, outPath;
    long long seed = 1;
    int ARMS = 3;
    // Shape parameters (defaults = the recipe that set the 05/06 records).
    double wedgeA = 0.05, wedgeB = 0.10;  // arm width: wedgeA*R + wedgeB*r
    double rexp   = 1.1;                  // radius exponent along the arm
    double core   = 0.06;                 // innermost radius fraction
    double jitf   = 0.012;                // isotropic jitter (fraction of R)
    string order  = "ext";                // core->tip ordering: ext|deg|hybrid
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        auto need = [&] { return string(argv[++i]); };
        if      (a == "-i")       inPath  = need();
        else if (a == "-o")       outPath = need();
        else if (a == "-s")       seed    = atoll(need().c_str());
        else if (a == "--arms")   ARMS    = atoi(need().c_str());
        else if (a == "--wedge")  { string w = need();
                                    sscanf(w.c_str(), "%lf,%lf", &wedgeA, &wedgeB); }
        else if (a == "--rexp")   rexp    = atof(need().c_str());
        else if (a == "--core")   core    = atof(need().c_str());
        else if (a == "--jitter") jitf    = atof(need().c_str());
        else if (a == "--order")  order   = need();
        else if (a == "-t")       (void)need();   // interface parity, unused
        else { fprintf(stderr, "tripod_init: unknown arg %s\n", a.c_str()); return 2; }
    }
    if (inPath.empty() || outPath.empty()) {
        fprintf(stderr,
                "usage: tripod_init -i in.json -o out.json [-s seed] [--arms N]\n"
                "       [--wedge a,b] [--rexp E] [--core F] [--jitter F]\n"
                "       [--order ext|deg|hybrid]\n");
        return 2;
    }
    ARMS = max(2, min(8, ARMS));
    mt19937_64 rng((uint64_t)seed * 0x9E3779B97F4A7C15ULL + 1);

    mjson::Value root = mjson::parseFile(inPath);
    string nodesKey = root.has("nodes") ? "nodes" : "Nodes";
    string edgesKey = root.has("edges") ? "edges" : "Edges";
    ll W  = root.has("width")  ? root["width"].asLL()  : 1000000;
    ll H  = root.has("height") ? root["height"].asLL() : 1000000;
    ll x0 = root.has("x") ? root["x"].asLL() : 0;
    ll y0 = root.has("y") ? root["y"].asLL() : 0;

    auto& nodesArr = root[nodesKey].arr;
    int n = (int)nodesArr.size();
    unordered_map<string,int> id2idx;
    for (int i = 0; i < n; i++)
        id2idx[nodesArr[i].has("id") ? idKey(nodesArr[i].at("id"), i)
                                     : to_string(i)] = i;

    vector<pair<int,int>> E;
    for (auto& ev : root[edgesKey].asArray()) {
        string s, t;
        if      (ev.has("source")) s = idKey(ev.at("source"), -1);
        else if (ev.has("from"))   s = idKey(ev.at("from"), -1);
        if      (ev.has("target")) t = idKey(ev.at("target"), -1);
        else if (ev.has("to"))     t = idKey(ev.at("to"), -1);
        auto its = id2idx.find(s), itt = id2idx.find(t);
        if (its == id2idx.end() || itt == id2idx.end()) continue;
        if (its->second != itt->second) E.push_back({its->second, itt->second});
    }

    // ---- spectral embedding: 2 smallest nontrivial eigvecs of L_norm ----
    vector<double> deg(n, 0.0);
    for (auto& e : E) { deg[e.first] += 1; deg[e.second] += 1; }
    vector<double> dis(n);
    for (int i = 0; i < n; i++) dis[i] = 1.0 / sqrt(max(deg[i], 1e-9));

    // trivial eigenvector of L_norm: v0_i ∝ sqrt(deg_i)
    vector<double> v0(n);
    {
        double s = 0;
        for (int i = 0; i < n; i++) { v0[i] = sqrt(max(deg[i], 0.0)); s += v0[i] * v0[i]; }
        s = sqrt(max(s, 1e-12));
        for (int i = 0; i < n; i++) v0[i] /= s;
    }
    // M x = x + D^-1/2 A D^-1/2 x  (top nontrivial eigvecs of M == bottom of L)
    auto applyM = [&](const vector<double>& x, vector<double>& y) {
        for (int i = 0; i < n; i++) y[i] = x[i];
        for (auto& e : E) {
            y[e.first]  += dis[e.first]  * dis[e.second] * x[e.second];
            y[e.second] += dis[e.second] * dis[e.first]  * x[e.first];
        }
    };
    // Subspace iteration with headroom: converging NS > NV vectors makes the
    // wanted top-NV (nontrivial) pair much more accurate for the same budget
    // — plain 2-vector power iteration left a visible partition-quality gap
    // vs the exact eigensolver (10-min SA: 218/222/72 vs 210/210/68).
    int NV = ARMS - 1;                       // embedding dimensions
    int NS = NV + 2;                         // extra subspace headroom
    vector<vector<double>> V(NS, vector<double>(n));
    normal_distribution<double> nrm(0.0, 1.0);
    for (auto& v : V) for (auto& x : v) x = nrm(rng);
    vector<double> tmp(n);
    auto dot = [&](const vector<double>& a, const vector<double>& b) {
        double s = 0; for (int i = 0; i < n; i++) s += a[i] * b[i]; return s;
    };
    for (int it = 0; it < 3000; it++) {
        for (int k = 0; k < NS; k++) {
            applyM(V[k], tmp);
            double c0 = dot(tmp, v0);
            for (int i = 0; i < n; i++) tmp[i] -= c0 * v0[i];
            for (int j = 0; j < k; j++) {
                double c = dot(tmp, V[j]);
                for (int i = 0; i < n; i++) tmp[i] -= c * V[j][i];
            }
            double nn = sqrt(max(dot(tmp, tmp), 1e-12));
            for (int i = 0; i < n; i++) V[k][i] = tmp[i] / nn;
        }
    }
    V.resize(NV);                            // keep only the wanted pair

    // ---- Lloyd k-means in the embedding ----
    vector<int> lab(n, 0);
    vector<vector<double>> C(ARMS, vector<double>(NV));
    {
        set<int> chosen;
        uniform_int_distribution<int> pick(0, n - 1);
        for (int c = 0; c < ARMS; c++) {
            int p;
            do { p = pick(rng); } while (chosen.count(p));
            chosen.insert(p);
            for (int k = 0; k < NV; k++) C[c][k] = V[k][p];
        }
        for (int round = 0; round < 60; round++) {
            bool changed = false;
            for (int i = 0; i < n; i++) {
                int best = 0; double bd = 1e300;
                for (int c = 0; c < ARMS; c++) {
                    double d2 = 0;
                    for (int k = 0; k < NV; k++) {
                        double d = V[k][i] - C[c][k];
                        d2 += d * d;
                    }
                    if (d2 < bd) { bd = d2; best = c; }
                }
                if (lab[i] != best) { lab[i] = best; changed = true; }
            }
            vector<int> cnt(ARMS, 0);
            for (auto& c : C) fill(c.begin(), c.end(), 0.0);
            for (int i = 0; i < n; i++) {
                cnt[lab[i]]++;
                for (int k = 0; k < NV; k++) C[lab[i]][k] += V[k][i];
            }
            for (int c = 0; c < ARMS; c++)
                if (cnt[c]) for (int k = 0; k < NV; k++) C[c][k] /= cnt[c];
            if (!changed && round > 0) break;
        }
    }

    // ---- externality: cross-cluster degree fraction ----
    vector<double> ext(n, 0.0);
    for (auto& e : E)
        if (lab[e.first] != lab[e.second]) { ext[e.first] += 1; ext[e.second] += 1; }
    for (int i = 0; i < n; i++) ext[i] /= max(deg[i], 1.0);

    // ---- wedge arms at 120 degrees ----
    double cx = x0 + W / 2.0, cy = y0 + H / 2.0;
    double R  = 0.48 * (double)min(W, H);
    vector<pair<double,double>> coord(n);
    for (int c = 0; c < ARMS; c++) {
        vector<int> mem;
        for (int i = 0; i < n; i++) if (lab[i] == c) mem.push_back(i);
        if (mem.empty()) continue;
        double ang = M_PI / 2 + 2.0 * M_PI * c / ARMS;
        double ux = cos(ang), uy = sin(ang);
        // core->tip ordering: bridging nodes (ext), hubs (deg), or a blend
        if (order == "deg")
            sort(mem.begin(), mem.end(),
                 [&](int a, int b) { return deg[a] > deg[b]; });
        else if (order == "hybrid")
            sort(mem.begin(), mem.end(), [&](int a, int b) {
                return ext[a] * sqrt(max(deg[a], 1.0)) >
                       ext[b] * sqrt(max(deg[b], 1.0));
            });
        else
            sort(mem.begin(), mem.end(),
                 [&](int a, int b) { return ext[a] > ext[b]; });
        for (size_t rank = 0; rank < mem.size(); rank++) {
            int v = mem[rank];
            double r = R * pow(core + (1.0 - core) * (rank + 0.5) / mem.size(),
                               rexp);
            normal_distribution<double> perpd(0.0, wedgeA * R + wedgeB * r);
            normal_distribution<double> jitd(0.0, jitf * R);
            double perp = perpd(rng);
            coord[v] = {cx + r * ux - perp * uy + jitd(rng),
                        cy + r * uy + perp * ux + jitd(rng)};
        }
    }

    // ---- clip + distinct integer coordinates ----
    set<pair<ll,ll>> seen;
    uniform_int_distribution<int> nud(-9, 9);
    for (int i = 0; i < n; i++) {
        ll X = (ll)llround(min(max(coord[i].first,  (double)x0), (double)(x0 + W)));
        ll Y = (ll)llround(min(max(coord[i].second, (double)y0), (double)(y0 + H)));
        while (seen.count({X, Y})) {
            X = min(max(X + nud(rng), x0), x0 + W);
            Y = min(max(Y + nud(rng), y0), y0 + H);
        }
        seen.insert({X, Y});
        nodesArr[i]["x"] = mjson::Value((long long)X);
        nodesArr[i]["y"] = mjson::Value((long long)Y);
    }

    mjson::writeFile(outPath, root, 0);
    vector<int> sizes(ARMS, 0);
    for (int i = 0; i < n; i++) sizes[lab[i]]++;
    fprintf(stderr, "tripod_init: n=%d m=%zu arms=%d sizes=[", n, E.size(), ARMS);
    for (int c = 0; c < ARMS; c++) fprintf(stderr, "%s%d", c ? "," : "", sizes[c]);
    fprintf(stderr, "] -> %s\n", outPath.c_str());
    return 0;
}
