// SAkGD - Simulated Annealing for Graph Drawing Contest 2025 (k-planarity)
// VERSION: 1.1.1
// Faithful C++ reimplementation of the approach described in:
//   Bianchetti & Moalic, "Winning the GD Challenge for the 4th Time: Our Approach"
//   33rd International Symposium on Graph Drawing and Network Visualization (GD 2025)
//   LIPIcs.GD.2025.43
//
// Three-stage heuristic:
//   1. Use the input layout (or a random layout) as the starting solution.
//   2. SA phase 1: minimise the total number of edge crossings.
//   3. SA phase 2: minimise the k-value (max crossings on a single edge),
//      using a local k-fitness with total-crossings as a tie-breaker.
//
// Build:  make
// Usage:  ./sakgd -i input.json -o output.json [-t total_minutes] [-p1 phase1_minutes]
//         ./sakgd input.json output.json
//
// Input JSON format (compatible with the GD contest format):
//   { "width": <int>, "height": <int>,
//     "nodes": [ { "id": <id>, "x": <int>, "y": <int> }, ... ],
//     "edges": [ { "source": <id>, "target": <id> }, ... ] }

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std;
using namespace std::chrono;

using ll  = long long;
using i64 = int64_t;

// ====================================================================
// Geometry
// ====================================================================
struct Pt {
    ll x = 0, y = 0;
    Pt() = default;
    Pt(ll a, ll b) : x(a), y(b) {}
    bool operator==(const Pt& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Pt& o) const { return !(*this == o); }
};

struct PtHash {
    size_t operator()(const Pt& p) const noexcept {
        size_t h = std::hash<ll>{}(p.x);
        h ^= std::hash<ll>{}(p.y) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

struct Edge {
    int u, v;
};

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

// True iff open segments (a,b) and (c,d) properly intersect (no shared endpoints).
static bool segCross(const Pt& a, const Pt& b, const Pt& c, const Pt& d) {
    if (a == c || a == d || b == c || b == d) return false;
    int d1 = sgn(crossp(c, d, a));
    int d2 = sgn(crossp(c, d, b));
    int d3 = sgn(crossp(a, b, c));
    int d4 = sgn(crossp(a, b, d));
    return (d1 != d2 && d3 != d4);
}

// True iff p lies strictly inside open segment (a, b)
// (collinear AND strictly between the endpoints; never at the endpoints).
static inline bool pointOnSegmentStrict(const Pt& p, const Pt& a, const Pt& b) {
    if (p == a || p == b) return false;
    if (crossp(a, b, p) != 0) return false;
    if (p.x < min(a.x, b.x) || p.x > max(a.x, b.x)) return false;
    if (p.y < min(a.y, b.y) || p.y > max(a.y, b.y)) return false;
    return true;
}

// ====================================================================
// Minimal JSON (only what we need for the GD contest format)
// ====================================================================
namespace mjson {

class Value;
using Object = std::map<std::string, Value>;
using Array  = std::vector<Value>;

class Value {
public:
    enum Type { NULL_T, BOOL_T, NUMBER_T, STRING_T, ARRAY_T, OBJECT_T };

    Type        type = NULL_T;
    bool        b    = false;
    double      num  = 0;
    std::string str;
    Array       arr;
    Object      obj;

    Value() = default;
    Value(double v)             : type(NUMBER_T), num(v) {}
    Value(int v)                : type(NUMBER_T), num((double)v) {}
    Value(long long v)          : type(NUMBER_T), num((double)v) {}
    Value(const char* v)        : type(STRING_T), str(v) {}
    Value(const std::string& v) : type(STRING_T), str(v) {}

    bool isObj() const { return type == OBJECT_T; }
    bool isArr() const { return type == ARRAY_T; }
    bool isNum() const { return type == NUMBER_T; }
    bool isStr() const { return type == STRING_T; }

    Value& operator[](const std::string& k) { type = OBJECT_T; return obj[k]; }
    const Value& operator[](const std::string& k) const { return obj.at(k); }
    Value& operator[](size_t i) { return arr[i]; }
    const Value& operator[](size_t i) const { return arr[i]; }

    const Value& at(const std::string& k) const { return obj.at(k); }
    Value&       at(const std::string& k)       { return obj.at(k); }

    int         asInt()    const { return (int)num; }
    long long   asLL()     const { return (long long)num; }
    double      asDouble() const { return num; }
    const std::string& asString() const { return str; }
    const Array&  asArray()  const { return arr; }
    const Object& asObject() const { return obj; }

    bool has(const std::string& k) const {
        return type == OBJECT_T && obj.find(k) != obj.end();
    }

    void serialize(std::ostream& os, int indent = 0, int depth = 0) const;
};

class Parser {
    const char* p;
    const char* end;

    void skipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }
    void expect(char c) {
        skipWs();
        if (p >= end || *p != c)
            throw std::runtime_error(std::string("JSON expected ") + c);
        p++;
    }
    bool peek(char c) { skipWs(); return p < end && *p == c; }

    std::string parseString() {
        expect('"');
        std::string s;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                p++;
                char c = *p++;
                switch (c) {
                    case 'n':  s += '\n'; break;
                    case 't':  s += '\t'; break;
                    case 'r':  s += '\r'; break;
                    case '\\': s += '\\'; break;
                    case '"':  s += '"';  break;
                    case '/':  s += '/';  break;
                    default:   s += c;
                }
            } else {
                s += *p++;
            }
        }
        expect('"');
        return s;
    }
    double parseNumber() {
        skipWs();
        const char* start = p;
        if (p < end && (*p == '-' || *p == '+')) p++;
        while (p < end && (isdigit((unsigned char)*p) || *p == '.' ||
                           *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) p++;
        return std::stod(std::string(start, p - start));
    }

public:
    Parser(const std::string& s) : p(s.c_str()), end(s.c_str() + s.size()) {}

    Value parse() {
        skipWs();
        if (p >= end) throw std::runtime_error("JSON empty input");
        return parseValue();
    }

    Value parseValue() {
        skipWs();
        if (p >= end) throw std::runtime_error("JSON unexpected EOF");
        if (*p == '{') return parseObject();
        if (*p == '[') return parseArray();
        if (*p == '"') {
            Value v; v.type = Value::STRING_T; v.str = parseString(); return v;
        }
        if (*p == 't') { p += 4; Value v; v.type = Value::BOOL_T; v.b = true;  return v; }
        if (*p == 'f') { p += 5; Value v; v.type = Value::BOOL_T; v.b = false; return v; }
        if (*p == 'n') { p += 4; return Value(); }
        Value v; v.type = Value::NUMBER_T; v.num = parseNumber(); return v;
    }

    Value parseObject() {
        expect('{');
        Value v; v.type = Value::OBJECT_T;
        if (peek('}')) { p++; return v; }
        while (true) {
            std::string key = parseString();
            expect(':');
            v.obj[key] = parseValue();
            skipWs();
            if (peek(',')) { p++; continue; }
            break;
        }
        expect('}');
        return v;
    }

    Value parseArray() {
        expect('[');
        Value v; v.type = Value::ARRAY_T;
        if (peek(']')) { p++; return v; }
        while (true) {
            v.arr.push_back(parseValue());
            skipWs();
            if (peek(',')) { p++; continue; }
            break;
        }
        expect(']');
        return v;
    }
};

inline Value parse(const std::string& s) { return Parser(s).parse(); }

inline void Value::serialize(std::ostream& os, int indent, int depth) const {
    auto ind = [&](int d) { for (int i = 0; i < d * indent; i++) os << ' '; };
    switch (type) {
        case NULL_T:   os << "null"; break;
        case BOOL_T:   os << (b ? "true" : "false"); break;
        case NUMBER_T:
            if (std::isfinite(num) && num == (double)(long long)num)
                os << (long long)num;
            else
                os << num;
            break;
        case STRING_T:
            os << '"';
            for (char c : str) {
                if      (c == '"')  os << "\\\"";
                else if (c == '\\') os << "\\\\";
                else if (c == '\n') os << "\\n";
                else if (c == '\t') os << "\\t";
                else                os << c;
            }
            os << '"';
            break;
        case ARRAY_T: {
            os << '[';
            if (indent && !arr.empty()) os << '\n';
            for (size_t i = 0; i < arr.size(); i++) {
                if (indent) ind(depth + 1);
                arr[i].serialize(os, indent, depth + 1);
                if (i + 1 < arr.size()) os << ',';
                if (indent) os << '\n';
            }
            if (indent && !arr.empty()) ind(depth);
            os << ']';
            break;
        }
        case OBJECT_T: {
            os << '{';
            if (indent && !obj.empty()) os << '\n';
            size_t i = 0;
            for (const auto& kv : obj) {
                if (indent) ind(depth + 1);
                os << '"' << kv.first << "\":";
                if (indent) os << ' ';
                kv.second.serialize(os, indent, depth + 1);
                if (++i < obj.size()) os << ',';
                if (indent) os << '\n';
            }
            if (indent && !obj.empty()) ind(depth);
            os << '}';
            break;
        }
    }
}

} // namespace mjson

