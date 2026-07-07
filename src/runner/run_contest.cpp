// run_contest - GD-2025 k-planarity batch runner. C++ port of
// scripts/run_contest.py.
//
// Methods : sa, sa-warm, sa-stress, staged, staged-adaptive, ils
// Workers : W parallel workers per (graph x method) combo, different seeds
// Timing  : group-based - small graphs get less budget, large graphs more
// Logging : detailed per-combo JSON + a plain results/report.html table,
//           regenerated after every combo. (The Python original also
//           generated an elaborate Chart.js report with live per-worker
//           status badges and per-run convergence charts; that's dropped
//           here in favor of the basic polling web dashboard in
//           src/server, which reads solver status/log files directly.)
//
// Usage:
//   run_contest                                          # prompts: which input set?
//   run_contest --input-set internal-contest --methods sa,staged --workers 1
//   run_contest --graphs-dir /path/to/graphs --warm-start
//   run_contest --report-only

#include "../common/engine.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace engine;

namespace {

std::string nowTimestamp() {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &tmv);
    return buf;
}

std::string isoTimestamp() {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
    return buf;
}

mjson::Value workerToJson(const WorkerResult& w) {
    mjson::Value v = mjson::Value::makeObject();
    v["worker_id"] = w.workerId;
    v["seed"] = (long long)w.seed;
    v["out_path"] = w.outPath;
    v["returncode"] = w.returncode;
    v["wall_clock_sec"] = w.wallClockSec;
    v["initial_k"] = w.initialK.has_value() ? mjson::Value((long long)*w.initialK) : mjson::Value();
    v["initial_totalX"] = w.initialTotalX.has_value() ? mjson::Value((long long)*w.initialTotalX) : mjson::Value();
    v["final_k"] = w.finalK.has_value() ? mjson::Value((long long)*w.finalK) : mjson::Value();
    v["final_totalX"] = w.finalTotalX.has_value() ? mjson::Value((long long)*w.finalTotalX) : mjson::Value();
    v["valid"] = w.valid;
    return v;
}

void writeCsv(const std::vector<mjson::Value>& combos, const std::string& path) {
    std::ofstream f(path);
    f << "graph,nodes,edges,baseline_k,method,n_workers,budget_min,wall_clock_sec,"
         "best_k,best_totalX,best_valid,improvement_pct,new_best_ever,seeds\n";
    auto field = [](const mjson::Value& v, const char* k) -> std::string {
        if (!v.has(k) || v.at(k).isNull()) return "";
        const auto& x = v.at(k);
        if (x.isNum()) return x.dump();
        if (x.type == mjson::Value::BOOL_T) return x.b ? "True" : "False";
        return x.asString();
    };
    for (auto& c : combos) {
        std::string seeds;
        if (c.has("seeds")) {
            for (auto& s : c.at("seeds").asArray()) { if (!seeds.empty()) seeds += ";"; seeds += s.dump(); }
        }
        f << field(c, "graph") << "," << field(c, "nodes") << "," << field(c, "edges") << ","
          << field(c, "baseline_k") << "," << field(c, "method") << "," << field(c, "n_workers") << ","
          << field(c, "budget_min") << "," << field(c, "wall_clock_sec") << "," << field(c, "best_k") << ","
          << field(c, "best_totalX") << "," << field(c, "best_valid") << "," << field(c, "improvement_pct") << ","
          << field(c, "new_best_ever") << "," << seeds << "\n";
    }
}

