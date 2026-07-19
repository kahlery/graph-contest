// Shared contest-tooling engine: graph/method registry, the run_method /
// run_combo multi-worker launch primitives, log/verify parsing, and
// bests.json bookkeeping. C++ port of the reusable parts of
// scripts/run_contest.py, consumed by both src/runner (batch experiments)
// and src/orchestrator (contest-day adaptive scheduler).
//
// Scope note: the Python original also carried ~15 one-off dated research
// method variants (sa-stress-sq2/pro/cong/swap/lahc/thr, sa-hilbert,
// sa-bary, sa-cong, sa-swap, and "-base" A/B comparison twins) whose source
// comments already record their verdicts ("lost the A/B", "kept only for
// further exploration"). Those aren't ported here — only the methods the
// README documents as the production set: sa, sa-warm, sa-stress, staged,
// staged-adaptive, ils.
#pragma once

#include "json.hpp"
#include "paths.hpp"
#include "subprocess.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace engine {

namespace fsys = std::filesystem;
using Clock = std::chrono::steady_clock;

// --------------------------------------------------------------------- //
// Paths — every binary (solvers included) lives in ./bin, one level below
// the repo root (see Makefile).
// --------------------------------------------------------------------- //
inline const std::string& ROOT() {
    static const std::string r = paths::realpathOf(paths::exeDir() + "/..");
    return r;
}
inline std::string INPUT_DIR() { return ROOT() + "/data/input"; }
inline std::string SAKGD()     { return ROOT() + "/bin/sakgd"; }
inline std::string APPROACH1() { return ROOT() + "/bin/approach1"; }
inline std::string STRESS_INIT() { return ROOT() + "/bin/stress_init"; }
inline std::string TRIPOD_INIT() { return ROOT() + "/bin/tripod_init"; }
inline std::string GRADX_INIT()  { return ROOT() + "/bin/gradx_init"; }

// Mirrors Python pathlib's `root / sub`: an absolute `sub` replaces `root`
// entirely rather than being appended to it.
inline std::string joinPath(const std::string& root, const std::string& sub) {
    if (!sub.empty() && sub[0] == '/') return sub;
    return root + "/" + sub;
}

inline bool fileExists(const std::string& p) { return fsys::exists(p); }
inline void mkdirs(const std::string& p) { fsys::create_directories(p); }
inline void copyFile(const std::string& src, const std::string& dst) {
    fsys::copy_file(src, dst, fsys::copy_options::overwrite_existing);
}

inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
inline bool isAllDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return isdigit(c); });
}
inline std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t p = s.find(',', start);
        std::string part = (p == std::string::npos) ? s.substr(start) : s.substr(start, p - start);
        start = (p == std::string::npos) ? s.size() + 1 : p + 1;
        part = trim(part);
        if (!part.empty()) out.push_back(part);
    }
    return out;
}

