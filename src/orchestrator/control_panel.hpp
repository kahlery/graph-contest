// Live control panel for contest_orchestrate: a tiny localhost HTTP server
// (no dependencies) that shows per-graph k / totalX descent curves in real
// time and accepts mid-run commands:
//   - focus <graph> [until k<=K] [for N seconds]  (overrides the auction)
//   - ban / unban <graph>                          (bank its budget)
//   - abort the running lease (children get SIGTERM; sakgd finishes
//     gracefully and writes its best-so-far, see gStopRequested in main.cpp)
//
// Data flow: the orchestrator pushes a serialized per-graph snapshot after
// every lease (updateGraphs) and announces lease starts (setCurrent). The
// panel thread answers GET /status by splicing that snapshot together with
// a LIVE mid-lease series it tails from the workers' --trace-file files
// ("<sec> <bestK> <bestX>" per second, min-envelope across workers).
#pragma once

#include "../common/subprocess.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <functional>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <set>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace panel {

struct Control {
    std::string focus;
    int untilK = -1;                 // clear focus once bestK <= untilK
    double focusDeadline = -1;       // orchestrator now() seconds; -1 = none
    std::set<std::string> banned;
};

class Server {
public:
    ~Server() { stop(); }

    void configure(const std::string& runId, double budget,
                   std::function<double()> nowFn) {
        std::lock_guard<std::mutex> lk(mu_);
        runId_ = runId; budget_ = budget; nowFn_ = std::move(nowFn);
    }

    bool start(int port) {
        if (port <= 0) return false;
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        int one = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // localhost only
        addr.sin_port = htons((uint16_t)port);
        if (bind(fd_, (sockaddr*)&addr, sizeof(addr)) != 0 ||
            listen(fd_, 16) != 0) {
            close(fd_); fd_ = -1;
            return false;
        }
        port_ = port;
        stop_ = false;
        th_ = std::thread([this]() { loop(); });
        return true;
    }

    void stop() {
        stop_ = true;
        if (th_.joinable()) th_.join();
        if (fd_ >= 0) { close(fd_); fd_ = -1; }
    }

    int port() const { return port_; }

    // ---- orchestrator -> panel ---------------------------------------- //
    void updateGraphs(const std::string& graphsJson) {
        std::lock_guard<std::mutex> lk(mu_);
        graphsJson_ = graphsJson;
    }
    void setCurrent(const std::string& graph, const std::string& method,
                    const std::string& qdir, double q, double startWall) {
        std::lock_guard<std::mutex> lk(mu_);
        curGraph_ = graph; curMethod_ = method; curQdir_ = qdir;
        curQ_ = q; curStart_ = startWall;
    }
    void clearCurrent() {
        std::lock_guard<std::mutex> lk(mu_);
        curGraph_.clear(); curQdir_.clear();
    }

    // ---- panel -> orchestrator ---------------------------------------- //
    Control control() {
        std::lock_guard<std::mutex> lk(mu_);
        return ctl_;
    }
    void clearFocus() {
        std::lock_guard<std::mutex> lk(mu_);
        ctl_.focus.clear(); ctl_.untilK = -1; ctl_.focusDeadline = -1;
    }

private:
    // ---- request handling ---------------------------------------------- //
    void loop() {
        while (!stop_) {
            struct pollfd pfd{fd_, POLLIN, 0};
            if (poll(&pfd, 1, 200) <= 0) continue;
            int c = accept(fd_, nullptr, nullptr);
            if (c < 0) continue;
            char buf[8192];
            ssize_t n = read(c, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = 0;
                handle(c, std::string(buf, (size_t)n));
            }
            close(c);
        }
    }

    static std::string qparam(const std::string& query, const std::string& key) {
        size_t p = 0;
        while (p < query.size()) {
            size_t amp = query.find('&', p);
            if (amp == std::string::npos) amp = query.size();
            size_t eq = query.find('=', p);
            if (eq != std::string::npos && eq < amp &&
                query.substr(p, eq - p) == key)
                return query.substr(eq + 1, amp - eq - 1);
            p = amp + 1;
        }
        return "";
    }

    void reply(int c, const std::string& status, const std::string& ctype,
               const std::string& body) {
        std::ostringstream os;
        os << "HTTP/1.1 " << status << "\r\nContent-Type: " << ctype
           << "\r\nContent-Length: " << body.size()
           << "\r\nConnection: close\r\n\r\n" << body;
        std::string s = os.str();
        size_t off = 0;
        while (off < s.size()) {
            ssize_t w = write(c, s.data() + off, s.size() - off);
            if (w <= 0) break;
            off += (size_t)w;
        }
    }

