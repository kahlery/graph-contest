// ===========================================================================
// BASELINE BUILD — pre-"k-critical vertex selection" snapshot.
// Original Phase-2 selection (total-crossing bias) for LNS/ILS/staged. Kept
// verbatim as the control for A/B vs the new default in src/approach1_lns.cpp.
// Build: `make baseline` -> ./approach1_baseline. Do NOT add features here.
// ===========================================================================
// approach1_lns.cpp — Large Neighbourhood Search for k-planarity minimization.
//
// Approach 1: LNS (Large Neighbourhood Search)
//   Destroy: BFS-connected neighbourhood from a high-crossing node.
//   Repair:  For each node in the neighbourhood, try 'candidates' random
//            positions and greedily commit the best strictly-improving one.
//   Restart: Every 'restartEvery' iterations pull back to the best known layout.
//
// Two-phase structure mirrors the SA solver:
//   Phase 1 — minimise total edge crossings.
//   Phase 2 — minimise k-value (max crossings per edge), crossings as tie-breaker.
//
// Build:  make approach1
// Usage:  ./approach1 -i input.json -o output.json [-t min] [-p1 min] [-s seed]
//                     [--mode {sa|lns|ils}]
//                     [--nh-size K] [--nh-cands R]
//                     [--ils-perturb P]
//
// All options that exist in ./sakgd work identically here.
// New options:
//   --mode sa          Run original SA (default, identical to sakgd)
//   --mode lns         Run Large Neighbourhood Search instead of SA
//   --mode ils         Run Iterated Local Search (SA + random kicks between rounds)
//   --nh-size K        Neighbourhood size per LNS iteration (default: n/10, min 3)
//   --nh-cands R       Candidate positions tried per node in repair (default: 50)
//   --ils-perturb P    Nodes randomly relocated per ILS kick (default: n/10, min 3)

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

// True iff p lies strictly inside open segment (a, b).
static inline bool pointOnSegmentStrict(const Pt& p, const Pt& a, const Pt& b) {
    if (p == a || p == b) return false;
    if (crossp(a, b, p) != 0) return false;
    if (p.x < min(a.x, b.x) || p.x > max(a.x, b.x)) return false;
    if (p.y < min(a.y, b.y) || p.y > max(a.y, b.y)) return false;
    return true;
}

// ====================================================================
// Minimal JSON
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
    ll   minCoordX = 0, minCoordY = 0;
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
        if (e.u == e.v) continue;
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
// Spatial grid
// ====================================================================
class Grid {
public:
    int gw = 1, gh = 1;
    ll  cellW = 1, cellH = 1;
    ll  ox = 0, oy = 0;

    vector<vector<int>>           cells;
    vector<vector<pair<int,int>>> edgeCells;

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
// Solver (SA + LNS)
// ====================================================================
struct MovePlan {
    int  v;
    Pt   oldPos, newPos;
    vector<tuple<int,int,int>>          pairChanges;
    vector<tuple<int,int,int>>          edgeCounts;
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

    vector<unordered_set<int>> xs;
    vector<int>                xc;
    vector<int>                cntPerK;
    int                        kVal    = 0;
    ll                         totalX  = 0;

    unordered_map<Pt, int, PtHash> occupied;

    Grid grid;

    // Vertex spatial grid (shares the edge grid's cell geometry). Built and
    // used only during the initial-layout repair. Not maintained during SA.
    vector<vector<int>> vCells;

    int        bestK = INT_MAX;
    ll         bestX = LLONG_MAX;
    vector<Pt> bestPos;

    string                          statusFile;
    string                          statusId   = "run";
    double                          statusInterval = 1.0;
    int                             curPhase   = 0;
    double                          curInitT   = 0;
    double                          curTempLim = 0;
    double                          curBudget  = 0;
    steady_clock::time_point        runStartedAt = steady_clock::now();
    steady_clock::time_point        phaseStartedAt = steady_clock::now();

    mt19937_64 rng;

    vector<double> cum;
    double         totalNodeW = 0.0;
    int            cumStaleCnt = 0;

    SAkGD() {
        rng.seed((uint64_t)chrono::steady_clock::now().time_since_epoch().count() ^
                 (uint64_t)(uintptr_t)this);
    }

    // ----- setup -------------------------------------------------------
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

