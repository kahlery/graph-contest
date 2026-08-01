CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -DNDEBUG -Wall -Wno-sign-compare -Wno-unused-variable -fno-stack-protector
LDFLAGS  ?=
THREADLIBS ?= -pthread

# Every binary below is built into ./bin.
BINDIR   := bin

SRC      := src/main.cpp
BIN      := $(BINDIR)/sakgd

SRC1     := src/approach1_lns.cpp
BIN1     := $(BINDIR)/approach1

# Baseline control builds: pre-k-critical-selection snapshot (original Phase-2
# vertex selection). Used only for A/B comparison against the new default.
SRCB     := src/main_baseline.cpp
BINB     := $(BINDIR)/sakgd_baseline
SRCB1    := src/approach1_lns_baseline.cpp
BINB1    := $(BINDIR)/approach1_baseline

# Contest tooling (batch runner, orchestrator, dashboard, stress-init) - see
# src/common/engine.hpp for the shared graph/method registry they build on.
TOOLS    := $(BINDIR)/stress_init $(BINDIR)/tripod_init $(BINDIR)/gradx_init \
            $(BINDIR)/run_contest \
            $(BINDIR)/contest_orchestrate $(BINDIR)/server $(BINDIR)/bench_reheat_sharing $(BINDIR)/xstat

.PHONY: all clean run debug approach1 baseline tools gui stop orchestrate

# Explicit default goal: several tools (run_contest, contest_orchestrate,
# server) shell out to a bare `make` via ensureBinaries() when a binary is
# missing, so the default goal must always be "all" - relying on "whichever
# rule appears first in the file" is fragile (e.g. a later-added phony
# target like `orchestrate` would otherwise become the default and call
# itself recursively).
.DEFAULT_GOAL := all

PORT ?= 8080

# CLI equivalent of the GUI's orchestrator run. Defaults to graphs 5/6/8 of
# internal-2026 with 6 workers and a 45-minute total wall-clock budget;
# override any of SET/GRAPHS/WORKERS/BUDGET on the command line, e.g.:
#   make orchestrate GRAPHS=instance_01,instance_02 WORKERS=8 BUDGET=1800
SET     ?= internal-2026
GRAPHS  ?= instance_05,instance_06,instance_08
WORKERS ?= 6
BUDGET  ?= 2700

orchestrate: $(BINDIR)/contest_orchestrate
	$(BINDIR)/contest_orchestrate --input-set $(SET) --only $(GRAPHS) \
		--budget $(BUDGET) --workers $(WORKERS)

# Builds the server, launches it in the background, and opens the control
# panel in the default browser.
gui: $(BINDIR)/server
	"$(CURDIR)/$(BINDIR)/server" --port $(PORT) & \
	sleep 0.5; \
	echo http://localhost:$(PORT)

# Kills any running processes started from this project's bin/ (server,
# run_contest, contest_orchestrate, etc.), matched by full path so unrelated
# processes elsewhere on the system are left untouched.
stop:
	-pkill -f "$(CURDIR)/$(BINDIR)/"
	@echo "Stopped any running graph-contest processes."

all: $(BIN) $(BIN1) tools

$(BINDIR):
	mkdir -p $(BINDIR)

$(BIN): $(SRC) | $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

$(BIN1): $(SRC1) | $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

tools: $(TOOLS)

$(BINDIR)/stress_init: src/tools/stress_init.cpp src/common/json.hpp src/common/subprocess.hpp src/common/paths.hpp | $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

$(BINDIR)/xstat: src/tools/xstat.cpp src/common/json.hpp | $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

$(BINDIR)/tripod_init: src/tools/tripod_init.cpp src/common/json.hpp | $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

$(BINDIR)/gradx_init: src/tools/gradx_init.cpp src/common/json.hpp | $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) $(THREADLIBS)

$(BINDIR)/run_contest: src/runner/run_contest.cpp src/common/engine.hpp | $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) $(THREADLIBS)

$(BINDIR)/contest_orchestrate: src/orchestrator/contest_orchestrate.cpp src/common/engine.hpp | $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) $(THREADLIBS)

$(BINDIR)/server: src/server/server.cpp src/common/engine.hpp | $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) $(THREADLIBS)

$(BINDIR)/bench_reheat_sharing: src/tools/bench_reheat_sharing.cpp src/common/engine.hpp | $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) $(THREADLIBS)

# `make baseline` builds the original-selection control binaries.
baseline: $(BINB) $(BINB1)

$(BINB): $(SRCB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

$(BINB1): $(SRCB1) | $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

debug: CXXFLAGS := -std=c++17 -O0 -g -Wall -Wno-sign-compare -Wno-unused-variable -fsanitize=address,undefined
debug: $(BIN) $(BIN1)

run: $(BIN)
	$(BIN) -i data/input/internal-contest/Automatic-1.json -o /tmp/sakgd_run_out.json -t 60 -p1 10

clean:
	rm -rf $(BINDIR)
