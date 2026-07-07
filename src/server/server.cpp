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
#include <sstream>
#include <string>
#include <sys/socket.h>
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
    if (mode == "orchestrator") {
        args = {ROOT() + "/bin/contest_orchestrate",
                "--budget", std::to_string(cfg.getDouble("budget_sec", 2700.0)),
                "--workers", std::to_string((long long)cfg.getLL("workers", 8)),
                "--out-dir", outDir,
                "--seed", std::to_string((long long)cfg.getLL("seed", 1))};
        if (!graphsDirField.empty()) { args.push_back("--graphs-dir"); args.push_back(graphsDirField); }
        else {
            args.push_back("--input-set"); args.push_back(inputSetField);
            if (!only.empty()) { args.push_back("--only"); args.push_back(only); }
        }
    } else {
        args = {ROOT() + "/bin/run_contest",
                "--methods", cfg.getStr("methods", "sa,sa-stress"),
                "--input-set", inputSetField,
                "--workers", std::to_string((long long)cfg.getLL("workers", 2)),
                "--minutes-small", std::to_string(cfg.getDouble("minutes_small", 5.0)),
                "--minutes-medium", std::to_string(cfg.getDouble("minutes_medium", 8.0)),
                "--minutes-large", std::to_string(cfg.getDouble("minutes_large", 15.0)),
                "--seed", std::to_string((long long)cfg.getLL("seed", 42)),
                "--out-dir", outDir};
        if (!only.empty()) { args.push_back("--only"); args.push_back(only); }
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

const char* INDEX_HTML = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>GD-2025 / K-PLANARITY</title>
<style>
:root {
  --bg:#ffffff; --fg:#111111; --muted:#6a6a6a; --line:#111111; --line-soft:#d6d6d6;
  --blue:#0065bd; --blue-dark:#003359;
  --mono: ui-monospace, "SF Mono", "JetBrains Mono", "Fira Code", Menlo, Consolas, "Liberation Mono", monospace;
}
* { box-sizing:border-box; }
html, body { margin:0; padding:0; }
body { background:var(--bg); color:var(--fg); font-family:var(--mono); font-size:13px; line-height:1.55; }
.wrap { max-width:1100px; margin:0 auto; padding:0 32px; }
a { color:var(--blue); }

header { border-bottom:3px solid var(--fg); padding:26px 0 16px; }
.kicker { font-size:11px; letter-spacing:.14em; color:var(--blue); font-weight:700; text-transform:uppercase; margin-bottom:6px; }
header h1 { margin:0; font-size:26px; font-weight:700; letter-spacing:-.01em; }
header .meta { margin-top:8px; color:var(--muted); font-size:12px; }

nav.modes { display:flex; border-bottom:1px solid var(--line); }
nav.modes button {
  font-family:var(--mono); font-size:11.5px; letter-spacing:.08em; text-transform:uppercase; font-weight:700;
  background:none; border:none; border-right:1px solid var(--line); padding:12px 22px; cursor:pointer; color:var(--muted);
}
nav.modes button.active { color:#fff; background:var(--blue); }
nav.modes button:hover:not(.active) { color:var(--fg); }

.grid { display:grid; grid-template-columns:1fr 1fr; border-bottom:1px solid var(--line); }
.panel { padding:22px 0; }
.panel:first-child { padding-right:28px; border-right:1px solid var(--line); }
.panel:last-child { padding-left:28px; }

h2 {
  font-size:11px; text-transform:uppercase; letter-spacing:.12em; font-weight:700; color:var(--muted);
  margin:0 0 16px; padding-bottom:8px; border-bottom:1px solid var(--line-soft);
}

.field { margin-bottom:14px; }
.field label { display:block; font-size:11px; text-transform:uppercase; letter-spacing:.06em; color:var(--muted); margin-bottom:4px; }
.field input:not([type=checkbox]), .field select {
  width:100%; font-family:var(--mono); font-size:13px; padding:7px 8px;
  border:1px solid var(--fg); background:#fff; color:var(--fg); border-radius:0; appearance:none;
}
.field input:focus, .field select:focus { outline:2px solid var(--blue); outline-offset:-1px; }
.row2 { display:grid; grid-template-columns:1fr 1fr; gap:12px; }
.row3 { display:grid; grid-template-columns:1fr 1fr 1fr; gap:12px; }

.chips { display:flex; flex-wrap:wrap; gap:6px; }
.chip { border:1px solid var(--fg); padding:5px 10px; font-size:12px; cursor:pointer; user-select:none; }
.chip.on { background:var(--blue); border-color:var(--blue); color:#fff; }
.chip:hover:not(.on) { background:#f2f2f2; }

.graphlist { border:1px solid var(--fg); margin-top:8px; max-height:170px; overflow-y:auto; }
.graphlist label {
  display:flex; align-items:center; gap:8px; padding:5px 8px; font-size:12px; cursor:pointer;
  border-bottom:1px solid var(--line-soft);
}
.graphlist label:last-child { border-bottom:none; }
.graphlist label:hover { background:#f6f8fb; }
.graphlist input[type=checkbox] { width:auto; accent-color:var(--blue); margin:0; }
.graphlist .gname { flex:1; }
.graphlist .gmeta { color:var(--muted); font-size:10.5px; }
.graphlist-actions { display:flex; justify-content:space-between; align-items:center; margin-top:6px; }
.linkbtn {
  background:none; border:none; font-family:var(--mono); font-size:11px; text-transform:uppercase;
  letter-spacing:.05em; color:var(--blue); cursor:pointer; padding:0;
}
.linkbtn:hover { color:var(--blue-dark); }

.btnrow { display:flex; gap:10px; margin-top:20px; }
button.action {
  font-family:var(--mono); font-weight:700; letter-spacing:.05em; text-transform:uppercase; font-size:12px;
  padding:10px 22px; border:1px solid var(--fg); background:var(--fg); color:#fff; cursor:pointer; border-radius:0;
}
button.action.stop { background:#fff; color:var(--fg); }
button.action:disabled { opacity:.35; cursor:not-allowed; }
button.action:not(:disabled):hover { background:var(--blue); border-color:var(--blue); color:#fff; }

.statusline { display:flex; align-items:center; gap:10px; font-size:13px; margin-bottom:14px; }
.dot { width:9px; height:9px; display:inline-block; background:var(--muted); }
.dot.on { background:var(--blue); }
.statuslabel { font-weight:700; text-transform:uppercase; letter-spacing:.05em; }
.statusmeta { color:var(--muted); margin-left:auto; font-size:12px; }

pre#log {
  margin:0; background:#fff; color:var(--fg); border:1px solid var(--fg); padding:12px;
  font-family:var(--mono); font-size:11.5px; line-height:1.5; max-height:320px; overflow:auto;
  white-space:pre-wrap; word-break:break-all;
}

.results { padding:24px 0 40px; }
.results h2 { display:flex; align-items:baseline; gap:8px; }
.results h2 .count { color:var(--muted); font-weight:400; text-transform:none; letter-spacing:0; font-size:11px; }
table { width:100%; border-collapse:collapse; font-size:12.5px; }
thead th {
  text-align:left; font-size:10.5px; text-transform:uppercase; letter-spacing:.08em; color:var(--muted);
  border-bottom:1px solid var(--fg); padding:7px 10px 8px; font-weight:700;
}
tbody td { padding:7px 10px; border-bottom:1px solid var(--line-soft); }
tbody tr:hover td { background:#f6f8fb; }
th.num, td.num { text-align:right; font-variant-numeric:tabular-nums; }
td.best { color:var(--blue); font-weight:700; }
.empty-row td { color:var(--muted); font-style:normal; }

footer { padding:18px 0 36px; color:var(--muted); font-size:11px; display:flex; justify-content:space-between; border-top:1px solid var(--line-soft); }
</style>
</head>
<body>
<div class="wrap">

<header>
  <div class="kicker">SAkGD — Contest Tooling</div>
  <h1>GD-2025 / K-PLANARITY</h1>
  <div class="meta">Control panel — polls <code>/api/status</code> every 3s · <a href="#" id="reportLink" target="_blank">full report ↗</a></div>
</header>

<nav class="modes">
  <button class="active" data-mode="batch" onclick="setMode('batch')">Batch runner</button>
  <button data-mode="orchestrator" onclick="setMode('orchestrator')">Orchestrator</button>
</nav>

<div class="grid">
  <div class="panel">
    <h2>Configure run</h2>

    <div class="field">
      <label>Input set</label>
      <select id="inputSet" onchange="loadGraphs()"><option value="">loading…</option></select>
      <div class="graphlist" id="graphList"></div>
      <div class="graphlist-actions">
        <span class="gmeta" id="graphCount"></span>
        <button type="button" class="linkbtn" onclick="uncheckAllGraphs()">Uncheck all</button>
      </div>
    </div>

    <div id="batchFields">
      <div class="field">
        <label>Methods</label>
        <div class="chips" id="methodChips"></div>
      </div>
      <div class="row3">
        <div class="field"><label>Workers</label><input id="workers" type="number" value="4"></div>
        <div class="field"><label>Seed</label><input id="seed" type="number" value="42"></div>
        <div class="field"><label>Out dir (blank = auto)</label><input id="outDir" type="text" placeholder="data/output/&lt;set&gt;"></div>
      </div>
      <div class="row3">
        <div class="field"><label>Min · small</label><input id="minSmall" type="number" value="5"></div>
        <div class="field"><label>Min · medium</label><input id="minMedium" type="number" value="8"></div>
        <div class="field"><label>Min · large</label><input id="minLarge" type="number" value="15"></div>
      </div>
    </div>

    <div id="orchFields" style="display:none">
      <div class="field">
        <label>Explicit graphs folder (overrides input set)</label>
        <input id="oGraphsDir" type="text" placeholder="/path/to/graphs">
      </div>
      <div class="row3">
        <div class="field"><label>Workers</label><input id="oWorkers" type="number" value="8"></div>
        <div class="field"><label>Budget (sec)</label><input id="oBudget" type="number" value="2700"></div>
        <div class="field"><label>Seed</label><input id="oSeed" type="number" value="1"></div>
      </div>
      <div class="field"><label>Out dir (blank = auto)</label><input id="oOutDir" type="text" placeholder="data/output/&lt;set&gt;"></div>
    </div>

    <div class="btnrow">
      <button class="action" id="startBtn" onclick="start()">▸ Start</button>
      <button class="action stop" id="stopBtn" onclick="stop()" disabled>■ Stop</button>
    </div>
  </div>

  <div class="panel">
    <h2>Status</h2>
    <div class="statusline">
      <span class="dot" id="dot"></span>
      <span class="statuslabel" id="statusText">Idle</span>
      <span class="statusmeta" id="elapsed"></span>
    </div>
    <pre id="log">(no output yet)</pre>
  </div>
</div>

<section class="results">
  <h2>Best results <span class="count" id="bestsCount"></span></h2>
  <table id="bestsTable">
    <thead><tr><th>Graph</th><th>Method</th><th class="num">k</th><th class="num">totalX</th></tr></thead>
    <tbody><tr class="empty-row"><td colspan="4">no results yet</td></tr></tbody>
  </table>
</section>

<footer>
  <span>bin/server</span>
  <span id="clock"></span>
</footer>

</div>
<script>
let mode = 'batch';
let methodsAvailable = [];
let selectedMethods = new Set(['sa', 'sa-stress']);
let graphsAvailable = [];
let selectedGraphs = new Set();

function setMode(m) {
  mode = m;
  document.querySelectorAll('nav.modes button').forEach(b => b.classList.toggle('active', b.dataset.mode === m));
  document.getElementById('batchFields').style.display = m === 'batch' ? 'block' : 'none';
  document.getElementById('orchFields').style.display = m === 'orchestrator' ? 'block' : 'none';
}

async function loadInputSets() {
  try {
    const r = await fetch('/api/input-sets');
    const sets = await r.json();
    const sel = document.getElementById('inputSet');
    sel.innerHTML = sets.length
      ? sets.map(s => `<option value="${s.name}">${s.name} (${s.count})</option>`).join('')
      : '<option value="">none found under data/input/</option>';
  } catch (e) { /* keep the loading placeholder */ }
  loadGraphs();
}

async function loadGraphs() {
  const name = document.getElementById('inputSet').value;
  document.getElementById('reportLink').href = name ? ('/report?input_set=' + encodeURIComponent(name)) : '/report';
  graphsAvailable = [];
  selectedGraphs = new Set();
  if (name) {
    try {
      const r = await fetch('/api/graphs?input_set=' + encodeURIComponent(name));
      graphsAvailable = await r.json();
      selectedGraphs = new Set(graphsAvailable.map(g => g.name)); // all checked by default
    } catch (e) { /* ignore */ }
  }
  renderGraphList();
}

function renderGraphList() {
  document.getElementById('graphList').innerHTML = graphsAvailable.map(g => `
    <label>
      <input type="checkbox" ${selectedGraphs.has(g.name) ? 'checked' : ''} onchange="toggleGraph('${g.name}')">
      <span class="gname">${g.name}</span>
      <span class="gmeta">n=${g.n} m=${g.m}</span>
    </label>`).join('');
  document.getElementById('graphCount').textContent =
    graphsAvailable.length ? (selectedGraphs.size + ' / ' + graphsAvailable.length + ' selected') : '';
}

function toggleGraph(name) {
  if (selectedGraphs.has(name)) selectedGraphs.delete(name); else selectedGraphs.add(name);
  renderGraphList();
}

function uncheckAllGraphs() {
  selectedGraphs.clear();
  renderGraphList();
}

async function loadMethods() {
  try {
    const r = await fetch('/api/methods');
    methodsAvailable = await r.json();
    renderChips();
  } catch (e) { /* ignore */ }
}

function renderChips() {
  document.getElementById('methodChips').innerHTML = methodsAvailable.map(m =>
    `<div class="chip ${selectedMethods.has(m.id) ? 'on' : ''}" onclick="toggleMethod('${m.id}')">${m.id}</div>`
  ).join('');
}

function toggleMethod(id) {
  if (selectedMethods.has(id)) selectedMethods.delete(id); else selectedMethods.add(id);
  renderChips();
}

async function start() {
  const cfg = { mode, input_set: document.getElementById('inputSet').value };
  if (mode === 'batch') {
    if (!selectedMethods.size) { alert('Pick at least one method.'); return; }
    Object.assign(cfg, {
      out_dir: document.getElementById('outDir').value,
      methods: [...selectedMethods].join(','),
      workers: +document.getElementById('workers').value,
      seed: +document.getElementById('seed').value,
      minutes_small: +document.getElementById('minSmall').value,
      minutes_medium: +document.getElementById('minMedium').value,
      minutes_large: +document.getElementById('minLarge').value,
    });
  } else {
    Object.assign(cfg, {
      out_dir: document.getElementById('oOutDir').value,
      graphs_dir: document.getElementById('oGraphsDir').value,
      workers: +document.getElementById('oWorkers').value,
      budget_sec: +document.getElementById('oBudget').value,
      seed: +document.getElementById('oSeed').value,
    });
  }
  if (!cfg.input_set && !cfg.graphs_dir) { alert('Pick an input set (or an explicit graphs folder).'); return; }
  if (cfg.input_set && !cfg.graphs_dir && graphsAvailable.length) {
    if (!selectedGraphs.size) { alert('Select at least one graph to run.'); return; }
    if (selectedGraphs.size < graphsAvailable.length) cfg.only = [...selectedGraphs].join(',');
  }
  const r = await fetch('/api/start', { method: 'POST', body: JSON.stringify(cfg) });
  const j = await r.json();
  if (j.error) alert(j.error);
  poll();
}

async function stop() {
  await fetch('/api/stop', { method: 'POST' });
  poll();
}

async function poll() {
  try {
    const r = await fetch('/api/status');
    const s = await r.json();
    document.getElementById('dot').classList.toggle('on', !!s.running);
    document.getElementById('statusText').textContent = s.running ? ('Running · ' + s.mode) : 'Idle';
    document.getElementById('elapsed').textContent = s.elapsed_sec ? (s.elapsed_sec.toFixed(0) + 's elapsed') : '';
    document.getElementById('startBtn').disabled = !!s.running;
    document.getElementById('stopBtn').disabled = !s.running;
    document.getElementById('log').textContent = s.log_tail || '(no output yet)';

    const tbody = document.querySelector('#bestsTable tbody');
    const entries = s.bests ? Object.entries(s.bests) : [];
    document.getElementById('bestsCount').textContent = entries.length ? ('— ' + entries.length + ' entries') : '';
    if (!entries.length) {
      tbody.innerHTML = '<tr class="empty-row"><td colspan="4">no results yet</td></tr>';
    } else {
      const rows = entries.map(([key, v]) => {
        const idx = key.lastIndexOf('__');
        return { graph: key.slice(0, idx), method: key.slice(idx + 2), k: v.k, totalX: v.totalX };
      }).sort((a, b) => a.graph.localeCompare(b.graph) || a.method.localeCompare(b.method));
      const bestByGraph = {};
      for (const row of rows) {
        if (row.k == null) continue;
        if (!(row.graph in bestByGraph) || row.k < bestByGraph[row.graph]) bestByGraph[row.graph] = row.k;
      }
      tbody.innerHTML = rows.map(row => {
        const isBest = row.k != null && row.k === bestByGraph[row.graph];
        return `<tr><td>${row.graph}</td><td>${row.method}</td>` +
               `<td class="num${isBest ? ' best' : ''}">${row.k ?? '—'}</td><td class="num">${row.totalX ?? '—'}</td></tr>`;
      }).join('');
    }
  } catch (e) { /* server briefly unreachable between polls; ignore */ }
}

function tickClock() {
  document.getElementById('clock').textContent = new Date().toISOString().slice(0, 19).replace('T', ' ') + ' UTC';
}
setInterval(tickClock, 1000);
tickClock();

loadInputSets();
loadMethods();
poll();
setInterval(poll, 3000);
</script>
</body>
</html>
)HTML";

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
            sendResponse(fd, 200, "text/html; charset=utf-8", INDEX_HTML);
        } else if (req.method == "GET" && path == "/api/status") {
            sendResponse(fd, 200, "application/json", apiStatus().dump());
        } else if (req.method == "GET" && path == "/api/methods") {
            sendResponse(fd, 200, "application/json", apiMethods().dump());
        } else if (req.method == "GET" && path == "/api/input-sets") {
            sendResponse(fd, 200, "application/json", apiInputSets().dump());
        } else if (req.method == "GET" && path == "/api/graphs") {
            auto it = query.find("input_set");
            sendResponse(fd, 200, "application/json", apiGraphs(it == query.end() ? "" : it->second).dump());
        } else if (req.method == "GET" && path == "/report") {
            std::string outDir;
            auto setIt = query.find("input_set");
            if (setIt != query.end() && !setIt->second.empty()) {
                outDir = joinPath(ROOT(), "data/output/" + setIt->second);
            } else {
                std::lock_guard<std::mutex> lk(g_run.mu); outDir = g_run.outDir;
            }
            if (outDir.empty()) outDir = joinPath(ROOT(), "data/output");
            std::string reportPath = outDir + "/report.html";
            std::ifstream f(reportPath);
            if (!f) { sendResponse(fd, 404, "text/plain", "no report.html yet under " + outDir); }
            else {
                std::ostringstream ss; ss << f.rdbuf();
                sendResponse(fd, 200, "text/html; charset=utf-8", ss.str());
            }
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
