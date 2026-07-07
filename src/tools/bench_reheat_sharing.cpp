// bench_reheat_sharing - sa-stress reheat + worker-sharing sweep harness.
// C++ port of tools/bench_reheat_sharing.py.
//
// Caches stress-inits per (graph, seed) so every reheat/xchg variant is
// compared on IDENTICAL inits (removes init RNG as a confound). Mirrors real
// sa-stress: warm sakgd from the stress init, p1_frac=0.2, kband=2. The only
// things that vary between configs are the solver reheat flags and the
// cross-worker sharing rounds.
//
// This was a one-off research sweep; its conclusion (4 rounds, half-sharing)
// is already baked into contest_orchestrate's defaults and documented in
// results/exp-2026-06-30-reheat-sharing.md. Kept for further parameter
// exploration.

#include "../common/engine.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <regex>
#include <thread>

using namespace engine;

namespace {

std::string workDirBase() {
    const char* env = std::getenv("BENCH_WORKDIR");
    std::string base = env ? env : "/tmp";
    return base + "/sa_bench";
}

std::optional<std::pair<int,int>> verify(const std::string& path) {
    auto res = proc::runCaptured({SAKGD(), "--verify", path}, "", 900.0);
    if (res.timedOut) return std::nullopt;
    static const std::regex re(R"(k=(\d+) totalCrossings=(\d+) vertexEdgeOverlap=(\w+))");
    std::smatch m;
    if (!std::regex_search(res.stdoutData, m, re)) return std::nullopt;
    if (m[3].str() != "no") return std::nullopt;
    return std::make_pair(std::stoi(m[1].str()), std::stoi(m[2].str()));
}

std::string makeInit(const std::string& initDir, const std::string& gdir, const std::string& graph,
                      long long seed, double initSec) {
    std::string out = initDir + "/g" + graph + "_s" + std::to_string(seed) + ".json";
    if (fileExists(out) && std::filesystem::file_size(out) > 0) return out;
    std::string gp = gdir + "/Automatic-" + graph + ".json";
    proc::runCaptured({STRESS_INIT(), "-i", gp, "-o", out, "-s", std::to_string(seed),
                        "-t", std::to_string(initSec)}, "", initSec + 30.0);
    return out;
}

void sakgdRun(const std::string& inp, const std::string& out, double budgetSec, long long seed,
              double p1Frac, const std::vector<std::string>& flags) {
    std::vector<std::string> cmd = {SAKGD(), "-i", inp, "-o", out,
                                     "-t", minsFmt(budgetSec / 60.0),
                                     "-p1", minsFmt(budgetSec * p1Frac / 60.0),
                                     "-s", std::to_string(seed), "--init", "input", "--kband", "2"};
    for (auto& f : flags) cmd.push_back(f);
    proc::runCaptured(cmd, "", budgetSec + 60.0);
}

struct Config { std::string name; std::vector<std::string> flags; int xchg = 1; std::string share = "all"; };

std::vector<Config> configSet(const std::string& name) {
    if (name == "sweep") {
        return {
            {"base", {}, 1, "all"},
            {"rh2",  {"--reheat", "2"}, 1, "all"},
            {"rh4",  {"--reheat", "4"}, 1, "all"},
            {"rh8",  {"--reheat", "8"}, 1, "all"},
            {"x2",   {}, 2, "all"},
            {"x4",   {}, 4, "all"},
        };
    }
    throw std::runtime_error("unknown config set: " + name);
}

std::optional<std::pair<int,int>> runConfigOnGraph(const Config& cfg, const std::string& graph,
                                                    const std::vector<long long>& seeds, double budgetSec,
                                                    double initSec, const std::string& initDir,
                                                    const std::string& workDir, const std::string& gdir) {
    int nw = (int)seeds.size();
    std::string tag = cfg.name + "_g" + graph;
    std::string eliteFile = workDir + "/" + tag + "_elite.json";
    std::string elitePath; // empty until first round produces one
    std::unique_ptr<Barrier> barrier;
    if (cfg.xchg > 1) barrier = std::make_unique<Barrier>(nw);

    std::vector<std::string> outs(nw);
    auto worker = [&](int wid) {
        long long seed = seeds[wid];
        std::string init = makeInit(initDir, gdir, graph, seed, initSec);
        if (cfg.xchg <= 1) {
            std::string out = workDir + "/" + tag + "_w" + std::to_string(wid) + ".json";
            sakgdRun(init, out, budgetSec, seed, 0.2, cfg.flags);
            outs[wid] = out;
            return;
        }
        bool independent = (cfg.share == "half" && wid < nw / 2);
        double rsec = budgetSec / cfg.xchg;
        std::string cur = init;
        for (int r = 0; r < cfg.xchg; r++) {
            std::string ro = workDir + "/" + tag + "_w" + std::to_string(wid) + "_r" + std::to_string(r) + ".json";
            bool adopt = (r > 0 && !elitePath.empty() && !independent);
            std::string src = adopt ? elitePath : cur;
            sakgdRun(src, ro, rsec, seed + (long long)r * 1000, r == 0 ? 0.2 : 0.02, cfg.flags);
            cur = ro;
            barrier->wait();
            if (wid == 0) {
                std::optional<std::pair<int,int>> best;
                std::string bestPath;
                for (int w = 0; w < nw; w++) {
                    std::string p = workDir + "/" + tag + "_w" + std::to_string(w) + "_r" + std::to_string(r) + ".json";
                    auto res = verify(p);
                    if (res.has_value() && (!best.has_value() || *res < *best)) { best = res; bestPath = p; }
                }
                if (best.has_value()) { copyFile(bestPath, eliteFile); elitePath = eliteFile; }
            }
            barrier->wait();
        }
        outs[wid] = cur;
    };

    std::vector<std::thread> threads;
    for (int w = 0; w < nw; w++) threads.emplace_back(worker, w);
    for (auto& t : threads) t.join();

    std::optional<std::pair<int,int>> best;
    for (auto& o : outs) {
        auto res = verify(o);
        if (res.has_value() && (!best.has_value() || *res < *best)) best = res;
    }
    return best;
}

struct Args {
    std::string graphs = "1,4,5,6";
    std::string graphsDir; // resolved to data/input/internal-contest if empty
    int workers = 3;
    double budget = 75.0;
    double initSec = 8.0;
    long long baseSeed = 1;
    std::string configs = "sweep";
    int maxproc = 9;
    std::string out;
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string s = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + s);
            return argv[++i];
        };
        if (s == "--graphs") a.graphs = next();
        else if (s == "--graphs-dir") a.graphsDir = next();
        else if (s == "--workers") a.workers = std::stoi(next());
        else if (s == "--budget") a.budget = std::stod(next());
        else if (s == "--init-sec") a.initSec = std::stod(next());
        else if (s == "--base-seed") a.baseSeed = std::stoll(next());
        else if (s == "--configs") a.configs = next();
        else if (s == "--maxproc") a.maxproc = std::stoi(next());
        else if (s == "--out") a.out = next();
        else throw std::runtime_error("unknown argument: " + s);
    }
    return a;
}

