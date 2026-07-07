// contest_orchestrate - deadline-safe adaptive orchestrator for the internal
// GD k-planarity contest. C++ port of scripts/contest_orchestrate.py.
//
// Given a fixed wall-clock budget (e.g. 40-50 min), it:
//   1. ANALYSES every graph (n, m, density) and picks a cold-start method -
//      sa-stress for sparse graphs (force-directed init wins), plain sa for
//      very dense ones (stress init hurts there).
//   2. EXPLORES: one cold lease per graph (hardest-first) to establish each
//      k and a warm best-so-far layout.
//   3. REALLOCATES greedily: each remaining quantum goes to the graph with
//      the best recent k-drop (tie-broken toward higher current k = more
//      room), warm-started from its best layout. Graphs that stop improving
//      are dropped, banking budget.
//   4. Runs every lease with the validated cooperative config: W workers,
//      --xchg-rounds with half-sharing (half explore independently, half
//      intensify on the shared elite).
//   5. NEVER overruns the budget: a per-subprocess backstop clamps every
//      solver to a hard deadline that reserves time for the final
//      per-graph verify + submission copy.
//
// Output: results/submission/<run_id>/<graph>.json (one best VALID layout
//         per graph) plus bests.json metadata and an orchestration.json
//         report.
//
// Usage:
//   contest_orchestrate --input-set internal-contest --budget 2700 --workers 8
//   contest_orchestrate --graphs-dir /path/to/graphs --budget 2700 --workers 8
//   contest_orchestrate                    # prompts: which input set? (lists data/input/*)
//   contest_orchestrate --self-test        # pure-logic checks, no solver

#include "../common/engine.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace engine;
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