// ====================================================================
// Input / output
// ====================================================================
struct GraphData {
    int  n = 0, m = 0;
    ll   W = 0, H = 0;
    ll   minCoordX = 0, minCoordY = 0;     // canvas origin
    vector<Pt>     pos;
    vector<Edge>   edges;
    vector<string> nodeIdStrs;
    vector<bool>   nodeIdIsString;
    bool           hasInitialPos = false;
    mjson::Value   originalJson;
};

static string slurp(const string& path) {
    ifstream f(path);
    if (!f) throw runtime_error("Cannot open " + path);
    stringstream ss; ss << f.rdbuf();
    return ss.str();
}

GraphData readGraph(const string& path) {
    GraphData g;
    g.originalJson = mjson::parse(slurp(path));
    const auto& root = g.originalJson;

    auto getInt = [&](const char* a, const char* b, ll def) -> ll {
        if (root.has(a)) return root[a].asLL();
        if (root.has(b)) return root[b].asLL();
        return def;
    };

    g.W = getInt("width",  "Width",  1000000);
    g.H = getInt("height", "Height", 1000000);
    g.minCoordX = getInt("x", "X", 0);
    g.minCoordY = getInt("y", "Y", 0);

    const mjson::Array* nodesArr = nullptr;
    if      (root.has("nodes")) nodesArr = &root["nodes"].asArray();
    else if (root.has("Nodes")) nodesArr = &root["Nodes"].asArray();
    if (!nodesArr) throw runtime_error("Input has no 'nodes' array");

    g.n = (int)nodesArr->size();
    g.pos.resize(g.n);
    g.nodeIdStrs.resize(g.n);
    g.nodeIdIsString.assign(g.n, false);

    unordered_map<string, int> idMap;
    for (int i = 0; i < g.n; i++) {
        const auto& nv = (*nodesArr)[i];
        string id; bool isStr = false;
        if (nv.has("id")) {
            const auto& iv = nv.at("id");
            if (iv.isStr())      { id = iv.asString(); isStr = true; }
            else if (iv.isNum()) { id = to_string(iv.asLL()); }
        } else {
            id = to_string(i);
        }
        g.nodeIdStrs[i]     = id;
        g.nodeIdIsString[i] = isStr;
        idMap[id]           = i;

        bool hx = nv.has("x"), hy = nv.has("y");
        if (hx && hy) {
            g.pos[i].x = nv.at("x").asLL();
            g.pos[i].y = nv.at("y").asLL();
            g.hasInitialPos = true;
        }
    }

    const mjson::Array* edgesArr = nullptr;
    if      (root.has("edges")) edgesArr = &root["edges"].asArray();
    else if (root.has("Edges")) edgesArr = &root["Edges"].asArray();
    if (!edgesArr) throw runtime_error("Input has no 'edges' array");

    g.edges.reserve(edgesArr->size());
    for (size_t i = 0; i < edgesArr->size(); i++) {
        const auto& ev = (*edgesArr)[i];
        string s, t;
        auto pickId = [&](const mjson::Value& iv) {
            return iv.isStr() ? iv.asString() : to_string(iv.asLL());
        };
        if      (ev.has("source")) s = pickId(ev.at("source"));
        else if (ev.has("from"))   s = pickId(ev.at("from"));
        if      (ev.has("target")) t = pickId(ev.at("target"));
        else if (ev.has("to"))     t = pickId(ev.at("to"));
        if (s.empty() || t.empty()) throw runtime_error("Edge missing source/target");
        auto its = idMap.find(s); auto itt = idMap.find(t);
        if (its == idMap.end()) throw runtime_error("Unknown source: " + s);
        if (itt == idMap.end()) throw runtime_error("Unknown target: " + t);
        Edge e{its->second, itt->second};
        if (e.u == e.v) continue;                 // drop self loops
        g.edges.push_back(e);
    }
    g.m = (int)g.edges.size();

    if (!g.hasInitialPos) {
        mt19937_64 rng(0xC0FFEEULL);
        uniform_int_distribution<ll> dx(g.minCoordX, g.minCoordX + g.W);
        uniform_int_distribution<ll> dy(g.minCoordY, g.minCoordY + g.H);
        for (int i = 0; i < g.n; i++) g.pos[i] = {dx(rng), dy(rng)};
        g.hasInitialPos = true;
    }
    return g;
}

void writeGraph(const string& path, const GraphData& g, const vector<Pt>& pos) {
    mjson::Value out = g.originalJson;
    string nodesKey = "nodes";
    if (!out.has(nodesKey) && out.has("Nodes")) nodesKey = "Nodes";
    if (out.has(nodesKey)) {
        auto& arr = out[nodesKey].arr;
        for (size_t i = 0; i < arr.size() && (int)i < g.n; i++) {
            arr[i]["x"] = mjson::Value((long long)pos[i].x);
            arr[i]["y"] = mjson::Value((long long)pos[i].y);
        }
    }
    ofstream f(path);
    if (!f) throw runtime_error("Cannot write " + path);
    out.serialize(f, 2);
    f << '\n';
}

// ====================================================================
// Spatial grid for fast crossing-candidate lookup
// ====================================================================
class Grid {
public:
    int gw = 1, gh = 1;
    ll  cellW = 1, cellH = 1;
    ll  ox = 0, oy = 0;                       // origin

    vector<vector<int>>           cells;       // edges per cell
    vector<vector<pair<int,int>>> edgeCells;   // for each edge: (cx, cy)*

    // Re-usable visit marker.
    vector<int> mark;
    int         stamp = 0;

    void init(ll x0, ll y0, ll W, ll H, int gridSide, int m) {
        gw = gridSide; gh = gridSide;
        cellW = max<ll>(1, (W + gw - 1) / gw);
        cellH = max<ll>(1, (H + gh - 1) / gh);
        ox = x0; oy = y0;
        cells.assign(gw * gh, {});
        edgeCells.assign(m, {});
        mark.assign(m, 0);
        stamp = 0;
    }

    inline int cx(ll x) const {
        ll t = (x - ox) / cellW;
        if (t < 0) t = 0;
        if (t >= gw) t = gw - 1;
        return (int)t;
    }
    inline int cy(ll y) const {
        ll t = (y - oy) / cellH;
        if (t < 0) t = 0;
        if (t >= gh) t = gh - 1;
        return (int)t;
    }

    void rangeFor(const Pt& a, const Pt& b,
                  int& cx0, int& cx1, int& cy0, int& cy1) const {
        cx0 = cx(min(a.x, b.x)); cx1 = cx(max(a.x, b.x));
        cy0 = cy(min(a.y, b.y)); cy1 = cy(max(a.y, b.y));
    }

    void addEdge(int e, const Pt& a, const Pt& b) {
        int cx0, cx1, cy0, cy1;
        rangeFor(a, b, cx0, cx1, cy0, cy1);
        auto& ec = edgeCells[e];
        ec.clear();
        ec.reserve((cx1 - cx0 + 1) * (cy1 - cy0 + 1));
        for (int yy = cy0; yy <= cy1; yy++) {
            int row = yy * gw;
            for (int xx = cx0; xx <= cx1; xx++) {
                cells[row + xx].push_back(e);
                ec.push_back({xx, yy});
            }
        }
    }

    void removeEdge(int e) {
        for (auto cell : edgeCells[e]) {
            auto& v = cells[cell.second * gw + cell.first];
            for (size_t i = 0; i < v.size(); i++) {
                if (v[i] == e) { v[i] = v.back(); v.pop_back(); break; }
            }
        }
        edgeCells[e].clear();
    }

    void newQuery() {
        if (++stamp == 0) { fill(mark.begin(), mark.end(), 0); stamp = 1; }
    }

    template<class F>
    void forCandidates(const Pt& a, const Pt& b, F&& f) {
        int cx0, cx1, cy0, cy1;
        rangeFor(a, b, cx0, cx1, cy0, cy1);
        for (int yy = cy0; yy <= cy1; yy++) {
            int row = yy * gw;
            for (int xx = cx0; xx <= cx1; xx++) {
                for (int e : cells[row + xx]) {
                    if (mark[e] != stamp) {
                        mark[e] = stamp;
                        f(e);
                    }
                }
            }
        }
    }
};

// ====================================================================
// SAkGD solver
// ====================================================================
struct MovePlan {
    int  v;
    Pt   oldPos, newPos;
    // Pair-level changes (each unique because pairs include the moved node):
    vector<tuple<int,int,int>>          pairChanges;   // (e1, e2, delta in {-1,+1})
    // Per-edge oldCount / newCount of edges whose count changed:
    vector<tuple<int,int,int>>          edgeCounts;    // (edge, oldCount, newCount)
    int  oldGlobalK = 0;
    int  oldLocalK  = 0;
    int  newLocalK  = 0;
    ll   dCross     = 0;
};

class SAkGD {
public:
    int n = 0, m = 0;
    ll  W = 0, H = 0, ox = 0, oy = 0;
    vector<Pt>           pos;
    vector<Edge>         edges;
    vector<vector<int>>  nodeEdges;

