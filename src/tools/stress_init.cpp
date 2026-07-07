// stress_init - force-directed initial layout generator (GD-2025 k-planarity).
// C++ port of tools/stress_init.py.
//
// Replicates the initialization recipe of the winning SAkGD entry
// (Bianchetti & Moalic, LIPIcs.GD.2025.43): a deterministic stress layout
// plus repeated stochastic force-directed runs, keeping the candidate with
// the lowest sampled crossing density. Uses graphviz engines as the layout
// back-ends: neato (stress majorization) and sfdp (multilevel force).
//
// Continuous coordinates are scaled to fill the contest grid, snapped to
// integer points, and position collisions are resolved by a spiral search
// for the nearest free cell. Vertex-on-edge overlaps are left to the
// solver's existing repair pass.
//
// Output is a contest-format JSON usable as a warm-start input for sakgd.
//
// Usage: stress_init -i input.json -o output.json [-s seed] [-t seconds]
//                     [--engine auto|neato|sfdp|both] [--max-attempts N]

#include "../common/json.hpp"
#include "../common/subprocess.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using ll = long long;
using Clock = std::chrono::steady_clock;

struct Pt2 { double x, y; };

static std::string idKey(const mjson::Value& iv, int fallbackIndex) {
    if (iv.isStr()) return iv.asString();
    if (iv.isNum()) return std::to_string(iv.asLL());
    return std::to_string(fallbackIndex);
}

struct Graph {
    mjson::Value data;
    std::string nodesKey, edgesKey;
    int n = 0;
    std::vector<std::pair<int,int>> edges;
    ll W = 1000000, H = 1000000, x0 = 0, y0 = 0;
};

static Graph loadGraph(const std::string& path) {
    Graph g;
    g.data = mjson::parseFile(path);
    const auto& root = g.data;

    g.nodesKey = root.has("nodes") ? "nodes" : "Nodes";
    g.edgesKey = root.has("edges") ? "edges" : "Edges";
    if (!root.has(g.nodesKey)) throw std::runtime_error("Input has no 'nodes' array");
    if (!root.has(g.edgesKey)) throw std::runtime_error("Input has no 'edges' array");

    g.W  = root.has("width")  ? root["width"].asLL()  : root.getLL("Width", 1000000);
    g.H  = root.has("height") ? root["height"].asLL() : root.getLL("Height", 1000000);
    g.x0 = root.has("x") ? root["x"].asLL() : root.getLL("X", 0);
    g.y0 = root.has("y") ? root["y"].asLL() : root.getLL("Y", 0);

    const auto& nodesArr = root[g.nodesKey].asArray();
    g.n = (int)nodesArr.size();
    std::unordered_map<std::string,int> id2idx;
    id2idx.reserve(g.n * 2);
    for (int i = 0; i < g.n; i++) {
        std::string id = nodesArr[i].has("id") ? idKey(nodesArr[i].at("id"), i)
                                                : std::to_string(i);
        id2idx[id] = i;
    }

    const auto& edgesArr = root[g.edgesKey].asArray();
    g.edges.reserve(edgesArr.size());
    for (const auto& ev : edgesArr) {
        std::string s, t;
        if      (ev.has("source")) s = idKey(ev.at("source"), -1);
        else if (ev.has("from"))   s = idKey(ev.at("from"), -1);
        if      (ev.has("target")) t = idKey(ev.at("target"), -1);
        else if (ev.has("to"))     t = idKey(ev.at("to"), -1);
        auto its = id2idx.find(s), itt = id2idx.find(t);
        if (its == id2idx.end() || itt == id2idx.end())
            throw std::runtime_error("Edge references unknown node id");
        g.edges.emplace_back(its->second, itt->second);
    }
    return g;
}

// Runs a graphviz engine over stdin dot source; returns node positions or
// empty vector on failure/timeout/incomplete output.
static std::vector<Pt2> runGraphviz(const std::string& engine, int n,
                                     const std::vector<std::pair<int,int>>& edges,
                                     long long seed, double timeoutSec) {
    std::ostringstream dot;
    dot << "graph G {\nnode [shape=point];\n";
    for (int i = 0; i < n; i++) dot << i << ";\n";
    for (auto& e : edges) dot << e.first << "--" << e.second << ";\n";
    dot << "}\n";

    std::vector<std::string> cmd = {engine, "-Tplain", "-Gstart=" + std::to_string(seed)};
    if (engine == "neato" && n > 1000) cmd.push_back("-Gmodel=subset");

    auto res = proc::runCaptured(cmd, dot.str(), timeoutSec);
    if (!res.ok) return {};

    std::vector<Pt2> pos(n, Pt2{0, 0});
    int seen = 0;
    std::istringstream iss(res.stdoutData);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.rfind("node ", 0) != 0) continue;
        std::istringstream ls(line);
        std::string tag; int i; double x, y;
        ls >> tag >> i >> x >> y;
        if (i < 0 || i >= n) continue;
        pos[i] = {x, y};
        seen++;
    }
    if (seen != n) return {};
    return pos;
}