void writeMd(const std::vector<mjson::Value>& combos, const std::string& path,
             const std::vector<std::string>& methods) {
    std::map<std::string, std::map<std::string, mjson::Value>> byGraph;
    for (auto& c : combos) byGraph[c.at("graph").asString()][c.at("method").asString()] = c;

    std::ostringstream out;
    out << "# Contest Results\n\nLower **k** is better; ties broken by totalX.\n\n";
    out << "| Graph | Nodes | Edges | Baseline k |";
    std::string sep = "|---|---|---|---|";
    for (auto& m : methods) { out << " " << m << " k | " << m << " totalX | workers | time(s) |"; sep += "---|---|---|---|"; }
    out << "\n" << sep << "\n";

    std::vector<std::string> graphs;
    for (auto& kv : byGraph) graphs.push_back(kv.first);
    std::sort(graphs.begin(), graphs.end(), [](auto& a, auto& b) { return a.size() != b.size() ? a.size() < b.size() : a < b; });

    for (auto& g : graphs) {
        auto& row = byGraph[g];
        std::optional<int> bkBest;
        for (auto& [m, c] : row) if (c.has("best_k") && c.at("best_k").isNum()) {
            int k = c.at("best_k").asInt();
            if (!bkBest.has_value() || k < *bkBest) bkBest = k;
        }
        std::string nodes = "?", edges = "?", baseline = "?";
        if (!row.empty()) {
            auto& meta = row.begin()->second;
            if (meta.has("nodes")) nodes = meta.at("nodes").dump();
            if (meta.has("edges")) edges = meta.at("edges").dump();
            if (meta.has("baseline_k") && !meta.at("baseline_k").isNull()) baseline = meta.at("baseline_k").dump();
        }
        out << "| " << g << " | " << nodes << " | " << edges << " | " << baseline << " |";
        for (auto& m : methods) {
            auto it = row.find(m);
            if (it == row.end() || !it->second.has("best_k") || !it->second.at("best_k").isNum()) {
                out << " \xe2\x80\x94 | \xe2\x80\x94 | \xe2\x80\x94 | \xe2\x80\x94 |";
                continue;
            }
            auto& c = it->second;
            int k = c.at("best_k").asInt();
            std::string kstr = (bkBest.has_value() && k == *bkBest) ? ("**" + std::to_string(k) + "**") : std::to_string(k);
            bool validFlag = c.has("best_valid") && c.at("best_valid").asBool();
            std::string tx = c.has("best_totalX") && !c.at("best_totalX").isNull() ? c.at("best_totalX").dump() : "\xe2\x80\x94";
            std::string nw = c.has("n_workers") ? c.at("n_workers").dump() : "?";
            std::string wc = c.has("wall_clock_sec") ? c.at("wall_clock_sec").dump() : "?";
            out << " " << kstr << (validFlag ? "" : " \xe2\x9a\xa0\xef\xb8\x8f") << " | " << tx << " | " << nw << " | " << wc << " |";
        }
        out << "\n";
    }
    std::ofstream f(path);
    f << out.str();
}

