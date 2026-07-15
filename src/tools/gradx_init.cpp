// gradx_init — gradient-descent layout initializer on the SigmoidX
// differentiable crossing surrogate (arXiv 2606.31119 sec 3.3), extended
// toward the LOCAL crossing number with a softmax edge-weight curriculum.
//
// Pipeline position: cold init family, like tripod_init/stress_init. The
// tool optimizes float coords in the unit square with hand-derived
// gradients + Adam, snapshots the true (k, totalX) every 25 epochs, and
// writes the best snapshot as distinct integer coords on the input canvas.
//
// Why the extras beyond the paper:
//  - SigmoidX alone has cheat minima (shrink a hot edge to zero length, or
//    collapse the layout onto a line: every pair goes guard-skipped and the
//    objective vanishes). An edge-length spring + short-range repulsion
//    forbid both.
//  - Phase A anneals the plain pair-sum at low->mid steepness (dense
//    signal everywhere); phase B re-weights pairs by a lagged softmax over
//    per-edge soft crossing counts, aiming the gradient at the hot band —
//    the local-k extension suggested by the course staff.
// Prototype validation (python, internal-2026): raw init 05 k=234
// (totalX 96k, below the then-record SA totalX), 06 k=242; +10-min warm SA:
// 05 = 183/183 (all-time record, rival 193), 06 = 195/196 vs tripod 211.
// 03/08 stay with their stress/staged families.
//
// CLI: gradx_init -i in.json -o out.json [-s seed] [-t seconds]
//      [--split f] [--t0 v] [--t1 v] [--t2 v] [--lr v] [--lambda-edge v]
//      [--lambda-rep v] [--gamma v] [--sharp0 v] [--sharp1 v]
//      [--threads N] [--jitter 0|1] [--fdcheck]

#include "../common/json.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using std::string;
using std::vector;
typedef long long ll;

static string idKey(const mjson::Value& iv, int fallbackIndex) {
    if (iv.isStr()) return iv.asString();
    if (iv.isNum()) return std::to_string(iv.asLL());
    return std::to_string(fallbackIndex);
}

struct Pt { double x = 0, y = 0; };

static inline double sigT(double z) {
    // numerically stable logistic
    if (z >= 0) return 1.0 / (1.0 + std::exp(-z));
    double e = std::exp(z);
    return e / (1.0 + e);
}

// ------------------------------------------------------------------ //
// One fused surrogate pass over the non-adjacent pair list.
// Accumulates the gradient (if grad!=null), per-edge soft counts ce,
// and the weighted objective total. Multithreaded, deterministic
// reduction (thread buffers summed in thread-index order).
// ------------------------------------------------------------------ //
struct PassResult { double total = 0; };

