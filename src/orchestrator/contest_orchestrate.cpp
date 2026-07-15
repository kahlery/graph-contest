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
// QMAX bumped 120 -> 240 (2026-07): it only binds late, when few graphs are
// live — exactly the large-graph grind, where longer continuous phase-2
// descents dominate (a single 3-min continuous warm run beat ten chopped
// 112s/xchg-4 leases on instance_05: k 231 -> 218).
constexpr double QMIN = 25.0, QMAX = 240.0;
constexpr double EXPLORE_CAP  = 0.45;
constexpr int    STALL_LEASES = 2;
constexpr int    MIN_LEASES   = 2;
constexpr double DENSE_DENS   = 8.0;
constexpr double LEASE_FLOOR  = 12.0;
constexpr double OVERRUN      = 1.5;
constexpr int    XCHG_ROUNDS  = 4;
// NH_SIZE 8: best LNS neighbourhood for the staged family (2026-07-08 sweep
// on instance_08: nh8 k=61 vs nh12 66/68 vs nh24 67 — smaller neighbourhoods
// build the sparser base the SA polish descends from). The corch_1783476249
// "overrun" that got this reverted to 12 was a red herring: the machine
// SLEPT mid-lease (04:26-04:56; child froze at t=30s, backstop fired on
// wake). approach1 honours -t exactly when awake (repro: 84.03s on an 84s
// budget). Run long/overnight sessions under `caffeinate -dims`.
constexpr int    NH_SIZE = 8, NH_CANDS = 64;

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

struct BidInputs {
    int leases; bool done; double lastDk; double lastDxf;
    std::optional<int> bestK;
    std::optional<int> target;   // when set, bid on gap-to-target, not raw k
};

double bid(const BidInputs& g) {
    if (g.leases == 0) return std::numeric_limits<double>::infinity();
    if (g.done) return -1.0;
    double boost = (g.lastDk > 0 || g.lastDxf > 0) ? 1.5 : 1.0;
    // With targets, raw-k bidding starves low-k graphs that are just as far
    // from THEIR target (focused run #5: instance_08 broke through to 70 in
    // explore, then never won another auction against k~210 peers). The
    // room that matters is the distance still to close.
    double room = (g.target.has_value() && g.bestK.has_value())
                      ? (double)std::max(1, *g.bestK - *g.target)
                      : (double)g.bestK.value_or(0);
    return room * boost / g.leases;
}

bool converged(int leases, int stalls) { return leases >= MIN_LEASES && stalls >= STALL_LEASES; }

// --------------------------------------------------------------------- //
struct GraphState {
    std::string tok, path;
    int n = 0, m = 0;
    std::string method0;
    std::optional<int> bestK, bestX;
    std::optional<int> target;   // --targets: bank budget once bestK <= target
    int altCold = 0;             // cold-restart family rotation index:
                                 // cycles tripod -> staged -> method0
    // Cross-family pollination bookkeeping: best (k, layout) seen per cold
    // FAMILY, plus which family produced the current warm chain. Every other
    // stall-driven cold restart warm-starts from the strongest OTHER family
    // instead of going fully cold — a stalled chain gets the competing
    // basin's head start while true cold re-rolls keep the diversity.
    std::map<std::string, std::pair<int, std::string>> famBest;
    std::string chainFam;
    bool pollinateNext = false;  // alternates with true cold restarts
    std::string warm;
    int leases = 0, stalls = 0;
    int warmLeases = 0;   // plain warm continuations run so far
    double lastDk = 0.0, lastDxf = 0.0;
    double spent = 0.0;
    bool done = false;
};