// --------------------------------------------------------------------- //
// Input sets: every directory directly under data/input/ is a selectable
// set of graphs to run. Archived suites (data/archive/) are never listed —
// they're reachable only via an explicit --graphs-dir path, not discovered.
// --------------------------------------------------------------------- //
inline std::string pathStem(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

inline std::pair<int,int> graphSize(const std::string& path) {
    auto v = mjson::parseFile(path);
    int n = v.has("nodes") ? (int)v["nodes"].asArray().size() : 0;
    int m = v.has("edges") ? (int)v["edges"].asArray().size() : 0;
    return {n, m};
}

// Degree-skew feature: maxDeg / avgDeg. Scale-free (hub-dominated) graphs
// separate cleanly from everything else on the official set — instance_05
// (BA) scores 11.8 while every other graph is <= 2.33 — so a threshold of
// 4 has a wide safety margin. Used to route hub-skewed graphs to the
// narrow-arm tripod variant (see the orchestrator's tripodFam()).
inline double degreeSkew(const std::string& path) {
    auto v = mjson::parseFile(path);
    if (!v.has("nodes") || !v.has("edges")) return 1.0;
    auto& nodes = v["nodes"].asArray();
    int n = (int)nodes.size();
    if (n == 0) return 1.0;
    std::map<std::string,int> idOf;
    for (int i = 0; i < n; i++) {
        auto& nd = nodes[i];
        std::string key = std::to_string(i);
        if (nd.has("id")) {
            auto& iv = nd.at("id");
            key = iv.isStr() ? iv.asString() : std::to_string(iv.asLL());
        }
        idOf[key] = i;
    }
    std::vector<int> deg(n, 0);
    long long m2 = 0;
    for (auto& ev : v["edges"].asArray()) {
        auto idKey = [](const mjson::Value& iv) {
            return iv.isStr() ? iv.asString() : std::to_string(iv.asLL());
        };
        std::string s, t;
        if      (ev.has("source")) s = idKey(ev.at("source"));
        else if (ev.has("from"))   s = idKey(ev.at("from"));
        if      (ev.has("target")) t = idKey(ev.at("target"));
        else if (ev.has("to"))     t = idKey(ev.at("to"));
        auto its = idOf.find(s), itt = idOf.find(t);
        if (its == idOf.end() || itt == idOf.end() ||
            its->second == itt->second) continue;
        deg[its->second]++; deg[itt->second]++; m2 += 2;
    }
    if (m2 == 0) return 1.0;
    double avg = (double)m2 / n;
    int mx = *std::max_element(deg.begin(), deg.end());
    return mx / avg;
}

// Budget group by graph complexity (n+m), not by a hardcoded graph name/index
// — generalizes to any input set. Thresholds calibrated so the 9 official
// contest graphs land in the same small/medium/large buckets as the
// original hand-picked {1,2,3,4}/{5,6,7,9}/{8} grouping.
inline std::string groupForSize(int n, int m) {
    int complexity = n + m;
    if (complexity < 1000) return "small";
    if (complexity < 8000) return "medium";
    return "large";
}
inline double budgetForSize(int n, int m, const std::map<std::string,double>& minutesMap) {
    return minutesMap.at(groupForSize(n, m));
}

// Every *.json in `dir` that parses as a graph (has a nodes array), as
// (name = file stem, absolute path, n, m). Submissions/results key off the
// original file name, so arbitrary input sets keep their own naming.
struct GraphEntry { std::string name, path; int n = 0, m = 0; };

inline std::vector<GraphEntry> scanGraphDir(const std::string& dir) {
    std::vector<GraphEntry> entries;
    std::vector<fsys::path> files;
    if (fsys::is_directory(dir)) {
        for (auto& e : fsys::directory_iterator(dir))
            if (e.path().extension() == ".json") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
        try {
            auto [n, m] = graphSize(f.string());
            if (n > 0) entries.push_back({f.stem().string(), fsys::absolute(f).string(), n, m});
        } catch (...) {}
    }
    return entries;
}

// Directory names directly under data/input/ (sorted), each one a
// selectable "input set".
inline std::vector<std::string> listInputSets() {
    std::vector<std::string> sets;
    std::string dir = INPUT_DIR();
    if (fsys::is_directory(dir)) {
        for (auto& e : fsys::directory_iterator(dir))
            if (e.is_directory()) sets.push_back(e.path().filename().string());
    }
    std::sort(sets.begin(), sets.end());
    return sets;
}

inline std::string inputSetDir(const std::string& name) { return INPUT_DIR() + "/" + name; }

// Last path component, trailing slashes ignored - used to derive a
// per-input-set output folder name (data/output/<basename>) regardless of
// whether the graphs came from --input-set, --graphs-dir, or the
// interactive prompt (all three ultimately resolve to a directory path).
inline std::string pathBasename(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    size_t slash = p.find_last_of('/');
    return (slash == std::string::npos) ? p : p.substr(slash + 1);
}

// Interactively asks which input set to run (used when a tool is launched
// with neither --input-set nor --graphs-dir). Reads one line from stdin;
// throws on EOF/invalid input rather than hanging — this matters because a
// child spawned with stdin wired to /dev/null (e.g. by the web server) hits
// immediate EOF instead of blocking forever.
inline std::string promptInputSet() {
    auto sets = listInputSets();
    if (sets.empty()) throw std::runtime_error("no input sets found under " + INPUT_DIR());
    fprintf(stderr, "Which input set do you want to run?\n");
    for (size_t i = 0; i < sets.size(); i++) fprintf(stderr, "  %zu) %s\n", i + 1, sets[i].c_str());
    fprintf(stderr, "> ");
    fflush(stderr);
    std::string line;
    if (!std::getline(std::cin, line))
        throw std::runtime_error("no input set selected (stdin closed) - pass --input-set or --graphs-dir");
    line = trim(line);
    char* end = nullptr;
    long idx = std::strtol(line.c_str(), &end, 10);
    if (end == line.c_str() || idx < 1 || (size_t)idx > sets.size())
        throw std::runtime_error("invalid selection: " + line);
    return sets[idx - 1];
}

// Resolves the --input-set/--graphs-dir/interactive-prompt trio to a
// concrete directory of graphs, shared by both run_contest and
// contest_orchestrate so their CLI behaves identically.
inline std::string resolveGraphsDir(const std::string& inputSet, const std::string& graphsDir) {
    if (!graphsDir.empty()) return graphsDir;
    if (!inputSet.empty()) return inputSetDir(inputSet);
    return inputSetDir(promptInputSet());
}

inline void ensureBinaries() {
    if (fileExists(SAKGD()) && fileExists(APPROACH1())) return;
    fprintf(stderr, "[build] running make ...\n");
    std::string cmd = "cd \"" + ROOT() + "\" && make";
    int rc = std::system(cmd.c_str());
    if (rc != 0 || !fileExists(SAKGD()) || !fileExists(APPROACH1())) {
        fprintf(stderr, "[build] FAILED - run `make` manually.\n");
        std::exit(1);
    }
}

// --------------------------------------------------------------------- //
// Number formatting matching the Python helpers exactly.
// --------------------------------------------------------------------- //
inline std::string minsFmt(double x) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.6f", x);
    std::string s(buf);
    size_t last = s.find_last_not_of('0');
    s.erase(last + 1);
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}
inline std::string fmtFixed1(double x) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.1f", x);
    return std::string(buf);
}