        int gridSide = max(8, min(256, (int)round(sqrt((double)max(m, 1)) / 1.5)));
        grid.init(ox, oy, W, H, gridSide, m);
        for (int i = 0; i < m; i++)
            grid.addEdge(i, pos[edges[i].u], pos[edges[i].v]);

        // Produce a *valid* initial layout (distinct positions, no vertex on a
        // non-incident edge). Repair the given layout in place; if it cannot
        // converge (dense graphs in a small canvas), scatter and repair.
        rebuildOccupied();
        syncEdgeGrid();
        bool ok = repairLayout();
        for (int attempt = 0; attempt < 12 && !ok; attempt++) {
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

    bool wouldCauseVertexEdgeOverlap(int v, const Pt& newPos) const {
        for (int e = 0; e < m; e++) {
            int a = edges[e].u, b = edges[e].v;
            if (a == v || b == v) continue;
            if (pointOnSegmentStrict(newPos, pos[a], pos[b])) return true;
        }
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
    // See src/main.cpp for the rationale. The old O(n*m)-per-probe routine was
    // too slow and non-convergent on dense graphs in a small canvas (e.g.
    // Automatic-8), leaving an invalid layout that makes main exit rc=3.

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

    bool wouldCauseVertexEdgeOverlapFast(int v, const Pt& newPos) {
        bool bad = false;
        grid.newQuery();
        grid.forCandidates(newPos, newPos, [&](int e) {
            if (bad) return;
            int a = edges[e].u, b = edges[e].v;
            if (a == v || b == v) return;
            if (pointOnSegmentStrict(newPos, pos[a], pos[b])) bad = true;
        });
        if (bad) return true;
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
                    moveVertexAll(v, cur);
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
                return stuck == 0;
        }
        return findVertexEdgeOverlapFast() < 0;
    }

    // ----- core --------------------------------------------------------
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

    // ----- live status -------------------------------------------------
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
        for (int i = 0; i < m; i++) grid.removeEdge(i);
        pos = bestPos;
        for (int i = 0; i < m; i++)
            grid.addEdge(i, pos[edges[i].u], pos[edges[i].v]);
        rebuildOccupied();
        buildVertexGrid();        // resync vertex grid for the fast overlap check
        computeAllCrossings();
        rebuildCum();
    }

    // ----- selection ---------------------------------------------------
    void rebuildCum() {
        cum.assign(n, 0.0);
        double t = 0.0;
        for (int i = 0; i < n; i++) {
            double w = 1.0;
            for (int e : nodeEdges[i]) w += (double)xc[e];
            t += w;
            cum[i] = t;
        }
        totalNodeW  = t;
        cumStaleCnt = 0;
    }