namespace {

// --- tunables (graphs here are <=600 nodes, so quanta are small) --------- //
constexpr double QMIN = 25.0, QMAX = 120.0;
constexpr double EXPLORE_CAP  = 0.45;
constexpr int    STALL_LEASES = 2;
constexpr int    MIN_LEASES   = 2;
constexpr double DENSE_DENS   = 8.0;
constexpr double LEASE_FLOOR  = 12.0;
constexpr double OVERRUN      = 1.5;
constexpr int    XCHG_ROUNDS  = 4;
constexpr int    NH_SIZE = 12, NH_CANDS = 64;

double clampd(double x, double lo, double hi) { return std::max(lo, std::min(hi, x)); }

bool commandExists(const std::string& name) {
    std::string cmd = "command -v " + name + " > /dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

// --------------------------------------------------------------------- //
// Pure helpers (unit-tested by --self-test; no solver, no machine-load noise)
// --------------------------------------------------------------------- //
std::string coldMethod(int n, int m, bool noGraphviz, double denseDens = DENSE_DENS) {
    if (m == 0) return "sa";
    if ((double)m / std::max(1, n) >= denseDens || noGraphviz) return "sa";
    return "sa-stress";
}

double quantumBase(double budget, int nLive, std::optional<double> override_ = std::nullopt) {
    if (override_.has_value()) return *override_;
    return clampd(budget / (4.0 * std::max(1, nLive)), QMIN, QMAX);
}

struct BidInputs { int leases; bool done; double lastDk; double lastDxf; std::optional<int> bestK; };

double bid(const BidInputs& g) {
    if (g.leases == 0) return std::numeric_limits<double>::infinity();
    if (g.done) return -1.0;
    double boost = (g.lastDk > 0 || g.lastDxf > 0) ? 1.5 : 1.0;
    return (double)g.bestK.value_or(0) * boost / g.leases;
}

bool converged(int leases, int stalls) { return leases >= MIN_LEASES && stalls >= STALL_LEASES; }

// --------------------------------------------------------------------- //
struct GraphState {
    std::string tok, path;
    int n = 0, m = 0;
    std::string method0;
    std::optional<int> bestK, bestX;
    std::string warm;
    int leases = 0, stalls = 0;
    double lastDk = 0.0, lastDxf = 0.0;
    double spent = 0.0;
    bool done = false;
};

class ContestOrchestrator {
public:
    ContestOrchestrator(const std::string& graphsDir, double budget, int workers,
                        const std::string& outDir, long long seed, int xchgRounds, bool halfShare,
                        double denseDens, std::optional<double> quantumOverride,
                        const std::string& only = "")
        : budget_(budget), W_(workers), seed_(seed), xchg_(xchgRounds), half_(halfShare),
          denseDens_(denseDens), qOverride_(quantumOverride), outRoot_(outDir) {
        runId_ = "corch_" + std::to_string((long long)time(nullptr));
        runDir_ = outRoot_ + "/runs/" + runId_;
        mkdirs(runDir_);
        bests_ = loadJsonDefault(outRoot_ + "/bests.json", mjson::Value::makeObject());
        noGraphviz_ = !commandExists("sfdp") && !commandExists("neato");
        G_ = build(graphsDir, only);
    }

    mjson::Value run() {
        t0_ = Clock::now();
        double deadlineIn = budget_ - std::min(verifyReserve(), 0.20 * budget_);
        hardDeadline_ = t0_ + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(deadlineIn));
        try {
            explore();
            greedy();
        } catch (std::exception& e) {
            fprintf(stderr, "[corch] scheduler aborted: %s\n", e.what());
        }
        return finalize();
    }

private:
    // config
    double budget_; int W_; long long seed_; int xchg_; bool half_; double denseDens_;
    std::optional<double> qOverride_; std::string outRoot_;
    std::string runId_, runDir_;
    mjson::Value bests_;
    bool noGraphviz_ = false;
    std::map<std::string, GraphState> G_;

    // run state
    TimePoint t0_;
    TimePoint hardDeadline_;
    double leaseTimeout_ = 0.0;
    double maxOverrun_ = 0.0;
    bool verbose_ = true;

    void log(const std::string& msg) {
        if (verbose_) fprintf(stderr, "[corch %6.1fs] %s\n", now(), msg.c_str());
    }
    double now() const { return std::chrono::duration<double>(Clock::now() - t0_).count(); }

    std::map<std::string, GraphState> build(const std::string& graphsDir, const std::string& only) {
        std::map<std::string, GraphState> G;
        std::set<std::string> keepSet;
        if (!only.empty()) { auto keep = splitCsv(only); keepSet = std::set<std::string>(keep.begin(), keep.end()); }
        for (auto& entry : scanGraphDir(graphsDir)) {
            if (!keepSet.empty() && !keepSet.count(entry.name)) continue;
            GraphState g;
            g.tok = entry.name; g.path = entry.path; g.n = entry.n; g.m = entry.m;
            g.method0 = coldMethod(entry.n, entry.m, noGraphviz_, denseDens_);
            g.done = (entry.m == 0);
            if (entry.m == 0) { g.bestK = 0; g.warm = entry.path; }
            G[entry.name] = g;
        }
        return G;
    }

    // --- budget accounting ------------------------------------------- //
    double verifyReserve() const {
        double s = 0;
        for (auto& [nm, g] : G_) s += 2.0 + g.n / 4000.0;
        return s;
    }
    double reserve() const {
        return std::min(std::max(20.0, 0.02 * budget_) + verifyReserve(), 0.30 * budget_) + maxOverrun_;
    }
    double remaining() const { return budget_ - now() - reserve(); }
    int nLive() const {
        int c = 0;
        for (auto& [nm, g] : G_) if (!g.done) c++;
        return std::max(1, c);
    }

    // --- backstop-clamped subprocess runner --------------------------- //
    std::pair<int,double> runWithBackstop(const std::vector<std::string>& cmd, const std::string& logPath) {
        auto t0 = Clock::now();
        double left = std::chrono::duration<double>(hardDeadline_ - Clock::now()).count();
        double to = std::max(1.0, std::min(leaseTimeout_, left));
        auto res = proc::runLogged(cmd, logPath, to);
        return {res.exitCode, std::chrono::duration<double>(Clock::now() - t0).count()};
    }

    // --- one lease ----------------------------------------------------- //
    bool lease(const std::string& nm, GraphState& g) {
        double rem = remaining();
        if (rem < LEASE_FLOOR) return false;
        double qb = quantumBase(budget_, nLive(), qOverride_);
        double q = std::min(qb, rem);
        bool cold = g.warm.empty();
        std::string method = cold ? g.method0 : "sa-warm";
        std::string warm = cold ? "" : g.warm;
        int xchg = std::max(1, std::min(xchg_, (int)(q / 18)));
        std::string qdir = runDir_ + "/" + nm + "/q" + std::to_string(g.leases);
        mkdirs(qdir);
        leaseTimeout_ = std::max(45.0, q * OVERRUN);

        char buf[256];
        snprintf(buf, sizeof(buf), "lease %-14s %-9s warm=%s q=%.0fs xchg=%d%s (k=%s, lease#%d)",
                 nm.c_str(), method.c_str(), warm.empty() ? "N" : "Y", q, xchg,
                 (xchg > 1 && half_) ? "h" : "",
                 g.bestK.has_value() ? std::to_string(*g.bestK).c_str() : "None", g.leases);
        log(buf);

        std::optional<WorkerResult> bestW;
        double wall = 0.0;
        try {
            RunFn rf = [this](const std::vector<std::string>& cmd, const std::string& log) {
                return runWithBackstop(cmd, log);
            };
            auto combo = runCombo(method, g.path, q / 60.0, cold ? 0.2 : 0.05,
                                   seed_ + (long long)g.leases * 100, qdir, W_, NH_SIZE, NH_CANDS,
                                   0.5, 0, warm, xchg, half_ && xchg > 1, rf);
            bestW = combo.best;
            wall = combo.wallTotal;
        } catch (std::exception& e) {
            log(std::string("  run_combo ERROR on ") + nm + ": " + e.what());
        }
        g.spent += wall;
        g.leases += 1;
        maxOverrun_ = std::max(maxOverrun_, wall - q);
        absorb(nm, g, bestW, method, wall, q);
        return true;
    }

    void absorb(const std::string& nm, GraphState& g, const std::optional<WorkerResult>& bestW,
                const std::string& method, double wall, double q) {
        auto coldFallback = [&] {
            if (g.warm.empty() && g.method0 != "sa") {
                g.method0 = "sa";
                log("  " + nm + ": cold " + method + " invalid -> fall back to plain sa");
            }
        };
        if (!bestW.has_value() || !bestW->valid) { g.stalls++; coldFallback(); return; }

        auto nk = bestW->finalK;
        auto nx = bestW->finalTotalX;
        auto v = verifyOutput(bestW->outPath);
        if (!(v.has_value() && v->valid && nk.has_value() && v->k == nk.value())) {
            g.stalls++; coldFallback(); return;
        }
        auto ok = g.bestK;
        double ox = g.bestX.has_value() ? (double)*g.bestX : std::numeric_limits<double>::infinity();
        bool improvedK = !ok.has_value() || nk.value() < ok.value();
        bool improvedX = ok.has_value() && nk.value() == ok.value() && nx.has_value() && (double)nx.value() < ox;
        if (improvedK || improvedX) {
            g.lastDk = (ok.has_value() && nk.value() < ok.value()) ? (double)(ok.value() - nk.value()) : 0.0;
            g.lastDxf = (ox != 0.0 && std::isfinite(ox) && nx.has_value() && (double)nx.value() < ox)
                       ? (ox - nx.value()) / ox : 0.0;
            g.bestK = nk; g.bestX = nx; g.warm = bestW->outPath;
            g.stalls = 0;
            updateBest(outRoot_, bests_, nm, method, nk, nx, bestW->outPath, runId_,
                      std::nullopt, W_, std::round(wall * 10.0) / 10.0);
        } else {
            g.lastDk = 0.0; g.lastDxf = 0.0;
            g.stalls++;
        }
        if (nk.value() == 0 || (wall < 0.4 * q && q >= QMIN)) g.done = true;
    }

    // --- scheduler ------------------------------------------------------ //
    void explore() {
        std::vector<std::pair<std::string, GraphState*>> todo;
        for (auto& [nm, g] : G_) if (!g.done) todo.push_back({nm, &g});
        std::sort(todo.begin(), todo.end(), [](auto& a, auto& b) {
            return (a.second->m + a.second->n) > (b.second->m + b.second->n);
        });
        if (todo.empty()) return;
        double qb = quantumBase(budget_, (int)todo.size(), qOverride_);
        if (!qOverride_.has_value() && qb * todo.size() > EXPLORE_CAP * budget_) {
            qOverride_ = std::max(QMIN / 2, EXPLORE_CAP * budget_ / todo.size());
            char buf[128]; snprintf(buf, sizeof(buf), "explore shrink: quantum -> %.0fs", *qOverride_);
            log(buf);
        }
        char buf[64]; snprintf(buf, sizeof(buf), "=== EXPLORE %zu graphs (hardest first) ===", todo.size());
        log(buf);
        for (auto& [nm, gp] : todo) {
            if (remaining() < LEASE_FLOOR) break;
            lease(nm, *gp);
        }
    }

    void greedy() {
        log("=== GREEDY marginal-gain reallocation ===");
        while (remaining() >= LEASE_FLOOR) {
            for (auto& [nm, g] : G_) {
                if (!g.done && converged(g.leases, g.stalls)) {
                    g.done = true;
                    char buf[64];
                    snprintf(buf, sizeof(buf), "converged: bank budget (k=%s)",
                            g.bestK.has_value() ? std::to_string(*g.bestK).c_str() : "None");
                    log(buf);
                }
            }
            std::vector<std::tuple<double,std::string,GraphState*>> live;
            for (auto& [nm, g] : G_) {
                if (!g.done) {
                    BidInputs bi{g.leases, g.done, g.lastDk, g.lastDxf, g.bestK};
                    live.push_back({bid(bi), nm, &g});
                }
            }
            if (live.empty()) { log("all graphs converged -> stop early, budget banked"); break; }
            std::sort(live.begin(), live.end(), [](auto& a, auto& b) { return std::get<0>(a) > std::get<0>(b); });
            auto& [score, nm, gp] = live[0];
            (void)score;
            if (!lease(nm, *gp)) break;
        }
    }

    // --- output ----------------------------------------------------------- //
    mjson::Value finalize() {
        saveJson(outRoot_ + "/bests.json", bests_);
        std::string subDir = outRoot_ + "/submission/" + runId_;
        mkdirs(subDir);
        mjson::Value summary = mjson::Value::makeObject();
        for (auto& [nm, g] : G_) {
            std::string src = (!g.warm.empty() && fileExists(g.warm)) ? g.warm : g.path;
            std::string sub = subDir + "/" + nm + ".json";
            std::optional<VerifyResult> v;
            if (!src.empty() && fileExists(src)) {
                copyFile(src, sub);
                if (g.n < 20000) v = verifyOutput(sub);
            }
            mjson::Value s = mjson::Value::makeObject();
            s["k"] = g.bestK.has_value() ? mjson::Value((long long)*g.bestK) : mjson::Value();
            s["totalX"] = g.bestX.has_value() ? mjson::Value((long long)*g.bestX) : mjson::Value();
            s["seconds"] = std::round(g.spent * 10.0) / 10.0;
            s["leases"] = g.leases;
            s["converged"] = g.done;
            s["method0"] = g.method0;
            s["n"] = g.n; s["m"] = g.m;
            s["submission"] = sub;
            s["verified_k"] = v.has_value() ? mjson::Value((long long)v->k) : mjson::Value();
            s["valid"] = v.has_value() ? mjson::Value(v->valid) : mjson::Value(g.m == 0);
            summary[nm] = s;
        }
        mjson::Value report = mjson::Value::makeObject();
        report["run_id"] = runId_;
        report["budget_sec"] = budget_;
        report["workers"] = W_;
        report["xchg_rounds"] = xchg_;
        report["half_share"] = half_;
        report["wall_sec"] = std::round(now() * 10.0) / 10.0;
        report["reserve_sec"] = std::round(reserve() * 10.0) / 10.0;
        report["max_overrun_sec"] = std::round(maxOverrun_ * 10.0) / 10.0;
        report["summary"] = summary;
        saveJson(runDir_ + "/orchestration.json", report);
        printReport(report);
        return report;
    }

    void printReport(const mjson::Value& report) {
        printf("\n%s\n", std::string(74, '=').c_str());
        printf("CONTEST ORCHESTRATION  run=%s  budget=%.0fs  wall=%.0fs  reserve=%.0fs\n",
               report["run_id"].asString().c_str(), report["budget_sec"].asDouble(),
               report["wall_sec"].asDouble(), report["reserve_sec"].asDouble());
        printf("%s\n", std::string(74, '-').c_str());
        printf("%-16s%6s%11s%7s%6s%6s%7s  method\n", "graph", "k", "totalX", "sec", "lease", "conv", "valid");
        std::vector<int> ks;
        std::vector<std::string> names;
        for (auto& kv : report["summary"].asObject()) names.push_back(kv.first);
        std::sort(names.begin(), names.end());
        for (auto& nm : names) {
            const auto& s = report["summary"][nm];
            std::string kStr = s["k"].isNull() ? "None" : std::to_string(s["k"].asLL());
            std::string xStr = s["totalX"].isNull() ? "None" : std::to_string(s["totalX"].asLL());
            if (!s["k"].isNull()) ks.push_back(s["k"].asInt());
            printf("%-16s%6s%11s%7.0f%6lld%6s%7s  %s\n",
                   nm.c_str(), kStr.c_str(), xStr.c_str(), s["seconds"].asDouble(),
                   s["leases"].asLL(), s["converged"].asBool() ? "Y" : "n",
                   s["valid"].asBool() ? "Y" : "N", s["method0"].asString().c_str());
        }
        printf("%s\n", std::string(74, '-').c_str());
        int worst = ks.empty() ? -1 : *std::max_element(ks.begin(), ks.end());
        long long sumK = 0; for (int k : ks) sumK += k;
        printf("worst-k=%s  sum-k=%s  wall=%.0f/%.0fs  submissions in %s/submission/%s/\n",
               ks.empty() ? "None" : std::to_string(worst).c_str(),
               ks.empty() ? "None" : std::to_string(sumK).c_str(),
               report["wall_sec"].asDouble(), report["budget_sec"].asDouble(),
               outRoot_.c_str(), runId_.c_str());
        printf("%s\n", std::string(74, '=').c_str());
    }
};

int selfTest() {
    bool ok = true;
    auto check = [&](const char* name, bool cond) {
        printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
        ok = ok && cond;
    };

    printf("cold_method() prior\n");
    check("sparse -> sa-stress", coldMethod(500, 1984, false) == "sa-stress");
    check("dense (m/n>=8) -> sa", coldMethod(100, 900, false) == "sa");
    check("empty -> sa", coldMethod(10, 0, false) == "sa");
    check("no graphviz -> sa", coldMethod(500, 1984, true) == "sa");
    check("dense threshold configurable", coldMethod(100, 900, false, 20.0) == "sa-stress");

    printf("quantum_base() bounds\n");
    check("clamped low -> QMIN", quantumBase(100, 9) == QMIN);
    check("mid", std::abs(quantumBase(2700, 9) - 75.0) < 1e-9);
    check("clamped high -> QMAX", quantumBase(100000, 1) == QMAX);
    check("override respected", quantumBase(2700, 9, 20.0) == 20.0);

    printf("bid() ordering (room/leases spread, improving boost)\n");
    check("unexplored bids infinite", bid({0, false, 0, 0, std::nullopt}) == std::numeric_limits<double>::infinity());
    check("done graph bids negative", bid({3, true, 0, 0, std::nullopt}) == -1.0);
    BidInputs hi{1, false, 0.0, 0.0, 250};
    BidInputs lo{1, false, 0.0, 0.0, 4};
    check("worst (high-k) graph prioritised over trivial", bid(hi) > bid(lo));
    BidInputs served = hi; served.leases = 6;
    BidInputs fresh{1, false, 0.0, 0.0, 85};
    check("least-serviced rotates in (heavily-served high-k yields to fresh peer)", bid(fresh) > bid(served));
    BidInputs impr = hi; impr.lastDk = 4.0;
    check("improving boost favours an active descent at equal room/leases", bid(impr) > bid(hi));

    printf("converged()\n");
    check("2 stalls after fair trial -> converged", converged(3, 2) == true);
    check("1 stall not converged", converged(3, 1) == false);
    check("single lease never converged", converged(1, 5) == false);

    printf("\n%s\n", ok ? "ALL PASS" : "SOME FAILED");
    return ok ? 0 : 1;
}

struct Args {
    std::string inputSet;
    std::string graphsDir;
    std::string only; // comma-separated graph names to keep (blank = all)
    double budget = 2700;
    int workers = 8;
    std::string outDir; // blank = auto: data/output/<input-set-name>
    long long seed = 1;
    int xchgRounds = XCHG_ROUNDS;
    bool noHalf = false;
    double denseDensity = DENSE_DENS;
    std::optional<double> quantum;
    bool selfTest = false;
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string s = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + s);
            return argv[++i];
        };
        if (s == "--input-set") a.inputSet = next();
        else if (s == "--graphs-dir") a.graphsDir = next();
        else if (s == "--only") a.only = next();
        else if (s == "--budget") a.budget = std::stod(next());
        else if (s == "--workers") a.workers = std::stoi(next());
        else if (s == "--out-dir") a.outDir = next();
        else if (s == "--seed") a.seed = std::stoll(next());
        else if (s == "--xchg-rounds") a.xchgRounds = std::stoi(next());
        else if (s == "--no-half") a.noHalf = true;
        else if (s == "--dense-density") a.denseDensity = std::stod(next());
        else if (s == "--quantum") a.quantum = std::stod(next());
        else if (s == "--self-test") a.selfTest = true;
        else throw std::runtime_error("unknown argument: " + s);
    }
    return a;
}

} // namespace

int main(int argc, char** argv) {
    // stdout is fully-buffered (not line-buffered) once redirected to a file
    // (e.g. by the web server) - force line buffering so the final report
    // table streams promptly instead of only appearing at process exit.
    setvbuf(stdout, nullptr, _IOLBF, 0);

    Args args;
    try {
        args = parseArgs(argc, argv);
    } catch (std::exception& e) {
        fprintf(stderr, "contest_orchestrate: %s\n", e.what());
        return 2;
    }
    if (args.selfTest) return selfTest();

    ensureBinaries();
    std::string graphsDir;
    try {
        graphsDir = resolveGraphsDir(args.inputSet, args.graphsDir);
    } catch (std::exception& e) {
        fprintf(stderr, "contest_orchestrate: %s\n", e.what());
        return 2;
    }
    // Nest output under data/output/<input-set-name> by default (mirrors
    // data/input/<set>/) so different sets never collide on graph names.
    std::string outDir = !args.outDir.empty() ? args.outDir : ("data/output/" + pathBasename(graphsDir));

    ContestOrchestrator orch(graphsDir, args.budget, args.workers, outDir, args.seed,
                             args.xchgRounds, !args.noHalf, args.denseDensity, args.quantum, args.only);
    orch.run();
    return 0;
}