static PassResult surrogatePass(const vector<Pt>& P,
                                const vector<std::pair<int,int>>& E,
                                const vector<std::pair<int,int>>& pairs,
                                double T, const vector<double>& wEdge,
                                vector<Pt>* grad, vector<double>& ce,
                                int nThreads) {
    const int n = (int)P.size();
    const int m = (int)E.size();
    const double marg = 8.0 / T;
    const double Mpk = sigT(T * 0.5) * sigT(T * 0.5);
    std::atomic<size_t> cursor{0};
    const size_t CHUNK = 8192;

    vector<vector<Pt>> gBuf(nThreads);
    vector<vector<double>> cBuf(nThreads);
    vector<double> tBuf(nThreads, 0.0);
    vector<std::thread> ths;
    for (int ti = 0; ti < nThreads; ti++) {
        ths.emplace_back([&, ti]() {
            vector<Pt>& G = gBuf[ti];
            if (grad) G.assign(n, Pt{});
            vector<double>& C = cBuf[ti];
            C.assign(m, 0.0);
            double tot = 0;
            for (;;) {
                size_t lo = cursor.fetch_add(CHUNK);
                if (lo >= pairs.size()) break;
                size_t hi = std::min(pairs.size(), lo + CHUNK);
                for (size_t pi = lo; pi < hi; pi++) {
                    int e1 = pairs[pi].first, e2 = pairs[pi].second;
                    const Pt A = P[E[e1].first], B = P[E[e1].second];
                    const Pt C2 = P[E[e2].first], D = P[E[e2].second];
                    double d1x = B.x - A.x, d1y = B.y - A.y;
                    double d2x = D.x - C2.x, d2y = D.y - C2.y;
                    double rx = C2.x - A.x, ry = C2.y - A.y;
                    double sx = D.x - A.x, sy = D.y - A.y;
                    double dlt = d1x * d2y - d1y * d2x;
                    double n1 = std::hypot(d1x, d1y), n2 = std::hypot(d2x, d2y);
                    if (std::fabs(dlt) <= 1e-9 * n1 * n2 + 1e-14) continue;
                    double t = (rx * sy - ry * sx) / dlt;
                    if (t < -marg || t > 1 + marg) continue;
                    double u = (rx * d1y - ry * d1x) / dlt;
                    if (u < -marg || u > 1 + marg) continue;
                    double s0 = sigT(T * t), s1 = sigT(T * (t - 1));
                    double q0 = sigT(T * u), q1 = sigT(T * (u - 1));
                    double Mt = s0 * (1 - s1) / Mpk;
                    double Mu = q0 * (1 - q1) / Mpk;
                    double Pc = Mt * Mu;
                    C[e1] += Pc; C[e2] += Pc;
                    double w = wEdge[e1] + wEdge[e2];
                    tot += w * Pc;
                    if (!grad) continue;
                    double dMt = T * (s0 * (1 - s0) * (1 - s1) - s0 * s1 * (1 - s1)) / Mpk;
                    double dMu = T * (q0 * (1 - q0) * (1 - q1) - q0 * q1 * (1 - q1)) / Mpk;
                    double gt = w * Mu * dMt / dlt;
                    double gu = w * Mt * dMu / dlt;
                    // partials: dDelta per point, dNt (Nt=cross(r,s)), dNu (Nu=cross(r,d1))
                    double dDAx = -d2y, dDAy = d2x;      // dDelta/dA ; dDelta/dB = -these
                    double dDCx = d1y,  dDCy = -d1x;     // dDelta/dC ; dDelta/dD = -these
                    double NtAx = ry - sy, NtAy = sx - rx;
                    double NtCx = sy,      NtCy = -sx;
                    double NtDx = -ry,     NtDy = rx;
                    double NuAx = ry - d1y, NuAy = d1x - rx;
                    double NuBx = -ry,      NuBy = rx;
                    double NuCx = d1y,      NuCy = -d1x;
                    int a = E[e1].first, b = E[e1].second;
                    int c = E[e2].first, d = E[e2].second;
                    G[a].x += gt * (NtAx - t * dDAx) + gu * (NuAx - u * dDAx);
                    G[a].y += gt * (NtAy - t * dDAy) + gu * (NuAy - u * dDAy);
                    G[b].x += gt * (-t * -dDAx)      + gu * (NuBx - u * -dDAx);
                    G[b].y += gt * (-t * -dDAy)      + gu * (NuBy - u * -dDAy);
                    G[c].x += gt * (NtCx - t * dDCx) + gu * (NuCx - u * dDCx);
                    G[c].y += gt * (NtCy - t * dDCy) + gu * (NuCy - u * dDCy);
                    G[d].x += gt * (NtDx - t * -dDCx) + gu * (-u * -dDCx);
                    G[d].y += gt * (NtDy - t * -dDCy) + gu * (-u * -dDCy);
                }
            }
            tBuf[ti] = tot;
        });
    }
    for (auto& t : ths) t.join();
    PassResult res;
    std::fill(ce.begin(), ce.end(), 0.0);
    for (int ti = 0; ti < nThreads; ti++) {
        res.total += tBuf[ti];
        for (int e = 0; e < m; e++) ce[e] += cBuf[ti][e];
        if (grad)
            for (int i = 0; i < n; i++) {
                (*grad)[i].x += gBuf[ti][i].x;
                (*grad)[i].y += gBuf[ti][i].y;
            }
    }
    return res;
}