    // For each edge: the set of edges it crosses (bidirectional).
    vector<unordered_set<int>> xs;
    vector<int>                xc;
    vector<int>                cntPerK;        // cntPerK[k] = #edges with xc==k
    int                        kVal    = 0;
    ll                         totalX  = 0;

    // Position uniqueness: at most one node per integer point.
    unordered_map<Pt, int, PtHash> occupied;

    Grid grid;

    // Vertex spatial grid (shares the edge grid's cell geometry). Answers
    // "is any vertex on this segment" queries quickly. Kept in sync during
    // SA via vGridMove in commitMove; rebuilt in restoreBest/repairLayout.
    vector<vector<int>> vCells;

    // best-so-far solution
    int        bestK = INT_MAX;
    ll         bestX = LLONG_MAX;
    vector<Pt> bestPos;

    // Live status writer (optional).
    string                          statusFile;
    string                          statusId   = "run";
    double                          statusInterval = 1.0;   // seconds

    // Convergence trace (optional): append "<absSec> <bestK> <bestX>" every
    // statusInterval seconds so the report can plot k/totalX over time. absSec
    // is measured from runStartedAt, so phase 1 and phase 2 share one timeline.
    string                          traceFile;
    int                             curPhase   = 0;
    double                          curInitT   = 0;
    double                          curTempLim = 0;
    double                          curBudget  = 0;
    steady_clock::time_point        runStartedAt = steady_clock::now();
    steady_clock::time_point        phaseStartedAt = steady_clock::now();

    // RNG
    mt19937_64 rng;

    // A coarse cumulative weight array for selectNode (rebuilt periodically).
    vector<double> cum;
    double         totalNodeW = 0.0;
    int            cumStaleCnt = 0;

    // k-critical vertex selection (phase 2). When selKBand >= 0, selectNode
    // biases hard toward vertices incident to "bottleneck" edges — those whose
    // crossing count is within `selKBand` of the current kVal — instead of the
    // phase-1 total-crossing weighting. kBand is the tunable band width.
    int            kBand    = 2;     // default band; override with --kband
    bool           lexK     = false; // phase-2 lexicographic (k, #edges@k, X)
                                     // off by default: A/B on Automatic-6
                                     // (dense) showed a clear regression
    bool           fitSq2   = false; // phase-2: xc^2 delta as the k-neutral
                                     // tie-break tier (dLocalK stays primary)
    bool           fitSq    = false; // phase-2 fitness = sum of xc^2 deltas
                                     // (soft max proxy: pressures ALL high-
                                     // crossing edges, not just the k band)
    int            reheatWaves = 0; // phase-2: waves without a bestK drop
                                    // before resetting temp to initT (0 = off)
    bool           kRepair  = false; // deterministic polish between waves
                                     // off by default: A/B on Automatic-8
                                     // showed it disrupts the cooled SA walk
    int            selKBand = -1;    // active band: <0 => phase-1 weighting
    int            lastCumK = -1;    // kVal at last rebuild (critical-mode resync)

    SAkGD() {
        rng.seed((uint64_t)chrono::steady_clock::now().time_since_epoch().count() ^
                 (uint64_t)(uintptr_t)this);
    }

    // Initial-layout mode: "auto" (sample both, keep the sparser),
    // "input" (always repair the given drawing), "bfs" (always snake).
    string initMode = "auto";

    // Estimate the average crossings per edge of the CURRENT pos by counting
    // exactly for S sampled edges (O(S*m), no grid needed). Used only to
    // choose the initial layout, so sampling noise is fine.
    double estimateAvgCross(int S) {
        if (m <= 1) return 0.0;
        S = min(S, m);
        mt19937_64 r(0xABCDEF12345ull);
        double sum = 0.0;
        for (int s = 0; s < S; s++) {
            int i = (S == m) ? s : (int)(r() % (uint64_t)m);
            const Pt& a = pos[edges[i].u];
            const Pt& b = pos[edges[i].v];
            int c = 0;
            for (int e = 0; e < m; e++) {
                if (e == i || sharesNode(i, e)) continue;
                const Pt& p = pos[edges[e].u];
                const Pt& q = pos[edges[e].v];
                if (!bboxOverlap(a, b, p, q)) continue;
                if (segCross(a, b, p, q)) c++;
            }
            sum += c;
        }
        return sum / S;
    }

    // Constructive initial layout: BFS order (all components) laid out along
    // a boustrophedon ("snake") path over a near-uniform grid covering the
    // canvas, with per-cell jitter to break the collinearity of exact grid
    // points. Graph-close vertices land geometrically close, so edges stay
    // short and crossings local — a far better SA start than a tangled
    // structured drawing.
    void bfsSnakeLayout() {
        vector<int> order;
        order.reserve(n);
        vector<char> seen(n, 0);
        vector<int> q;
        q.reserve(n);
        for (int s = 0; s < n; s++) {
            if (seen[s]) continue;
            seen[s] = 1;
            q.clear();
            q.push_back(s);
            for (size_t qi = 0; qi < q.size(); qi++) {
                int u = q[qi];
                order.push_back(u);
                for (int e : nodeEdges[u]) {
                    int w = (edges[e].u == u) ? edges[e].v : edges[e].u;
                    if (!seen[w]) { seen[w] = 1; q.push_back(w); }
                }
            }
        }
        ll cols = max<ll>(2, (ll)llround(ceil(
            sqrt((double)n * (double)max<ll>(1, W) / (double)max<ll>(1, H)))));
        ll rows = max<ll>(2, (n + cols - 1) / cols);
        ll dx = max<ll>(1, W / (cols - 1));
        ll dy = max<ll>(1, H / (rows - 1));
        mt19937_64 jr(0x5EEDB0B5ull);
        for (size_t idx = 0; idx < order.size(); idx++) {
            ll row = (ll)idx / cols, col = (ll)idx % cols;
            if (row & 1) col = cols - 1 - col;
            ll x = min(W, col * dx + (dx > 1 ? (ll)(jr() % (uint64_t)dx) : 0));
            ll y = min(H, row * dy + (dy > 1 ? (ll)(jr() % (uint64_t)dy) : 0));
            pos[order[idx]] = {ox + x, oy + y};
        }
    }

    // Barycenter smoothing of the CURRENT pos: pull each vertex toward the
    // mean of its graph neighbours, rescaling the bounding box back onto the
    // canvas every round so the layout cannot collapse to the centre. On
    // near-planar graphs this cuts the snake layout's crossing density by a
    // further ~7x (Automatic-8: avg 92 -> 14 crossings/edge in 50 rounds).
    // Duplicate/collinear integer positions left by the final rounding are
    // resolved by the regular repair pipeline downstream.
    void barycenterSmooth(int rounds) {
        if (n <= 2 || m == 0) return;
        vector<double> px(n), py(n), ax(n), ay(n);
        vector<int> cnt(n);
        for (int i = 0; i < n; i++) {
            px[i] = (double)(pos[i].x - ox);
            py[i] = (double)(pos[i].y - oy);
        }
        for (int r = 0; r < rounds; r++) {
            fill(ax.begin(), ax.end(), 0.0);
            fill(ay.begin(), ay.end(), 0.0);
            fill(cnt.begin(), cnt.end(), 0);
            for (int e = 0; e < m; e++) {
                int u = edges[e].u, v = edges[e].v;
                ax[u] += px[v]; ay[u] += py[v]; cnt[u]++;
                ax[v] += px[u]; ay[v] += py[u]; cnt[v]++;
            }
            double mnx = 1e300, mny = 1e300, mxx = -1e300, mxy = -1e300;
            for (int i = 0; i < n; i++) {
                if (cnt[i]) {
                    px[i] = 0.5 * px[i] + 0.5 * ax[i] / cnt[i];
                    py[i] = 0.5 * py[i] + 0.5 * ay[i] / cnt[i];
                }
                mnx = min(mnx, px[i]); mxx = max(mxx, px[i]);
                mny = min(mny, py[i]); mxy = max(mxy, py[i]);
            }
            double spx = max(1e-9, mxx - mnx), spy = max(1e-9, mxy - mny);
            for (int i = 0; i < n; i++) {
                px[i] = (px[i] - mnx) / spx * (double)W;
                py[i] = (py[i] - mny) / spy * (double)H;
            }
        }
        for (int i = 0; i < n; i++) {
            ll x = (ll)llround(px[i]);
            ll y = (ll)llround(py[i]);
            if (x < 0) x = 0; if (x > W) x = W;
            if (y < 0) y = 0; if (y > H) y = H;
            pos[i] = {ox + x, oy + y};
        }
    }