// --------------------------------------------------------------------- //
// Log / verify parsing
// --------------------------------------------------------------------- //
struct LogParse {
    std::optional<int> baselineK, baselineTotalX;
    bool baselineOverlap = false;
    std::optional<int> k, totalX;
};

inline LogParse parseLog(const std::string& logPath) {
    LogParse out;
    std::string text;
    try { text = mjson::slurp(logPath); } catch (...) { return out; }
    static const std::regex INITIAL_RE(R"(Initial:\s*k=(\d+)\s+totalX=(\d+)\s+vertexEdgeOverlap=(no|YES))");
    static const std::regex FINAL_RE(R"(Final best:\s*k=(\d+)\s+totalX=(\d+))");
    std::smatch m;
    if (std::regex_search(text, m, INITIAL_RE)) {
        out.baselineK = std::stoi(m[1].str());
        out.baselineTotalX = std::stoi(m[2].str());
        out.baselineOverlap = (m[3].str() == "YES");
    }
    if (std::regex_search(text, m, FINAL_RE)) {
        out.k = std::stoi(m[1].str());
        out.totalX = std::stoi(m[2].str());
    }
    return out;
}

struct VerifyResult { int k = 0; int totalX = 0; bool valid = false; };

inline std::optional<VerifyResult> verifyOutput(const std::string& path) {
    if (!fileExists(path)) return std::nullopt;
    auto res = proc::runCaptured({SAKGD(), "--verify", path}, "", 1800.0);
    if (res.timedOut) return std::nullopt;
    static const std::regex VERIFY_RE(R"(k=(\d+)\s+totalCrossings=(\d+)\s+vertexEdgeOverlap=(no|yes))");
    std::smatch m;
    if (!std::regex_search(res.stdoutData, m, VERIFY_RE)) return std::nullopt;
    VerifyResult v;
    v.k = std::stoi(m[1].str());
    v.totalX = std::stoi(m[2].str());
    v.valid = (m[3].str() == "no");
    return v;
}

inline std::optional<int> verifyK(const std::string& path) {
    auto res = proc::runCaptured({SAKGD(), "--verify", path}, "", 600.0);
    if (res.timedOut) return std::nullopt;
    std::smatch m;
    if (!std::regex_search(res.stdoutData, m, std::regex(R"(k=(\d+))"))) return std::nullopt;
    if (res.stdoutData.find("vertexEdgeOverlap=no") == std::string::npos) return std::nullopt;
    return std::stoi(m[1].str());
}

// --------------------------------------------------------------------- //
// Method registry — see the file-level comment for what was intentionally
// left out relative to the Python original.
// --------------------------------------------------------------------- //
struct Stage {
    std::string bin;      // "sakgd" | "approach1" | "stress" | "tripod" | "gradx"
    std::string mode;     // approach1 --mode value (empty = none / plain sakgd)
    std::string tag;      // multi-stage file/log tag
    std::string frac;     // "full" | "lns" | "init" | "flash" | "rest"
    bool warm = false;    // true: input is the previous stage's output
    std::string initmode; // --init override
    std::vector<std::string> extra;
};

struct MethodSpec {
    std::string id, label;
    int kband = 2;
    std::vector<Stage> stages;
    // Run stage 0 ONCE per combo at full machine width and warm every
    // worker from its output (instead of each worker computing its own
    // starved copy). The exp06 isolation showed init QUALITY, not chain
    // chopping, gates the SA depth: strong-init arms hit 189/207 while
    // in-hour-style starved inits stalled at 214/215.
    bool sharedInit = false;
};

