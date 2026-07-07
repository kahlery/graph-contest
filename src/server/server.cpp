// server - lightweight control-panel HTTP server for the contest tooling.
// C++ port of scripts/server.py + scripts/dashboard.py + dashboard/index.html.
//
// Scope note: replaced with a deliberately basic light-mode page that polls
// /api/status every 3s (per explicit request), rather than the Python
// original's live per-worker log-scraping dashboard. Serves one page with a
// start/stop control for either the batch runner (run_contest) or the
// contest-day orchestrator (contest_orchestrate), and tails whichever one is
// currently running.
//
// Usage: server [--port 8080]

#include "../common/engine.hpp"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <tuple>
#include <unistd.h>
#include <vector>

using namespace engine;

namespace {

// --------------------------------------------------------------------- //
// Minimal blocking HTTP/1.1 server: one request per connection, small
// bodies only (JSON control messages). Adequate for a local control panel.
// --------------------------------------------------------------------- //
struct HttpRequest { std::string method, path, body; };

bool readRequest(int fd, HttpRequest& req) {
    std::string buf;
    char chunk[4096];
    ssize_t n;
    size_t headerEnd = std::string::npos;
    while ((n = recv(fd, chunk, sizeof(chunk), 0)) > 0) {
        buf.append(chunk, n);
        headerEnd = buf.find("\r\n\r\n");
        if (headerEnd != std::string::npos) break;
        if (buf.size() > 1 << 20) return false;
    }
    if (headerEnd == std::string::npos) return false;

    std::istringstream head(buf.substr(0, headerEnd));
    std::string line;
    std::getline(head, line);
    std::istringstream reqline(line);
    reqline >> req.method >> req.path;

    size_t contentLength = 0;
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        for (auto& c : key) c = tolower((unsigned char)c);
        if (key == "content-length") contentLength = std::stoul(trim(line.substr(pos + 1)));
    }
    size_t haveBody = buf.size() - (headerEnd + 4);
    std::string body = buf.substr(headerEnd + 4);
    while (haveBody < contentLength && (n = recv(fd, chunk, sizeof(chunk), 0)) > 0) {
        body.append(chunk, n);
        haveBody += n;
    }
    req.body = body;
    return true;
}

void sendResponse(int fd, int code, const std::string& contentType, const std::string& body) {
    const char* statusText = (code == 200) ? "OK" : (code == 404) ? "Not Found" : "Error";
    std::ostringstream head;
    head << "HTTP/1.1 " << code << " " << statusText << "\r\n"
         << "Content-Type: " << contentType << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Connection: close\r\n\r\n";
    std::string h = head.str();
    send(fd, h.data(), h.size(), 0);
    send(fd, body.data(), body.size(), 0);
}

// --------------------------------------------------------------------- //
// Run state: at most one active batch/orchestrator child at a time.
// --------------------------------------------------------------------- //
struct RunState {
    std::mutex mu;
    bool running = false;
    std::string mode;      // "batch" | "orchestrator"
    std::string outDir;
    std::string logPath;
    std::chrono::steady_clock::time_point startedAt;
    double estTotalSec = 0; // 0 = unknown; see apiStart's estimate for each mode
    std::unique_ptr<proc::Child> child;
};
RunState g_run;

std::string tailFile(const std::string& path, int maxLines = 60) {
    std::ifstream f(path);
    if (!f) return "";
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) {
        lines.push_back(line);
        if ((int)lines.size() > maxLines) lines.erase(lines.begin());
    }
    std::ostringstream out;
    for (auto& l : lines) out << l << "\n";
    return out.str();
}