    // ----- setup ------------------------------------------------------
    void setup(const GraphData& g) {
        n         = g.n;
        m         = g.m;
        W         = g.W;
        H         = g.H;
        ox        = g.minCoordX;
        oy        = g.minCoordY;
        pos       = g.pos;
        edges     = g.edges;
        nodeEdges.assign(n, {});
        for (int i = 0; i < m; i++) {
            nodeEdges[edges[i].u].push_back(i);
            nodeEdges[edges[i].v].push_back(i);
        }

        // Optional constructive initial layout. The given drawing can be
        // catastrophically tangled (Automatic-8: ~45M crossings, k≈10000 —
        // far beyond what local moves can untangle in any realistic budget,
        // and computeAllCrossings alone takes minutes on it). In auto mode,
        // sample-estimate the crossing density of the input layout and of a
        // BFS snake layout, and keep whichever is clearly sparser.
        if (initMode != "input" && n > 1) {
            double inAvg = estimateAvgCross(300);
            vector<Pt> inputPos = pos;
            bfsSnakeLayout();
            double snAvg = estimateAvgCross(300);
            vector<Pt> snakePos = pos;
            barycenterSmooth(50);
            double smAvg = estimateAvgCross(300);
            const char* chosen;
            if (initMode != "bfs" && !(min(snAvg, smAvg) < 0.8 * inAvg)) {
                pos = inputPos;  chosen = "input";
            } else if (smAvg <= snAvg) {
                chosen = "bfs-snake+smooth";
            } else {
                pos = snakePos;  chosen = "bfs-snake";
            }
            cerr << "init: avg crossings/edge  input≈" << inAvg
                 << "  bfs-snake≈" << snAvg << "  +smooth≈" << smAvg
                 << "  -> using " << chosen << " layout\n";
        }

        int gridSide = max(8, min(256, (int)round(sqrt((double)max(m, 1)) / 1.5)));
        grid.init(ox, oy, W, H, gridSide, m);
        for (int i = 0; i < m; i++)
            grid.addEdge(i, pos[edges[i].u], pos[edges[i].v]);

        // Produce a *valid* initial layout: distinct integer positions and no
        // vertex lying on a non-incident edge. First try to repair the given
        // layout in place; if that cannot converge (typical for dense graphs
        // in a small canvas whose input is a structured drawing), fall back to
        // scattering the vertices and repairing the scatter, which has far
        // fewer collinear degeneracies.
        rebuildOccupied();
        syncEdgeGrid();
        bool ok = repairLayout();
        for (int attempt = 0; attempt < 12 && !ok; attempt++) {
            cerr << "setup: in-place repair failed -> scatter fallback"
                    " (attempt " << attempt + 1 << "/12)\n";
            scatterPositions(0x9E3779B97F4A7C15ull * (uint64_t)(attempt + 1));
            ok = repairLayout();
        }
        if (!ok) {
            cerr << "warning: could not fully resolve vertex-edge overlaps "
                    "in initial layout\n";
        }

        computeAllCrossings();
        rebuildCum();
        saveBest();
    }

    // Rebuild the edge grid so it reflects the current positions.
    void syncEdgeGrid() {
        for (int i = 0; i < m; i++) grid.removeEdge(i);
        for (int i = 0; i < m; i++)
            grid.addEdge(i, pos[edges[i].u], pos[edges[i].v]);
    }

    void rebuildOccupied() {
        occupied.clear();
        occupied.reserve(n * 2);
        mt19937_64 lr(0xDEADBEEF);
        for (int i = 0; i < n; i++) {
            int tries = 0;
            while (occupied.count(pos[i])) {
                ll dx = (ll)(int)lr() % 5 - 2;
                ll dy = (ll)(int)lr() % 5 - 2;
                Pt q  = {pos[i].x + dx, pos[i].y + dy};
                if (q.x < ox)        q.x = ox;
                if (q.x > ox + W)    q.x = ox + W;
                if (q.y < oy)        q.y = oy;
                if (q.y > oy + H)    q.y = oy + H;
                pos[i] = q;
                if (++tries > 1000) break;
            }
            occupied[pos[i]] = i;
        }
    }

    // True iff moving vertex v to newPos would cause a vertex-on-edge
    // overlap (a vertex strictly on the interior of an edge it does not
    // belong to). Such layouts are invalid for the GD contest, so the
    // caller must reject the move.
    //
    // This is currently O(m + degree(v) * n) per query. For large graphs,
    // the spatial grid could be used to limit candidates; the simple
    // version is fine for the graph sizes we target during SA tuning.
    bool wouldCauseVertexEdgeOverlap(int v, const Pt& newPos) const {
        // 1) newPos must not lie on any edge that does not contain v.
        for (int e = 0; e < m; e++) {
            int a = edges[e].u, b = edges[e].v;
            if (a == v || b == v) continue;
            if (pointOnSegmentStrict(newPos, pos[a], pos[b])) return true;
        }
        // 2) No other vertex u (and not v's mate on that edge) may lie on
        //    v's new incident segments.
        const auto& inc = nodeEdges[v];
        for (int e : inc) {
            int otherV = (edges[e].u == v ? edges[e].v : edges[e].u);
            const Pt& b = pos[otherV];
            for (int u = 0; u < n; u++) {
                if (u == v || u == otherV) continue;
                if (pointOnSegmentStrict(pos[u], newPos, b)) return true;
            }
        }
        return false;
    }

    // Detect any vertex-on-edge overlap in the *current* layout and return
    // the offending vertex id (the one lying on someone else's edge), or -1.
    int findVertexEdgeOverlap() const {
        for (int v = 0; v < n; v++) {
            for (int e = 0; e < m; e++) {
                int a = edges[e].u, b = edges[e].v;
                if (a == v || b == v) continue;
                if (pointOnSegmentStrict(pos[v], pos[a], pos[b])) return v;
            }
        }
        return -1;
    }

    // ----- grid-accelerated initial-layout repair --------------------
    //
    // The original disentangler scanned all (vertex, edge) pairs — O(n*m) per
    // probe — and only relocated one vertex per scan. On dense graphs in a
    // small canvas (e.g. Automatic-8: 10466 nodes / 20288 edges in 342x294)
    // that is far too slow and, worse, fails to converge: long structured
    // edges sweep across the dense vertex field so almost every probed spot
    // lands on some edge. The routines below use the edge grid and a parallel
    // vertex grid to answer overlap queries against only nearby candidates,
    // making full repair tractable and reliable.

    inline int vCellOf(const Pt& p) const {
        return grid.cy(p.y) * grid.gw + grid.cx(p.x);
    }
    void buildVertexGrid() {
        vCells.assign((size_t)grid.gw * grid.gh, {});
        for (int i = 0; i < n; i++) vCells[vCellOf(pos[i])].push_back(i);
    }
    void vGridMove(int v, const Pt& oldp, const Pt& newp) {
        int oc = vCellOf(oldp), nc = vCellOf(newp);
        if (oc == nc) return;
        auto& ov = vCells[oc];
        for (size_t i = 0; i < ov.size(); i++)
            if (ov[i] == v) { ov[i] = ov.back(); ov.pop_back(); break; }
        vCells[nc].push_back(v);
    }

    // Move v to q, keeping occupied, the edge grid (for v's incident edges)
    // and the vertex grid all in sync. Caller guarantees q is free.
    void moveVertexAll(int v, const Pt& q) {
        Pt old = pos[v];
        if (old == q) return;
        for (int e : nodeEdges[v]) grid.removeEdge(e);
        occupied.erase(old);
        pos[v] = q;
        occupied[q] = v;
        for (int e : nodeEdges[v])
            grid.addEdge(e, pos[edges[e].u], pos[edges[e].v]);
        vGridMove(v, old, q);
    }

    // Grid-accelerated equivalent of wouldCauseVertexEdgeOverlap. Requires the
    // edge grid and vertex grid to reflect the current layout.
    bool wouldCauseVertexEdgeOverlapFast(int v, const Pt& newPos) {
        // 1) newPos must not lie on a non-incident edge.
        bool bad = false;
        grid.newQuery();
        grid.forCandidates(newPos, newPos, [&](int e) {
            if (bad) return;
            int a = edges[e].u, b = edges[e].v;
            if (a == v || b == v) return;
            if (pointOnSegmentStrict(newPos, pos[a], pos[b])) bad = true;
        });
        if (bad) return true;
        // 2) no other vertex may lie on v's incident edges (newPos -> mate).
        for (int e : nodeEdges[v]) {
            int other   = (edges[e].u == v ? edges[e].v : edges[e].u);
            const Pt& b = pos[other];
            int cx0, cx1, cy0, cy1;
            grid.rangeFor(newPos, b, cx0, cx1, cy0, cy1);
            for (int yy = cy0; yy <= cy1; yy++) {
                int row = yy * grid.gw;
                for (int xx = cx0; xx <= cx1; xx++) {
                    for (int u : vCells[row + xx]) {
                        if (u == v || u == other) continue;
                        if (pointOnSegmentStrict(pos[u], newPos, b)) return true;
                    }
                }
            }
        }
        return false;
    }

    // Grid-accelerated scan: return any vertex lying on a non-incident edge in
    // the current layout, or -1 if clean. Only the edge grid is required.
    int findVertexEdgeOverlapFast() {
        for (int v = 0; v < n; v++) {
            bool bad = false;
            const Pt& p = pos[v];
            grid.newQuery();
            grid.forCandidates(p, p, [&](int e) {
                if (bad) return;
                int a = edges[e].u, b = edges[e].v;
                if (a == v || b == v) return;
                if (pointOnSegmentStrict(p, pos[a], pos[b])) bad = true;
            });
            if (bad) return v;
        }
        return -1;
    }