// Plain (no-JS) status table, regenerated after every combo. The live
// polling web dashboard (src/server) is the primary place to watch a run;
// this is a static fallback that also works with `file://`.
void writeSimpleReport(const std::string& outRoot, const mjson::Value& history, const mjson::Value& bests) {
    std::map<std::pair<std::string,std::string>, mjson::Value> bestResults;
    if (bests.isObj()) {
        for (auto& [key, val] : bests.asObject()) {
            auto pos = key.find("__");
            if (pos == std::string::npos) continue;
            bestResults[{key.substr(0, pos), key.substr(pos + 2)}] = val;
        }
    }
    std::set<std::string> allGraphs, allMethods;
    std::map<std::string, std::pair<long long,long long>> graphMeta;
    if (history.has("runs")) {
        for (auto& run : history.at("runs").asArray()) {
            if (!run.has("combos")) continue;
            for (auto& c : run.at("combos").asArray()) {
                allGraphs.insert(c.at("graph").asString());
                allMethods.insert(c.at("method").asString());
                if (c.has("nodes")) graphMeta[c.at("graph").asString()] = {c.at("nodes").asLL(), c.at("edges").asLL()};
            }
        }
    }
    std::vector<std::string> graphs(allGraphs.begin(), allGraphs.end());
    std::sort(graphs.begin(), graphs.end(), [](auto& a, auto& b) { return a.size() != b.size() ? a.size() < b.size() : a < b; });
    std::vector<std::string> methods(allMethods.begin(), allMethods.end());

    size_t nRuns = history.has("runs") ? history.at("runs").asArray().size() : 0;

    std::ostringstream html;
    html <<
        "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>GD-2025 / K-PLANARITY — Report</title><style>\n"
        ":root{--bg:#ffffff;--fg:#111111;--muted:#6a6a6a;--line:#111111;--line-soft:#d6d6d6;--blue:#0065bd;\n"
        "  --mono:ui-monospace,\"SF Mono\",\"JetBrains Mono\",\"Fira Code\",Menlo,Consolas,\"Liberation Mono\",monospace;}\n"
        "*{box-sizing:border-box}html,body{margin:0;padding:0}\n"
        "body{background:var(--bg);color:var(--fg);font-family:var(--mono);font-size:13px;line-height:1.55}\n"
        ".wrap{max-width:1100px;margin:0 auto;padding:0 32px 40px}\n"
        "header{border-bottom:3px solid var(--fg);padding:26px 0 16px}\n"
        ".kicker{font-size:11px;letter-spacing:.14em;color:var(--blue);font-weight:700;text-transform:uppercase;margin-bottom:6px}\n"
        "header h1{margin:0;font-size:26px;font-weight:700;letter-spacing:-.01em}\n"
        "header .meta{margin-top:8px;color:var(--muted);font-size:12px}\n"
        "section{padding:24px 0}\n"
        "h2{font-size:11px;text-transform:uppercase;letter-spacing:.12em;font-weight:700;color:var(--muted);\n"
        "   margin:0 0 16px;padding-bottom:8px;border-bottom:1px solid var(--line-soft)}\n"
        ".tbl-scroll{overflow-x:auto}\n"
        "table{width:100%;border-collapse:collapse;font-size:12.5px;min-width:max-content}\n"
        "thead th{text-align:left;font-size:10.5px;text-transform:uppercase;letter-spacing:.08em;color:var(--muted);\n"
        "         border-bottom:1px solid var(--fg);padding:7px 10px 8px;font-weight:700;white-space:nowrap}\n"
        "tbody td{padding:7px 10px;border-bottom:1px solid var(--line-soft);white-space:nowrap}\n"
        "tbody tr:hover td{background:#f6f8fb}\n"
        "th.num,td.num{text-align:right;font-variant-numeric:tabular-nums}\n"
        "td.best{color:var(--blue);font-weight:700}\n"
        "td:first-child,th:first-child{position:sticky;left:0;background:var(--bg)}\n"
        "tbody tr:hover td:first-child{background:#f6f8fb}\n"
        "</style></head><body><div class=\"wrap\">\n"
        "<header><div class=\"kicker\">SAkGD — Contest Tooling</div><h1>GD-2025 / K-PLANARITY</h1>"
        "<div class=\"meta\">" << graphs.size() << " graphs · " << nRuns << " runs · static report, no JS</div></header>\n"
        "<section><h2>Best results</h2><div class=\"tbl-scroll\"><table><thead><tr>"
        "<th>Graph</th><th class=\"num\">Nodes</th><th class=\"num\">Edges</th>";
    for (auto& m : methods) html << "<th class=\"num\">" << m << " k</th><th class=\"num\">" << m << " totalX</th>";
    html << "</tr></thead><tbody>";
    for (auto& g : graphs) {
        auto meta = graphMeta.count(g) ? graphMeta[g] : std::make_pair(0LL, 0LL);
        html << "<tr><td>" << g << "</td><td class=\"num\">" << meta.first << "</td><td class=\"num\">" << meta.second << "</td>";
        std::optional<long long> best;
        for (auto& m : methods) {
            auto it = bestResults.find({g, m});
            if (it != bestResults.end() && it->second.has("k") && !it->second.at("k").isNull()) {
                long long k = it->second.at("k").asLL();
                if (!best.has_value() || k < *best) best = k;
            }
        }
        for (auto& m : methods) {
            auto it = bestResults.find({g, m});
            if (it == bestResults.end() || !it->second.has("k") || it->second.at("k").isNull()) {
                html << "<td class=\"num\">\xe2\x80\x94</td><td class=\"num\">\xe2\x80\x94</td>";
                continue;
            }
            long long k = it->second.at("k").asLL();
            std::string tx = it->second.has("totalX") && !it->second.at("totalX").isNull()
                             ? std::to_string(it->second.at("totalX").asLL()) : "\xe2\x80\x94";
            bool isBest = best.has_value() && k == *best;
            html << "<td class=\"num" << (isBest ? " best" : "") << "\">" << k << "</td><td class=\"num\">" << tx << "</td>";
        }
        html << "</tr>";
    }
    html << "</tbody></table></div></section></div></body></html>";
    std::ofstream f(outRoot + "/report.html");
    f << html.str();
}