inline const std::vector<MethodSpec>& METHODS() {
    static const std::vector<MethodSpec> m = {
        // "--cands 4": phase-2 best-of-4 benefit analysis — plan 4 candidate
        // positions exactly per move and feed only the best dE to the accept
        // rule. A/B (2026-07, warm 3-min runs on internal-2026 03/05/06/08,
        // seed 555) beat or matched plain SA on every graph: k 11->9, 235->226,
        // 228->224, 82->80, with totalX down 10-25%.
        // Cold starts add "--cands-ramp 1": the early descent (far from any
        // optimum) wants move volume, so C ramps 1 -> C/2 -> C over the phase
        // budget; warm continuations are already in the fine-descent regime
        // where the full best-of-4 pays from the first move.
        // "--edge-move 5": rigid-segment translation of the hottest incident
        // edge. Was 20; the 2026-07 move-log analysis (05/06/08, warm+cold)
        // showed it eating ~20% of evaluations at 1.5-8.5% acceptance and
        // ~0% localK-lowering share, at 2-4 planMove cost per attempt.
        // "--bandit 1": credit-weighted slot sampling (localK-lowering accepts,
        // sliding window). Broke two plateaus in the 2026-07-15 paired A/B:
        // 05 warm 196 -> 192 (then chains to 188), 06 warm 202 -> 194,
        // while control/drift/level-clear stayed flat. 08 indifferent (61).
        // "--cands-mix 1": the 4 candidate slots test distinct crossing-derived
        // hypotheses. Post-analysis composition: gauss walk / hot-edge shrink /
        // (subset centroid | random-partner reflect) / hottest-partner reflect —
        // reflection got double weight as the top ΔlocalK<0 producer (9-17%
        // of its accepts vs 3-11% for the blind walk).
        {"sa", "SA", 2, {
            {"sakgd", "", "", "full", false, "",
             {"--cands", "4", "--cands-ramp", "1", "--edge-move", "5",
              "--cands-mix", "1", "--bandit", "1"}},
        }},
        {"sa-warm", "SA (warm cont.)", 2, {
            {"sakgd", "", "", "full", false, "input",
             {"--cands", "4", "--edge-move", "5", "--cands-mix", "1",
              "--bandit", "1"}},
        }},
        {"sa-stress", "SA (stress init)", 2, {
            {"stress", "", "init", "init", false, "", {}},
            {"sakgd", "", "sa", "rest", true, "",
             {"--cands", "4", "--cands-ramp", "1", "--edge-move", "5",
              "--cands-mix", "1", "--bandit", "1"}},
        }},
        // Tripod: structural init exploiting the winning tripod layout
        // family (see src/tools/tripod_init.cpp). 10-min cold A/B on
        // internal-2026: 05 = 210 vs sa-stress 234 / staged 235; warm chains
        // from tripod layouts set the 05/06 records (196 / 202).
        {"tripod", "SA (tripod init)", 2, {
            {"tripod", "", "init", "flash", false, "", {}},
            {"sakgd", "", "sa", "rest", true, "",
             {"--cands", "4", "--cands-ramp", "1", "--edge-move", "5",
              "--cands-mix", "1", "--bandit", "1"}},
        }},
        // Narrow-arm tripod: the 78-run param sweep found --wedge 0.02,0.06
        // an instance_05-SPECIFIC win (cold 206/208 vs base 218/222); it is
        // neutral on 06 and slightly negative on 08, so the orchestrator maps
        // tripod -> tripod-n for instance_05 only.
        {"tripod-n", "SA (tripod narrow)", 2, {
            {"tripod", "", "init", "flash", false, "", {"--wedge", "0.02,0.06"}},
            {"sakgd", "", "sa", "rest", true, "",
             {"--cands", "4", "--cands-ramp", "1", "--edge-move", "5",
              "--cands-mix", "1", "--bandit", "1"}},
        }},
        // Gradx: gradient descent on the SigmoidX differentiable crossing
        // surrogate (arXiv 2606.31119 sec 3.3) with a local-k softmax
        // curriculum — see src/tools/gradx_init.cpp. Gate A/B (10-min SA
        // from the init): 05 = 183/183 (all-time record, tripod control
        // 201), 06 = 195/196 (control 211). The init stage needs real
        // seconds (frac "init" = 0.08, not "flash"). The SA stage must NOT
        // run phase 1: gradx layouts are already crossing-sparse and the
        // hot totalX anneal wrecks them — initmode "input" keeps the
        // coordinates, and the trailing "-p1 0" overrides the p1Frac the
        // runner injects (sakgd arg parsing is last-wins).
        {"gradx", "SA (gradx init)", 2, {
            // sharedInit: ONE full-width gradx_init per combo (the init's
            // totalX decides how deep the SA lands; eight starved 2-thread
            // copies cost 06 ~25 k-points in the exp06 isolation).
            {"gradx", "", "init", "init", false, "", {}},
            {"sakgd", "", "sa", "rest", true, "input",
             {"--cands", "4", "--edge-move", "5", "--cands-mix", "1",
              "--bandit", "1", "-p1", "0"}},
        }, /*sharedInit=*/true},
        {"staged", "Staged", 2, {
            {"approach1", "lns", "lns", "lns", false, "", {}},
            {"sakgd", "", "sa", "rest", true, "",
             {"--cands", "4", "--edge-move", "5", "--cands-mix", "1",
              "--bandit", "1"}},
        }},
        {"staged-adaptive", "Staged-Adaptive", 2, {
            {"approach1", "lns-adaptive", "lns", "lns", false, "", {}},
            {"sakgd", "", "sa", "rest", true, "", {}},
        }},
        {"ils", "ILS", 2, {
            {"approach1", "ils", "", "full", false, "", {}},
        }},
    };
    return m;
}