// Scales continuous coords to fill the grid, snaps to distinct integer
// points; collisions resolved by a spiral search over the square ring at
// increasing Chebyshev radius (matches tools/stress_init.py exactly).
static std::vector<std::pair<ll,ll>> snapToGrid(const std::vector<Pt2>& pos,
                                                 ll W, ll H, ll x0, ll y0,
                                                 std::mt19937_64& rng) {
    int n = (int)pos.size();
    double loX = pos[0].x, hiX = pos[0].x, loY = pos[0].y, hiY = pos[0].y;
    for (auto& p : pos) {
        loX = std::min(loX, p.x); hiX = std::max(hiX, p.x);
        loY = std::min(loY, p.y); hiY = std::max(hiY, p.y);
    }
    double spanX = std::max(hiX - loX, 1e-9);
    double spanY = std::max(hiY - loY, 1e-9);

    std::vector<ll> ix(n), iy(n);
    for (int i = 0; i < n; i++) {
        double sx = (pos[i].x - loX) / spanX * (double)W;
        double sy = (pos[i].y - loY) / spanY * (double)H;
        ix[i] = std::clamp((ll)std::llround(sx), (ll)0, W);
        iy[i] = std::clamp((ll)std::llround(sy), (ll)0, H);
    }

    struct PairHash { size_t operator()(const std::pair<ll,ll>& p) const {
        return std::hash<ll>{}(p.first) * 1000003u ^ std::hash<ll>{}(p.second);
    }};
    std::unordered_set<std::pair<ll,ll>, PairHash> occupied;
    occupied.reserve(n * 2);
    std::vector<std::pair<ll,ll>> out(n);

    std::vector<int> order(n);
    for (int i = 0; i < n; i++) order[i] = i;
    std::shuffle(order.begin(), order.end(), rng);

    ll maxR = std::max(W, H);
    for (int i : order) {
        ll x = ix[i], y = iy[i];
        if (!occupied.count({x, y})) {
            occupied.insert({x, y});
            out[i] = {x, y};
            continue;
        }
        bool placed = false;
        for (ll r = 1; r <= maxR && !placed; r++) {
            for (ll dx = -r; dx <= r && !placed; dx++) {
                bool edgeCol = (dx == -r || dx == r);
                if (edgeCol) {
                    for (ll dy = -r; dy <= r; dy++) {
                        ll cx = x + dx, cy = y + dy;
                        if (cx < 0 || cx > W || cy < 0 || cy > H) continue;
                        if (!occupied.count({cx, cy})) {
                            occupied.insert({cx, cy});
                            out[i] = {cx, cy};
                            placed = true;
                            break;
                        }
                    }
                } else {
                    for (ll dy : {-r, r}) {
                        ll cx = x + dx, cy = y + dy;
                        if (cx < 0 || cx > W || cy < 0 || cy > H) continue;
                        if (!occupied.count({cx, cy})) {
                            occupied.insert({cx, cy});
                            out[i] = {cx, cy};
                            placed = true;
                            break;
                        }
                    }
                }
            }
        }
        if (!placed) throw std::runtime_error("grid full: cannot place all nodes");
    }
    for (int i = 0; i < n; i++) { out[i].first += x0; out[i].second += y0; }
    return out;
}

// Estimates average crossings per edge via random sampling of edge pairs.
static double sampledAvgCrossings(const std::vector<std::pair<ll,ll>>& coords,
                                   const std::vector<std::pair<int,int>>& edges,
                                   std::mt19937_64& rng, long long samples = 200000) {
    long long m = (long long)edges.size();
    long long totalPairs = m * (m - 1) / 2;
    if (totalPairs <= 0) return 0.0;
    long long s = std::min(samples, totalPairs);

    std::uniform_int_distribution<long long> dist(0, m - 1);
    auto cross = [](double ox, double oy, double ux, double uy, double vx, double vy) {
        return (ux - ox) * (vy - oy) - (uy - oy) * (vx - ox);
    };

    long long kept = 0, hit = 0;
    for (long long k = 0; k < s; k++) {
        long long a = dist(rng), b = dist(rng);
        if (a == b) continue;
        int a0 = edges[a].first, a1 = edges[a].second;
        int b0 = edges[b].first, b1 = edges[b].second;
        if (a0 == b0 || a0 == b1 || a1 == b0 || a1 == b1) continue;
        kept++;
        double p1x = (double)coords[a0].first, p1y = (double)coords[a0].second;
        double p2x = (double)coords[a1].first, p2y = (double)coords[a1].second;
        double q1x = (double)coords[b0].first, q1y = (double)coords[b0].second;
        double q2x = (double)coords[b1].first, q2y = (double)coords[b1].second;
        double d1 = cross(p1x, p1y, p2x, p2y, q1x, q1y);
        double d2 = cross(p1x, p1y, p2x, p2y, q2x, q2y);
        double d3 = cross(q1x, q1y, q2x, q2y, p1x, p1y);
        double d4 = cross(q1x, q1y, q2x, q2y, p2x, p2y);
        if (d1 * d2 < 0 && d3 * d4 < 0) hit++;
    }
    if (kept == 0) return 0.0;
    double rate = (double)hit / (double)kept;
    double estTotal = rate * (double)totalPairs;
    return 2.0 * estTotal / (double)m;
}