class ContestOrchestrator {
public:
    ContestOrchestrator(const std::string& graphsDir, double budget, int workers,
                        const std::string& outDir, long long seed, int xchgRounds, bool halfShare,
                        double denseDens, std::optional<double> quantumOverride,
                        const std::string& only = "",
                        const std::map<std::string,int>& targets = {})
        : budget_(budget), W_(workers), seed_(seed), xchg_(xchgRounds), half_(halfShare),
          denseDens_(denseDens), qOverride_(quantumOverride), outRoot_(outDir),
          label_(pathBasename(graphsDir)) {
        runId_ = "corch_" + std::to_string((long long)time(nullptr));
        runDir_ = outRoot_ + "/runs/" + runId_;
        mkdirs(runDir_);
        bests_ = loadJsonDefault(outRoot_ + "/bests.json", mjson::Value::makeObject());
        noGraphviz_ = !commandExists("sfdp") && !commandExists("neato");
        G_ = build(graphsDir, only);
        for (auto& [nm, g] : G_) {
            auto it = targets.find(nm);
            if (it != targets.end()) g.target = it->second;
        }
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
    std::string label_; // input-set / graphs-dir name, prefixed onto bests.json keys
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
            // Large graphs open with the structural family directly: in four
            // consecutive full-9 hours (seeds 10/12/13/14) the sa-stress
            // explore lease LOST to the tripod family lease on every big
            // graph (05: 250 vs 217, 06: 238 vs 215, 08: 82 vs 68 in seed
            // 14). Starting in the winning basin hands the warm chain the
            // explore lease's ~100s and a 240s head start.
            if (entry.n + entry.m >= 1000) g.method0 = "tripod";
            // The biggest SPARSE-ISH graphs open on GRADX (SigmoidX
            // gradient-descent init): 10-min gate A/Bs put it far ahead of
            // tripod there — 05: 183-187 vs 201, 06: 186/186 (record, ties
            // the rival) vs 211, 08: 70/70 vs 75. instance_03 (n+m=1201)
            // stays tripod (gate 11/11 vs stress path 10), and the density
            // gate (m<=4n) keeps dense geometric graphs like instance_04
            // (m/n=6.2, seed-17: gradx-open cost it its 31-32 band) on the
            // tripod opener — SigmoidX has quadratically less room per pair
            // on dense graphs.
            if (entry.n + entry.m >= 2000 && entry.m <= 4 * entry.n)
                g.method0 = "gradx";
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
    // forceColdMethod: run this lease cold with the given method regardless
    // of warm state (dual-family explore); absorb() keeps it only if better.
    // The tripod param sweep made narrow arms (--wedge 0.02,0.06) an
    // instance_05-specific win (cold -12); neutral on 06, slightly negative
    // on 08 — so only 05 gets the tripod-n variant.
    static std::string tripodFam(const std::string& nm) {
        return nm.find("instance_05") != std::string::npos ? "tripod-n"
                                                           : "tripod";
    }

    bool lease(const std::string& nm, GraphState& g,
               const std::string& forceColdMethodIn = "") {
        std::string forceColdMethod = forceColdMethodIn == "tripod"
                                          ? tripodFam(nm) : forceColdMethodIn;
        double rem = remaining();
        if (rem < LEASE_FLOOR) return false;
        double qb = quantumBase(budget_, nLive(), qOverride_);
        // Large graphs get a doubled quantum: their continuous anneals keep
        // paying well past 112s (corch_1783425898: instance_05 still dropping
        // k at lease end), while nLive() stays high all hour because small
        // near-target graphs rarely reach the formal converged() state, which
        // pins quantumBase at ~budget/(4*9) and starves the grind of depth.
        // Only from the third lease on: the explore + second-lease-guarantee
        // passes must stay cheap or they'd eat the whole greedy budget.
        double q = std::min(qb, rem);
        // (2026-07-15) QMAX_GRIND=480 deep legs were tried for one hour
        // (seed 16) and REVERTED: instance_06 landed at 206 — the worst of
        // the five-seed 198-207 band — so cooling depth is not what blocks
        // it, and the longer warm lease doubled instance_03's dead urgency
        // spend (789s at dk=0). Short legs also harvest more 8-worker draws.
        if (g.n + g.m >= 1000 && g.leases >= 2)
            q = std::min(std::min(2.0 * qb, QMAX), rem);
        // A forced-family lease (staged = LNS then SA) needs room for the LNS
        // stage to actually build its sparser layout family.
        if (!forceColdMethod.empty())
            q = std::min(std::min(2.0 * qb, QMAX), rem);
        bool cold = g.warm.empty() || !forceColdMethod.empty();
        // Diversified cold restart: on large graphs the warm chain can lock
        // into a shallow basin (ab4 A/B: instance_06 warm chain dead-ended at
        // 221 while a fresh-seed cold descent reached 206, crossing 217 within
        // one doubled quantum). On every odd stall, spend the lease on a fresh
        // cold multi-start instead of another warm continuation; absorb()
        // keeps it only if it beats the incumbent, so the downside is one
        // lease of budget and the upside is a whole new basin.
        // Family rotation is for big-gap graphs stuck in a basin; a graph
        // within 3 of its target needs warm POLISH plus the 8-worker seed
        // lottery, not a from-scratch family re-roll (full-9 run seed 7:
        // instance_04's stall-rotation cold restarts burned its urgency
        // leases while the warm continuation that found 32 never ran).
        bool closable = g.target.has_value() && g.bestK.has_value() &&
                        *g.bestK - *g.target <= 3;
        // Never family-rotate before the warm chain has run at least once:
        // seed 17 let a dual-explore stall (dk=0 on the staged lease) push
        // instance_05 straight into rotation while its gradx chain — the
        // strongest asset of the hour (gate: 199 -> ~185) — never started.
        bool coldRestart = !cold && !closable && (g.n + g.m >= 1000) &&
                           g.warmLeases >= 1 && (g.stalls % 2 == 1);
        // Cold restarts rotate through layout FAMILIES, not just seeds:
        // tripod (structural 3-arm init — 05/06 records), staged (LNS->SA,
        // sparser family — 08 records), then the force-directed method0.
        static const char* FAMILIES[] = {"tripod", "staged", nullptr};
        std::string method = !forceColdMethod.empty() ? forceColdMethod
                           : cold ? g.method0
                           : coldRestart
                               ? (FAMILIES[g.altCold % 3]
                                      ? std::string(FAMILIES[g.altCold % 3])
                                      : g.method0)
                               : "sa-warm";
        if (method == "tripod") method = tripodFam(nm);
        std::string warm = (cold || coldRestart) ? "" : g.warm;
        // Cross-family pollination: every other stall-driven restart, if a
        // DIFFERENT family holds a layout within 10% of the incumbent k,
        // warm-continue from it instead of going fully cold — the stalled
        // chain inherits the competing basin's head start.
        // method already carries the tripod->tripod-n remap, so cold and
        // cold-restart leases book their family under the variant that ran.
        std::string famUsed = (cold || coldRestart) ? method
                            : (g.chainFam.empty() ? g.method0 : g.chainFam);
        if (coldRestart) {
            if (g.pollinateNext) {
                const std::string* alt = nullptr;
                std::string altFam;
                int altK = std::numeric_limits<int>::max();
                for (auto& [fam, best] : g.famBest) {
                    if (fam == g.chainFam) continue;
                    if (!fileExists(best.second)) continue;
                    if (best.first < altK &&
                        (double)best.first <=
                            1.10 * (double)g.bestK.value_or(best.first)) {
                        altK   = best.first;
                        alt    = &best.second;
                        altFam = fam;
                    }
                }
                if (alt) {
                    method  = "sa-warm";
                    warm    = *alt;
                    famUsed = altFam;
                    log("  " + nm + ": pollinate from " + altFam +
                        " best k=" + std::to_string(altK));
                } else {
                    g.altCold++;   // no donor: true cold as usual
                }
            } else {
                g.altCold++;
            }
            g.pollinateNext = !g.pollinateNext;
        }
        // Chopping a lease into xchg rounds resets the SA cooling schedule
        // every q/xchg seconds. On big graphs that keeps phase 2 permanently
        // hot: the corch_1783421945 A/B showed one continuous 3-min descent
        // beating an hour of 28s-round leases (instance_05 k 231 -> 218).
        // So large graphs run each lease as ONE continuous anneal (workers
        // still explore independently and share via the warm handoff between
        // leases); only small graphs keep the round-based elite exchange.
        int xchg = (g.n + g.m >= 1000)
                       ? 1
                       : std::max(1, std::min(xchg_, (int)(q / 18)));
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
            // Warm continuations skip phase 1 entirely (p1Frac 0): re-running
            // the hot totalX anneal on an already k-optimised layout first
            // degrades it and then burns budget re-converging — the k walk
            // (phase 2) can resume directly from the warm layout.
            auto combo = runCombo(method, g.path, q / 60.0,
                                   (cold || coldRestart) ? 0.2 : 0.0,
                                   seed_ + (long long)g.leases * 100, qdir, W_, NH_SIZE, NH_CANDS,
                                   /*lnsFrac*/ 0.35, 0, warm, xchg,
                                   half_ && xchg > 1, rf);
            bestW = combo.best;
            wall = combo.wallTotal;
        } catch (std::exception& e) {
            log(std::string("  run_combo ERROR on ") + nm + ": " + e.what());
        }
        g.spent += wall;
        g.leases += 1;
        if (method == "sa-warm" && !warm.empty()) g.warmLeases += 1;
        maxOverrun_ = std::max(maxOverrun_, wall - q);
        absorb(nm, g, bestW, method, wall, q, famUsed);

        std::string kStr = g.bestK.has_value() ? std::to_string(*g.bestK) : "None";
        log(std::string(buf) + " k=" + kStr);
        return true;
    }

