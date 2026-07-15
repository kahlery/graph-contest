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

// Flat, allocation-free replacement for unordered_set<int>, used for the
// per-edge crossing sets (xs) and the small per-move scratch sets in
// planMove(). These stay small in practice (post-smoothing initial layouts
// average single/low-double-digit crossings per edge, and SA drives that
// down further), so a linear scan over a contiguous vector avoids the
// per-insert/erase heap-node churn and cache-missing pointer chasing that
// unordered_set incurs for sets this size - it's the same "small vector"
// tradeoff libraries like boost::flat_set are built on.
struct SmallIntSet {
    vector<int> v;
    bool contains(int x) const {
        for (int y : v) if (y == x) return true;
        return false;
    }
    size_t count(int x) const { return contains(x) ? 1 : 0; }
    void insert(int x) {
        if (!contains(x)) v.push_back(x);
    }
    void erase(int x) {
        for (size_t i = 0; i < v.size(); i++) {
            if (v[i] == x) { v[i] = v.back(); v.pop_back(); return; }
        }
    }
    size_t size() const { return v.size(); }
    void clear() { v.clear(); }
    vector<int>::const_iterator begin() const { return v.begin(); }
    vector<int>::const_iterator end()   const { return v.end(); }
};

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
    vector<SmallIntSet> xs;
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

    // Move benefit/harm instrumentation (optional): appends one compact
    // record per evaluated phase-2 move so the winning/losing move geometry
    // can be analysed offline. Accepted moves are logged in full; rejected
    // ones are 1/16-sampled to bound file size.
    // Line: slot acc dLK dX dist d2c_b d2c_a r_b r_a deg xcHot
    //   slot: 0-3 = cands-mix hypothesis slot, -1 = single/iid-gauss,
    //         9 = coupled edge-translation
    //   d2c: distance to the node's neighbour centroid (before/after)
    //   r:   distance to the layout's centre of mass (before/after)
    string   moveLogFile;
    FILE*    moveLog = nullptr;
    unsigned mlRejCnt = 0;
    double   gcx = 0, gcy = 0;       // centre of mass, updated O(1) per commit

    void moveLogOpen() {
        if (!moveLogFile.empty() && !moveLog)
            moveLog = fopen(moveLogFile.c_str(), "w");
    }
    void recomputeCenter() {
        double sx = 0, sy = 0;
        for (int i = 0; i < n; i++) { sx += pos[i].x; sy += pos[i].y; }
        gcx = sx / max(1, n); gcy = sy / max(1, n);
    }
    void nbCentroid(int v, double& cx, double& cy) const {
        const auto& inc = nodeEdges[v];
        if (inc.empty()) { cx = pos[v].x; cy = pos[v].y; return; }
        double sx = 0, sy = 0;
        for (int e : inc) {
            int u = (edges[e].u == v) ? edges[e].v : edges[e].u;
            sx += pos[u].x; sy += pos[u].y;
        }
        cx = sx / inc.size(); cy = sy / inc.size();
    }
    static double dist2d(double ax, double ay, double bx, double by) {
        double dx = ax - bx, dy = ay - by;
        return sqrt(dx * dx + dy * dy);
    }
    // Log one evaluated move. Call BEFORE commit (positions unchanged).
    void moveLogRecord(int v, int slot, bool acc, int dLK, ll dX,
                       const Pt& oldP, const Pt& newP) {
        if (!moveLog) return;
        if (!acc && (++mlRejCnt & 15u) != 0) return;
        double cx, cy;
        nbCentroid(v, cx, cy);
        int xh = 0;
        for (int e : nodeEdges[v]) if (xc[e] > xh) xh = xc[e];
        fprintf(moveLog, "%d %d %d %lld %.0f %.0f %.0f %.0f %.0f %zu %d\n",
                slot, acc ? 1 : 0, dLK, (long long)dX,
                dist2d(oldP.x, oldP.y, newP.x, newP.y),
                dist2d(oldP.x, oldP.y, cx, cy),
                dist2d(newP.x, newP.y, cx, cy),
                dist2d(oldP.x, oldP.y, gcx, gcy),
                dist2d(newP.x, newP.y, gcx, gcy),
                nodeEdges[v].size(), xh);
    }

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
    int            placeMode = 0;   // 0 = plain Gaussian proposal,
                                    // 1 = congestion-aware (pick emptiest of
                                    // C Gaussian candidates by edge-grid cell)
                                    // 2 = barycenter-pull (bias toward neighbour
                                    // centroid + Gaussian jitter; force-directed
                                    // proposal inside the SA acceptance loop)
                                    // 3 = smart mixture: neighbour-informed
                                    // proposals (random-subset centroid, near a
                                    // random neighbour, two-neighbour midpoint)
                                    // mixed with the plain Gaussian walk
    int            candsP2 = 1;     // phase-2 proposals evaluated per move:
                                    // plan all C candidates exactly, feed only
                                    // the best dE to the acceptance rule
    int            candsP1 = 1;     // same for phase 1
    int            edgeMoveP = 0;   // phase-2 % chance of a coupled edge-
                                    // translation move (both endpoints of a
                                    // bottleneck-ish incident edge shift by
                                    // one shared delta) instead of a single-
                                    // vertex proposal
    bool           bandit = false;   // sample cands-mix slots by a sliding-
                                    // window credit (localK-lowering accepts)
                                    // instead of one-of-each; floors keep
                                    // every hypothesis alive. Bounded upside
                                    // (best-of-C already evaluates all four
                                    // exactly) but auto-adapts on unseen
                                    // graphs where the winning hypothesis
                                    // mix is unknown.
    double         slotCredit[4] = {1, 1, 1, 1};
    long long      banditTick = 0;
    int            banditSlot() {
        double w[4], tot = 0;
        for (int s = 0; s < 4; s++) {
            w[s] = 0.15 + slotCredit[s];
            tot += w[s];
        }
        double r = uniform_real_distribution<double>(0, tot)(rng);
        for (int s = 0; s < 3; s++) { r -= w[s]; if (r < 0) return s; }
        return 3;
    }
    bool           pairMove = false;  // edgeMoveP budget runs the coordinated
                                    // two-endpoint move instead of rigid
                                    // translation (see attemptPairMove)
    bool           slot0Drift = false; // slot-0: drift walk instead of the
                                    // blind Gaussian (see proposeCandidate)
    bool           levelClear = false; // run the level-clearing sweep at each
                                    // phase-2 budget-reheat (stagnation) point
    bool           gridAnneal = false; // coarse-to-fine proposal quantisation:
                                    // crossings depend only on the combinatorial
                                    // arrangement, so proposals finer than the
                                    // current structural resolution are wasted
                                    // planMoves (move-log: improving moves have
                                    // median dist 4-12k, tiny moves ~never win).
                                    // Snap proposals to a grid that halves as
                                    // the phase budget burns: ~min(W,H)/64 -> 1.
                                    // A/B verdict (2026-07-11, paired tripod
                                    // cold 10-min, 05/06/08 x 2 seeds): cuts
                                    // variance (rescues bad seeds: 230->213,
                                    // 81->71) but does not raise the ceiling
                                    // and regresses 06 (213/215 -> 219/219);
                                    // the 8-seed workers already harvest the
                                    // best draw, so this stays OFF by default.
    ll             gridStep = 1;    // current cell size (updated in runSA)
    bool           candsRamp = false; // ramp C up over the phase budget:
                                    // early descent (far from optimum) wants
                                    // move VOLUME (C=1); the cooled fine
                                    // descent wants move QUALITY (full C).
                                    // <30% budget: 1, 30-60%: ceil(C/2), then C
    int            acceptMode = 0;  // 0 = Metropolis, 1 = threshold-accepting,
                                    // 2 = late-acceptance hill climbing (LAHC)
    bool           swapMove = false; // when a proposal lands on an occupied
                                     // point, swap the two vertices instead of
                                     // discarding the (otherwise wasted) move
    vector<double> lahcHist;        // LAHC: rolling history of fitness values
    size_t         lahcIdx  = 0;
    double         fitAcc   = 0.0;  // LAHC scalar: cumulative accepted dE
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
    // BFS visit order over all components: graph-adjacent vertices land near
    // each other in the order, so any locality-preserving space-filling path
    // (snake or Hilbert) keeps their geometric distance small too.
    vector<int> bfsOrder() {
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
        return order;
    }

    // Map a Hilbert-curve distance d (0..S^2-1) to grid cell (x,y), S = 2^k.
    static void hilbertD2XY(ll S, ll d, ll& x, ll& y) {
        x = 0; y = 0;
        for (ll s = 1; s < S; s <<= 1) {
            ll rx = 1 & (d / 2);
            ll ry = 1 & (d ^ rx);
            if (ry == 0) {                       // rotate quadrant
                if (rx == 1) { x = s - 1 - x; y = s - 1 - y; }
                ll t = x; x = y; y = t;
            }
            x += s * rx;
            y += s * ry;
            d /= 4;
        }
    }

    // Like bfsSnakeLayout but lays the BFS order along a Hilbert curve. The
    // Hilbert curve preserves 2-D locality better than a boustrophedon: two
    // points close in curve order are always close in the plane (the snake
    // only guarantees this within a row), so graph neighbours stay nearer and
    // edges stay shorter. Cheap deterministic init, an extra auto candidate.
    void hilbertLayout() {
        vector<int> order = bfsOrder();
        ll side = 1;
        while (side * side < (ll)n) side <<= 1;   // smallest 2^k with side^2>=n
        ll dx = max<ll>(1, W / max<ll>(1, side - 1));
        ll dy = max<ll>(1, H / max<ll>(1, side - 1));
        mt19937_64 jr(0x5EEDB0B5ull);
        for (size_t idx = 0; idx < order.size(); idx++) {
            ll hx, hy;
            hilbertD2XY(side, (ll)idx, hx, hy);
            ll x = min(W, hx * dx + (dx > 1 ? (ll)(jr() % (uint64_t)dx) : 0));
            ll y = min(H, hy * dy + (dy > 1 ? (ll)(jr() % (uint64_t)dy) : 0));
            pos[order[idx]] = {ox + x, oy + y};
        }
    }

    void bfsSnakeLayout() {
        vector<int> order = bfsOrder();
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
        if (initMode == "hilbert" && n > 1) {
            // Forced Hilbert init (skip the sampling race; deterministic).
            hilbertLayout();
            cerr << "init: forced hilbert layout, avg crossings/edge≈"
                 << estimateAvgCross(300) << "\n";
        } else if (initMode != "input" && n > 1) {
            double inAvg = estimateAvgCross(300);
            vector<Pt> inputPos = pos;
            bfsSnakeLayout();
            double snAvg = estimateAvgCross(300);
            vector<Pt> snakePos = pos;
            barycenterSmooth(50);
            double smAvg = estimateAvgCross(300);
            vector<Pt> smoothPos = pos;
            // Hilbert is a cheap extra candidate in auto mode.
            hilbertLayout();
            double hbAvg = estimateAvgCross(300);
            vector<Pt> hilbPos = pos;
            const char* chosen;
            double bestConstr = min(min(snAvg, smAvg), hbAvg);
            if (initMode != "bfs" && !(bestConstr < 0.8 * inAvg)) {
                pos = inputPos;   chosen = "input";
            } else if (hbAvg <= smAvg && hbAvg <= snAvg) {
                pos = hilbPos;    chosen = "hilbert";
            } else if (smAvg <= snAvg) {
                pos = smoothPos;  chosen = "bfs-snake+smooth";
            } else {
                pos = snakePos;   chosen = "bfs-snake";
            }
            cerr << "init: avg crossings/edge  input≈" << inAvg
                 << "  bfs-snake≈" << snAvg << "  +smooth≈" << smAvg
                 << "  hilbert≈" << hbAvg
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
        recomputeCenter();        // wholesale position change: resync exactly
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

        // Congestion-aware proposal: draw C Gaussian candidates and keep the
        // one landing in the least edge-dense grid cell. The edge grid's
        // per-cell vector size is exactly "how many edges' bounding boxes
        // cover this cell" — a cheap local congestion signal. Picking the
        // emptiest cell biases bottleneck endpoints toward landing spots that
        // are likely to shed crossings, without touching planMove.
        if (placeMode == 1) {
            ll bestX = pos[v].x, bestY = pos[v].y;
            int bestDen = INT_MAX;
            for (int c = 0; c < 3; c++) {
                ll cx = pos[v].x + (ll)llround(nd(rng));
                ll cy = pos[v].y + (ll)llround(nd(rng));
                if (cx < ox) cx = ox; if (cx > ox + W) cx = ox + W;
                if (cy < oy) cy = oy; if (cy > oy + H) cy = oy + H;
                int den = (int)grid.cells[grid.cy(cy) * grid.gw + grid.cx(cx)].size();
                if (den < bestDen) { bestDen = den; bestX = cx; bestY = cy; }
            }
            ll nx = bestX, ny = bestY;
            if (nx == pos[v].x && ny == pos[v].y) {
                nx += (uniform_int_distribution<int>(0, 1)(rng) ? 1 : -1);
                ny += (uniform_int_distribution<int>(0, 1)(rng) ? 1 : -1);
                if (nx < ox) nx = ox; if (nx > ox + W) nx = ox + W;
                if (ny < oy) ny = oy; if (ny > oy + H) ny = oy + H;
            }
            return {nx, ny};
        }

        // Barycenter-pull proposal: bias the move toward the centroid of v's
        // graph neighbours, then add Gaussian jitter. This folds a force-
        // directed step into the SA proposal — bottleneck vertices drift toward
        // where their incident edges "want" them (shorter edges => fewer
        // crossings) while SA acceptance still gates worsening moves. Falls back
        // to plain Gaussian when v has no neighbours.
        if (placeMode == 2) {
            const auto& inc = nodeEdges[v];
            if (!inc.empty()) {
                double sx = 0, sy = 0;
                for (int e : inc) {
                    int other = (edges[e].u == v ? edges[e].v : edges[e].u);
                    sx += pos[other].x; sy += pos[other].y;
                }
                double cx = sx / (double)inc.size();
                double cy = sy / (double)inc.size();
                const double alpha = 0.5; // pull fraction toward centroid
                double tx = pos[v].x + alpha * (cx - pos[v].x);
                double ty = pos[v].y + alpha * (cy - pos[v].y);
                ll bnx = (ll)llround(tx + nd(rng));
                ll bny = (ll)llround(ty + nd(rng));
                if (bnx < ox) bnx = ox; if (bnx > ox + W) bnx = ox + W;
                if (bny < oy) bny = oy; if (bny > oy + H) bny = oy + H;
                if (bnx == pos[v].x && bny == pos[v].y) {
                    bnx += (uniform_int_distribution<int>(0, 1)(rng) ? 1 : -1);
                    bny += (uniform_int_distribution<int>(0, 1)(rng) ? 1 : -1);
                    if (bnx < ox) bnx = ox; if (bnx > ox + W) bnx = ox + W;
                    if (bny < oy) bny = oy; if (bny > oy + H) bny = oy + H;
                }
                return {bnx, bny};
            }
        }

        // Smart mixture proposal: with probability pNb, derive the target from
        // the positions of v's graph neighbours instead of a blind random walk.
        // Three neighbour-informed strategies, all + Gaussian jitter:
        //   a) random-subset centroid — centroid of a random subset (>=2) of
        //      neighbours; subsumes the full barycenter and generalises it,
        //   b) near a random neighbour — shortens that incident edge directly,
        //   c) two-neighbour midpoint — for cycle/chain-like local structure
        //      (deg-2 vertices: exactly the point that straightens the chain).
        // Neighbour-informed targets need less jitter than the exploring walk,
        // so they use a tighter sigma; the remaining mass falls through to the
        // plain Gaussian so global exploration never dies.
        if (placeMode == 3) {
            const auto& inc = nodeEdges[v];
            double rs = uniform_real_distribution<double>(0, 1)(rng);
            if (!inc.empty() && rs < 0.60) {
                auto nbPos = [&](int e) -> const Pt& {
                    return pos[(edges[e].u == v) ? edges[e].v : edges[e].u];
                };
                double tx, ty;
                double r2 = uniform_real_distribution<double>(0, 1)(rng);
                if (inc.size() >= 2 && r2 < 0.45) {
                    // (a) centroid of a random subset of 2..deg neighbours
                    int deg = (int)inc.size();
                    int cntN = uniform_int_distribution<int>(2, deg)(rng);
                    double sx = 0, sy = 0;
                    for (int t = 0; t < cntN; t++) {
                        const Pt& q = nbPos(inc[uniform_int_distribution<int>(
                            0, deg - 1)(rng)]);
                        sx += (double)q.x; sy += (double)q.y;
                    }
                    tx = sx / cntN; ty = sy / cntN;
                } else if (inc.size() >= 2 && r2 < 0.70) {
                    // (c) midpoint of two distinct random neighbours
                    int deg = (int)inc.size();
                    int i1 = uniform_int_distribution<int>(0, deg - 1)(rng);
                    int i2 = uniform_int_distribution<int>(0, deg - 2)(rng);
                    if (i2 >= i1) i2++;
                    const Pt& q1 = nbPos(inc[i1]);
                    const Pt& q2 = nbPos(inc[i2]);
                    tx = 0.5 * (q1.x + q2.x); ty = 0.5 * (q1.y + q2.y);
                } else {
                    // (b) near a random neighbour
                    const Pt& q = nbPos(inc[uniform_int_distribution<int>(
                        0, (int)inc.size() - 1)(rng)]);
                    tx = (double)q.x; ty = (double)q.y;
                }
                double sloc = max(2.0, sigma * 0.35);
                normal_distribution<double> ndl(0.0, sloc);
                ll nx = (ll)llround(tx + ndl(rng));
                ll ny = (ll)llround(ty + ndl(rng));
                if (nx < ox) nx = ox; if (nx > ox + W) nx = ox + W;
                if (ny < oy) ny = oy; if (ny > oy + H) ny = oy + H;
                if (nx == pos[v].x && ny == pos[v].y) {
                    nx += (uniform_int_distribution<int>(0, 1)(rng) ? 1 : -1);
                    ny += (uniform_int_distribution<int>(0, 1)(rng) ? 1 : -1);
                    if (nx < ox) nx = ox; if (nx > ox + W) nx = ox + W;
                    if (ny < oy) ny = oy; if (ny > oy + H) ny = oy + H;
                }
                return {nx, ny};
            }
            // fall through to the plain Gaussian walk
        }

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
        vector<SmallIntSet> newXsI(incidents.size());

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
        SmallIntSet involvedSet;
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
        gcx += (double)(plan.newPos.x - plan.oldPos.x) / max(1, n);
        gcy += (double)(plan.newPos.y - plan.oldPos.y) / max(1, n);
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

    // ----- acceptance rule -------------------------------------------
    // Decide whether to accept a move of cost delta dE at temperature
    // currentTemp, under the active acceptMode. LAHC mutates its rolling
    // history here so every evaluated move (normal or swap) advances it
    // consistently; fitAcc is only updated when the move is accepted.
    bool acceptByRule(double dE, double currentTemp) {
        if (acceptMode == 1) {              // threshold accepting
            return dE <= currentTemp;
        }
        if (acceptMode == 2) {              // late-acceptance hill climbing
            bool acc = (dE <= 0.0) || (fitAcc + dE <= lahcHist[lahcIdx]);
            if (acc) fitAcc += dE;
            lahcHist[lahcIdx] = fitAcc;
            lahcIdx = (lahcIdx + 1) % lahcHist.size();
            return acc;
        }
        if (dE <= 0.0) return true;         // Metropolis (default)
        double prob = exp(-dE / max(1e-9, currentTemp));
        return uniform_real_distribution<double>(0.0, 1.0)(rng) < prob;
    }

    // ----- coupled two-vertex swap move ------------------------------
    // When a Gaussian proposal lands on a point occupied by another vertex,
    // the plain SA walk discards it (and still pays the cooling step). On a
    // crowded canvas (Automatic-8 ~10% occupancy) that wastes ~10% of all
    // proposals. Instead, swap v1 and the occupant v2. The swap is realised
    // as three EXACT single-vertex moves through a free temp point T, so it
    // reuses planMove/commitMove verbatim and keeps every incremental
    // structure (xs, xc, kVal, totalX, grids, occupied) in sync. The net
    // crossing/k delta is read off totalX/kVal; on reject or invalidity the
    // swap (its own inverse) is replayed to restore the exact prior state.
    // Returns true iff the swap was attempted (accepted or cleanly rolled
    // back); false iff no free temp point was available (caller falls back
    // to discarding, as before).
    bool attemptSwap(int v1, int v2, int phase, double currentTemp,
                     MovePlan& plan) {
        Pt P1 = pos[v1], P2 = pos[v2];
        // Find a free temp point: jitter around P1, then a few wider tries.
        Pt T{0, 0};
        bool gotT = false;
        for (int t = 0; t < 16 && !gotT; t++) {
            ll rx = (ll)(uniform_int_distribution<int>(-8, 8)(rng));
            ll ry = (ll)(uniform_int_distribution<int>(-8, 8)(rng));
            Pt cand{P1.x + rx, P1.y + ry};
            if (cand.x < ox) cand.x = ox; if (cand.x > ox + W) cand.x = ox + W;
            if (cand.y < oy) cand.y = oy; if (cand.y > oy + H) cand.y = oy + H;
            if (cand == P1 || cand == P2) continue;
            if (occupied.find(cand) == occupied.end()) { T = cand; gotT = true; }
        }
        if (!gotT) return false;

        ll  totalX0 = totalX;
        int kVal0   = kVal;

        // Forward swap: v1: P1->T, v2: P2->P1, v1: T->P2.
        planMove(v1, T,  plan); commitMove(plan);
        planMove(v2, P1, plan); commitMove(plan);
        planMove(v1, P2, plan); commitMove(plan);

        ll  dCross   = totalX - totalX0;
        int dGlobalK = kVal - kVal0;

        bool valid = !wouldCauseVertexEdgeOverlapFast(v1, P2) &&
                     !wouldCauseVertexEdgeOverlapFast(v2, P1);

        double dE;
        if (phase == 1)        dE = (double)dCross;
        else if (dGlobalK != 0) dE = (double)dGlobalK;
        else                    dE = (double)dCross /
                                     max(1.0, (double)max<ll>(1, totalX));

        bool acc = valid && acceptByRule(dE, currentTemp);
        if (acc) {
            if (kVal < bestK || (kVal == bestK && totalX < bestX)) saveBest();
            return true;
        }
        // Reject / invalid: replay the swap to restore the exact prior state.
        planMove(v1, T,  plan); commitMove(plan);
        planMove(v2, P2, plan); commitMove(plan);
        planMove(v1, P1, plan); commitMove(plan);
        return true;
    }

    // ----- crossing-informed candidate generation ---------------------
    // The best-of-C benefit analysis is only as good as its hypotheses:
    // C iid Gaussian draws waste planMove budget re-testing the same blind
    // direction. With candsMix on, each candidate slot tests a DIFFERENT
    // crossing-derived hypothesis; the exact planMove evaluation then picks
    // the winner. Slots:
    //   0  Gaussian walk (exploration baseline; global-jump rules apply)
    //   1  hot-edge shrink — pull v toward the mate of its most-crossed
    //      incident edge; a shorter edge sweeps less area
    //   2  random-subset neighbour centroid (local density hypothesis)
    //   3  partner-side reflection — mirror v across the line of the hottest
    //      crossing partner of v's hottest edge: the side switch geometrically
    //      removes that crossing (planMove verifies the collateral)
    bool candsMix = false;

    int hottestIncident(int v) const {
        const auto& inc = nodeEdges[v];
        if (inc.empty()) return -1;
        int e = inc[0];
        for (int e2 : inc) if (xc[e2] > xc[e]) e = e2;
        return e;
    }

    Pt jitterClamp(double tx, double ty, double sig) {
        normal_distribution<double> nd(0.0, max(1.0, sig));
        ll nx = (ll)llround(tx + nd(rng));
        ll ny = (ll)llround(ty + nd(rng));
        if (nx < ox) nx = ox; if (nx > ox + W) nx = ox + W;
        if (ny < oy) ny = oy; if (ny > oy + H) ny = oy + H;
        return {nx, ny};
    }

    // Snap a proposal to the current annealed grid: cell centre plus a small
    // jitter (grid-aligned points would otherwise pile onto shared lines and
    // trip the vertex-on-edge validity check). No-op once cells are fine.
    Pt gridSnap(Pt p) {
        ll gs = gridStep;
        if (!gridAnneal || gs <= 2) return p;
        ll half = gs / 2, jr = max<ll>(1, gs / 4);
        ll x = ox + ((p.x - ox) / gs) * gs + half
                 + (ll)(rng() % (uint64_t)jr) - jr / 2;
        ll y = oy + ((p.y - oy) / gs) * gs + half
                 + (ll)(rng() % (uint64_t)jr) - jr / 2;
        if (x < ox) x = ox; if (x > ox + W) x = ox + W;
        if (y < oy) y = oy; if (y > oy + H) y = oy + H;
        return {x, y};
    }

    // Reflect v across the line of edge f (+ jitter); ok=false when f is
    // degenerate. Shared by the two reflection slots in proposeCandidate.
    Pt reflectAcross(int v, int f, double sigma, bool& ok) {
        ok = false;
        const Pt& a = pos[edges[f].u];
        const Pt& b = pos[edges[f].v];
        double abx = (double)(b.x - a.x), aby = (double)(b.y - a.y);
        double len2 = abx * abx + aby * aby;
        if (len2 < 1.0) return pos[v];
        double apx = (double)(pos[v].x - a.x);
        double apy = (double)(pos[v].y - a.y);
        double t   = (apx * abx + apy * aby) / len2;
        double fx  = a.x + t * abx, fy = a.y + t * aby;
        ok = true;
        return jitterClamp(2.0 * fx - pos[v].x, 2.0 * fy - pos[v].y,
                           sigma * 0.25);
    }

    Pt proposeCandidate(int v, int slot, double T, double initT) {
        double scale = sqrt((double)max<ll>(1, W) * (double)max<ll>(1, H));
        double tFrac = (initT > 0) ? T / initT : 1.0;
        if (tFrac < 0.01) tFrac = 0.01;
        if (tFrac > 1.0)  tFrac = 1.0;
        double sigma = max(1.0, scale * (0.005 + 0.05 * tFrac));

        int e = (slot == 1 || slot == 3) ? hottestIncident(v) : -1;
        switch (slot & 3) {
            case 0: {                       // drift walk (--slot0 drift)
                // Move-log calibrated replacement for the blind Gaussian:
                // improving accepts are LONG (median ~0.008*scale, p90
                // ~0.04*scale), head toward the neighbour centroid (57-61%)
                // and slightly inward. Propose exactly that: log-normal
                // distance along the centroid direction (occasionally the
                // layout core) with angular noise; the frozen-phase sigma
                // survives only as the jitter floor.
                if (!slot0Drift) break;     // default: plain selectPlace walk
                double ncx, ncy;
                if (uniform_real_distribution<double>(0, 1)(rng) < 0.2) {
                    ncx = gcx; ncy = gcy;   // inward: layout centre of mass
                } else {
                    nbCentroid(v, ncx, ncy);
                }
                double ddx = ncx - pos[v].x, ddy = ncy - pos[v].y;
                double baseAng = (ddx * ddx + ddy * ddy > 1.0)
                    ? atan2(ddy, ddx)
                    : uniform_real_distribution<double>(0, 2 * M_PI)(rng);
                double ang = baseAng +
                    normal_distribution<double>(0.0, 0.6)(rng);
                normal_distribution<double> lnd(log(0.008 * scale), 1.1);
                double d = exp(lnd(rng));
                return jitterClamp(pos[v].x + d * cos(ang),
                                   pos[v].y + d * sin(ang),
                                   max(2.0, sigma * 0.1));
            }
            case 1: {                       // hot-edge shrink toward the mate
                if (e < 0) break;
                int u = (edges[e].u == v) ? edges[e].v : edges[e].u;
                double t = 0.25 + 0.5 *
                    uniform_real_distribution<double>(0, 1)(rng);
                double tx = pos[v].x + t * (double)(pos[u].x - pos[v].x);
                double ty = pos[v].y + t * (double)(pos[u].y - pos[v].y);
                return jitterClamp(tx, ty, sigma * 0.25);
            }
            case 2: {                       // centroid OR random-partner reflect
                // 50/50: random-subset neighbour centroid (arm-forming pull)
                // or reflection across a RANDOM crossing partner (diversity
                // for the top ΔlocalK move type — see slot 3's note).
                if (uniform_int_distribution<int>(0, 1)(rng) == 0) {
                    int eh = hottestIncident(v);
                    if (eh >= 0 && xs[eh].size() > 0) {
                        int pick = uniform_int_distribution<int>(
                            0, (int)xs[eh].size() - 1)(rng);
                        int f = *(xs[eh].begin() + pick);
                        bool ok = false;
                        Pt p = reflectAcross(v, f, sigma, ok);
                        if (ok) return p;
                    }
                }
                const auto& inc = nodeEdges[v];
                if (inc.size() < 2) break;
                int deg = (int)inc.size();
                int cnt = uniform_int_distribution<int>(2, deg)(rng);
                double sx = 0, sy = 0;
                for (int t = 0; t < cnt; t++) {
                    int e2 = inc[uniform_int_distribution<int>(0, deg - 1)(rng)];
                    int u  = (edges[e2].u == v) ? edges[e2].v : edges[e2].u;
                    sx += (double)pos[u].x; sy += (double)pos[u].y;
                }
                return jitterClamp(sx / cnt, sy / cnt, sigma * 0.35);
            }
            case 3: {                       // partner-side reflection
                // Move-log analysis (2026-07, 05/06/08 warm+cold): reflection
                // has the highest share of localK-lowering accepts of all
                // slots (9-17% vs gauss 3-11%), so it earns two flavours:
                // slot 3 reflects across the HOTTEST crossing partner, and
                // slot 2 (below) alternates centroid with a RANDOM partner.
                if (e < 0 || xs[e].size() == 0) break;
                int f = -1;
                for (int f2 : xs[e]) if (f < 0 || xc[f2] > xc[f]) f = f2;
                if (f < 0) break;
                bool ok = false;
                Pt p = reflectAcross(v, f, sigma, ok);
                if (ok) return p;
                break;
            }
        }
        return selectPlace(v, T, initT, /*localOnly*/ true);
    }

    // ----- coupled edge-translation move ------------------------------
    // Single-vertex moves cannot rescue a bottleneck edge whose BOTH
    // endpoints sit in locally-good positions while the edge itself sweeps
    // a congested region — any one-endpoint move first makes things worse,
    // so the walk never finds the two-step escape. Translating both
    // endpoints by the same delta moves the edge as a rigid segment.
    // Realised as two exact single-vertex moves (planMove/commitMove keep
    // every incremental structure in sync — the attemptSwap pattern); on
    // reject or invalidity the translation is replayed in reverse.
    // Returns true iff the move was attempted.
    bool attemptEdgeMove(int e, double sigma, int phase, double currentTemp,
                         MovePlan& plan) {
        int u = edges[e].u, v = edges[e].v;
        Pt U = pos[u], V = pos[v];
        normal_distribution<double> nd(0.0, sigma);
        ll dx = (ll)llround(nd(rng));
        ll dy = (ll)llround(nd(rng));
        if (dx == 0 && dy == 0) dx = 1;
        auto shift = [&](const Pt& p) {
            Pt q{p.x + dx, p.y + dy};
            if (q.x < ox) q.x = ox; if (q.x > ox + W) q.x = ox + W;
            if (q.y < oy) q.y = oy; if (q.y > oy + H) q.y = oy + H;
            return q;
        };
        Pt NU = shift(U), NV = shift(V);
        if ((NU == U && NV == V) || NU == NV) return false;
        {   // both landing spots must be free (of anyone but u/v themselves)
            auto itu = occupied.find(NU);
            if (itu != occupied.end() && itu->second != u && itu->second != v)
                return false;
            auto itv = occupied.find(NV);
            if (itv != occupied.end() && itv->second != u && itv->second != v)
                return false;
        }

        ll  totalX0 = totalX;
        int kVal0   = kVal;

        planMove(u, NU, plan); commitMove(plan);
        planMove(v, NV, plan); commitMove(plan);

        ll  dCross   = totalX - totalX0;
        int dGlobalK = kVal - kVal0;

        bool valid = !wouldCauseVertexEdgeOverlapFast(u, NU) &&
                     !wouldCauseVertexEdgeOverlapFast(v, NV);

        double dE;
        if (phase == 1)         dE = (double)dCross;
        else if (dGlobalK != 0) dE = (double)dGlobalK;
        else                    dE = (double)dCross /
                                     max(1.0, (double)max<ll>(1, totalX));

        bool acc = valid && acceptByRule(dE, currentTemp);
        if (moveLog && phase == 2)
            moveLogRecord(u, /*slot*/9, acc, dGlobalK, dCross, U, NU);
        if (acc) {
            if (kVal < bestK || (kVal == bestK && totalX < bestX)) saveBest();
            return true;
        }
        planMove(v, V, plan); commitMove(plan);
        planMove(u, U, plan); commitMove(plan);
        return true;
    }

    // ----- coordinated two-endpoint move -------------------------------
    // Successor to the rigid edge translation (which the move-log showed at
    // ~0% localK-lowering): near the plateau, single-endpoint moves on a
    // bottleneck edge are often mutually blocked — each endpoint's best move
    // only pays if the OTHER endpoint moves too. So: best-of-C plan endpoint
    // u (own fitness), tentatively commit; best-of-C plan endpoint v; judge
    // the JOINT delta off the global counters (attemptEdgeMove pattern) and
    // revert both on reject/invalidity. Costs up to ~2C planMoves per
    // attempt; runs on the edgeMoveP budget when pairMove is set.
    bool attemptPairMove(int e, int phase, double currentTemp, MovePlan& plan) {
        int u = edges[e].u, v = edges[e].v;
        Pt U = pos[u], V = pos[v];
        ll  totalX0 = totalX;
        int kVal0   = kVal;

        auto bestFor = [&](int w, Pt& out) -> bool {
            bool found = false;
            double bestE = 0;
            for (int c = 0; c < max(2, candsP2); c++) {
                Pt p = candsMix ? proposeCandidate(w, c, currentTemp, curInitT)
                                : selectPlace(w, currentTemp, curInitT, true);
                if (p == pos[w]) continue;
                auto it = occupied.find(p);
                if (it != occupied.end() && it->second != w) continue;
                planMove(w, p, plan);
                double fe = (phase == 1)
                    ? (double)plan.dCross
                    : (double)(plan.newLocalK - plan.oldLocalK) +
                      (double)plan.dCross /
                          max(1.0, (double)max<ll>(1, totalX));
                if (!found || fe < bestE) { found = true; bestE = fe; out = p; }
            }
            return found;
        };

        Pt NU, NV;
        if (!bestFor(u, NU)) return false;
        planMove(u, NU, plan); commitMove(plan);
        bool haveV = bestFor(v, NV);
        if (haveV) { planMove(v, NV, plan); commitMove(plan); }

        ll  dCross   = totalX - totalX0;
        int dGlobalK = kVal - kVal0;
        bool valid = !wouldCauseVertexEdgeOverlapFast(u, pos[u]) &&
                     !wouldCauseVertexEdgeOverlapFast(v, pos[v]);

        double dE;
        if (phase == 1)         dE = (double)dCross;
        else if (dGlobalK != 0) dE = (double)dGlobalK;
        else                    dE = (double)dCross /
                                     max(1.0, (double)max<ll>(1, totalX));

        bool acc = valid && acceptByRule(dE, currentTemp);
        if (moveLog && phase == 2)
            moveLogRecord(u, /*slot*/8, acc, dGlobalK, dCross, U, pos[u]);
        if (acc) {
            if (kVal < bestK || (kVal == bestK && totalX < bestX)) saveBest();
            return true;
        }
        if (haveV) { planMove(v, V, plan); commitMove(plan); }
        planMove(u, U, plan); commitMove(plan);
        return true;
    }

    // ----- deterministic level-clearing sweep --------------------------
    // Vertex-movement primitive (Radermacher et al., JEA 2019), modernised
    // 2026-07-11 with the move-log findings: at the plateau, k drops only
    // when EVERY edge at the bottleneck level loses a crossing, and the
    // moves that do that are reflections and long targeted jumps — not the
    // blind gauss+uniform samples this pass used to draw. For each endpoint
    // of every edge at level >= kVal-1:
    //   - reflect across EACH crossing partner's line (deterministically
    //     flips that pair's side; planMove prices the collateral),
    //   - neighbour-centroid pulls (half and full),
    //   - a few long log-normal jumps (calibrated to the improving-move
    //     length distribution: median ~0.008*scale).
    // Scored lexicographically (dLocalK, then bottleneck-level count dTop,
    // then dCross); only strictly-improving commits, so k cannot rise.
    // Returns number of committed moves.
    int kRepairPass(int maxEdges, int candsPerNode) {
        vector<int> top;
        for (int e = 0; e < m; e++) if (xc[e] >= kVal - 1) top.push_back(e);
        shuffle(top.begin(), top.end(), rng);
        if ((int)top.size() > maxEdges) top.resize(maxEdges);

        MovePlan plan;
        int committed = 0;
        double scale = sqrt((double)max<ll>(1, W) * (double)max<ll>(1, H));

        struct Cand { int lk; int dtop; ll dx; Pt p; };
        vector<Pt>   cands;
        vector<Cand> ranked;

        for (int e : top) {
            if (xc[e] < kVal - 1) continue;     // may have improved already
            for (int v : {edges[e].u, edges[e].v}) {
                Pt curPos = pos[v];
                cands.clear();
                // (a) reflection across every crossing partner of e
                for (int f : xs[e]) {
                    bool ok = false;
                    Pt p = reflectAcross(v, f, scale * 0.004, ok);
                    if (ok) cands.push_back(p);
                }
                // (b) neighbour-centroid pulls
                double ncx, ncy;
                nbCentroid(v, ncx, ncy);
                for (double t : {0.5, 1.0})
                    cands.push_back(jitterClamp(
                        curPos.x + t * (ncx - curPos.x),
                        curPos.y + t * (ncy - curPos.y), scale * 0.008));
                // (c) long log-normal jumps toward random directions
                int extra = min(8, max(2, candsPerNode - (int)cands.size()));
                normal_distribution<double> lnd(log(0.008 * scale), 0.9);
                for (int j = 0; j < extra; j++) {
                    double d  = exp(lnd(rng));
                    double an = uniform_real_distribution<double>(
                                    0, 2 * M_PI)(rng);
                    cands.push_back(jitterClamp(curPos.x + d * cos(an),
                                                curPos.y + d * sin(an), 1.0));
                }

                // exact evaluation; keep a small ranked list so the deferred
                // validity check is paid only for actual winners
                ranked.clear();
                int oldLK = -1;
                for (const Pt& p : cands) {
                    if (p == curPos) continue;
                    auto it = occupied.find(p);
                    if (it != occupied.end() && it->second != v) continue;
                    planMove(v, p, plan);
                    oldLK = plan.oldLocalK;
                    int dtop = 0;
                    for (auto& ec : plan.edgeCounts) {
                        int oldC = std::get<1>(ec), newC = std::get<2>(ec);
                        dtop += (int)(newC >= kVal) - (int)(oldC >= kVal);
                    }
                    Cand c{plan.newLocalK, dtop, plan.dCross, p};
                    auto worse = [](const Cand& a, const Cand& b) {
                        if (a.lk   != b.lk)   return a.lk   > b.lk;
                        if (a.dtop != b.dtop) return a.dtop > b.dtop;
                        return a.dx > b.dx;
                    };
                    ranked.push_back(c);
                    for (size_t i = ranked.size() - 1;
                         i > 0 && worse(ranked[i - 1], ranked[i]); i--)
                        std::swap(ranked[i - 1], ranked[i]);
                    if (ranked.size() > 3) ranked.pop_back();
                }
                if (oldLK < 0) continue;
                for (const Cand& c : ranked) {
                    bool improving =
                        c.lk < oldLK ||
                        (c.lk == oldLK && (c.dtop < 0 ||
                                           (c.dtop == 0 && c.dx < 0)));
                    if (!improving) break;      // ranked: rest are no better
                    if (wouldCauseVertexEdgeOverlapFast(v, c.p)) continue;
                    planMove(v, c.p, plan);
                    commitMove(plan);
                    committed++;
                    if (kVal < bestK || (kVal == bestK && totalX < bestX))
                        saveBest();
                    break;
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
        if (phase == 2) { moveLogOpen(); recomputeCenter(); }

        auto elapsed = [&] {
            return duration_cast<duration<double>>(
                steady_clock::now() - phaseStartedAt).count();
        };

        long long moves = 0, accepts = 0;
        double startingTemp = initT;

        // Grid-anneal schedule: start at ~min(W,H)/64 (power of two), halve
        // once per equal slice of the phase budget until cell size 1.
        ll  gaStep0  = 1;
        int gaLevels = 1;
        if (gridAnneal) {
            ll target = max<ll>(4, min(W, H) / 64);
            while (gaStep0 < target) { gaStep0 <<= 1; gaLevels++; }
        }
        gridStep = gaStep0;
        double repairSpent  = 0.0;
        int    lastBestK    = bestK;   // reheat bookkeeping
        int    staleWaves   = 0;

        // LAHC: fixed-length history of the cumulative-cost trajectory. A move
        // is accepted if it does not worsen the cost relative to the value the
        // walk held L steps ago, which tolerates controlled worsening without
        // an explicit temperature. Reset per phase from the current cost (0).
        if (acceptMode == 2) {
            const size_t L = 5000;
            fitAcc = 0.0;
            lahcIdx = 0;
            lahcHist.assign(L, 0.0);
        }

        cerr << "[phase " << phase << "] start  initT=" << initT
             << " decT=" << decT << " decTW=" << decTW << " tLim=" << tLim
             << "  budget=" << timeLimitSec << "s"
             << "  initial k=" << kVal << " totalX=" << totalX << "\n";

        MovePlan plan, planBest;
        plan.pairChanges.reserve(256);
        plan.edgeCounts .reserve(256);
        planBest.pairChanges.reserve(256);
        planBest.edgeCounts .reserve(256);

        // Phase fitness of a planned move (shared by the single-proposal path
        // and the best-of-C benefit analysis below).
        auto fitOf = [&](const MovePlan& pl) -> double {
            if (phase == 1) return (double)pl.dCross;
            if (fitSq) {
                // Squared-crossings fitness: dE = sum(newC^2 - oldC^2),
                // normalised so that +-1 crossing on a bottleneck-level
                // edge costs ~1 (comparable to the dLocalK unit below).
                double dsq = 0.0;
                for (auto& ec : pl.edgeCounts) {
                    double oldC = (double)std::get<1>(ec);
                    double newC = (double)std::get<2>(ec);
                    dsq += newC * newC - oldC * oldC;
                }
                return dsq / max(1.0, 2.0 * (double)kVal);
            }
            int dLocalK = pl.newLocalK - pl.oldLocalK;
            if (dLocalK != 0) return (double)dLocalK;
            // Lexicographic middle objective: before k itself can drop,
            // every edge sitting at the bottleneck level k must lose a
            // crossing. Pricing one of the cntPerK[k] top-level edges at
            // 1/cntPerK[k] makes clearing the whole level worth ~1 unit of k.
            int dTop = 0;
            if (lexK) {
                for (auto& ec : pl.edgeCounts) {
                    int oldC = std::get<1>(ec);
                    int newC = std::get<2>(ec);
                    dTop += (int)(newC >= kVal) - (int)(oldC >= kVal);
                }
            }
            if (dTop != 0) return (double)dTop / max(1, cntPerK[kVal]);
            if (fitSq2) {
                // k-neutral tie-break by squared-crossings delta: among moves
                // that don't touch the bottleneck, prefer ones that unload
                // high-crossing edges.
                double dsq = 0.0;
                for (auto& ec : pl.edgeCounts) {
                    double oldC = (double)std::get<1>(ec);
                    double newC = (double)std::get<2>(ec);
                    dsq += newC * newC - oldC * oldC;
                }
                return dsq / max(1.0, 2.0 * (double)kVal *
                                      (double)max<ll>(1, totalX));
            }
            return (double)pl.dCross / max(1.0, (double)max<ll>(1, totalX));
        };

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

        // Budget-fill reheat ceiling. A cooling-only schedule freezes when the
        // wave temperature reaches tLim, which on many graphs happens long
        // before the wall-clock budget is spent (e.g. a 10-min Automatic-8 run
        // freezing after ~80s). Rather than idle away the remainder, once the
        // schedule hits the floor we restore the incumbent and reheat to this
        // ceiling, which decays toward a gentle floor so successive restarts
        // anneal into a fine descent instead of the too-hot full-initT reheat
        // that lost the earlier A/B. Best is preserved across reheats, so the
        // result is always >= the cooling-only behaviour.
        double reheatCeil = initT;
        while (kVal > 0 && elapsed() < timeLimitSec) {
        while (kVal > 0 && startingTemp > tLim && elapsed() < timeLimitSec) {
            double currentTemp = startingTemp;
            while (kVal > 0 && currentTemp > tLim && elapsed() < timeLimitSec) {
                int v = selectNode();

                if (gridAnneal) {
                    int shift = (int)(gaLevels *
                                      (elapsed() / max(1e-9, timeLimitSec)));
                    if (shift >= gaLevels) shift = gaLevels - 1;
                    gridStep = max<ll>(1, gaStep0 >> shift);
                }

                // Coupled edge translation: selectNode is already biased to
                // bottleneck vertices; ride that bias and shift v's hottest
                // incident edge as a rigid segment.
                if (phase == 2 && edgeMoveP > 0 && !nodeEdges[v].empty() &&
                    (int)(rng() % 100) < edgeMoveP) {
                    int eHot = nodeEdges[v][0];
                    for (int e : nodeEdges[v])
                        if (xc[e] > xc[eHot]) eHot = e;
                    if (pairMove) {
                        attemptPairMove(eHot, phase, currentTemp, plan);
                    } else {
                        double scale = sqrt((double)max<ll>(1, W) *
                                            (double)max<ll>(1, H));
                        double tFrac = (initT > 0) ? currentTemp / initT : 1.0;
                        if (tFrac < 0.01) tFrac = 0.01;
                        if (tFrac > 1.0)  tFrac = 1.0;
                        double sigma = max(1.0,
                                           scale * (0.005 + 0.05 * tFrac));
                        attemptEdgeMove(eHot, sigma, phase, currentTemp, plan);
                    }
                    moves++;
                    currentTemp *= decT;
                    continue;
                }

                int C = (phase == 2) ? candsP2 : candsP1;
                if (candsRamp && C > 1) {
                    double fr = elapsed() / max(1e-9, timeLimitSec);
                    if      (fr < 0.30) C = 1;
                    else if (fr < 0.60) C = (C + 1) / 2;
                }

                MovePlan* act = nullptr;   // the plan fed to the accept rule
                double dE = 0.0;
                int actSlot = -1;          // instrumentation: winning proposal slot

                if (C <= 1) {
                    Pt newPos = gridSnap(selectPlace(v, currentTemp, initT,
                                                     phase == 2));
                    if (newPos == pos[v]) { currentTemp *= decT; continue; }
                    // Forbid two distinct nodes sharing the same point.
                    {
                        auto it = occupied.find(newPos);
                        if (it != occupied.end() && it->second != v) {
                            // Occupied: optionally turn the otherwise-wasted
                            // proposal into a coupled swap with the occupant.
                            if (swapMove)
                                attemptSwap(v, it->second, phase, currentTemp, plan);
                            moves++;
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
                    dE  = fitOf(plan);
                    act = &plan;
                } else {
                    // Benefit analysis: plan C candidate positions exactly and
                    // keep only the best dE. Each move costs C planMove()s but
                    // lands far more often — the acceptance rule then gates the
                    // *best* available local move, not a blind sample. The
                    // vertex-edge validity check is deferred to the winner so
                    // its cost is paid once per move, not per candidate.
                    bool found = false;
                    for (int c = 0; c < C; c++) {
                        int slotC = (bandit && candsMix && phase == 2)
                                        ? banditSlot() : c;
                        Pt p = gridSnap((candsMix && phase == 2)
                                   ? proposeCandidate(v, slotC, currentTemp, initT)
                                   : selectPlace(v, currentTemp, initT, phase == 2));
                        if (p == pos[v]) continue;
                        auto it = occupied.find(p);
                        if (it != occupied.end() && it->second != v) continue;
                        planMove(v, p, plan);
                        double e = fitOf(plan);
                        if (!found || e < dE) {
                            found = true; dE = e;
                            actSlot = (candsMix && phase == 2) ? slotC : -1;
                            std::swap(plan, planBest);
                        }
                    }
                    if (!found) { moves++; currentTemp *= decT; continue; }
                    if (wouldCauseVertexEdgeOverlapFast(v, planBest.newPos)) {
                        moves++; currentTemp *= decT; continue;
                    }
                    act = &planBest;
                }

                bool acc = acceptByRule(dE, currentTemp);

                if (phase == 2 && moveLog)
                    moveLogRecord(v, actSlot, acc,
                                  act->newLocalK - act->oldLocalK,
                                  act->dCross, act->oldPos, act->newPos);

                if (acc) {
                    if (bandit && actSlot >= 0 &&
                        act->newLocalK < act->oldLocalK)
                        slotCredit[actSlot & 3] += 1.0;
                    commitMove(*act);
                    accepts++;
                    if (kVal < bestK ||
                        (kVal == bestK && totalX < bestX)) {
                        saveBest();
                    }
                }
                moves++;
                if (bandit && (++banditTick & 4095) == 0)
                    for (double& cscore : slotCredit) cscore *= 0.5;
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
        // Wave schedule reached the temperature floor. If wall-clock budget is
        // left, reheat from the best and run another (cooler) descent; the
        // outer loop only exits when the time budget or k=0 is reached.
        if (kVal > 0 && startingTemp <= tLim && elapsed() < timeLimitSec) {
            restoreBest();
            // Level-clearing sweep at the stagnation point: the walk is
            // frozen and restored to the incumbent — exactly when the
            // deterministic bottleneck sweep is cheapest to run and most
            // likely to clear the last edges pinning the current k. Capped
            // at ~10% of the phase's elapsed time (like inter-wave repair).
            if (phase == 2 && levelClear &&
                repairSpent < 0.10 * max(1.0, elapsed())) {
                double rt0 = elapsed();
                int c = kRepairPass(/*maxEdges*/64, /*candsPerNode*/8);
                repairSpent += elapsed() - rt0;
                if (c > 0)
                    cerr << "  [phase 2] level-clear: " << c
                         << " moves, k=" << kVal << " bestK=" << bestK << "\n";
            }
            reheatCeil   = max(tLim * 8.0, reheatCeil * 0.5);
            startingTemp = reheatCeil;
            staleWaves   = 0;
            cerr << "  [phase " << phase << "] budget-reheat sT=" << startingTemp
                 << " at t=" << (int)elapsed() << "s bestK=" << bestK
                 << " bestX=" << bestX << "\n";
        }
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
        "  --cands N     phase-2 best-of-N benefit analysis: plan N candidate\n"
        "                positions per move, accept-test only the best (default: 1)\n"
        "  --cands1 N    same for phase 1 (default: 1)\n"
        "  --cands-ramp 0|1  ramp C up over the phase budget: 1 -> C/2 -> C at\n"
        "                30%%/60%% elapsed — volume early, quality late (default: 0)\n"
        "  --place MODE  proposal: gauss|cong|bary|smart (default: gauss;\n"
        "                smart = neighbour-informed mixture: random-subset centroid,\n"
        "                near-neighbour, two-neighbour midpoint)\n"
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
    string placeArg  = "gauss"; // proposal: gauss | cong (congestion-aware) | bary (barycenter-pull) | smart (neighbour-informed mixture)
    int    candsArg  = 1;     // phase-2 candidates per move (best-of-C benefit analysis)
    int    cands1Arg = 1;     // phase-1 candidates per move
    int    candsRampArg = 0;  // 1 = ramp C up over the phase budget (volume->quality)
    int    edgeMoveArg  = 0;  // phase-2 % chance of a coupled edge-translation move
    int    candsMixArg  = 0;  // 1 = crossing-informed candidate slots in best-of-C
    string moveLogArg;        // path: phase-2 move benefit/harm instrumentation
    int    gridAnnealArg = 0; // 1 = coarse-to-fine proposal quantisation
    int    levelClearArg = 0; // 1 = level-clearing sweep at phase-2 stagnation
    string slot0Arg = "gauss"; // slot-0 proposal: gauss | drift
    int    pairMoveArg = 0;   // 1 = coordinated two-endpoint move on edgeMoveP budget
    int    banditArg   = 0;   // 1 = credit-weighted cands-mix slot sampling
    string acceptArg = "metropolis"; // metropolis | threshold | lahc
    int    swapArg   = 0;     // 1 = enable coupled swap move on occupied hits

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
        else if (a == "--place")             placeArg   = need("--place");
        else if (a == "--cands")             candsArg   = atoi(need("--cands"));
        else if (a == "--cands1")            cands1Arg  = atoi(need("--cands1"));
        else if (a == "--cands-ramp")        candsRampArg = atoi(need("--cands-ramp"));
        else if (a == "--edge-move")         edgeMoveArg  = atoi(need("--edge-move"));
        else if (a == "--cands-mix")         candsMixArg  = atoi(need("--cands-mix"));
        else if (a == "--move-log")          moveLogArg   = need("--move-log");
        else if (a == "--grid-anneal")       gridAnnealArg = atoi(need("--grid-anneal"));
        else if (a == "--level-clear")       levelClearArg = atoi(need("--level-clear"));
        else if (a == "--slot0")             slot0Arg      = need("--slot0");
        else if (a == "--pair-move")         pairMoveArg   = atoi(need("--pair-move"));
        else if (a == "--bandit")            banditArg     = atoi(need("--bandit"));
        else if (a == "--accept")            acceptArg  = need("--accept");
        else if (a == "--swap")              swapArg    = atoi(need("--swap"));
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
    solver.placeMode      = (placeArg == "cong") ? 1 : (placeArg == "bary") ? 2
                          : (placeArg == "smart") ? 3 : 0;
    solver.candsP2        = max(1, candsArg);
    solver.candsP1        = max(1, cands1Arg);
    solver.candsRamp      = (candsRampArg != 0);
    solver.edgeMoveP      = max(0, min(90, edgeMoveArg));
    solver.candsMix       = (candsMixArg != 0);
    solver.moveLogFile    = moveLogArg;
    solver.gridAnneal     = (gridAnnealArg != 0);
    solver.levelClear     = (levelClearArg != 0);
    solver.slot0Drift     = (slot0Arg == "drift");
    solver.pairMove       = (pairMoveArg != 0);
    solver.bandit         = (banditArg != 0);
    solver.acceptMode     = (acceptArg == "threshold") ? 1
                          : (acceptArg == "lahc")      ? 2 : 0;
    solver.swapMove       = (swapArg != 0);
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

    // Snapshot helper: derive a sibling path by replacing the .json suffix.
    auto snapPath = [&](const string& suffix) -> string {
        const string ext = ".json";
        if (outputFile.size() > ext.size() &&
            outputFile.compare(outputFile.size() - ext.size(), ext.size(), ext) == 0)
            return outputFile.substr(0, outputFile.size() - ext.size()) + suffix + ext;
        return outputFile + suffix + ext;
    };

    // Snapshot 0: layout after init/repair, before any SA.
    if (!outputFile.empty())
        writeGraph(snapPath("_snap0_initial"), g, solver.pos);

    // Paper parameters (Table 1):
    //   min cross : initT=50  decT=0.999  decTW=0.99  tLim=0.01
    //   min k     : initT=1   decT=0.9999 decTW=0.99  tLim=0.01
    solver.runSA(/*phase*/1, p1T0,  0.999, 0.99, 0.01, phase1Min * 60.0);

    // Snapshot 1: best layout after phase 1 (minimised total crossings).
    if (!outputFile.empty())
        writeGraph(snapPath("_snap1_phase1"), g, solver.bestPos);

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