    void handle(int c, const std::string& req) {
        size_t sp1 = req.find(' ');
        size_t sp2 = req.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) return;
        std::string method = req.substr(0, sp1);
        std::string url = req.substr(sp1 + 1, sp2 - sp1 - 1);
        std::string path = url, query;
        size_t qm = url.find('?');
        if (qm != std::string::npos) { path = url.substr(0, qm); query = url.substr(qm + 1); }

        if (method == "GET" && path == "/") {
            reply(c, "200 OK", "text/html; charset=utf-8", pageHtml());
        } else if (method == "GET" && path == "/status") {
            reply(c, "200 OK", "application/json", statusJson());
        } else if (method == "POST" && path == "/control") {
            reply(c, "200 OK", "application/json", applyControl(query));
        } else {
            reply(c, "404 Not Found", "text/plain", "not found");
        }
    }

    std::string applyControl(const std::string& q) {
        std::string action = qparam(q, "action");
        std::string graph = qparam(q, "graph");
        std::lock_guard<std::mutex> lk(mu_);
        if (action == "focus" && !graph.empty()) {
            ctl_.focus = graph;
            std::string uk = qparam(q, "until_k");
            std::string us = qparam(q, "until_sec");
            ctl_.untilK = uk.empty() ? -1 : atoi(uk.c_str());
            ctl_.focusDeadline =
                us.empty() ? -1 : (nowFn_ ? nowFn_() : 0) + atof(us.c_str());
            note_ = "focus " + graph;
        } else if (action == "clear") {
            ctl_.focus.clear(); ctl_.untilK = -1; ctl_.focusDeadline = -1;
            note_ = "focus cleared";
        } else if (action == "ban" && !graph.empty()) {
            ctl_.banned.insert(graph);
            note_ = "ban " + graph;
        } else if (action == "unban" && !graph.empty()) {
            ctl_.banned.erase(graph);
            note_ = "unban " + graph;
        } else if (action == "abort") {
            if (!curGraph_.empty()) {
                proc::abortAll().store(true);
                note_ = "abort lease (" + curGraph_ + ")";
            } else {
                note_ = "abort ignored: no lease running";
            }
        } else {
            return "{\"ok\":false}";
        }
        return "{\"ok\":true,\"note\":\"" + note_ + "\"}";
    }

    // min-envelope across the current lease's worker trace files
    std::string liveSeriesJson(const std::string& qdir) {
        std::vector<std::pair<long long, long long>> env;   // idx -> (k, x)
        DIR* d = opendir(qdir.c_str());
        if (!d) return "[]";
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            std::string f = e->d_name;
            if (f.size() < 7 || f.substr(f.size() - 6) != ".trace") continue;
            FILE* fp = fopen((qdir + "/" + f).c_str(), "r");
            if (!fp) continue;
            double t; long long k, x;
            while (fscanf(fp, "%lf %lld %lld", &t, &k, &x) == 3) {
                size_t i = (size_t)t;
                if (env.size() <= i) env.resize(i + 1, {-1, -1});
                if (env[i].first < 0 || k < env[i].first ||
                    (k == env[i].first && x < env[i].second))
                    env[i] = {k, x};
            }
            fclose(fp);
        }
        closedir(d);
        std::ostringstream os;
        os << "[";
        bool first = true;
        long long lastK = -1, lastX = -1;
        for (size_t i = 0; i < env.size(); i++) {
            if (env[i].first < 0) continue;
            // best-so-far envelope: monotone in k
            if (lastK >= 0 && (env[i].first > lastK ||
                               (env[i].first == lastK && env[i].second >= lastX)))
                env[i] = {lastK, lastX};
            lastK = env[i].first; lastX = env[i].second;
            if (!first) os << ",";
            first = false;
            os << "[" << i << "," << env[i].first << "," << env[i].second << "]";
        }
        os << "]";
        return os.str();
    }

    std::string statusJson() {
        std::string graphs, cur, live, ctl, runId;
        double budget, wall;
        {
            std::lock_guard<std::mutex> lk(mu_);
            runId = runId_; budget = budget_;
            wall = nowFn_ ? nowFn_() : 0;
            graphs = graphsJson_.empty() ? "{}" : graphsJson_;
            if (!curGraph_.empty()) {
                std::ostringstream os;
                os << "{\"graph\":\"" << curGraph_ << "\",\"method\":\""
                   << curMethod_ << "\",\"q\":" << curQ_
                   << ",\"start\":" << curStart_ << "}";
                cur = os.str();
            } else cur = "null";
            std::ostringstream os;
            os << "{\"focus\":\"" << ctl_.focus << "\",\"until_k\":" << ctl_.untilK
               << ",\"focus_left\":"
               << (ctl_.focusDeadline < 0 ? -1.0 : ctl_.focusDeadline - wall)
               << ",\"note\":\"" << note_ << "\",\"banned\":[";
            bool f = true;
            for (auto& b : ctl_.banned) { if (!f) os << ","; f = false; os << "\"" << b << "\""; }
            os << "]}";
            ctl = os.str();
            if (!curGraph_.empty() && !curQdir_.empty()) {
                std::ostringstream ls;
                ls << "{\"graph\":\"" << curGraph_ << "\",\"start\":" << curStart_
                   << ",\"series\":" << liveSeriesJson(curQdir_) << "}";
                live = ls.str();
            } else live = "null";
        }
        std::ostringstream os;
        os << "{\"run\":\"" << runId << "\",\"budget\":" << budget
           << ",\"wall\":" << wall << ",\"current\":" << cur
           << ",\"control\":" << ctl << ",\"graphs\":" << graphs
           << ",\"live\":" << live << "}";
        return os.str();
    }

    static std::string pageHtml();

    int fd_ = -1, port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread th_;
    std::mutex mu_;
    std::string runId_, graphsJson_;
    double budget_ = 0;
    std::function<double()> nowFn_;
    std::string curGraph_, curMethod_, curQdir_;
    double curQ_ = 0, curStart_ = 0;
    Control ctl_;
    std::string note_;
};