inline const MethodSpec* methodById(const std::string& id) {
    for (auto& m : METHODS()) if (m.id == id) return &m;
    return nullptr;
}

inline std::vector<double> resolveFracs(const MethodSpec& spec, double lnsFrac) {
    constexpr double INIT_FRAC = 0.08;
    std::vector<std::optional<double>> fr;
    for (auto& st : spec.stages) {
        if (st.frac == "full")      fr.push_back(1.0);
        else if (st.frac == "lns")  fr.push_back(lnsFrac);
        else if (st.frac == "init") fr.push_back(INIT_FRAC);
        else if (st.frac == "flash") fr.push_back(0.01); // instant init stages
        else                        fr.push_back(std::nullopt);
    }
    double used = 0;
    for (auto& x : fr) if (x.has_value()) used += *x;
    std::vector<double> out;
    for (auto& x : fr) out.push_back(x.has_value() ? *x : (1.0 - used));
    return out;
}

inline std::pair<std::string,std::string> stageFiles(const MethodSpec& spec, size_t idx,
                                                       const std::string& outDir, const std::string& suffix) {
    bool multi = spec.stages.size() > 1;
    bool isLast = idx == spec.stages.size() - 1;
    const std::string& base = spec.id;
    if (!multi) return { outDir + "/" + base + suffix + ".json", outDir + "/" + base + suffix + ".log" };
    std::string tag = spec.stages[idx].tag.empty() ? ("s" + std::to_string(idx)) : spec.stages[idx].tag;
    std::string out = outDir + "/" + (isLast ? (base + suffix + ".json") : (base + "_" + tag + suffix + ".json"));
    std::string log = outDir + "/" + base + "_" + tag + suffix + ".log";
    return {out, log};
}

using RunFn = std::function<std::pair<int,double>(const std::vector<std::string>&, const std::string&)>;

inline std::pair<int,double> defaultRun(const std::vector<std::string>& cmd, const std::string& log) {
    auto r = proc::runLogged(cmd, log, -1.0);
    return {r.exitCode, r.wallSec};
}

struct RunMethodResult { std::string outPath; int rc = 0; double sec = 0; std::string firstLog, finalLog; };