    void absorb(const std::string& nm, GraphState& g, const std::optional<WorkerResult>& bestW,
                const std::string& method, double wall, double q,
                const std::string& famUsed = "") {
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
        // Per-family record for cross-family pollination — kept even when the
        // lease does not beat the overall incumbent (a family's own best can
        // still seed a future pollination lease).
        if (!famUsed.empty()) {
            auto it = g.famBest.find(famUsed);
            if (it == g.famBest.end() || nk.value() < it->second.first)
                g.famBest[famUsed] = {nk.value(), bestW->outPath};
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
            if (!famUsed.empty()) g.chainFam = famUsed;
            g.stalls = 0;
            // bests.json keys are qualified as "<input-set>/<graph>__<method>" so
            // entries stay unambiguous once viewed alongside other sets in the GUI;
            // submission files/dirs (nm alone) are untouched - contest format.
            std::string qname = label_.empty() ? nm : (label_ + "/" + nm);
            updateBest(outRoot_, bests_, qname, method, nk, nx, bestW->outPath, runId_,
                      std::nullopt, W_, std::round(wall * 10.0) / 10.0, g.n, g.m);
        } else {
            g.lastDk = 0.0; g.lastDxf = 0.0;
            g.stalls++;
        }
        if (nk.value() == 0 || (wall < 0.4 * q && q >= QMIN)) g.done = true;
        // Target reached: bank the rest of this graph's budget for the ones
        // still above target (run #3 spent ~30% of the hour re-leasing graphs
        // that were already at the score we're chasing).
        if (!g.done && g.target.has_value() && g.bestK.has_value() &&
            *g.bestK <= *g.target) {
            g.done = true;
            log(nm + ": target " + std::to_string(*g.target) + " reached (k=" +
                std::to_string(*g.bestK) + ") -> bank budget");
        }
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
        // Dual-family explore: the best layout family is graph-specific and
        // cannot be predicted. Large graphs now OPEN on tripod (method0),
        // so the second cold lease comes from STAGED — the LNS family that
        // holds the 08 record, with the bandit-armed SA tail validated in
        // paired A/Bs (same LNS layout + same tail seed: 67v69, 66v68).
        // sa-stress is out of the big-graph picture entirely: it lost the
        // family race in four consecutive full-9 hours (seeds 10/12/13/14).
        // absorb() keeps whichever family won; warm leases build on that.
        for (auto& [nm, gp] : todo) {
            if (gp->done || gp->n + gp->m < 1000) continue;
            if (remaining() < LEASE_FLOOR) break;
            lease(nm, *gp, "staged");
        }
    }