mjson::Value apiStatus() {
    mjson::Value v = mjson::Value::makeObject();
    std::lock_guard<std::mutex> lk(g_run.mu);
    bool running = g_run.running;
    if (running && g_run.child) {
        int code;
        if (g_run.child->poll(code)) running = false; // finished since last check
    }
    v["running"] = running;
    v["mode"] = g_run.mode;
    v["out_dir"] = g_run.outDir;
    double elapsed = g_run.startedAt.time_since_epoch().count()
                    ? std::chrono::duration<double>(std::chrono::steady_clock::now() - g_run.startedAt).count()
                    : 0.0;
    v["elapsed_sec"] = g_run.mode.empty() ? mjson::Value() : mjson::Value(std::round(elapsed * 10.0) / 10.0);
    v["remaining_sec"] = (!g_run.mode.empty() && g_run.estTotalSec > 0)
                        ? mjson::Value(std::round(std::max(0.0, g_run.estTotalSec - elapsed) * 10.0) / 10.0)
                        : mjson::Value();
    v["log_tail"] = g_run.logPath.empty() ? "" : tailFile(g_run.logPath);
    g_run.running = running;

    if (!g_run.outDir.empty()) {
        auto bests = loadJsonDefault(g_run.outDir + "/bests.json", mjson::Value::makeObject());
        v["bests"] = bests;
    }
    return v;
}

mjson::Value apiMethods() {
    mjson::Value arr = mjson::Value::makeArray();
    for (auto& m : METHODS()) {
        mjson::Value o = mjson::Value::makeObject();
        o["id"] = m.id; o["label"] = m.label;
        arr.push_back(o);
    }
    return arr;
}

mjson::Value apiInputSets() {
    mjson::Value arr = mjson::Value::makeArray();
    for (auto& s : listInputSets()) {
        mjson::Value o = mjson::Value::makeObject();
        o["name"] = s;
        o["count"] = (long long)scanGraphDir(inputSetDir(s)).size();
        arr.push_back(o);
    }
    return arr;
}

mjson::Value apiGraphs(const std::string& inputSet) {
    mjson::Value arr = mjson::Value::makeArray();
    if (inputSet.empty()) return arr;
    for (auto& e : scanGraphDir(inputSetDir(inputSet))) {
        mjson::Value o = mjson::Value::makeObject();
        o["name"] = e.name; o["n"] = e.n; o["m"] = e.m;
        arr.push_back(o);
    }
    return arr;
}