inline std::string Server::pageHtml() {
    return R"HTML(<!doctype html><html><head><meta charset="utf-8">
<title>corch panel</title>
<style>
body{font:13px -apple-system,Menlo,monospace;background:#111;color:#ddd;margin:16px}
h2{margin:0 0 6px 0;font-size:16px} .dim{color:#888}
table{border-collapse:collapse;margin:10px 0}
td,th{padding:3px 10px;border-bottom:1px solid #333;text-align:right;cursor:pointer}
th{color:#9ad} td:first-child,th:first-child{text-align:left}
tr.sel{background:#1d2b3a} tr.done{color:#6a6} tr.banned{color:#a66;text-decoration:line-through}
tr.cur td:first-child{color:#fd5}
button{background:#234;border:1px solid #468;color:#dde;padding:2px 8px;margin:0 2px;cursor:pointer;border-radius:3px}
button:hover{background:#345} input,select{background:#222;color:#ddd;border:1px solid #555;width:70px;padding:2px}
#bar{height:8px;background:#222;border:1px solid #444;margin:6px 0;width:640px}
#fill{height:100%;background:#2a7;width:0}
.ctl{margin:8px 0;padding:8px;border:1px solid #333;background:#181818}
svg{background:#181818;border:1px solid #333}
.note{color:#fd5}
</style></head><body>
<h2>contest_orchestrate <span id=run class=dim></span></h2>
<div><span id=wall></span> / <span id=bud></span>s &nbsp; <span id=curline class=dim></span></div>
<div id=bar><div id=fill></div></div>
<div class=ctl>
 odak: <select id=fsel></select>
 k&le; <input id=fk type=number placeholder="yok">
 süre(sn) <input id=fs type=number placeholder="yok">
 <label><input id=fnow type=checkbox style="width:auto"> şimdi geç (lease'i kes)</label>
 <button onclick="doFocus()">Odaklan</button>
 <button onclick="post('action=clear')">Odağı bırak</button>
 <button onclick="post('action=abort')">Lease'i kes</button>
 <span id=note class=note></span>
</div>
<table id=tbl><thead><tr><th>graf</th><th>k</th><th>totalX</th><th>sn</th><th>lease</th><th>durum</th><th></th></tr></thead><tbody></tbody></table>
<div><b id=chartTitle></b></div>
<svg id=chart width=760 height=280></svg>
<script>
let S=null, sel=null;
function post(q){fetch('/control?'+q,{method:'POST'}).then(r=>r.json()).then(j=>{document.getElementById('note').textContent=j.note||'';poll();});}
function doFocus(){
  let g=document.getElementById('fsel').value; if(!g)return;
  let q='action=focus&graph='+g;
  let k=document.getElementById('fk').value; if(k) q+='&until_k='+k;
  let s=document.getElementById('fs').value; if(s) q+='&until_sec='+s;
  post(q);
  if(document.getElementById('fnow').checked) post('action=abort');
}
function fmt(x){return x==null?'—':x;}
function poll(){fetch('/status').then(r=>r.json()).then(j=>{S=j;render();}).catch(()=>{});}
function render(){
  if(!S)return;
  document.getElementById('run').textContent=S.run;
  document.getElementById('wall').textContent=S.wall.toFixed(0);
  document.getElementById('bud').textContent=S.budget.toFixed(0);
  document.getElementById('fill').style.width=(100*S.wall/S.budget).toFixed(1)+'%';
  let cl='';
  if(S.current) cl='lease: '+S.current.graph+' · '+S.current.method+' · '+
    (S.wall-S.current.start).toFixed(0)+'/'+S.current.q.toFixed(0)+'s';
  if(S.control.focus) cl+='  |  ODAK: '+S.control.focus+
    (S.control.until_k>=0?' (k≤'+S.control.until_k+')':'')+
    (S.control.focus_left>=0?' ('+S.control.focus_left.toFixed(0)+'sn)':'');
  document.getElementById('curline').textContent=cl;
  let names=Object.keys(S.graphs).sort();
  if(!sel&&names.length)sel=names[0];
  let fsel=document.getElementById('fsel');
  if(fsel.options.length!=names.length){fsel.innerHTML=names.map(n=>'<option>'+n+'</option>').join('');}
  let tb=document.querySelector('#tbl tbody');tb.innerHTML='';
  for(let n of names){
    let g=S.graphs[n];
    let banned=S.control.banned.includes(n);
    let tr=document.createElement('tr');
    tr.className=(n==sel?'sel ':'')+(banned?'banned ':g.done?'done ':'')+
      (S.current&&S.current.graph==n?'cur':'');
    let st=banned?'ban':g.done?'bitti':'';
    tr.innerHTML='<td>'+n+'</td><td>'+fmt(g.k)+'</td><td>'+fmt(g.x)+'</td><td>'+
      g.sec.toFixed(0)+'</td><td>'+g.leases+'</td><td>'+st+'</td>'+
      '<td><button onclick="event.stopPropagation();post(\'action='+(banned?'unban':'ban')+'&graph='+n+'\')">'+(banned?'geri al':'bırak')+'</button></td>';
    tr.onclick=()=>{sel=n;render();};
    tb.appendChild(tr);
  }
  drawChart();
}
function drawChart(){
  let svg=document.getElementById('chart');
  let W=760,H=280,L=52,R=72,T=14,B=26;
  let g=S.graphs[sel]; if(!g){svg.innerHTML='';return;}
  let pts=(g.hist||[]).slice();
  if(S.live&&S.live.graph==sel)
    for(let p of S.live.series) pts.push([S.live.start+p[0],p[1],p[2]]);
  document.getElementById('chartTitle').textContent=sel+'  k='+fmt(g.k)+'  totalX='+fmt(g.x);
  if(pts.length<2){svg.innerHTML='<text x=20 y=30 fill="#888">veri bekleniyor…</text>';return;}
  let t0=0,t1=Math.max(S.budget,pts[pts.length-1][0]);
  let ks=pts.map(p=>p[1]),xs=pts.map(p=>p[2]);
  let k0=Math.min(...ks),k1=Math.max(...ks),x0=Math.min(...xs),x1=Math.max(...xs);
  if(k1==k0)k1=k0+1; if(x1==x0)x1=x0+1;
  let sx=t=>L+(W-L-R)*(t-t0)/(t1-t0);
  let syk=k=>T+(H-T-B)*(1-(k-k0)/(k1-k0));
  let syx=x=>T+(H-T-B)*(1-(x-x0)/(x1-x0));
  let pk=pts.map(p=>sx(p[0]).toFixed(1)+','+syk(p[1]).toFixed(1)).join(' ');
  let px=pts.map(p=>sx(p[0]).toFixed(1)+','+syx(p[2]).toFixed(1)).join(' ');
  let s='';
  for(let i=0;i<=4;i++){
    let y=T+(H-T-B)*i/4;
    s+='<line x1='+L+' y1='+y+' x2='+(W-R)+' y2='+y+' stroke="#2a2a2a"/>';
    s+='<text x=4 y='+(y+4)+' fill="#7ac" font-size=11>'+Math.round(k1-(k1-k0)*i/4)+'</text>';
    s+='<text x='+(W-R+6)+' y='+(y+4)+' fill="#c97" font-size=11>'+Math.round(x1-(x1-x0)*i/4)+'</text>';
  }
  s+='<polyline points="'+px+'" fill="none" stroke="#c97" stroke-width=1 stroke-dasharray="4 3"/>';
  s+='<polyline points="'+pk+'" fill="none" stroke="#7ac" stroke-width=2/>';
  s+='<text x='+L+' y='+(H-8)+' fill="#666" font-size=11>0s</text>';
  s+='<text x='+(W-R-30)+' y='+(H-8)+' fill="#666" font-size=11>'+Math.round(t1)+'s</text>';
  s+='<text x='+(L+8)+' y='+(T+12)+' fill="#7ac" font-size=11>k</text>';
  s+='<text x='+(W-R-60)+' y='+(T+12)+' fill="#c97" font-size=11>totalX</text>';
  svg.innerHTML=s;
}
setInterval(poll,2000);poll();
</script></body></html>)HTML";
}

} // namespace panel