    void greedy() {
        // Anti-starvation prologue: bid() is proportional to bestK, so the
        // high-k grind graphs monopolize every quantum and near-target small
        // graphs never see a second lease (corch_1783421945: instance_03 sat
        // one k above target all hour with leases=1). One guaranteed second
        // lease each, hardest-first, before the marginal-gain auction.
        {
            std::vector<std::pair<std::string, GraphState*>> starved;
            for (auto& [nm, g] : G_)
                if (!g.done && g.leases < 2) starved.push_back({nm, &g});
            std::sort(starved.begin(), starved.end(), [](auto& a, auto& b) {
                return a.second->bestK.value_or(0) > b.second->bestK.value_or(0);
            });
            if (!starved.empty()) log("=== SECOND-LEASE guarantee ===");
            for (auto& [nm, gp] : starved) {
                if (remaining() < LEASE_FLOOR) break;
                lease(nm, *gp);
            }
        }
        // WARM-CHAIN guarantee: every large graph gets one warm continuation
        // before the auction. Seed 17: instance_05's gradx explore hit 199
        // (best explore ever) and then lost every auction on its small
        // gap-to-target — 340s total while instance_06 burned 820s at a
        // plateau. The chain lease is where the init families pay off; it
        // must not depend on winning a bid.
        {
            std::vector<std::pair<std::string,GraphState*>> chainless;
            for (auto& [nm, g] : G_)
                if (!g.done && g.n + g.m >= 1000 && g.warmLeases == 0 &&
                    !g.warm.empty())
                    chainless.push_back({nm, &g});
            std::sort(chainless.begin(), chainless.end(), [](auto& a, auto& b) {
                return a.second->bestK.value_or(0) > b.second->bestK.value_or(0);
            });
            if (!chainless.empty()) log("=== WARM-CHAIN guarantee ===");
            for (auto& [nm, gp] : chainless) {
                if (remaining() < LEASE_FLOOR) break;
                lease(nm, *gp);
            }
        }
        log("=== GREEDY marginal-gain reallocation ===");
        while (remaining() >= LEASE_FLOOR) {
            for (auto& [nm, g] : G_) {
                // Large graphs need headroom for cold-restart attempts (each
                // failed restart is a stall), so they converge at 4 stalls.
                int stallsEff = (g.n + g.m >= 1000) ? g.stalls - 2 : g.stalls;
                if (!g.done && converged(g.leases, stallsEff)) {
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
                    BidInputs bi{g.leases, g.done, g.lastDk, g.lastDxf, g.bestK, g.target};
                    double b = bid(bi);
                    // Closable-gap urgency: a graph within 3 of its target can
                    // be closed by one lucky lease. But it IS a lottery —
                    // full-9 seed-7 let two such graphs eat 60% of the hour
                    // and close nothing while the k~230 graphs sat at their
                    // raw explore values. Cap: at most 2 urgency leases each
                    // (leases<4 incl. explore) and none in the last quarter
                    // of the budget, which stays reserved for the big grind.
                    // Progress gate: the LAST urgency lease (leases==3) must
                    // be earned by a k drop in the previous one. Seeds 12+13
                    // both spent 3x240s on instance_03 at dk=0 throughout
                    // (the k=9 basin is cold-seed luck, unreachable by warm
                    // polish), while instance_04 improved every lease on its
                    // way to 31 — pay for progress, stop refilling dead ends.
                    if (g.target.has_value() && g.bestK.has_value() &&
                        g.leases < 4 && remaining() > 0.25 * budget_ &&
                        (g.leases < 3 || g.lastDk > 0)) {
                        int gap = *g.bestK - *g.target;
                        if (gap > 0 && gap <= 3) b = 1e6 / (1.0 + g.leases);
                    }
                    live.push_back({b, nm, &g});
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
    std::string targets; // "name=k,name=k": bank a graph's budget at k<=target
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
        else if (s == "--targets") a.targets = next();
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

    std::map<std::string,int> targets;
    for (auto& part : splitCsv(args.targets)) {
        size_t eq = part.find('=');
        if (eq == std::string::npos) {
            fprintf(stderr, "contest_orchestrate: bad --targets entry: %s\n", part.c_str());
            return 2;
        }
        targets[trim(part.substr(0, eq))] = std::stoi(part.substr(eq + 1));
    }

    ContestOrchestrator orch(graphsDir, args.budget, args.workers, outDir, args.seed,
                             args.xchgRounds, !args.noHalf, args.denseDensity, args.quantum,
                             args.only, targets);
    orch.run();
    return 0;
}