    // Randomly place all vertices at distinct integer points in the canvas and
    // rebuild the edge grid. Used as a fallback start when the given layout is
    // too degenerate to repair in place.
    void scatterPositions(uint64_t seed) {
        mt19937_64 r(seed);
        occupied.clear();
        occupied.reserve((size_t)n * 2);
        uniform_int_distribution<ll> dx(ox, ox + W), dy(oy, oy + H);
        for (int i = 0; i < n; i++) {
            Pt q; int tries = 0;
            do { q = {dx(r), dy(r)}; } while (occupied.count(q) && ++tries < 2000);
            pos[i] = q;
            occupied[q] = i;
        }
        syncEdgeGrid();
    }

    // Repair the current layout into a valid GD drawing in place. Returns true
    // iff fully resolved. Assumes occupied + edge grid already reflect pos.
    bool repairLayout() {
        buildVertexGrid();
        mt19937_64 lr(0xC0FFEEull);

        auto relocate = [&](int v) -> bool {
            Pt cur = pos[v];
            for (int radius = 1; radius <= 256; radius *= 2) {
                for (int t = 0; t < 32; t++) {
                    ll dx = ((ll)(uint32_t)lr() % (2 * radius + 1)) - radius;
                    ll dy = ((ll)(uint32_t)lr() % (2 * radius + 1)) - radius;
                    Pt q = {cur.x + dx, cur.y + dy};
                    if (q.x < ox)     q.x = ox;
                    if (q.x > ox + W) q.x = ox + W;
                    if (q.y < oy)     q.y = oy;
                    if (q.y > oy + H) q.y = oy + H;
                    if (q == cur) continue;
                    auto it = occupied.find(q);
                    if (it != occupied.end() && it->second != v) continue;
                    moveVertexAll(v, q);
                    if (!wouldCauseVertexEdgeOverlapFast(v, q)) return true;
                    moveVertexAll(v, cur);   // revert
                }
            }
            return false;
        };

        const int MAXPASS = 60;
        for (int pass = 0; pass < MAXPASS; pass++) {
            int fixed = 0, stuck = 0;
            for (int v = 0; v < n; v++) {
                if (!wouldCauseVertexEdgeOverlapFast(v, pos[v])) continue;
                if (relocate(v)) fixed++;
                else             stuck++;
            }
            if (fixed == 0)
                return stuck == 0;   // clean if nothing left, else give up
        }
        return findVertexEdgeOverlapFast() < 0;
    }

    // ----- core utility ----------------------------------------------
    inline bool sharesNode(int e1, int e2) const {
        const Edge& a = edges[e1]; const Edge& b = edges[e2];
        return a.u == b.u || a.u == b.v || a.v == b.u || a.v == b.v;
    }

    void computeAllCrossings() {
        xs.assign(m, {});
        xc.assign(m, 0);
        totalX = 0;
        kVal   = 0;

        for (int i = 0; i < m; i++) {
            const Pt& a = pos[edges[i].u];
            const Pt& b = pos[edges[i].v];
            grid.newQuery();
            grid.forCandidates(a, b, [&](int e) {
                if (e <= i) return;
                if (sharesNode(i, e)) return;
                const Pt& c = pos[edges[e].u];
                const Pt& d = pos[edges[e].v];
                if (!bboxOverlap(a, b, c, d)) return;
                if (segCross(a, b, c, d)) {
                    xs[i].insert(e);
                    xs[e].insert(i);
                    totalX++;
                }
            });
        }
        cntPerK.assign(1, 0);
        for (int i = 0; i < m; i++) {
            xc[i] = (int)xs[i].size();
            if ((int)cntPerK.size() <= xc[i]) cntPerK.resize(xc[i] + 1, 0);
            cntPerK[xc[i]]++;
            if (xc[i] > kVal) kVal = xc[i];
        }
    }

    void changeEdgeCount(int e, int newCount) {
        int oldCount = xc[e];
        if (oldCount == newCount) return;
        cntPerK[oldCount]--;
        if ((int)cntPerK.size() <= newCount) cntPerK.resize(newCount + 1, 0);
        cntPerK[newCount]++;
        xc[e] = newCount;
        if (newCount > kVal) kVal = newCount;
        while (kVal > 0 && cntPerK[kVal] == 0) kVal--;
    }

    void saveBest() {
        bestK   = kVal;
        bestX   = totalX;
        bestPos = pos;
    }

    // ----- live status JSON ------------------------------------------
    // Append one convergence sample (best-so-far) on the absolute timeline.
    void appendTrace() {
        if (traceFile.empty()) return;
        double absSec = duration_cast<duration<double>>(
            steady_clock::now() - runStartedAt).count();
        int bk = (bestK == INT_MAX) ? kVal   : bestK;   // best-so-far (fallback: current)
        ll  bx = (bestK == INT_MAX) ? totalX : bestX;
        ofstream f(traceFile, std::ios::app);
        if (!f) return;
        f.setf(std::ios::fixed); f.precision(2);
        f << absSec << ' ' << bk << ' ' << bx << '\n';
    }

    void writeStatus(double currentTemp, ll moves, ll accepts, const char* state) {
        if (statusFile.empty()) return;
        string tmp = statusFile + ".tmp";
        ofstream f(tmp);
        if (!f) return;

        double phaseElapsed = duration_cast<duration<double>>(
            steady_clock::now() - phaseStartedAt).count();
        double runElapsed = duration_cast<duration<double>>(
            steady_clock::now() - runStartedAt).count();

        const auto& shown = bestPos.empty() ? pos : bestPos;

        f.setf(std::ios::fixed); f.precision(6);
        f << "{";
        f << "\"id\":\""    << statusId       << "\",";
        f << "\"state\":\"" << state          << "\",";
        f << "\"phase\":"   << curPhase       << ",";
        f << "\"currentTemp\":" << currentTemp << ",";
        f << "\"initT\":"     << curInitT     << ",";
        f << "\"tempLimit\":" << curTempLim   << ",";
        f << "\"phaseElapsedSec\":" << phaseElapsed << ",";
        f << "\"phaseBudgetSec\":"  << curBudget    << ",";
        f << "\"runElapsedSec\":"   << runElapsed   << ",";
        f << "\"moves\":"     << moves        << ",";
        f << "\"accepts\":"   << accepts      << ",";
        f << "\"k\":"         << kVal         << ",";
        f << "\"totalX\":"    << totalX       << ",";
        f << "\"bestK\":"     << (bestK == INT_MAX  ? -1 : bestK) << ",";
        f << "\"bestX\":"     << (bestX == LLONG_MAX? -1 : bestX) << ",";
        f << "\"n\":"         << n            << ",";
        f << "\"m\":"         << m            << ",";
        f << "\"W\":"         << W            << ",";
        f << "\"H\":"         << H            << ",";
        f << "\"ox\":"        << ox           << ",";
        f << "\"oy\":"        << oy           << ",";
        f << "\"pos\":[";
        for (int i = 0; i < (int)shown.size(); i++) {
            if (i) f << ",";
            f << "[" << shown[i].x << "," << shown[i].y << "]";
        }
        f << "],";
        f << "\"edges\":[";
        for (int i = 0; i < m; i++) {
            if (i) f << ",";
            f << "[" << edges[i].u << "," << edges[i].v << "]";
        }
        f << "]";
        f << "}";
        f.close();
        std::rename(tmp.c_str(), statusFile.c_str());
    }

    void restoreBest() {
        if (bestPos.empty()) return;
        // Tear down grid, set positions, rebuild grid + crossings.
        for (int i = 0; i < m; i++) grid.removeEdge(i);
        pos = bestPos;
        for (int i = 0; i < m; i++)
            grid.addEdge(i, pos[edges[i].u], pos[edges[i].v]);
        rebuildOccupied();
        buildVertexGrid();        // resync vertex grid for the fast overlap check
        computeAllCrossings();
        rebuildCum();
    }

    // ----- selection -------------------------------------------------
    // selectNode():  weight of node v = 1 + sum(xc[e] for e incident to v).
    // We use a cumulative array and refresh it occasionally; perfectly
    // exact weighting is not required, only the bias.
    void rebuildCum() {
        cum.assign(n, 0.0);
        double t = 0.0;
        if (selKBand < 0) {
            // Phase-1 weighting: weight(v) = 1 + sum of crossings on incident edges.
            for (int i = 0; i < n; i++) {
                double w = 1.0;
                for (int e : nodeEdges[i]) w += (double)xc[e];
                t += w;
                cum[i] = t;
            }
        } else {
            // k-critical weighting: only edges within `selKBand` of kVal count,
            // with quadratic emphasis on proximity to kVal. Non-critical
            // vertices share a flat base mass sized relative to the critical
            // mass (base*n = critSum/3, i.e. ~25% of the total), so the bias
            // stays hard regardless of n while room-making moves on neighbours
            // remain possible. A fixed base (0.1) would swamp the critical
            // mass on large graphs (0.1*n >> critSum) and degrade the
            // selection to near-uniform exactly where k matters most.
            int thr = kVal - selKBand;
            if (thr < 1) thr = 1;
            double critSum = 0.0;
            for (int i = 0; i < n; i++) {
                double w = 0.0;
                for (int e : nodeEdges[i]) {
                    if (xc[e] >= thr) {
                        double d = (double)(xc[e] - thr + 1);
                        w += d * d;
                    }
                }
                cum[i]   = w;
                critSum += w;
            }
            double base = (critSum > 0.0) ? critSum / (3.0 * n) : 1.0;
            for (int i = 0; i < n; i++) {
                t += cum[i] + base;
                cum[i] = t;
            }
        }
        totalNodeW  = t;
        lastCumK    = kVal;
        cumStaleCnt = 0;
    }