std::vector<std::string> splitCommaSimple(const std::string& s) {
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

// Bounded-concurrency work queue: `poolSize` worker threads pull tasks.
void runPool(int poolSize, int n, const std::function<void(int)>& task) {
    std::atomic<int> next{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < poolSize; t++) {
        threads.emplace_back([&] {
            while (true) {
                int i = next.fetch_add(1);
                if (i >= n) break;
                task(i);
            }
        });
    }
    for (auto& t : threads) t.join();
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    try {
        args = parseArgs(argc, argv);
    } catch (std::exception& e) {
        fprintf(stderr, "bench_reheat_sharing: %s\n", e.what());
        return 2;
    }

    std::string sp = workDirBase();
    std::string initDir = sp + "/inits";
    std::string workDir = sp + "/work";
    mkdirs(initDir);
    mkdirs(workDir);
    if (args.out.empty()) args.out = sp + "/results.json";

    auto graphs = splitCommaSimple(args.graphs);
    std::vector<long long> seeds;
    for (int i = 0; i < args.workers; i++) seeds.push_back(args.baseSeed + i);
    auto configs = configSet(args.configs);
    std::string gdir = args.graphsDir.empty() ? ROOT() + "/data/input/internal-contest" : args.graphsDir;

    printf("[init] %zu graphs x %d seeds, %.1fs box...\n", graphs.size(), args.workers, args.initSec);
    fflush(stdout);
    std::vector<std::pair<std::string,long long>> initUnits;
    for (auto& g : graphs) for (auto s : seeds) initUnits.push_back({g, s});
    runPool(args.maxproc, (int)initUnits.size(), [&](int i) {
        makeInit(initDir, gdir, initUnits[i].first, initUnits[i].second, args.initSec);
    });

    std::vector<std::pair<const Config*,std::string>> units;
    for (auto& c : configs) for (auto& g : graphs) units.push_back({&c, g});
    int unitPool = std::max(1, args.maxproc / args.workers);
    printf("[bench] %zu units, %d concurrent, W=%d budget=%.0fs\n",
           units.size(), unitPool, args.workers, args.budget);
    fflush(stdout);

    std::mutex resultsMu;
    std::map<std::string, std::map<std::string, std::optional<std::pair<int,int>>>> results;
    auto t0 = std::chrono::steady_clock::now();

    runPool(unitPool, (int)units.size(), [&](int i) {
        auto& [cfg, g] = units[i];
        auto r = runConfigOnGraph(*cfg, g, seeds, args.budget, args.initSec, initDir, workDir, gdir);
        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        {
            std::lock_guard<std::mutex> lk(resultsMu);
            char buf[128];
            if (r.has_value()) snprintf(buf, sizeof(buf), "k=%d x=%d", r->first, r->second);
            else snprintf(buf, sizeof(buf), "INVALID");
            printf("  %6s g%s: %s  [%.0fs]\n", cfg->name.c_str(), g.c_str(), buf, elapsed);
            fflush(stdout);
            results[cfg->name][g] = r;
        }
    });

    printf("\n=== RESULTS (k; tiebreak totalX) ===\n");
    std::string hdr = "config  ";
    for (auto& g : graphs) { char b[16]; snprintf(b, sizeof(b), "  g%4s", g.c_str()); hdr += b; }
    hdr += "   sum_k";
    printf("%s\n", hdr.c_str());
    printf("%s\n", std::string(hdr.size(), '-').c_str());

    std::vector<std::pair<std::string,long long>> rows;
    for (auto& c : configs) {
        long long sk = 0;
        std::string line = c.name; line.resize(std::max(line.size(), (size_t)8), ' ');
        for (auto& g : graphs) {
            auto r = results[c.name][g];
            char cell[16];
            if (r.has_value()) { snprintf(cell, sizeof(cell), "%6d", r->first); sk += r->first; }
            else snprintf(cell, sizeof(cell), "   INV");
            line += cell;
        }
        char sumBuf[16]; snprintf(sumBuf, sizeof(sumBuf), "   %5lld", sk);
        line += sumBuf;
        rows.push_back({c.name, sk});
        printf("%s\n", line.c_str());
    }
    printf("%s\n", std::string(hdr.size(), '-').c_str());
    auto best = *std::min_element(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.second < b.second; });
    printf("best sum_k: %s (%lld)\n", best.first.c_str(), best.second);

    mjson::Value out = mjson::Value::makeObject();
    mjson::Value params = mjson::Value::makeObject();
    params["graphs"] = args.graphs; params["workers"] = args.workers; params["budget"] = args.budget;
    params["init_sec"] = args.initSec; params["base_seed"] = (long long)args.baseSeed;
    params["configs"] = args.configs; params["maxproc"] = args.maxproc; params["out"] = args.out;
    out["params"] = params;
    mjson::Value seedsArr = mjson::Value::makeArray();
    for (auto s : seeds) seedsArr.push_back((long long)s);
    out["seeds"] = seedsArr;
    mjson::Value resultsJson = mjson::Value::makeObject();
    for (auto& [name, byGraph] : results) {
        mjson::Value gv = mjson::Value::makeObject();
        for (auto& [g, r] : byGraph) {
            if (r.has_value()) {
                mjson::Value pair = mjson::Value::makeArray();
                pair.push_back((long long)r->first);
                pair.push_back((long long)r->second);
                gv[g] = pair;
            } else {
                gv[g] = mjson::Value();
            }
        }
        resultsJson[name] = gv;
    }
    out["results"] = resultsJson;
    saveJson(args.out, out);
    printf("[saved] %s\n", args.out.c_str());
    return 0;
}