    int selectNode() {
        if (cumStaleCnt > max(64, n / 4)) rebuildCum();
        if (totalNodeW <= 0)
            return uniform_int_distribution<int>(0, n - 1)(rng);
        double r = uniform_real_distribution<double>(0.0, totalNodeW)(rng);
        int lo = 0, hi = n - 1;
        while (lo < hi) {
            int mid = (lo + hi) >> 1;
            if (cum[mid] >= r) hi = mid;
            else               lo = mid + 1;
        }
        return lo;
    }

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
            nx += (uniform_int_distribution<int>(0, 1)(rng) ? 1 : -1);
            ny += (uniform_int_distribution<int>(0, 1)(rng) ? 1 : -1);
            if (nx < ox)     nx = ox;
            if (nx > ox + W) nx = ox + W;
            if (ny < oy)     ny = oy;
            if (ny > oy + H) ny = oy + H;
        }
        return {nx, ny};
    }

    // ----- move planning / commit --------------------------------------
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

        static thread_local unordered_map<int,int> deltaCount;
        deltaCount.clear();
        deltaCount.reserve(incidents.size() * 8 + 4);

        for (int i : incidents) {
            if (xc[i] > plan.oldLocalK) plan.oldLocalK = xc[i];
            for (int e2 : xs[i]) if (xc[e2] > plan.oldLocalK) plan.oldLocalK = xc[e2];
        }

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

        for (size_t k = 0; k < incidents.size(); k++) {
            int i = incidents[k];
            const auto& nx_i = newXsI[k];
            for (int e : xs[i]) {
                if (!nx_i.count(e)) {
                    plan.pairChanges.emplace_back(i, e, -1);
                    deltaCount[i]--;
                    deltaCount[e]--;
                    plan.dCross--;
                }
            }
            for (int e : nx_i) {
                if (!xs[i].count(e)) {
                    plan.pairChanges.emplace_back(i, e, +1);
                    deltaCount[i]++;
                    deltaCount[e]++;
                    plan.dCross++;
                }
            }
        }

        unordered_set<int> involvedSet;
        for (int i : incidents) {
            involvedSet.insert(i);
            for (int e2 : xs[i]) involvedSet.insert(e2);
        }
        for (auto& kv : deltaCount) involvedSet.insert(kv.first);

        for (int e : involvedSet) {
            int newCnt = xc[e] + (deltaCount.count(e) ? deltaCount[e] : 0);
            if (newCnt > plan.newLocalK) plan.newLocalK = newCnt;
            if (deltaCount.count(e))
                plan.edgeCounts.emplace_back(e, xc[e], newCnt);
        }
    }

    void commitMove(const MovePlan& plan) {
        const auto& incidents = nodeEdges[plan.v];
        for (int i : incidents) grid.removeEdge(i);
        occupied.erase(plan.oldPos);
        pos[plan.v] = plan.newPos;
        occupied[plan.newPos] = plan.v;
        for (int i : incidents) grid.addEdge(i, pos[edges[i].u], pos[edges[i].v]);
        // keep the vertex grid in sync so the fast overlap check stays correct.
        vGridMove(plan.v, plan.oldPos, plan.newPos);

        for (auto& pc : plan.pairChanges) {
            int e1 = std::get<0>(pc);
            int e2 = std::get<1>(pc);
            int d  = std::get<2>(pc);
            if (d > 0) { xs[e1].insert(e2); xs[e2].insert(e1); }
            else        { xs[e1].erase(e2);  xs[e2].erase(e1);  }
        }
        for (auto& ec : plan.edgeCounts) {
            changeEdgeCount(std::get<0>(ec), std::get<2>(ec));
        }
        totalX     += plan.dCross;
        cumStaleCnt += (int)plan.edgeCounts.size();
    }

    // ---- SA -----------------------------------------------------------
    void runSA(int phase,
               double initT, double decT, double decTW, double tLim,
               double timeLimitSec)
    {
        phaseStartedAt = steady_clock::now();
        curPhase   = phase;
        curInitT   = initT;
        curTempLim = tLim;
        curBudget  = timeLimitSec;

        auto elapsed = [&] {
            return duration_cast<duration<double>>(
                steady_clock::now() - phaseStartedAt).count();
        };

        long long moves = 0, accepts = 0;
        double startingTemp = initT;

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
        double nextReport = 0.5;
        double reportEvery = 30.0;
        double nextStatus  = 0.0;

        writeStatus(startingTemp, moves, accepts, "running");

        while (kVal > 0 && startingTemp > tLim && elapsed() < timeLimitSec) {
            double currentTemp = startingTemp;
            while (kVal > 0 && currentTemp > tLim && elapsed() < timeLimitSec) {
                int v      = selectNode();
                Pt  newPos = selectPlace(v, currentTemp, initT, phase == 2);
                if (newPos == pos[v]) { currentTemp *= decT; continue; }
                {
                    auto it = occupied.find(newPos);
                    if (it != occupied.end() && it->second != v) {
                        currentTemp *= decT; continue;
                    }
                }
                if (wouldCauseVertexEdgeOverlapFast(v, newPos)) {
                    currentTemp *= decT; continue;
                }

                planMove(v, newPos, plan);

                double dE;
                if (phase == 1) {
                    dE = (double)plan.dCross;
                } else {
                    int  dLocalK = plan.newLocalK - plan.oldLocalK;
                    if (dLocalK != 0) dE = (double)dLocalK;
                    else dE = (double)plan.dCross /
                              max(1.0, (double)max<ll>(1, totalX));
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
                    if (kVal < bestK || (kVal == bestK && totalX < bestX))
                        saveBest();
                }
                moves++;
                currentTemp *= decT;

                double el = elapsed();
                if (el >= nextStatus) {
                    writeStatus(currentTemp, moves, accepts, "running");
                    nextStatus = el + statusInterval;
                }
                if (el >= nextReport) {
                    if (el >= reportEvery) reportTime(currentTemp);
                    nextReport = el + reportEvery;
                }
            }
            startingTemp *= decTW;
            restoreBest();
        }

        writeStatus(startingTemp, moves, accepts, "phase-done");

        cerr << "[phase " << phase << "] end    moves=" << moves
             << " accepts=" << accepts
             << "  bestK=" << bestK << " bestX=" << bestX << "\n";
    }

    // ---- LNS ----------------------------------------------------------

    // BFS-connected neighbourhood of 'size' nodes starting from a
    // crossing-weight-biased node (same bias as selectNode).
    vector<int> selectNeighbourhood(int size) {
        size = max(1, min(size, n));
        int start = selectNode();

        vector<bool> inNH(n, false);
        vector<int>  nh, bfsQ;
        nh.reserve(size);
        bfsQ.reserve(size);

        inNH[start] = true;
        nh.push_back(start);
        bfsQ.push_back(start);

        for (int qi = 0; qi < (int)bfsQ.size() && (int)nh.size() < size; qi++) {
            int v = bfsQ[qi];
            vector<int> nbrs;
            nbrs.reserve(nodeEdges[v].size());
            for (int e : nodeEdges[v]) {
                int nb = (edges[e].u == v) ? edges[e].v : edges[e].u;
                if (!inNH[nb]) nbrs.push_back(nb);
            }
            shuffle(nbrs.begin(), nbrs.end(), rng);
            for (int nb : nbrs) {
                if ((int)nh.size() >= size) break;
                inNH[nb] = true;
                nh.push_back(nb);
                bfsQ.push_back(nb);
            }
        }

        // Pad with random nodes if BFS exhausted (disconnected graph).
        while ((int)nh.size() < size) {
            int v = uniform_int_distribution<int>(0, n - 1)(rng);
            if (!inNH[v]) { inNH[v] = true; nh.push_back(v); }
        }

        return nh;
    }

    // LNS main loop.
    // Phase 1: minimise total crossings.  Phase 2: minimise k-value.
    // nhSize:     nodes per neighbourhood  (0 = auto: n/10, min 3).
    // candidates: positions tried per node (0 = auto: 50).
    // adaptive:   if true, dynamically resize neighbourhood based on improvement rate.
    void runLNS(int phase,
                double initT, double tLim,
                double timeLimitSec,
                int nhSize, int candidates,
                bool adaptive = false)
    {
        phaseStartedAt = steady_clock::now();
        curPhase   = 10 + phase;   // 11 / 12 so the dashboard can distinguish
        curInitT   = initT;
        curTempLim = tLim;
        curBudget  = timeLimitSec;

        auto elapsed = [&] {
            return duration_cast<duration<double>>(
                steady_clock::now() - phaseStartedAt).count();
        };

        // Auto neighbourhood size: n/10, but capped so a single LNS iteration
        // (nhSize * candidates planMoves) stays bounded on huge graphs. Without
        // this, Automatic-8 (n=10466) would use nhSize=1046, making one
        // iteration cost tens of thousands of planMoves over ~38M crossings —
        // minutes per iteration, blowing past the time budget.
        if (nhSize    <= 0) nhSize    = max(3, min(n / 10, 64));
        if (candidates <= 0) candidates = 50;

        const int nhMin = 3;
        const int nhMax = max(nhMin, n / 3);

        // Restart from best every restartEvery iterations to prevent drift.
        int restartEvery = max(10, 500 / nhSize);

        cerr << "[LNS" << (adaptive ? "-adaptive" : "") << " phase " << phase << "] start"
             << "  nhSize=" << nhSize << " candidates=" << candidates
             << "  restartEvery=" << restartEvery
             << "  budget=" << timeLimitSec << "s"
             << "  initial k=" << kVal << " totalX=" << totalX << "\n";

        long long iters = 0, improves = 0;
        // Adaptive: track improvements in a sliding window of 50 iterations.
        const int adaptWindow = 50;
        long long windowImproves = 0;
        double nextReport = 30.0;
        double nextStatus = 0.0;

        MovePlan plan, bestPlan;
        plan.pairChanges.reserve(512);
        plan.edgeCounts.reserve(512);
        bestPlan.pairChanges.reserve(512);
        bestPlan.edgeCounts.reserve(512);

        writeStatus(initT, iters, improves, "running");

        while (kVal > 0 && elapsed() < timeLimitSec) {
            // Temperature decays geometrically over the full budget;
            // controls Gaussian exploration radius inside selectPlace.
            double tFrac = elapsed() / max(1.0, timeLimitSec);
            if (tFrac > 1.0) tFrac = 1.0;
            double T = initT * pow(max(tLim, 1e-9) / max(initT, 1e-9), tFrac);
            if (T < tLim) T = tLim;

            // Destroy: select a connected neighbourhood.
            vector<int> nh = selectNeighbourhood(nhSize);

            // Repair: for each node in the neighbourhood, find the best
            // strictly-improving position among 'candidates' random draws.
            for (int v : nh) {
                // Bound a single iteration to the time budget. On huge graphs
                // one node's `candidates` planMoves are costly, so the coarse
                // per-iteration check at the while() head can overshoot badly;
                // re-check here so LNS honours `-t` even mid-neighbourhood.
                if (elapsed() >= timeLimitSec) break;

                bool   foundBetter  = false;
                double bestDeltaFit = 0.0;  // only commit when dFit < 0

                for (int r = 0; r < candidates; r++) {
                    Pt newPos = selectPlace(v, T, initT, /*localOnly=*/false);
                    if (newPos == pos[v]) continue;
                    {
                        auto it = occupied.find(newPos);
                        if (it != occupied.end() && it->second != v) continue;
                    }
                    if (wouldCauseVertexEdgeOverlapFast(v, newPos)) continue;

                    planMove(v, newPos, plan);

                    double dFit;
                    if (phase == 1) {
                        dFit = (double)plan.dCross;
                    } else {
                        int dK = plan.newLocalK - plan.oldLocalK;
                        if (dK != 0)
                            dFit = (double)dK;
                        else
                            dFit = (double)plan.dCross /
                                   max(1.0, (double)max<ll>(1, totalX));
                    }

                    if (dFit < bestDeltaFit) {
                        bestDeltaFit = dFit;
                        bestPlan     = plan;
                        foundBetter  = true;
                    }
                }

                if (foundBetter) commitMove(bestPlan);
            }

            if (kVal < bestK || (kVal == bestK && totalX < bestX)) {
                saveBest();
                improves++;
                windowImproves++;
            }

            iters++;

            // Adaptive: resize neighbourhood every adaptWindow iterations.
            if (adaptive && iters % adaptWindow == 0) {
                double rate = (double)windowImproves / adaptWindow;
                if (rate < 0.05) {
                    // Stalled — expand neighbourhood to escape local optimum.
                    nhSize = min(nhMax, (int)(nhSize * 1.5 + 1));
                    restartEvery = max(10, 500 / nhSize);
                    cerr << "  [LNS-adaptive] stalled (rate=" << rate
                         << ") -> nhSize=" << nhSize << "\n";
                } else if (rate > 0.25 && nhSize > nhMin) {
                    // Improving well — shrink for finer-grained search.
                    nhSize = max(nhMin, (int)(nhSize / 1.3));
                    restartEvery = max(10, 500 / nhSize);
                    cerr << "  [LNS-adaptive] improving (rate=" << rate
                         << ") -> nhSize=" << nhSize << "\n";
                }
                windowImproves = 0;
            }

            // Periodic restart from best to bound quality degradation.
            if (iters % restartEvery == 0) restoreBest();

            double el = elapsed();
            if (el >= nextStatus) {
                writeStatus(T, iters, improves, "running");
                nextStatus = el + statusInterval;
            }
            if (el >= nextReport) {
                cerr << "  [LNS" << (adaptive ? "-adaptive" : "") << " " << phase
                     << "] t=" << (int)el
                     << "s  bestK=" << bestK << " bestX=" << bestX
                     << "  curK=" << kVal << " curX=" << totalX
                     << "  nhSize=" << nhSize
                     << "  iters=" << iters << " improves=" << improves << "\n";
                nextReport = el + 30.0;
            }
        }

        writeStatus(0, iters, improves, "phase-done");
        cerr << "[LNS" << (adaptive ? "-adaptive" : "") << " phase " << phase << "] end"
             << "  iters=" << iters << " improves=" << improves
             << "  bestK=" << bestK << " bestX=" << bestX << "\n";
    }

    // ---- ILS (Iterated Local Search) ------------------------------------

    // Random perturbation kick used between inner SA rounds.
    // Relocates 'size' nodes to uniformly-random valid canvas positions.
    // The idea: SA converges to a local optimum; the kick disrupts the layout
    // enough that the next SA round explores a different basin.
    void kick(int size) {
        size = max(1, min(size, n));

        // Random permutation so we don't always kick the same nodes.
        vector<int> order(n);
        for (int i = 0; i < n; i++) order[i] = i;
        shuffle(order.begin(), order.end(), rng);

        MovePlan plan;
        plan.pairChanges.reserve(256);
        plan.edgeCounts.reserve(256);

        int kicked = 0;
        for (int v : order) {
            if (kicked >= size) break;
            for (int attempt = 0; attempt < 200; attempt++) {
                ll nx = uniform_int_distribution<ll>(ox, ox + W)(rng);
                ll ny = uniform_int_distribution<ll>(oy, oy + H)(rng);
                Pt newPos{nx, ny};
                if (newPos == pos[v]) continue;
                {
                    auto it = occupied.find(newPos);
                    if (it != occupied.end() && it->second != v) continue;
                }
                if (wouldCauseVertexEdgeOverlapFast(v, newPos)) continue;
                planMove(v, newPos, plan);
                commitMove(plan);
                kicked++;
                break;
            }
        }
        cerr << "[ILS] kick: moved " << kicked << "/" << size << " nodes\n";
    }

    // ILS main loop.
    // Alternates SA annealing runs with random kicks to escape local optima.
    // Each inner SA runs for innerBudget seconds (total / targetRounds).
    // perturbSize: nodes kicked per perturbation (0 = auto: n/10, min 3).
    void runILS(int phase,
                double initT, double decT, double decTW, double tLim,
                double totalTimeSec, int perturbSize) {
        auto t0 = steady_clock::now();
        auto totalElapsed = [&]() {
            return duration_cast<duration<double>>(
                steady_clock::now() - t0).count();
        };

        if (perturbSize <= 0) perturbSize = max(3, n / 10);

        // Target ~5 inner SA rounds; each at least 10 s.
        int targetRounds   = max(2, min(5, (int)(totalTimeSec / 10.0)));
        double innerBudget = totalTimeSec / targetRounds;

        cerr << "[ILS phase " << phase << "] start"
             << "  perturbSize=" << perturbSize
             << "  innerBudget=" << (int)innerBudget << "s"
             << "  totalBudget=" << (int)totalTimeSec << "s"
             << "  initial k=" << kVal << " totalX=" << totalX << "\n";

        int round = 0;
        while (kVal > 0 && totalElapsed() < totalTimeSec) {
            double remaining = totalTimeSec - totalElapsed();
            if (remaining <= 1.0) break;
            double budget = min(innerBudget, remaining);

            cerr << "[ILS phase " << phase << "] round " << round + 1
                 << "/" << targetRounds
                 << "  budget=" << (int)budget << "s"
                 << "  curBestK=" << bestK << "\n";

            runSA(phase, initT, decT, decTW, tLim, budget);
            round++;

            if (kVal <= 0 || totalElapsed() >= totalTimeSec) break;

            // Restore global best, then apply random kick so the next SA
            // round starts from a perturbed version of the best solution.
            restoreBest();
            kick(perturbSize);
        }

        restoreBest();
        cerr << "[ILS phase " << phase << "] end  rounds=" << round
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
        "  " << prog << " input.json output.json\n"
        "  " << prog << " --verify input.json\n"
        "\nOptions (shared with sakgd):\n"
        "  -t  total time budget in minutes         (default: 60)\n"
        "  -p1 time budget of phase 1 in minutes    (default: 10)\n"
        "  -s  RNG seed                             (default: time-based)\n"
        "  --status-file PATH    write live status JSON to PATH\n"
        "  --status-id   STRING  identifier shown in the dashboard\n"
        "  --status-interval SEC seconds between status dumps (default: 1.0)\n"
        "  --verify              parse the input, report metrics and exit\n"
        "\nApproach-1 options:\n"
        "  --mode sa             Run simulated annealing (default)\n"
        "  --mode lns            Run Large Neighbourhood Search\n"
        "  --mode lns-adaptive   Run LNS with dynamic neighbourhood sizing\n"
        "  --mode ils            Run Iterated Local Search (SA + random kicks)\n"
        "  --nh-size K           LNS neighbourhood size   (default: n/10, min 3)\n"
        "  --nh-cands R          LNS candidates per node  (default: 50)\n"
        "  --ils-perturb P       ILS kick size            (default: n/10, min 3)\n";
}