    int selectNode() {
        // In k-critical mode the band depends on kVal, so resync when kVal
        // moves — rate-limited via cumStaleCnt so an oscillating kVal cannot
        // trigger a full O(n+m) rebuild every few moves.
        if (cumStaleCnt > max(64, n / 4) ||
            (selKBand >= 0 && kVal != lastCumK && cumStaleCnt >= 8)) rebuildCum();
        if (totalNodeW <= 0) {
            return uniform_int_distribution<int>(0, n - 1)(rng);
        }
        double r = uniform_real_distribution<double>(0.0, totalNodeW)(rng);
        // binary search over cum
        int lo = 0, hi = n - 1;
        while (lo < hi) {
            int mid = (lo + hi) >> 1;
            if (cum[mid] >= r) hi = mid;
            else               lo = mid + 1;
        }
        return lo;
    }

    // selectPlace():  Gaussian around the current position; with small
    // probability we sample globally to escape local optima.
    Pt selectPlace(int v, double T, double initT, bool localOnly) {
        double scale = sqrt((double)max<ll>(1, W) * (double)max<ll>(1, H));
        double tFrac = (initT > 0) ? T / initT : 1.0;
        if (tFrac < 0.01) tFrac = 0.01;
        if (tFrac > 1.0)  tFrac = 1.0;
        double sigma = scale * (0.005 + 0.05 * tFrac);
        if (sigma < 1.0) sigma = 1.0;

        if (!localOnly) {
            double rg = uniform_real_distribution<double>(0, 1)(rng);
            if (rg < 0.05) {
                ll x = uniform_int_distribution<ll>(ox, ox + W)(rng);
                ll y = uniform_int_distribution<ll>(oy, oy + H)(rng);
                return {x, y};
            }
        }

        normal_distribution<double> nd(0.0, sigma);
        ll dx = (ll)llround(nd(rng));
        ll dy = (ll)llround(nd(rng));
        ll nx = pos[v].x + dx;
        ll ny = pos[v].y + dy;
        if (nx < ox)        nx = ox;
        if (nx > ox + W)    nx = ox + W;
        if (ny < oy)        ny = oy;
        if (ny > oy + H)    ny = oy + H;
        if (nx == pos[v].x && ny == pos[v].y) {
            // Avoid no-op: nudge by 1.
            nx += (uniform_int_distribution<int>(0, 1)(rng) ? 1 : -1);
            ny += (uniform_int_distribution<int>(0, 1)(rng) ? 1 : -1);
            if (nx < ox)     nx = ox;
            if (nx > ox + W) nx = ox + W;
            if (ny < oy)     ny = oy;
            if (ny > oy + H) ny = oy + H;
        }
        return {nx, ny};
    }

    // ----- move planning ---------------------------------------------
    // Plans the move v -> newPos without modifying any global state.
    void planMove(int v, Pt newPos, MovePlan& plan) {
        plan.v          = v;
        plan.oldPos     = pos[v];
        plan.newPos     = newPos;
        plan.oldGlobalK = kVal;
        plan.dCross     = 0;
        plan.pairChanges.clear();
        plan.edgeCounts.clear();
        plan.oldLocalK  = 0;
        plan.newLocalK  = 0;

        const auto& incidents = nodeEdges[v];

        // Per-edge delta in crossing count (only edges involved in changing pairs).
        // Use a small hash map keyed on edge id.
        static thread_local unordered_map<int,int> deltaCount;
        deltaCount.clear();
        deltaCount.reserve(incidents.size() * 8 + 4);

        // Old local K (incident edges + edges crossing them).
        for (int i : incidents) {
            if (xc[i] > plan.oldLocalK) plan.oldLocalK = xc[i];
            for (int e2 : xs[i]) if (xc[e2] > plan.oldLocalK) plan.oldLocalK = xc[e2];
        }

        // New crossings of each incident edge.
        // We re-use the grid's mark array (per query).
        vector<unordered_set<int>> newXsI(incidents.size());

        for (size_t k = 0; k < incidents.size(); k++) {
            int  i  = incidents[k];
            Pt   p1 = (edges[i].u == v) ? newPos : pos[edges[i].u];
            Pt   p2 = (edges[i].v == v) ? newPos : pos[edges[i].v];
            grid.newQuery();
            auto& dst = newXsI[k];
            grid.forCandidates(p1, p2, [&](int e) {
                if (e == i) return;
                if (sharesNode(i, e)) return;
                Pt c = (edges[e].u == v) ? newPos : pos[edges[e].u];
                Pt d = (edges[e].v == v) ? newPos : pos[edges[e].v];
                if (!bboxOverlap(p1, p2, c, d)) return;
                if (segCross(p1, p2, c, d)) dst.insert(e);
            });
        }

        // Compute pair changes (each pair appears at most once because
        // shared-node pairs are skipped, and the moved node's incident edges
        // never pair with each other geometrically).
        for (size_t k = 0; k < incidents.size(); k++) {
            int i = incidents[k];
            const auto& nx_i = newXsI[k];
            // Removed: was crossing, no longer.
            for (int e : xs[i]) {
                if (!nx_i.count(e)) {
                    plan.pairChanges.emplace_back(i, e, -1);
                    deltaCount[i]--;
                    deltaCount[e]--;
                    plan.dCross--;
                }
            }
            // Added: now crossing, was not.
            for (int e : nx_i) {
                if (!xs[i].count(e)) {
                    plan.pairChanges.emplace_back(i, e, +1);
                    deltaCount[i]++;
                    deltaCount[e]++;
                    plan.dCross++;
                }
            }
        }

        // Build edgeCounts and newLocalK.
        // newLocalK should also include the (possibly updated) counts of all
        // involved edges (incident + their old crossings).
        unordered_set<int> involvedSet;
        for (int i : incidents) {
            involvedSet.insert(i);
            for (int e2 : xs[i]) involvedSet.insert(e2);
        }
        for (auto& kv : deltaCount) involvedSet.insert(kv.first);

        for (int e : involvedSet) {
            int newCnt = xc[e] + (deltaCount.count(e) ? deltaCount[e] : 0);
            if (newCnt > plan.newLocalK) plan.newLocalK = newCnt;
            if (deltaCount.count(e)) {
                plan.edgeCounts.emplace_back(e, xc[e], newCnt);
            }
        }
    }

    // ----- commit ----------------------------------------------------
    void commitMove(const MovePlan& plan) {
        // grid: incident edges change cells.
        const auto& incidents = nodeEdges[plan.v];
        for (int i : incidents) grid.removeEdge(i);
        occupied.erase(plan.oldPos);
        pos[plan.v] = plan.newPos;
        occupied[plan.newPos] = plan.v;
        for (int i : incidents) grid.addEdge(i, pos[edges[i].u], pos[edges[i].v]);
        // keep the vertex grid in sync so the fast overlap check stays correct.
        vGridMove(plan.v, plan.oldPos, plan.newPos);

        // pair-level updates of xs.
        for (auto& pc : plan.pairChanges) {
            int e1 = std::get<0>(pc);
            int e2 = std::get<1>(pc);
            int d  = std::get<2>(pc);
            if (d > 0) {
                xs[e1].insert(e2);
                xs[e2].insert(e1);
            } else {
                xs[e1].erase(e2);
                xs[e2].erase(e1);
            }
        }
        // per-edge count updates (this also updates cntPerK & kVal).
        for (auto& ec : plan.edgeCounts) {
            int e        = std::get<0>(ec);
            int newCount = std::get<2>(ec);
            changeEdgeCount(e, newCount);
        }
        totalX     += plan.dCross;
        cumStaleCnt += (int)plan.edgeCounts.size();
    }