struct Args {
    std::string input, output;
    long long seed = 1;
    double timeBudget = 60.0;
    std::string engine = "auto";
    int maxAttempts = 8;
};

static Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string s = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + s);
            return argv[++i];
        };
        if (s == "-i" || s == "--input") a.input = next();
        else if (s == "-o" || s == "--output") a.output = next();
        else if (s == "-s" || s == "--seed") a.seed = std::stoll(next());
        else if (s == "-t" || s == "--time-budget") a.timeBudget = std::stod(next());
        else if (s == "--engine") a.engine = next();
        else if (s == "--max-attempts") a.maxAttempts = std::stoi(next());
        else throw std::runtime_error("unknown argument: " + s);
    }
    if (a.input.empty() || a.output.empty())
        throw std::runtime_error("--input and --output are required");
    return a;
}

int main(int argc, char** argv) {
    Args args;
    try {
        args = parseArgs(argc, argv);
    } catch (std::exception& e) {
        std::cerr << "stress_init: " << e.what() << "\n"
                  << "usage: stress_init -i input.json -o output.json [-s seed] "
                     "[-t seconds] [--engine auto|neato|sfdp|both] [--max-attempts N]\n";
        return 2;
    }

    auto t0 = Clock::now();
    Graph g;
    try {
        g = loadGraph(args.input);
    } catch (std::exception& e) {
        std::cerr << "stress_init: failed to load " << args.input << ": " << e.what() << "\n";
        return 1;
    }
    int n = g.n;

    std::mt19937_64 rngPy((uint64_t)args.seed * 2 + 1);   // rotation angle + shuffle/spiral
    std::mt19937_64 rngScore((uint64_t)args.seed * 2);    // crossing-density sampling

    std::vector<std::string> engines;
    if (args.engine == "auto") {
        engines = (n > 3000) ? std::vector<std::string>{"sfdp"}
                              : std::vector<std::string>{"sfdp", "neato"};
    } else if (args.engine == "both") {
        engines = {"neato", "sfdp"};
    } else {
        engines = {args.engine};
    }

    std::vector<std::pair<ll,ll>> bestCoords;
    double bestScore = std::numeric_limits<double>::infinity();
    std::string bestTag;
    int attempt = 0;

    while (attempt < args.maxAttempts) {
        double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
        double remain = args.timeBudget - elapsed;
        if (attempt > 0 && remain <= 1.0) break;

        const std::string& engine = engines[attempt % engines.size()];
        long long gvSeed = args.seed * 1000 + attempt;
        double timeout = std::max(remain, 30.0);
        auto pos = runGraphviz(engine, n, g.edges, gvSeed, timeout);
        attempt++;

        if (pos.empty()) {
            std::cerr << "[stress-init] attempt " << attempt << " " << engine
                       << ": failed/timeout\n";
            continue;
        }

        std::uniform_real_distribution<double> angDist(0.0, 2.0 * M_PI);
        double ang = angDist(rngPy);
        double c = std::cos(ang), s = std::sin(ang);
        for (auto& p : pos) {
            double x = p.x * c - p.y * s;
            double y = p.x * s + p.y * c;
            p = {x, y};
        }

        std::vector<std::pair<ll,ll>> coords;
        try {
            coords = snapToGrid(pos, g.W, g.H, g.x0, g.y0, rngPy);
        } catch (std::exception& e) {
            std::cerr << "[stress-init] attempt " << attempt << " " << engine
                       << ": " << e.what() << "\n";
            continue;
        }

        double score = sampledAvgCrossings(coords, g.edges, rngScore);
        std::cerr << "[stress-init] attempt " << attempt << " " << engine
                   << ": est avg crossings/edge = " << score << "\n";
        if (score < bestScore) {
            bestScore = score;
            bestCoords = coords;
            bestTag = engine;
        }
    }

    if (bestCoords.empty()) {
        std::cerr << "[stress-init] all attempts failed; copying input layout\n";
        try {
            mjson::writeFile(args.output, g.data, 0);
        } catch (std::exception& e) {
            std::cerr << "stress_init: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    auto& nodesArr = g.data[g.nodesKey].arr;
    for (int i = 0; i < n && i < (int)nodesArr.size(); i++) {
        nodesArr[i]["x"] = mjson::Value((long long)bestCoords[i].first);
        nodesArr[i]["y"] = mjson::Value((long long)bestCoords[i].second);
    }
    try {
        mjson::writeFile(args.output, g.data, 0);
    } catch (std::exception& e) {
        std::cerr << "stress_init: " << e.what() << "\n";
        return 1;
    }

    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
    std::cerr << "[stress-init] done: engine=" << bestTag
               << " est avg crossings/edge=" << bestScore
               << " elapsed=" << elapsed << "s\n";
    return 0;
}