inline RunMethodResult runMethod(const MethodSpec& spec, const std::string& gpath, double totalMin, double p1Frac,
                                  long long seed, const std::string& outDir, const std::string& suffix,
                                  int nhSize, int nhCands, int ilsPerturb, double lnsFrac,
                                  const RunFn& runFn = defaultRun) {
    auto fracs = resolveFracs(spec, lnsFrac);
    int kband = spec.kband;
    std::string prevOut;
    std::vector<std::string> logs;
    int rcFinal = 0;
    double secTotal = 0;
    for (size_t idx = 0; idx < spec.stages.size(); idx++) {
        const Stage& st = spec.stages[idx];
        double stageMin = totalMin * fracs[idx];
        std::string inp = (st.warm && !prevOut.empty() && fileExists(prevOut)) ? prevOut : gpath;
        auto [out, log] = stageFiles(spec, idx, outDir, suffix);
        std::string trace = log.substr(0, log.size() - 4) + ".trace";
        std::vector<std::string> cmd;
        if (st.bin == "stress") {
            cmd = {STRESS_INIT(), "-i", inp, "-o", out, "-s", std::to_string(seed),
                   "-t", fmtFixed1(stageMin * 60.0)};
        } else if (st.bin == "tripod") {
            cmd = {TRIPOD_INIT(), "-i", inp, "-o", out, "-s", std::to_string(seed)};
            for (auto& e : st.extra) cmd.push_back(e);
        } else if (st.bin == "gradx") {
            // gradx honors -t (budget-adaptive epochs + stall early-exit).
            cmd = {GRADX_INIT(), "-i", inp, "-o", out, "-s", std::to_string(seed),
                   "-t", fmtFixed1(stageMin * 60.0)};
            for (auto& e : st.extra) cmd.push_back(e);
        } else {
            std::string binpath = (st.bin == "sakgd") ? SAKGD() : APPROACH1();
            cmd = {binpath, "-i", inp, "-o", out, "-t", minsFmt(stageMin),
                   "-p1", minsFmt(stageMin * p1Frac), "-s", std::to_string(seed),
                   "--trace-file", trace};
            if (!st.mode.empty()) { cmd.push_back("--mode"); cmd.push_back(st.mode); }
            if (st.mode == "lns" || st.mode == "lns-adaptive") {
                cmd.push_back("--nh-size");  cmd.push_back(std::to_string(nhSize));
                cmd.push_back("--nh-cands"); cmd.push_back(std::to_string(nhCands));
            }
            if (st.mode == "ils" && ilsPerturb > 0) {
                cmd.push_back("--ils-perturb"); cmd.push_back(std::to_string(ilsPerturb));
            }
            if (kband != 2) { cmd.push_back("--kband"); cmd.push_back(std::to_string(kband)); }
            if (!st.initmode.empty()) { cmd.push_back("--init"); cmd.push_back(st.initmode); }
            for (auto& e : st.extra) cmd.push_back(e);
        }
        auto [rc, sec] = runFn(cmd, log);
        // Stage wall-vs-budget telemetry (appended to the stage's own log):
        // an overrun here means the child ignored -t, the backstop misfired,
        // or the machine dozed mid-stage (see corch_1783476249 post-mortem).
        {
            FILE* lf = fopen(log.c_str(), "a");
            if (lf) {
                fprintf(lf, "[stage] %s wall=%.1fs budget=%.1fs rc=%d\n",
                        st.bin.c_str(), sec, stageMin * 60.0, rc);
                fclose(lf);
            }
        }
        if (rcFinal == 0) rcFinal = rc;
        secTotal += sec;
        logs.push_back(log);
        prevOut = out;
    }
    return {prevOut, rcFinal, secTotal, logs.front(), logs.back()};
}

// Reusable cyclic barrier for the xchg-rounds worker cooperation protocol.
class Barrier {
public:
    explicit Barrier(int n) : n_(n), count_(n) {}
    void wait() {
        std::unique_lock<std::mutex> lk(m_);
        int g = gen_;
        if (--count_ == 0) {
            count_ = n_;
            gen_++;
            cv_.notify_all();
        } else {
            cv_.wait(lk, [&] { return g != gen_; });
        }
    }
private:
    std::mutex m_;
    std::condition_variable cv_;
    int n_, count_, gen_ = 0;
};

inline void electElite(const std::string& outDir, const std::string& method, int nWorkers,
                        int r, const std::string& elite) {
    std::optional<int> bestK;
    std::string bestP;
    for (int wid = 0; wid < nWorkers; wid++) {
        std::string p = outDir + "/" + method + "_w" + std::to_string(wid) + "_r" + std::to_string(r) + ".json";
        if (!fileExists(p)) continue;
        auto k = verifyK(p);
        if (k.has_value() && (!bestK.has_value() || *k < *bestK)) { bestK = k; bestP = p; }
    }
    if (!bestP.empty()) copyFile(bestP, elite);
}

struct WorkerResult {
    int workerId = 0;
    long long seed = 0;
    std::string outPath;
    int returncode = 0;
    double wallClockSec = 0;
    std::optional<int> initialK, initialTotalX, finalK, finalTotalX;
    bool valid = false;
};

struct ComboResult {
    std::vector<WorkerResult> workers;
    std::optional<WorkerResult> best;
    double wallTotal = 0;
};