// exact proper-crossing max/total on float coords (threaded)
static void exactK(const vector<Pt>& P, const vector<std::pair<int,int>>& E,
                   const vector<std::pair<int,int>>& pairs, int nThreads,
                   int& kOut, ll& txOut) {
    const int m = (int)E.size();
    std::atomic<size_t> cursor{0};
    const size_t CHUNK = 16384;
    vector<vector<int>> cBuf(nThreads);
    vector<std::thread> ths;
    for (int ti = 0; ti < nThreads; ti++) {
        ths.emplace_back([&, ti]() {
            vector<int>& C = cBuf[ti];
            C.assign(m, 0);
            auto ccw = [](const Pt& p, const Pt& q, const Pt& r) {
                return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
            };
            for (;;) {
                size_t lo = cursor.fetch_add(CHUNK);
                if (lo >= pairs.size()) break;
                size_t hi = std::min(pairs.size(), lo + CHUNK);
                for (size_t pi = lo; pi < hi; pi++) {
                    int e1 = pairs[pi].first, e2 = pairs[pi].second;
                    const Pt& A = P[E[e1].first]; const Pt& B = P[E[e1].second];
                    const Pt& C2 = P[E[e2].first]; const Pt& D = P[E[e2].second];
                    if (ccw(A, C2, D) * ccw(B, C2, D) < 0 &&
                        ccw(A, B, C2) * ccw(A, B, D) < 0) { C[e1]++; C[e2]++; }
                }
            }
        });
    }
    for (auto& t : ths) t.join();
    int k = 0; ll tx = 0;
    vector<ll> ce(m, 0);
    for (int ti = 0; ti < nThreads; ti++)
        for (int e = 0; e < m; e++) ce[e] += cBuf[ti][e];
    for (int e = 0; e < m; e++) { k = std::max(k, (int)ce[e]); tx += ce[e]; }
    kOut = k; txOut = tx / 2;
}

// spring + short-range repulsion (single-threaded; n<=600)
static void regGrads(const vector<Pt>& P, const vector<std::pair<int,int>>& E,
                     double lamE, double lamR, double L0, double r0,
                     vector<Pt>& grad) {
    const int n = (int)P.size();
    const int m = (int)E.size();
    if (lamE > 0) {
        double c0 = 2.0 * lamE / (m * L0 * L0);
        for (auto& e : E) {
            double dx = P[e.second].x - P[e.first].x;
            double dy = P[e.second].y - P[e.first].y;
            double dl = std::max(std::hypot(dx, dy), 1e-12);
            double coef = c0 * (dl - L0) / dl;
            grad[e.second].x += coef * dx; grad[e.second].y += coef * dy;
            grad[e.first].x -= coef * dx;  grad[e.first].y -= coef * dy;
        }
    }
    if (lamR > 0) {
        double c0 = 2.0 * lamR / (n * r0 * r0);
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++) {
                double dx = P[i].x - P[j].x, dy = P[i].y - P[j].y;
                double d = std::hypot(dx, dy);
                if (d >= r0) continue;
                d = std::max(d, 1e-12);
                double coef = -c0 * (r0 - d) / d;
                grad[i].x += coef * dx; grad[i].y += coef * dy;
                grad[j].x -= coef * dx; grad[j].y -= coef * dy;
            }
    }
}