    // ----- deterministic k-repair pass --------------------------------
    // Vertex-movement primitive (Radermacher et al., JEA 2019): for the
    // endpoints of edges sitting at the bottleneck level k, evaluate a
    // sampled candidate set of positions exactly (via planMove) and commit
    // the best strictly-improving one. Catches sure wins the probabilistic
    // SA walk misses. Returns number of committed moves.
    int kRepairPass(int maxEdges, int candsPerNode) {
        vector<int> top;
        for (int e = 0; e < m; e++) if (xc[e] == kVal) top.push_back(e);
        shuffle(top.begin(), top.end(), rng);
        if ((int)top.size() > maxEdges) top.resize(maxEdges);

        MovePlan plan;
        int committed = 0;
        double scale = sqrt((double)max<ll>(1, W) * (double)max<ll>(1, H));
        normal_distribution<double> nd(0.0, max(1.0, scale * 0.05));

        for (int e : top) {
            if (xc[e] != kVal) continue;        // may have improved already
            for (int v : {edges[e].u, edges[e].v}) {
                Pt   curPos  = pos[v];
                Pt   bestPos = curPos;
                int  bestLK  = INT_MAX;
                int  bestD   = INT_MAX;
                int  oldLK   = -1;
                for (int c = 0; c < candsPerNode; c++) {
                    Pt p;
                    if (c % 2 == 0) {           // local Gaussian candidate
                        p.x = curPos.x + (ll)llround(nd(rng));
                        p.y = curPos.y + (ll)llround(nd(rng));
                    } else {                    // global uniform candidate
                        p.x = uniform_int_distribution<ll>(ox, ox + W)(rng);
                        p.y = uniform_int_distribution<ll>(oy, oy + H)(rng);
                    }
                    if (p.x < ox) p.x = ox; if (p.x > ox + W) p.x = ox + W;
                    if (p.y < oy) p.y = oy; if (p.y > oy + H) p.y = oy + H;
                    if (p == curPos) continue;
                    {
                        auto it = occupied.find(p);
                        if (it != occupied.end() && it->second != v) continue;
                    }
                    if (wouldCauseVertexEdgeOverlapFast(v, p)) continue;
                    planMove(v, p, plan);
                    oldLK = plan.oldLocalK;
                    if (plan.newLocalK < bestLK ||
                        (plan.newLocalK == bestLK && plan.dCross < bestD)) {
                        bestLK  = plan.newLocalK;
                        bestD   = plan.dCross;
                        bestPos = p;
                    }
                }
                if (oldLK < 0) continue;
                if (bestLK < oldLK || (bestLK == oldLK && bestD < 0)) {
                    planMove(v, bestPos, plan);
                    commitMove(plan);
                    committed++;
                    if (kVal < bestK || (kVal == bestK && totalX < bestX))
                        saveBest();
                }
            }
        }
        return committed;
    }

    // ----- SA shell (Algorithm 1) ------------------------------------
    // Phase: 1 = minimise total crossings, 2 = minimise k-value (dual fitness).
    void runSA(int phase,
               double initT, double decT, double decTW, double tLim,
               double timeLimitSec)
    {
        phaseStartedAt = steady_clock::now();
        curPhase   = phase;
        curInitT   = initT;
        curTempLim = tLim;
        curBudget  = timeLimitSec;

        // Phase 2 minimises the bottleneck k; switch to k-critical vertex
        // selection. Phase 1 minimises total crossings; keep the broad weighting.
        selKBand = (phase == 2 && kBand >= 0) ? kBand : -1;
        rebuildCum();

        auto elapsed = [&] {
            return duration_cast<duration<double>>(
                steady_clock::now() - phaseStartedAt).count();
        };

        long long moves = 0, accepts = 0;
        double startingTemp = initT;
        double repairSpent  = 0.0;
        int    lastBestK    = bestK;   // reheat bookkeeping
        int    staleWaves   = 0;

        cerr << "[phase " << phase << "] start  initT=" << initT
             << " decT=" << decT << " decTW=" << decTW << " tLim=" << tLim
             << "  budget=" << timeLimitSec << "s"
             << "  initial k=" << kVal << " totalX=" << totalX << "\n";

        MovePlan plan;
        plan.pairChanges.reserve(256);
        plan.edgeCounts .reserve(256);

        auto reportTime = [&](double currentTemp) {
            cerr << "  [phase " << phase << "] t=" << (int)elapsed()
                 << "s  bestK=" << bestK << " bestX=" << bestX
                 << "  curK=" << kVal << " curX=" << totalX
                 << "  moves=" << moves << " accepts=" << accepts
                 << " sT=" << startingTemp << "\n";
            writeStatus(currentTemp, moves, accepts, "running");
        };
        double nextReport = 0.5;        // first dump comes quickly
        double reportEvery = 30.0;      // log to stderr every 30s
        double nextStatus  = 0.0;       // immediate first dump

        writeStatus(startingTemp, moves, accepts, "running");

        while (kVal > 0 && startingTemp > tLim && elapsed() < timeLimitSec) {
            double currentTemp = startingTemp;
            while (kVal > 0 && currentTemp > tLim && elapsed() < timeLimitSec) {
                int v       = selectNode();
                Pt  newPos  = selectPlace(v, currentTemp, initT, phase == 2);
                if (newPos == pos[v]) { currentTemp *= decT; continue; }
                // Forbid two distinct nodes sharing the same point.
                {
                    auto it = occupied.find(newPos);
                    if (it != occupied.end() && it->second != v) {
                        currentTemp *= decT; continue;
                    }
                }
                // Forbid layouts where a vertex lies strictly on an edge
                // it is not incident to (vertex-edge overlap is invalid for
                // GD-contest scoring: crossings on that edge are ill-defined).
                if (wouldCauseVertexEdgeOverlapFast(v, newPos)) {
                    currentTemp *= decT; continue;
                }

                planMove(v, newPos, plan);

                double dE;
                if (phase == 1) {
                    dE = (double)plan.dCross;
                } else if (fitSq) {
                    // Squared-crossings fitness: dE = sum(newC^2 - oldC^2),
                    // normalised so that +-1 crossing on a bottleneck-level
                    // edge costs ~1 (comparable to the dLocalK unit below).
                    double dsq = 0.0;
                    for (auto& ec : plan.edgeCounts) {
                        double oldC = (double)std::get<1>(ec);
                        double newC = (double)std::get<2>(ec);
                        dsq += newC * newC - oldC * oldC;
                    }
                    dE = dsq / max(1.0, 2.0 * (double)kVal);
                } else {
                    int  dLocalK = plan.newLocalK - plan.oldLocalK;
                    if (dLocalK != 0) dE = (double)dLocalK;
                    else {
                        // Lexicographic middle objective: before k itself can
                        // drop, every edge sitting at the bottleneck level k
                        // must lose a crossing. Pricing one of the cntPerK[k]
                        // top-level edges at 1/cntPerK[k] makes clearing the
                        // whole level worth ~1, i.e. one unit of k.
                        int dTop = 0;
                        if (lexK) {
                            for (auto& ec : plan.edgeCounts) {
                                int oldC = std::get<1>(ec);
                                int newC = std::get<2>(ec);
                                dTop += (int)(newC >= kVal) - (int)(oldC >= kVal);
                            }
                        }
                        if (dTop != 0)
                            dE = (double)dTop / max(1, cntPerK[kVal]);
                        else if (fitSq2) {
                            // k-neutral tie-break by squared-crossings delta:
                            // among moves that don't touch the bottleneck,
                            // prefer ones that unload high-crossing edges.
                            double dsq = 0.0;
                            for (auto& ec : plan.edgeCounts) {
                                double oldC = (double)std::get<1>(ec);
                                double newC = (double)std::get<2>(ec);
                                dsq += newC * newC - oldC * oldC;
                            }
                            dE = dsq / max(1.0, 2.0 * (double)kVal *
                                                (double)max<ll>(1, totalX));
                        } else
                            dE = (double)plan.dCross /
                                 max(1.0, (double)max<ll>(1, totalX));
                    }
                }

                bool acc = false;
                if (dE <= 0.0) acc = true;
                else {
                    double prob = exp(-dE / max(1e-9, currentTemp));
                    acc = uniform_real_distribution<double>(0.0, 1.0)(rng) < prob;
                }

                if (acc) {
                    commitMove(plan);
                    accepts++;
                    if (kVal < bestK ||
                        (kVal == bestK && totalX < bestX)) {
                        saveBest();
                    }
                }
                moves++;
                currentTemp *= decT;

                double el = elapsed();
                if (el >= nextStatus) {
                    writeStatus(currentTemp, moves, accepts, "running");
                    appendTrace();
                    nextStatus = el + statusInterval;
                }
                if (el >= nextReport) {
                    if (el >= reportEvery) reportTime(currentTemp);
                    nextReport = el + reportEvery;
                }
            }
            startingTemp *= decTW;
            // Stagnation reheat (phase 2): when bestK hasn't dropped for
            // reheatWaves consecutive waves, reset the wave temperature to
            // initT so the walk can escape the frozen local optimum instead
            // of spending the rest of the budget at tLim.
            if (phase == 2 && reheatWaves > 0) {
                if (bestK < lastBestK) { lastBestK = bestK; staleWaves = 0; }
                else if (++staleWaves >= reheatWaves) {
                    startingTemp = initT;
                    staleWaves   = 0;
                    cerr << "  [phase 2] reheat at t=" << (int)elapsed()
                         << "s bestK=" << bestK << "\n";
                }
            }
            // Deterministic polish between waves, capped at ~10% of the
            // phase's elapsed time so it never starves the SA walk.
            if (phase == 2 && kRepair &&
                repairSpent < 0.10 * max(1.0, elapsed())) {
                double rt0 = elapsed();
                kRepairPass(/*maxEdges*/16, /*candsPerNode*/64);
                repairSpent += elapsed() - rt0;
            }
            // Each new wave starts from the best known solution (paper, line
            // 19). A full restore recomputes all crossings — 32s of a 120s
            // Automatic-8 phase 1 (95 waves) — so skip it when the current
            // state already matches the best k and is within ~1% of its X.
            if (kVal != bestK || totalX > bestX + max<ll>(4, bestX / 100))
                restoreBest();
        }

        writeStatus(startingTemp, moves, accepts, "phase-done");

        cerr << "[phase " << phase << "] end    moves=" << moves
             << " accepts=" << accepts
             << "  bestK=" << bestK << " bestX=" << bestX << "\n";
    }
};