// Runs n_workers in parallel with different seeds; returns every worker's
// result plus the best valid one. xchg_rounds > 1 makes workers cooperate:
// the budget splits into R rounds and, after each, every worker warm-starts
// from the shared "elite" (lowest-k) layout so far. half_share keeps the
// low-half worker ids independent (their own previous layout) for
// exploration while the high-half adopt the elite for intensification.
inline ComboResult runCombo(const std::string& method, const std::string& gpath, double totalMin, double p1Frac,
                             long long baseSeed, const std::string& outDir, int nWorkers, int nhSize, int nhCands,
                             double lnsFrac, int ilsPerturb, const std::string& gpathWarm = "",
                             int xchgRounds = 1, bool halfShare = false, const RunFn& runFn = defaultRun) {
    const MethodSpec* specP = methodById(method);
    if (!specP) throw std::runtime_error("unknown method: " + method);
    const MethodSpec& registrySpec = *specP;
    // Shared-init methods run stage 0 once here at full machine width; the
    // workers then all warm from its output and split only the remaining
    // budget. (Cold combos only — warm continuations never carry an init.)
    MethodSpec sharedSpec;
    const MethodSpec* specUse = &registrySpec;
    std::string gpathWarmEff = gpathWarm;
    double totalMinEff = totalMin;
    if (registrySpec.sharedInit && gpathWarm.empty() &&
        registrySpec.stages.size() >= 2) {
        double initMin = totalMin * resolveFracs(registrySpec, lnsFrac)[0];
        const Stage& st0 = registrySpec.stages[0];
        std::string sout = outDir + "/" + method + "_init_shared.json";
        std::string slog = outDir + "/" + method + "_init_shared.log";
        std::vector<std::string> cmd = {GRADX_INIT(), "-i", gpath, "-o", sout,
                                        "-s", std::to_string(baseSeed),
                                        "-t", fmtFixed1(initMin * 60.0)};
        for (auto& e : st0.extra) cmd.push_back(e);
        runFn(cmd, slog);
        if (fileExists(sout)) {
            sharedSpec = registrySpec;
            sharedSpec.stages.erase(sharedSpec.stages.begin());
            specUse = &sharedSpec;
            gpathWarmEff = sout;
            totalMinEff = std::max(0.05, totalMin - initMin);
        }
    }
    const MethodSpec& spec = *specUse;
    totalMin = totalMinEff;
    int kband = spec.kband;
    std::vector<std::string> extra = spec.stages.back().extra;
    std::string elite = outDir + "/" + method + "_elite.json";
    std::unique_ptr<Barrier> barrier;
    if (xchgRounds > 1) barrier = std::make_unique<Barrier>(nWorkers);

    std::vector<WorkerResult> results(nWorkers);

    auto workerFn = [&](int wid) {
        long long seed = baseSeed + wid;
        std::string suffix = "_w" + std::to_string(wid);
        std::string inp = (!gpathWarmEff.empty() && fileExists(gpathWarmEff)) ? gpathWarmEff : gpath;
        auto t0 = Clock::now();

        std::string out, glogPath;
        int rc = 0;
        LogParse first, final_;

        if (xchgRounds <= 1) {
            auto rm = runMethod(spec, inp, totalMin, p1Frac, seed, outDir, suffix,
                                 nhSize, nhCands, ilsPerturb, lnsFrac, runFn);
            out = rm.outPath; rc = rm.rc; glogPath = rm.finalLog;
            first = parseLog(rm.firstLog);
            final_ = parseLog(glogPath);
        } else {
            double rmin = totalMin / xchgRounds;
            bool independent = halfShare && wid < nWorkers / 2;
            for (int r = 0; r < xchgRounds; r++) {
                std::string prevSelf = out;
                if (r == 0) {
                    auto rm = runMethod(spec, inp, rmin, p1Frac, seed, outDir,
                                        suffix + "_r" + std::to_string(r),
                                        nhSize, nhCands, ilsPerturb, lnsFrac, runFn);
                    out = rm.outPath; rc = rm.rc; glogPath = rm.finalLog;
                    first = parseLog(rm.firstLog);
                } else {
                    std::string src = (independent && !prevSelf.empty() && fileExists(prevSelf)) ? prevSelf
                                     : (fileExists(elite) ? elite : inp);
                    out = outDir + "/" + method + suffix + "_r" + std::to_string(r) + ".json";
                    glogPath = outDir + "/" + method + suffix + "_r" + std::to_string(r) + ".log";
                    // p1 0: warm rounds continue the k walk directly — see the
                    // orchestrator's lease() for why phase 1 is skipped warm.
                    std::vector<std::string> cmd = {
                        SAKGD(), "-i", src, "-o", out, "-t", minsFmt(rmin),
                        "-p1", "0", "-s", std::to_string(seed + (long long)r * 1000),
                        "--init", "input"};
                    if (kband != 2) { cmd.push_back("--kband"); cmd.push_back(std::to_string(kband)); }
                    for (auto& e : extra) cmd.push_back(e);
                    auto rr = runFn(cmd, glogPath);
                    rc = rr.first;
                }
                final_ = parseLog(glogPath);
                if (barrier) {
                    barrier->wait();
                    if (wid == 0) electElite(outDir, method, nWorkers, r, elite);
                    barrier->wait();
                }
            }
        }

        double wall = std::chrono::duration<double>(Clock::now() - t0).count();
        WorkerResult wr;
        wr.workerId = wid; wr.seed = seed; wr.outPath = out; wr.returncode = rc;
        wr.wallClockSec = std::round(wall * 10.0) / 10.0;
        wr.initialK = first.baselineK; wr.initialTotalX = first.baselineTotalX;
        wr.finalK = final_.k; wr.finalTotalX = final_.totalX;
        wr.valid = (rc == 0 && fileExists(out) && final_.k.has_value());
        results[wid] = wr;
    };

    auto wallStart = Clock::now();
    std::vector<std::thread> threads;
    threads.reserve(nWorkers);
    for (int w = 0; w < nWorkers; w++) threads.emplace_back(workerFn, w);
    for (auto& t : threads) t.join();
    double wallTotal = std::chrono::duration<double>(Clock::now() - wallStart).count();

    std::optional<WorkerResult> best;
    for (auto& w : results) {
        if (!w.valid) continue;
        if (!best.has_value() || w.finalK.value() < best->finalK.value() ||
            (w.finalK.value() == best->finalK.value() &&
             w.finalTotalX.value_or(0) < best->finalTotalX.value_or(0))) {
            best = w;
        }
    }
    return {results, best, wallTotal};
}