struct Args {
    std::string methods = "sa,staged,ils,staged-adaptive";
    std::string inputSet;
    std::string graphsDir;
    std::string only; // comma-separated graph names to keep (blank = all)
    int workers = 2;
    double minutesSmall = 5.0, minutesMedium = 8.0, minutesLarge = 15.0;
    double stagedLnsFrac = 0.3, p1Frac = 0.2;
    int nhSizeCap = 40, nhCands = 30, ilsPerturb = 0;
    int xchgRounds = 1;
    bool xchgHalf = false;
    long long seed = 42;
    std::string outDir; // blank = auto: data/output/<input-set-name>
    std::string runName;
    bool warmStart = false;
    bool reportOnly = false;
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string s = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + s);
            return argv[++i];
        };
        if (s == "--methods") a.methods = next();
        else if (s == "--input-set") a.inputSet = next();
        else if (s == "--graphs-dir") a.graphsDir = next();
        else if (s == "--only") a.only = next();
        else if (s == "--workers") a.workers = std::stoi(next());
        else if (s == "--minutes-small") a.minutesSmall = std::stod(next());
        else if (s == "--minutes-medium") a.minutesMedium = std::stod(next());
        else if (s == "--minutes-large") a.minutesLarge = std::stod(next());
        else if (s == "--staged-lns-frac") a.stagedLnsFrac = std::stod(next());
        else if (s == "--p1-frac") a.p1Frac = std::stod(next());
        else if (s == "--nh-size-cap") a.nhSizeCap = std::stoi(next());
        else if (s == "--nh-cands") a.nhCands = std::stoi(next());
        else if (s == "--ils-perturb") a.ilsPerturb = std::stoi(next());
        else if (s == "--xchg-rounds") a.xchgRounds = std::stoi(next());
        else if (s == "--xchg-half") a.xchgHalf = true;
        else if (s == "--seed") a.seed = std::stoll(next());
        else if (s == "--out-dir") a.outDir = next();
        else if (s == "--run-name") a.runName = next();
        else if (s == "--warm-start") a.warmStart = true;
        else if (s == "--report-only") a.reportOnly = true;
        else throw std::runtime_error("unknown argument: " + s);
    }
    return a;
}

} // namespace