// finite-difference check of the surrogate gradient (small random configs)
static bool fdCheck() {
    std::mt19937_64 rng(7);
    std::uniform_real_distribution<double> U(0.1, 0.9), W(0.2, 1.0);
    vector<std::pair<int,int>> E = {{0,1},{2,3},{4,5},{6,7},{1,2},{5,6}};
    vector<std::pair<int,int>> pairs;
    for (int i = 0; i < (int)E.size(); i++)
        for (int j = i + 1; j < (int)E.size(); j++) {
            auto& a = E[i]; auto& b = E[j];
            if (a.first!=b.first && a.first!=b.second && a.second!=b.first && a.second!=b.second)
                pairs.push_back({i, j});
        }
    bool ok = true;
    for (double T : {4.0, 12.0, 40.0}) {
        for (int trial = 0; trial < 7; trial++) {
            vector<Pt> P(8);
            for (auto& p : P) { p.x = U(rng); p.y = U(rng); }
            vector<double> w(E.size());
            for (auto& x : w) x = W(rng);
            vector<double> ce(E.size());
            vector<Pt> g(8);
            surrogatePass(P, E, pairs, T, w, &g, ce, 2);
            for (int probe = 0; probe < 6; probe++) {
                int i = (int)(rng() % 8), c = (int)(rng() % 2);
                double h = 1e-6;
                auto evalAt = [&](double delta) {
                    vector<Pt> Q = P;
                    (c == 0 ? Q[i].x : Q[i].y) += delta;
                    vector<double> ce2(E.size());
                    surrogatePass(Q, E, pairs, T, w, nullptr, ce2, 2);
                    double f = 0;
                    for (size_t e = 0; e < E.size(); e++) f += w[e] * ce2[e];
                    return f;
                };
                double num = (evalAt(h) - evalAt(-h)) / (2 * h);
                double ana = c == 0 ? g[i].x : g[i].y;
                double rel = std::fabs(num - ana) /
                             std::max({1e-8, std::fabs(num), std::fabs(ana)});
                if (rel > 1e-4 && std::fabs(num - ana) > 1e-8) {
                    fprintf(stderr, "FD FAIL T=%.0f i=%d c=%d num=%.6e ana=%.6e rel=%.2e\n",
                            T, i, c, num, ana, rel);
                    ok = false;
                }
            }
        }
    }
    fprintf(stderr, "gradx_init fdcheck: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

int main(int argc, char** argv) {
    string inPath, outPath;
    ll seed = 1;
    double budget = 15.0;          // seconds; -t is honored
    double split = 0.45, t0 = 4, t1 = 24, t2 = 60, lr0 = 0.03;
    double lamE = 0.2, lamR = 0.5, gamma = 0.15, sharp0 = 2, sharp1 = 8;
    int nThreads = (int)std::max(2u, std::thread::hardware_concurrency());
    bool jitter = true, doFd = false;
    auto need = [&](int& i) -> string {
        if (i + 1 >= argc) { fprintf(stderr, "gradx_init: missing value for %s\n", argv[i]); exit(2); }
        return argv[++i];
    };
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        if (a == "-i" || a == "--input") inPath = need(i);
        else if (a == "-o" || a == "--output") outPath = need(i);
        else if (a == "-s" || a == "--seed") seed = std::stoll(need(i));
        else if (a == "-t") budget = std::stod(need(i));
        else if (a == "--split") split = std::stod(need(i));
        else if (a == "--t0") t0 = std::stod(need(i));
        else if (a == "--t1") t1 = std::stod(need(i));
        else if (a == "--t2") t2 = std::stod(need(i));
        else if (a == "--lr") lr0 = std::stod(need(i));
        else if (a == "--lambda-edge") lamE = std::stod(need(i));
        else if (a == "--lambda-rep") lamR = std::stod(need(i));
        else if (a == "--gamma") gamma = std::stod(need(i));
        else if (a == "--sharp0") sharp0 = std::stod(need(i));
        else if (a == "--sharp1") sharp1 = std::stod(need(i));
        else if (a == "--threads") nThreads = std::stoi(need(i));
        else if (a == "--jitter") jitter = std::stoi(need(i)) != 0;
        else if (a == "--fdcheck") doFd = true;
        else { fprintf(stderr, "gradx_init: unknown arg %s\n", a.c_str()); return 2; }
    }
    if (doFd) return fdCheck() ? 0 : 1;
    if (inPath.empty() || outPath.empty()) {
        fprintf(stderr, "usage: gradx_init -i in.json -o out.json [-s seed] [-t seconds]\n");
        return 2;
    }
    auto wallNow = []() {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    double w0 = wallNow();

    mjson::Value root = mjson::parseFile(inPath);
    string nodesKey = root.has("nodes") ? "nodes" : "Nodes";
    string edgesKey = root.has("edges") ? "edges" : "Edges";
    ll Wc = root.has("width") ? root["width"].asLL() : 1000000;
    ll Hc = root.has("height") ? root["height"].asLL() : 1000000;
    ll ox = root.has("x") ? root["x"].asLL() : 0;
    ll oy = root.has("y") ? root["y"].asLL() : 0;
    double W = (double)Wc, H = (double)Hc;

    auto& nodesArr = root[nodesKey].arr;
    const int n = (int)nodesArr.size();
    std::unordered_map<string,int> idOf;
    for (int i = 0; i < n; i++)
        idOf[nodesArr[i].has("id") ? idKey(nodesArr[i].at("id"), i)
                                   : std::to_string(i)] = i;
    vector<std::pair<int,int>> E;
    std::set<std::pair<int,int>> seenE;
    for (auto& ev : root[edgesKey].asArray()) {
        string s, t;
        if      (ev.has("source")) s = idKey(ev.at("source"), -1);
        else if (ev.has("from"))   s = idKey(ev.at("from"), -1);
        if      (ev.has("target")) t = idKey(ev.at("target"), -1);
        else if (ev.has("to"))     t = idKey(ev.at("to"), -1);
        auto its = idOf.find(s), itt = idOf.find(t);
        if (its == idOf.end() || itt == idOf.end()) continue;
        int a = its->second, b = itt->second;
        if (a == b) continue;
        auto key = std::minmax(a, b);
        if (!seenE.insert({key.first, key.second}).second) continue;
        E.push_back({key.first, key.second});
    }
    const int m = (int)E.size();

    vector<std::pair<int,int>> pairs;
    pairs.reserve((size_t)m * (m - 1) / 2);
    for (int i = 0; i < m; i++)
        for (int j = i + 1; j < m; j++)
            if (E[i].first != E[j].first && E[i].first != E[j].second &&
                E[i].second != E[j].first && E[i].second != E[j].second)
                pairs.push_back({i, j});

    std::mt19937_64 rng((uint64_t)seed * 0x9E3779B97F4A7C15ULL + 1);
    std::uniform_real_distribution<double> UI(0.15, 0.85), UJ(-0.3, 0.3), UL(-0.5, 0.5);
    vector<Pt> P(n);
    for (auto& p : P) { p.x = UI(rng); p.y = UI(rng); }
    if (jitter) {
        t1 *= std::pow(2.0, UJ(rng));
        t2 *= std::pow(2.0, UJ(rng));
        lr0 *= std::pow(2.0, UL(rng));
    }

    const double L0 = std::sqrt(1.0 / n), r0 = 0.5 / std::sqrt((double)n);
    vector<double> wEdge(m, 1.0 / m), ce(m, 0.0), cePrev;
    vector<Pt> mAd(n), vAd(n), grad(n);
    const double b1 = 0.9, b2 = 0.99, eps = 1e-8;
    int step = 0, epoch = 0, bestEp = -1, sinceBest = 0;
    int bestK = 1 << 30; ll bestTx = (1LL << 62);
    vector<Pt> bestP = P;
    const double tSplit = split * budget;

    while (true) {
        double el = wallNow() - w0;
        if (el >= budget) break;
        bool inB = el >= tSplit;
        double T, lr;
        if (!inB) {
            double pr = std::min(1.0, el / std::max(1e-9, tSplit));
            T = t0 * std::pow(t1 / t0, pr);
            lr = 0.01 + (lr0 - 0.01) * (0.5 + 0.5 * std::cos(M_PI * pr));
            std::fill(wEdge.begin(), wEdge.end(), 1.0 / m);
        } else {
            double pr = std::min(1.0, (el - tSplit) / std::max(1e-9, budget - tSplit));
            T = t1 * std::pow(t2 / t1, pr);
            lr = 0.003 + (0.01 - 0.003) * (0.5 + 0.5 * std::cos(M_PI * pr));
            if (!cePrev.empty()) {
                double cmax = 0;
                for (double c : cePrev) cmax = std::max(cmax, c);
                if (cmax > 0) {
                    vector<double> sorted = cePrev;
                    std::nth_element(sorted.begin(), sorted.begin() + m / 2, sorted.end());
                    double cmed = sorted[m / 2];
                    double sharp = sharp0 + (sharp1 - sharp0) * pr;
                    double beta = sharp / (cmax - cmed + 1.0);
                    double sum = 0;
                    for (int e = 0; e < m; e++) { wEdge[e] = std::exp(beta * (cePrev[e] - cmax)); sum += wEdge[e]; }
                    for (int e = 0; e < m; e++) wEdge[e] = (1 - gamma) * wEdge[e] / sum + gamma / m;
                }
            }
        }
        for (auto& g : grad) g = Pt{};
        surrogatePass(P, E, pairs, T, wEdge, &grad, ce, nThreads);
        cePrev = ce;
        regGrads(P, E, lamE, lamR, L0, r0, grad);
        step++;
        double c1 = 1 - std::pow(b1, step), c2 = 1 - std::pow(b2, step);
        for (int i = 0; i < n; i++) {
            mAd[i].x = b1 * mAd[i].x + (1 - b1) * grad[i].x;
            mAd[i].y = b1 * mAd[i].y + (1 - b1) * grad[i].y;
            vAd[i].x = b2 * vAd[i].x + (1 - b2) * grad[i].x * grad[i].x;
            vAd[i].y = b2 * vAd[i].y + (1 - b2) * grad[i].y * grad[i].y;
            P[i].x -= lr * (mAd[i].x / c1) / (std::sqrt(vAd[i].x / c2) + eps);
            P[i].y -= lr * (mAd[i].y / c1) / (std::sqrt(vAd[i].y / c2) + eps);
            P[i].x = std::min(0.98, std::max(0.02, P[i].x));
            P[i].y = std::min(0.98, std::max(0.02, P[i].y));
        }
        epoch++;
        if (epoch % 25 == 0) {
            int k; ll tx;
            exactK(P, E, pairs, nThreads, k, tx);
            if (k < bestK || (k == bestK && tx < bestTx)) {
                bestK = k; bestTx = tx; bestP = P; bestEp = epoch; sinceBest = 0;
            } else if (++sinceBest >= 8 && inB) {
                break;   // 200 epochs without improvement, deep in phase B
            }
        }
    }
    {
        int k; ll tx;
        exactK(P, E, pairs, nThreads, k, tx);
        if (k < bestK || (k == bestK && tx < bestTx)) {
            bestK = k; bestTx = tx; bestP = P; bestEp = epoch;
        }
    }

    // scale best snapshot to canvas + distinct-integer snap (tripod style)
    double lox = 1e18, loy = 1e18, hix = -1e18, hiy = -1e18;
    for (auto& p : bestP) {
        lox = std::min(lox, p.x); hix = std::max(hix, p.x);
        loy = std::min(loy, p.y); hiy = std::max(hiy, p.y);
    }
    double spx = std::max(hix - lox, 1e-9), spy = std::max(hiy - loy, 1e-9);
    double scale = std::min(0.96 * W / spx, 0.96 * H / spy);
    std::set<std::pair<ll,ll>> used;
    std::uniform_int_distribution<int> J(-9, 9);
    for (int i = 0; i < n; i++) {
        ll x = ox + llround((bestP[i].x - lox) * scale + (W - spx * scale) / 2);
        ll y = oy + llround((bestP[i].y - loy) * scale + (H - spy * scale) / 2);
        while (used.count({x, y})) {
            x = std::min(ox + Wc, std::max(ox, x + J(rng)));
            y = std::min(oy + Hc, std::max(oy, y + J(rng)));
        }
        used.insert({x, y});
        nodesArr[i]["x"] = mjson::Value((long long)x);
        nodesArr[i]["y"] = mjson::Value((long long)y);
    }
    mjson::writeFile(outPath, root, 0);
    fprintf(stderr,
            "gradx_init: n=%d m=%d pairs=%zu epochs=%d best_k=%d best_tx=%lld "
            "at_ep=%d wall=%.1fs -> %s\n",
            n, m, pairs.size(), epoch, bestK, bestTx, bestEp,
            wallNow() - w0, outPath.c_str());
    return 0;
}