int main(int argc, char** argv) {
    string inputFile, outputFile;
    double totalMin  = 60.0;
    double phase1Min = 10.0;
    long long seed   = -1;
    bool verifyOnly  = false;
    string statusFile, statusId = "run";
    double statusInterval = 1.0;
    string mode = "sa";
    int lnsNhSize  = 0;
    int lnsCands   = 0;
    int ilsPerturb = 0;

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
        else if (a == "--status-file")       statusFile = need("--status-file");
        else if (a == "--status-id")         statusId   = need("--status-id");
        else if (a == "--status-interval")   statusInterval = atof(need("--status-interval"));
        else if (a == "--verify")            verifyOnly = true;
        else if (a == "--mode")              mode       = need("--mode");
        else if (a == "--nh-size")           lnsNhSize  = atoi(need("--nh-size"));
        else if (a == "--nh-cands")          lnsCands   = atoi(need("--nh-cands"));
        else if (a == "--ils-perturb")       ilsPerturb = atoi(need("--ils-perturb"));
        else if (a == "-h" || a == "--help") { printUsage(argv[0]); return 0; }
        else if (inputFile.empty())  inputFile  = a;
        else if (outputFile.empty()) outputFile = a;
        else { printUsage(argv[0]); return 1; }
    }
    if (inputFile.empty()) { printUsage(argv[0]); return 1; }
    if (outputFile.empty() && !verifyOnly) outputFile = inputFile + ".out.json";
    if (mode != "sa" && mode != "lns" && mode != "lns-adaptive" && mode != "ils") {
        cerr << "Unknown --mode '" << mode << "' (expected 'sa', 'lns', 'lns-adaptive', or 'ils')\n";
        return 1;
    }

    cerr << "Reading: " << inputFile << "\n";
    GraphData g = readGraph(inputFile);
    cerr << "n=" << g.n << " m=" << g.m
         << " W=" << g.W << " H=" << g.H << "\n";

    SAkGD solver;
    if (seed >= 0) solver.rng.seed((uint64_t)seed);
    solver.statusFile     = statusFile;
    solver.statusId       = statusId;
    solver.statusInterval = max(0.05, statusInterval);
    solver.runStartedAt   = steady_clock::now();
    solver.setup(g);
    int veInit = solver.findVertexEdgeOverlapFast();
    cerr << "Initial: k=" << solver.kVal
         << " totalX=" << solver.totalX
         << "  vertexEdgeOverlap=" << (veInit < 0 ? "no" : "YES")
         << "\n";

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

    double remaining = max(0.0, (totalMin - phase1Min) * 60.0);

    if (mode == "lns" || mode == "lns-adaptive") {
        bool adaptive = (mode == "lns-adaptive");
        cerr << "Mode: " << (adaptive ? "LNS-adaptive" : "LNS") << " (Large Neighbourhood Search)"
             << "  nhSize=" << (lnsNhSize > 0 ? to_string(lnsNhSize) : "auto")
             << " cands=" << (lnsCands > 0 ? to_string(lnsCands) : "auto") << "\n";
        solver.runLNS(/*phase*/1, 50.0, 0.01, phase1Min * 60.0, lnsNhSize, lnsCands, adaptive);
        solver.runLNS(/*phase*/2,  1.0, 0.01, remaining,        lnsNhSize, lnsCands, adaptive);
    } else if (mode == "ils") {
        cerr << "Mode: ILS (Iterated Local Search)"
             << "  perturbSize=" << (ilsPerturb > 0 ? to_string(ilsPerturb) : "auto") << "\n";
        solver.runILS(/*phase*/1, 50.0,  0.999, 0.99, 0.01, phase1Min * 60.0, ilsPerturb);
        solver.runILS(/*phase*/2,  1.0, 0.9999, 0.99, 0.01, remaining,        ilsPerturb);
    } else {
        cerr << "Mode: SA (Simulated Annealing)\n";
        solver.runSA(/*phase*/1, 50.0,  0.999, 0.99, 0.01, phase1Min * 60.0);
        solver.runSA(/*phase*/2,  1.0, 0.9999, 0.99, 0.01, remaining);
    }

    cerr << "Final best: k=" << solver.bestK
         << " totalX=" << solver.bestX << "\n";

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