int main(int argc, char** argv) {
    // stdout is fully-buffered (not line-buffered) once redirected to a file
    // (e.g. by the web server) - force line buffering so live log tailing
    // actually shows progress instead of only appearing at exit.
    setvbuf(stdout, nullptr, _IOLBF, 0);

    Args args;
    try {
        args = parseArgs(argc, argv);
    } catch (std::exception& e) {
        fprintf(stderr, "run_contest: %s\n", e.what());
        return 2;
    }

    std::map<std::string,double> minutesMap = {
        {"small", args.minutesSmall}, {"medium", args.minutesMedium}, {"large", args.minutesLarge}};

    if (args.reportOnly) {
        // No graphs to resolve here - just regenerate from whatever's on disk.
        // --input-set/--graphs-dir/--out-dir all still work to pick which
        // per-set output folder to report on.
        std::string label = !args.graphsDir.empty() ? pathBasename(args.graphsDir) : args.inputSet;
        std::string outDir = !args.outDir.empty() ? args.outDir
                             : label.empty() ? "data/output" : ("data/output/" + label);
        std::string outRoot = joinPath(ROOT(), outDir);
        mjson::Value emptyHistory = mjson::Value::makeObject();
        emptyHistory["runs"] = mjson::Value::makeArray();
        mjson::Value history = loadJsonDefault(outRoot + "/history.json", emptyHistory);
        mjson::Value bests = loadJsonDefault(outRoot + "/bests.json", mjson::Value::makeObject());
        writeSimpleReport(outRoot, history, bests);
        return 0;
    }

    auto methods = splitCsv(args.methods);
    for (auto& m : methods) {
        if (!methodById(m)) {
            fprintf(stderr, "Unknown method '%s'. Choose from: sa, sa-warm, sa-stress, staged, "
                            "staged-adaptive, ils\n", m.c_str());
            return 1;
        }
    }

    ensureBinaries();
    std::string graphsDir;
    try {
        graphsDir = resolveGraphsDir(args.inputSet, args.graphsDir);
    } catch (std::exception& e) {
        fprintf(stderr, "run_contest: %s\n", e.what());
        return 2;
    }

    // Nest output under data/output/<input-set-name> by default (mirrors
    // data/input/<set>/) so different sets never collide on graph names.
    std::string outDir = !args.outDir.empty() ? args.outDir : ("data/output/" + pathBasename(graphsDir));
    std::string outRoot = joinPath(ROOT(), outDir);
    mkdirs(outRoot);

    mjson::Value emptyHistory = mjson::Value::makeObject();
    emptyHistory["runs"] = mjson::Value::makeArray();
    mjson::Value history = loadJsonDefault(outRoot + "/history.json", emptyHistory);
    mjson::Value bests = loadJsonDefault(outRoot + "/bests.json", mjson::Value::makeObject());

    auto graphEntries = scanGraphDir(graphsDir);
    if (!args.only.empty()) {
        auto keep = splitCsv(args.only);
        std::set<std::string> keepSet(keep.begin(), keep.end());
        graphEntries.erase(std::remove_if(graphEntries.begin(), graphEntries.end(),
                            [&](auto& e) { return !keepSet.count(e.name); }),
                           graphEntries.end());
    }
    if (graphEntries.empty()) {
        fprintf(stderr, "run_contest: no graphs found in %s\n", graphsDir.c_str());
        return 1;
    }
    writeSimpleReport(outRoot, history, bests);

    std::string runId = nowTimestamp();
    std::string runDir = outRoot + "/runs/" + runId;
    mkdirs(runDir);

    double totalEst = 0;
    for (auto& e : graphEntries) totalEst += budgetForSize(e.n, e.m, minutesMap);
    totalEst *= methods.size();
    printf("[run] id=%s\n", runId.c_str());
    printf("[run] input set: %s\n", graphsDir.c_str());
    printf("[run] graphs=%zu  methods=%zu  workers=%d\n", graphEntries.size(), methods.size(), args.workers);
    printf("[run] budgets: small=%.0fm  medium=%.0fm  large=%.0fm\n",
           args.minutesSmall, args.minutesMedium, args.minutesLarge);
    printf("[run] estimated wall-clock: %.0f min (%.1fh)\n\n", totalEst, totalEst / 60.0);

    std::vector<mjson::Value> allCombos;
    std::vector<std::string> gnames;

    for (auto& entry : graphEntries) {
        const std::string& gpathOrig = entry.path;
        const std::string& gname = entry.name;
        gnames.push_back(gname);
        int n = entry.n, m = entry.m;
        double budget = budgetForSize(n, m, minutesMap);
        int nhSize = std::max(3, std::min(n / 10, args.nhSizeCap));
        std::string gOutDir = runDir + "/" + gname;
        mkdirs(gOutDir);

        if (n > 3000) printf("[note] %s has %d nodes - initial setup is heavy; giving %.0f min per combo.\n",
                             gname.c_str(), n, budget);
        printf("=== %s  (n=%d, m=%d, budget=%.0fmin) ===\n", gname.c_str(), n, m, budget);

        std::optional<int> baselineK;

        for (auto& method : methods) {
            std::string warmPath;
            if (args.warmStart) {
                std::string key = bestKey(gname, method);
                if (bests.has(key) && bests[key].has("layout_path")) {
                    std::string lp = bests[key]["layout_path"].asString();
                    if (fileExists(lp)) { warmPath = lp; printf("  [%s] warm-starting from %s\n", method.c_str(), lp.c_str()); }
                }
            }

            auto combo = runCombo(method, gpathOrig, budget, args.p1Frac, args.seed, gOutDir, args.workers,
                                   nhSize, args.nhCands, args.stagedLnsFrac, args.ilsPerturb,
                                   warmPath, args.xchgRounds, args.xchgHalf);

            for (auto& w : combo.workers) if (!baselineK.has_value() && w.initialK.has_value()) baselineK = w.initialK;

            std::optional<int> bestK = combo.best ? combo.best->finalK : std::nullopt;
            std::optional<int> bestTx = combo.best ? combo.best->finalTotalX : std::nullopt;
            bool valid = combo.best ? combo.best->valid : false;
            std::string bestOut = combo.best ? combo.best->outPath : "";

            std::optional<double> improv;
            if (bestK.has_value() && baselineK.has_value() && *baselineK > 0)
                improv = std::round((*baselineK - *bestK) / (double)*baselineK * 1000.0) / 10.0;

            bool newBest = updateBest(outRoot, bests, gname, method, bestK, bestTx, bestOut, runId,
                                      budget, args.workers, std::round(combo.wallTotal * 10.0) / 10.0);

            std::string improvStr;
            if (improv.has_value()) {
                char b[64];
                snprintf(b, sizeof(b), "  (%+.1f%% vs baseline)", *improv);
                improvStr = b;
            }
            printf("  %-16s  k=%s  totalX=%s  wall=%.1fs  workers=%zu%s%s%s\n",
                   method.c_str(),
                   bestK.has_value() ? std::to_string(*bestK).c_str() : "None",
                   bestTx.has_value() ? std::to_string(*bestTx).c_str() : "None",
                   combo.wallTotal, combo.workers.size(),
                   valid ? "" : "  [INVALID]",
                   improvStr.c_str(),
                   newBest ? "  *** NEW BEST ***" : "");

            mjson::Value rec = mjson::Value::makeObject();
            rec["graph"] = gname; rec["nodes"] = n; rec["edges"] = m; rec["method"] = method;
            rec["budget_min"] = budget; rec["n_workers"] = args.workers;
            mjson::Value seeds = mjson::Value::makeArray();
            for (auto& w : combo.workers) seeds.push_back(mjson::Value((long long)w.seed));
            rec["seeds"] = seeds;
            rec["wall_clock_sec"] = std::round(combo.wallTotal * 10.0) / 10.0;
            rec["baseline_k"] = baselineK.has_value() ? mjson::Value((long long)*baselineK) : mjson::Value();
            rec["best_k"] = bestK.has_value() ? mjson::Value((long long)*bestK) : mjson::Value();
            rec["best_totalX"] = bestTx.has_value() ? mjson::Value((long long)*bestTx) : mjson::Value();
            rec["best_valid"] = valid;
            rec["improvement_pct"] = improv.has_value() ? mjson::Value(*improv) : mjson::Value();
            rec["new_best_ever"] = newBest;
            mjson::Value detail = mjson::Value::makeArray();
            for (auto& w : combo.workers) detail.push_back(workerToJson(w));
            rec["workers_detail"] = detail;
            allCombos.push_back(rec);

            saveJson(outRoot + "/bests.json", bests);
            mjson::Value h2 = loadJsonDefault(outRoot + "/history.json", emptyHistory);
            bool found = false;
            mjson::Value combosArr = mjson::Value::makeArray();
            for (auto& c : allCombos) combosArr.push_back(c);
            for (auto& run : h2["runs"].arr) {
                if (run.has("id") && run["id"].asString() == runId) { run["combos"] = combosArr; found = true; break; }
            }
            if (!found) {
                mjson::Value entry = mjson::Value::makeObject();
                entry["id"] = runId; entry["name"] = args.runName; entry["timestamp"] = isoTimestamp();
                mjson::Value methodsArr = mjson::Value::makeArray();
                for (auto& mm : methods) methodsArr.push_back(mm);
                entry["methods"] = methodsArr;
                entry["workers"] = args.workers;
                mjson::Value graphsArr = mjson::Value::makeArray();
                for (auto& gg : gnames) graphsArr.push_back(gg);
                entry["graphs"] = graphsArr;
                mjson::Value mm2 = mjson::Value::makeObject();
                mm2["small"] = args.minutesSmall; mm2["medium"] = args.minutesMedium; mm2["large"] = args.minutesLarge;
                entry["minutes_map"] = mm2;
                entry["combos"] = combosArr;
                h2["runs"].arr.push_back(entry);
            }
            saveJson(outRoot + "/history.json", h2);
            history = h2;
            writeSimpleReport(outRoot, history, bests);
        }
        printf("\n");
    }

    writeCsv(allCombos, runDir + "/summary.csv");
    writeMd(allCombos, runDir + "/summary.md", methods);

    mjson::Value detail = mjson::Value::makeObject();
    detail["run_id"] = runId; detail["name"] = args.runName; detail["timestamp"] = isoTimestamp();
    mjson::Value cfg = mjson::Value::makeObject();
    mjson::Value methodsArr = mjson::Value::makeArray();
    for (auto& mm : methods) methodsArr.push_back(mm);
    cfg["methods"] = methodsArr; cfg["workers"] = args.workers;
    cfg["minutes_small"] = args.minutesSmall; cfg["minutes_medium"] = args.minutesMedium;
    cfg["minutes_large"] = args.minutesLarge; cfg["seed"] = (long long)args.seed; cfg["warm_start"] = args.warmStart;
    detail["config"] = cfg;
    mjson::Value combosArr2 = mjson::Value::makeArray();
    for (auto& c : allCombos) combosArr2.push_back(c);
    detail["combos"] = combosArr2;
    saveJson(runDir + "/detailed.json", detail);

    printf("[done] %s\n", runDir.c_str());
    printf("[done] open %s/report.html in a browser\n", outRoot.c_str());
    return 0;
}