// Splits "/api/graphs?input_set=foo" into ("/api/graphs", {input_set: foo}).
std::pair<std::string, std::map<std::string,std::string>> splitQuery(const std::string& path) {
    auto pos = path.find('?');
    if (pos == std::string::npos) return {path, {}};
    std::map<std::string,std::string> params;
    std::string base = path.substr(0, pos);
    std::string query = path.substr(pos + 1);
    size_t start = 0;
    while (start <= query.size()) {
        size_t amp = query.find('&', start);
        std::string kv = (amp == std::string::npos) ? query.substr(start) : query.substr(start, amp - start);
        start = (amp == std::string::npos) ? query.size() + 1 : amp + 1;
        auto eq = kv.find('=');
        if (eq == std::string::npos) continue;
        params[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    return {base, params};
}

mjson::Value apiStart(const mjson::Value& cfg) {
    mjson::Value res = mjson::Value::makeObject();
    std::lock_guard<std::mutex> lk(g_run.mu);
    if (g_run.running) { res["error"] = "a run is already active"; return res; }

    std::string mode = cfg.getStr("mode", "batch");
    std::string graphsDirField = cfg.getStr("graphs_dir", "");
    std::string inputSetField = cfg.getStr("input_set", "");
    std::string only = cfg.getStr("only", "");

    // Nest output under data/output/<input-set-name> by default (mirrors
    // data/input/<set>/), same auto-naming as the CLI tools.
    std::string label = !graphsDirField.empty() ? pathBasename(graphsDirField) : inputSetField;
    std::string outDirField = cfg.getStr("out_dir", "");
    std::string outDir = joinPath(ROOT(), outDirField.empty()
        ? (label.empty() ? "data/output" : ("data/output/" + label)) : outDirField);
    mkdirs(outDir);
    std::string logPath = outDir + "/server_run.log";

    std::vector<std::string> args;
    double estTotalSec = 0;
    if (mode == "orchestrator") {
        // The orchestrator is designed to never overrun this budget, so it
        // doubles as our total-wall-clock estimate for "time left".
        double budgetSec = cfg.getDouble("budget_sec", 2700.0);
        args = {ROOT() + "/bin/contest_orchestrate",
                "--budget", std::to_string(budgetSec),
                "--workers", std::to_string((long long)cfg.getLL("workers", 8)),
                "--out-dir", outDir,
                "--seed", std::to_string((long long)cfg.getLL("seed", 1))};
        if (!graphsDirField.empty()) { args.push_back("--graphs-dir"); args.push_back(graphsDirField); }
        else {
            args.push_back("--input-set"); args.push_back(inputSetField);
            if (!only.empty()) { args.push_back("--only"); args.push_back(only); }
        }
        estTotalSec = budgetSec;
    } else {
        std::string methodsField = cfg.getStr("methods", "sa,sa-stress");
        double minutesSmall = cfg.getDouble("minutes_small", 5.0);
        double minutesMedium = cfg.getDouble("minutes_medium", 8.0);
        double minutesLarge = cfg.getDouble("minutes_large", 15.0);
        args = {ROOT() + "/bin/run_contest",
                "--methods", methodsField,
                "--input-set", inputSetField,
                "--workers", std::to_string((long long)cfg.getLL("workers", 2)),
                "--minutes-small", std::to_string(minutesSmall),
                "--minutes-medium", std::to_string(minutesMedium),
                "--minutes-large", std::to_string(minutesLarge),
                "--seed", std::to_string((long long)cfg.getLL("seed", 42)),
                "--out-dir", outDir};
        if (!only.empty()) { args.push_back("--only"); args.push_back(only); }

        // Mirrors run_contest's own totalEst calc (sum of per-graph size
        // budgets x method count) so /api/status can report time left.
        try {
            std::string gdir = !graphsDirField.empty() ? graphsDirField : inputSetDir(inputSetField);
            auto entries = scanGraphDir(gdir);
            if (!only.empty()) {
                auto keep = splitCsv(only);
                std::set<std::string> keepSet(keep.begin(), keep.end());
                entries.erase(std::remove_if(entries.begin(), entries.end(),
                              [&](auto& e) { return !keepSet.count(e.name); }), entries.end());
            }
            std::map<std::string,double> minutesMap = {
                {"small", minutesSmall}, {"medium", minutesMedium}, {"large", minutesLarge}};
            double totalMin = 0;
            for (auto& e : entries) totalMin += budgetForSize(e.n, e.m, minutesMap);
            estTotalSec = totalMin * 60.0 * (double)splitCsv(methodsField).size();
        } catch (...) { estTotalSec = 0; }
    }

    try {
        g_run.child = std::make_unique<proc::Child>(proc::Child::spawn(args, ROOT(), logPath, logPath));
    } catch (std::exception& e) {
        res["error"] = e.what();
        return res;
    }
    g_run.running = true;
    g_run.mode = mode;
    g_run.outDir = outDir;
    g_run.logPath = logPath;
    g_run.startedAt = std::chrono::steady_clock::now();
    g_run.estTotalSec = estTotalSec;
    res["ok"] = true;
    return res;
}

mjson::Value apiStop() {
    mjson::Value res = mjson::Value::makeObject();
    std::lock_guard<std::mutex> lk(g_run.mu);
    if (g_run.running && g_run.child) {
        g_run.child->killIfRunning();
        g_run.running = false;
        res["ok"] = true;
    } else {
        res["error"] = "no active run";
    }
    return res;
}

// --------------------------------------------------------------------- //
// GUI: the control panel (gui/index.html) is a real file under gui/, not
// an embedded string or per-output-set artifact. Its full-report section
// is rendered client-side from GET /api/report, which is computed on
// demand from a given input set's bests.json/history.json.
// --------------------------------------------------------------------- //
std::string readFileOr(const std::string& path, const std::string& fallback) {
    try { return mjson::slurp(path); } catch (...) { return fallback; }
}

std::string contentTypeFor(const std::string& path) {
    auto endsWith = [&](const char* ext) {
        size_t n = strlen(ext);
        return path.size() >= n && path.compare(path.size() - n, n, ext) == 0;
    };
    if (endsWith(".css"))  return "text/css";
    if (endsWith(".html")) return "text/html; charset=utf-8";
    if (endsWith(".js"))   return "application/javascript";
    if (endsWith(".json")) return "application/json";
    return "application/octet-stream";
}

// Builds the full-report JSON for one input set from its bests.json +
// history.json (mirrors run_contest's console/CSV summary). Consumed by
// gui/index.html, which renders it into the page's "Full report" table.
mjson::Value apiReport(const std::string& setName) {
    std::string outDir = !setName.empty() ? joinPath(ROOT(), "data/output/" + setName)
                                           : joinPath(ROOT(), "data/output");
    mjson::Value emptyHistory = mjson::Value::makeObject();
    emptyHistory["runs"] = mjson::Value::makeArray();
    mjson::Value history = loadJsonDefault(outDir + "/history.json", emptyHistory);
    mjson::Value bests = loadJsonDefault(outDir + "/bests.json", mjson::Value::makeObject());

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
    std::map<std::tuple<std::string,std::string,std::string>, mjson::Value> comboLookup;
    if (history.has("runs")) {
        for (auto& run : history.at("runs").asArray()) {
            std::string runId = run.has("id") ? run.at("id").asString() : "";
            if (!run.has("combos")) continue;
            for (auto& c : run.at("combos").asArray()) {
                std::string g = c.at("graph").asString(), m = c.at("method").asString();
                allGraphs.insert(g); allMethods.insert(m);
                if (c.has("nodes")) graphMeta[g] = {c.at("nodes").asLL(), c.at("edges").asLL()};
                comboLookup[{g, m, runId}] = c;
            }
        }
    }
    std::vector<std::string> graphs(allGraphs.begin(), allGraphs.end());
    std::sort(graphs.begin(), graphs.end(), [](auto& a, auto& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    std::vector<std::string> methods(allMethods.begin(), allMethods.end());
    size_t nRuns = history.has("runs") ? history.at("runs").asArray().size() : 0;

    mjson::Value out = mjson::Value::makeObject();
    out["input_set"] = setName;
    std::ostringstream meta; meta << graphs.size() << " graphs \xc2\xb7 " << nRuns << " runs";
    out["meta"] = meta.str();

    mjson::Value methodsArr = mjson::Value::makeArray();
    for (auto& m : methods) methodsArr.push_back(mjson::Value(m));
    out["methods"] = methodsArr;

    mjson::Value graphsArr = mjson::Value::makeArray();
    for (auto& g : graphs) {
        auto gm = graphMeta.count(g) ? graphMeta[g] : std::make_pair(0LL, 0LL);
        std::optional<long long> best;
        for (auto& m : methods) {
            auto it = bestResults.find({g, m});
            if (it != bestResults.end() && it->second.has("k") && !it->second.at("k").isNull()) {
                long long k = it->second.at("k").asLL();
                if (!best.has_value() || k < *best) best = k;
            }
        }
        mjson::Value row = mjson::Value::makeObject();
        row["graph"] = g;
        row["nodes"] = gm.first;
        row["edges"] = gm.second;
        row["best_k"] = best.has_value() ? mjson::Value(*best) : mjson::Value();

        mjson::Value results = mjson::Value::makeObject();
        for (auto& m : methods) {
            auto it = bestResults.find({g, m});
            mjson::Value cell = mjson::Value::makeObject();
            if (it == bestResults.end() || !it->second.has("k") || it->second.at("k").isNull()) {
                cell["k"] = mjson::Value();
                cell["totalX"] = mjson::Value();
                cell["initK"] = mjson::Value();
                cell["sec"] = mjson::Value();
                cell["workers"] = mjson::Value();
            } else {
                const mjson::Value& b = it->second;
                cell["k"] = b.at("k").asLL();
                cell["totalX"] = (b.has("totalX") && !b.at("totalX").isNull()) ? b.at("totalX") : mjson::Value();
                cell["sec"] = (b.has("wall_clock_sec") && !b.at("wall_clock_sec").isNull()) ? b.at("wall_clock_sec") : mjson::Value();
                cell["workers"] = (b.has("n_workers") && !b.at("n_workers").isNull()) ? b.at("n_workers") : mjson::Value();
                std::string runId = b.has("run_id") ? b.at("run_id").asString() : "";
                auto cit = comboLookup.find({g, m, runId});
                cell["initK"] = (cit != comboLookup.end() && cit->second.has("baseline_k") && !cit->second.at("baseline_k").isNull())
                                 ? mjson::Value(cit->second.at("baseline_k").asLL()) : mjson::Value();
            }
            results[m] = cell;
        }
        row["results"] = results;
        graphsArr.push_back(row);
    }
    out["graphs"] = graphsArr;
    return out;
}

} // namespace

int main(int argc, char** argv) {
    int port = 8080;
    for (int i = 1; i < argc; i++) {
        std::string s = argv[i];
        if (s == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }
    // Without this, every child we spawn (run_contest, contest_orchestrate, ...)
    // inherits this fd across fork() and keeps the port bound for its whole
    // lifetime even after the server itself exits.
    fcntl(server_fd, F_SETFD, FD_CLOEXEC);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(server_fd, 16) < 0) { perror("listen"); return 1; }

    printf("[server] listening on http://localhost:%d\n", port);

    while (true) {
        int fd = accept(server_fd, nullptr, nullptr);
        if (fd < 0) continue;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        HttpRequest req;
        if (!readRequest(fd, req)) { close(fd); continue; }
        auto [path, query] = splitQuery(req.path);

        if (req.method == "GET" && (path == "/" || path == "/index.html")) {
            sendResponse(fd, 200, "text/html; charset=utf-8",
                        readFileOr(ROOT() + "/gui/index.html", "<html><body>missing gui/index.html</body></html>"));
        } else if (req.method == "GET" && path.rfind("/gui/", 0) == 0 && path.find("..") == std::string::npos) {
            std::string assetPath = ROOT() + path;
            std::ifstream f(assetPath, std::ios::binary);
            if (!f) sendResponse(fd, 404, "text/plain", "not found");
            else {
                std::ostringstream ss; ss << f.rdbuf();
                sendResponse(fd, 200, contentTypeFor(path), ss.str());
            }
        } else if (req.method == "GET" && path == "/api/status") {
            sendResponse(fd, 200, "application/json", apiStatus().dump());
        } else if (req.method == "GET" && path == "/api/methods") {
            sendResponse(fd, 200, "application/json", apiMethods().dump());
        } else if (req.method == "GET" && path == "/api/input-sets") {
            sendResponse(fd, 200, "application/json", apiInputSets().dump());
        } else if (req.method == "GET" && path == "/api/graphs") {
            auto it = query.find("input_set");
            sendResponse(fd, 200, "application/json", apiGraphs(it == query.end() ? "" : it->second).dump());
        } else if (req.method == "GET" && path == "/api/report") {
            // Rendered on demand from bests.json + history.json - no HTML
            // file is ever written into data/output/<set>/; the GUI lives
            // solely under gui/ (see apiReport, consumed by gui/index.html).
            std::string setName;
            auto setIt = query.find("input_set");
            if (setIt != query.end() && !setIt->second.empty()) setName = setIt->second;
            else { std::lock_guard<std::mutex> lk(g_run.mu); setName = pathBasename(g_run.outDir); }

            sendResponse(fd, 200, "application/json", apiReport(setName).dump());
        } else if (req.method == "POST" && path == "/api/start") {
            mjson::Value cfg;
            try { cfg = req.body.empty() ? mjson::Value::makeObject() : mjson::parse(req.body); }
            catch (...) { cfg = mjson::Value::makeObject(); }
            sendResponse(fd, 200, "application/json", apiStart(cfg).dump());
        } else if (req.method == "POST" && path == "/api/stop") {
            sendResponse(fd, 200, "application/json", apiStop().dump());
        } else {
            sendResponse(fd, 404, "text/plain", "not found");
        }
        close(fd);
    }
    return 0;
}