// ====================================================================
// CLI
// ====================================================================
static void printUsage(const char* prog) {
    cerr <<
        "Usage:\n"
        "  " << prog << " -i input.json -o output.json"
                       "  [-t minutes] [-p1 minutes] [-s seed]\n"
        "  " << prog << " input.json output.json"
                       "         (positional form, defaults to 60 / 10 minutes)\n"
        "  " << prog << " --verify input.json"
                       "             (only report k and total crossings)\n"
        "\nOptions:\n"
        "  -t  total time budget in minutes         (default: 60)\n"
        "  -p1 time budget of phase 1 in minutes    (default: 10)\n"
        "  -s  RNG seed                             (default: time-based)\n"
        "  --kband N  phase-2 k-critical selection band, -1 to disable (default: 2)\n"
        "  --lexk 0|1 phase-2 lexicographic fitness (k, #edges at k, totalX)\n"
        "             (default: 0 — hurts dense graphs)\n"
        "  --krepair 0|1 deterministic vertex-move polish of bottleneck edges\n"
        "             between phase-2 waves (default: 0)\n"
        "  --init MODE  initial layout: auto|input|bfs (default: auto —\n"
        "               sample crossing density, keep input unless BFS snake is sparser)\n"
        "  --status-file PATH    write live status JSON to PATH\n"
        "  --status-id   STRING  identifier shown in the dashboard\n"
        "  --status-interval SEC seconds between status dumps (default: 1.0)\n"
        "  --verify              parse the input, report metrics and exit\n";
}

int main(int argc, char** argv) {
    string inputFile, outputFile;
    double totalMin  = 60.0;
    double phase1Min = 10.0;
    long long seed   = -1;
    bool verifyOnly  = false;
    string statusFile, statusId = "run";
    string traceFile;
    double statusInterval = 1.0;
    int    kbandArg  = 2;     // phase-2 k-critical selection band
    int    lexkArg   = 0;     // phase-2 lexicographic fitness, 1 to enable
    int    krepairArg = 0;    // inter-wave deterministic k-repair, 1 to enable
    string initMode  = "auto";
    double p1T0   = 50.0;     // phase-1 starting temperature (paper Table 1)
    double p2T0   = 1.0;      // phase-2 starting temperature
    double p2DecT = 0.9999;   // phase-2 per-move cooling factor
    int    reheatArg = 0;     // phase-2 stagnation reheat, waves (0 = off)
    int    polishArg = 0;     // final deterministic k-repair polish (0 = off)
    string fitArg    = "k";   // phase-2 fitness: k (paper dual) | sq (xc^2)

    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        auto need = [&](const char* /*flag*/) {
            if (i + 1 >= argc) { printUsage(argv[0]); exit(1); }
            return argv[++i];
        };
        if      (a == "-i")                  inputFile  = need("-i");
        else if (a == "-o")                  outputFile = need("-o");
        else if (a == "-t")                  totalMin   = atof(need("-t"));
        else if (a == "-p1")                 phase1Min  = atof(need("-p1"));
        else if (a == "-s")                  seed       = atoll(need("-s"));
        else if (a == "--kband")             kbandArg   = atoi(need("--kband"));
        else if (a == "--lexk")              lexkArg    = atoi(need("--lexk"));
        else if (a == "--krepair")           krepairArg = atoi(need("--krepair"));
        else if (a == "--init")              initMode   = need("--init");
        else if (a == "--p1-t0")             p1T0       = atof(need("--p1-t0"));
        else if (a == "--p2-t0")             p2T0       = atof(need("--p2-t0"));
        else if (a == "--p2-dect")           p2DecT     = atof(need("--p2-dect"));
        else if (a == "--reheat")            reheatArg  = atoi(need("--reheat"));
        else if (a == "--polish")            polishArg  = atoi(need("--polish"));
        else if (a == "--fit")               fitArg     = need("--fit");
        else if (a == "--status-file")       statusFile = need("--status-file");
        else if (a == "--status-id")         statusId   = need("--status-id");
        else if (a == "--status-interval")   statusInterval = atof(need("--status-interval"));
        else if (a == "--trace-file")        traceFile  = need("--trace-file");
        else if (a == "--verify")            verifyOnly = true;
        else if (a == "-h" || a == "--help") { printUsage(argv[0]); return 0; }
        else if (inputFile.empty())  inputFile  = a;
        else if (outputFile.empty()) outputFile = a;
        else { printUsage(argv[0]); return 1; }
    }
    if (inputFile.empty()) { printUsage(argv[0]); return 1; }
    if (outputFile.empty() && !verifyOnly) outputFile = inputFile + ".out.json";

    cerr << "Reading: " << inputFile << "\n";
    GraphData g = readGraph(inputFile);
    cerr << "n=" << g.n << " m=" << g.m
         << " W=" << g.W << " H=" << g.H << "\n";

    SAkGD solver;
    if (seed >= 0) solver.rng.seed((uint64_t)seed);
    if (verifyOnly) initMode = "input";   // verify must report the file as-is
    solver.statusFile     = statusFile;
    solver.statusId       = statusId;
    solver.statusInterval = max(0.05, statusInterval);
    solver.traceFile      = traceFile;
    solver.kBand          = kbandArg;
    solver.lexK           = (lexkArg != 0);
    solver.kRepair        = (krepairArg != 0);
    solver.reheatWaves    = reheatArg;
    solver.fitSq          = (fitArg == "sq");
    solver.fitSq2         = (fitArg == "sq2");
    solver.initMode       = initMode;
    solver.runStartedAt   = steady_clock::now();
    if (!traceFile.empty()) ofstream(traceFile, std::ios::trunc);  // start clean
    solver.setup(g);
    int veInit = solver.findVertexEdgeOverlapFast();
    cerr << "Initial: k=" << solver.kVal
         << " totalX=" << solver.totalX
         << "  vertexEdgeOverlap=" << (veInit < 0 ? "no" : "YES")
         << "\n";

    // Initial dump so the dashboard sees the run before SA starts.
    solver.curPhase = 0; solver.curInitT = 0; solver.curTempLim = 0; solver.curBudget = 0;
    solver.phaseStartedAt = steady_clock::now();
    solver.writeStatus(0, 0, 0, "starting");

    if (verifyOnly) {
        cout << "k=" << solver.kVal
             << " totalCrossings=" << solver.totalX
             << " vertexEdgeOverlap=" << (veInit < 0 ? "no" : "yes")
             << "\n";
        if (veInit >= 0) return 2;
        return 0;
    }

    // Paper parameters (Table 1):
    //   min cross : initT=50  decT=0.999  decTW=0.99  tLim=0.01
    //   min k     : initT=1   decT=0.9999 decTW=0.99  tLim=0.01
    solver.runSA(/*phase*/1, p1T0,  0.999, 0.99, 0.01, phase1Min * 60.0);

    double remaining = max(0.0, (totalMin - phase1Min) * 60.0);
    // Final-polish budget is carved out of phase 2 so -t stays honest.
    double polishSec = polishArg ? min(15.0, 0.05 * totalMin * 60.0) : 0.0;
    solver.runSA(/*phase*/2, p2T0, p2DecT, 0.99, 0.01, remaining - polishSec);

    if (polishArg) {
        // Deterministic strictly-improving polish of the best layout.
        // Unlike the inter-wave --krepair (which disrupts the cooled SA
        // walk), this runs once at the very end, so it can only improve.
        solver.restoreBest();
        auto t0 = steady_clock::now();
        int rounds = 0, committed = 0;
        while (duration_cast<duration<double>>(
                   steady_clock::now() - t0).count() < polishSec) {
            int c = solver.kRepairPass(/*maxEdges*/32, /*candsPerNode*/96);
            committed += c; rounds++;
            if (c == 0) break;
        }
        cerr << "[polish] rounds=" << rounds << " moves=" << committed
             << "  bestK=" << solver.bestK << " bestX=" << solver.bestX << "\n";
    }

    cerr << "Final best: k=" << solver.bestK
         << " totalX=" << solver.bestX << "\n";

    // Sanity check: the saved best layout must be a valid GD-contest drawing.
    {
        solver.restoreBest();
        int veFinal = solver.findVertexEdgeOverlapFast();
        if (veFinal >= 0) {
            cerr << "ERROR: best layout has vertex-edge overlap (vertex "
                 << veFinal << "). This should not happen.\n";
            return 3;
        }
    }

    cerr << "Writing: " << outputFile << "\n";
    writeGraph(outputFile, g, solver.bestPos);

    solver.curPhase = 3;
    solver.writeStatus(0, 0, 0, "done");
    return 0;
}