// --------------------------------------------------------------------- //
// bests.json / history bookkeeping
// --------------------------------------------------------------------- //
inline mjson::Value loadJsonDefault(const std::string& path, mjson::Value def) {
    if (!fileExists(path)) return def;
    try { return mjson::parseFile(path); } catch (...) { return def; }
}
inline void saveJson(const std::string& path, const mjson::Value& v) { mjson::writeFile(path, v, 2); }

inline std::string bestKey(const std::string& graph, const std::string& method) { return graph + "__" + method; }

inline bool updateBest(const std::string& outRoot, mjson::Value& bests,
                        const std::string& graph, const std::string& method,
                        std::optional<int> k, std::optional<int> totalX,
                        const std::string& layoutPath, const std::string& runId,
                        std::optional<double> budgetMin = std::nullopt,
                        std::optional<int> nWorkers = std::nullopt,
                        std::optional<double> wallClockSec = std::nullopt,
                        std::optional<int> nodes = std::nullopt,
                        std::optional<int> edges = std::nullopt) {
    if (!k.has_value()) return false;
    std::string key = bestKey(graph, method);
    bool better = true;
    if (bests.has(key)) {
        const auto& cur = bests[key];
        int curK = cur.has("k") ? cur["k"].asInt() : std::numeric_limits<int>::max();
        if (k.value() > curK) better = false;
        else if (k.value() == curK) {
            double curX = (cur.has("totalX") && cur["totalX"].isNum())
                          ? cur["totalX"].asDouble() : std::numeric_limits<double>::infinity();
            better = totalX.has_value() && (double)totalX.value() < curX;
        }
    }
    if (!better) return false;
    std::string bestDir = outRoot + "/best/" + graph;
    mkdirs(bestDir);
    std::string dest = bestDir + "/" + method + ".json";
    if (!layoutPath.empty() && fileExists(layoutPath)) copyFile(layoutPath, dest);
    mjson::Value entry = mjson::Value::makeObject();
    entry["k"] = mjson::Value((long long)k.value());
    entry["totalX"] = totalX.has_value() ? mjson::Value((long long)totalX.value()) : mjson::Value();
    entry["run_id"] = mjson::Value(runId);
    entry["layout_path"] = mjson::Value(dest);
    entry["budget_min"] = budgetMin.has_value() ? mjson::Value(*budgetMin) : mjson::Value();
    entry["n_workers"] = nWorkers.has_value() ? mjson::Value(*nWorkers) : mjson::Value();
    entry["wall_clock_sec"] = wallClockSec.has_value() ? mjson::Value(*wallClockSec) : mjson::Value();
    entry["nodes"] = nodes.has_value() ? mjson::Value((long long)*nodes) : mjson::Value();
    entry["edges"] = edges.has_value() ? mjson::Value((long long)*edges) : mjson::Value();
    bests[key] = entry;
    return true;
}

} // namespace engine
