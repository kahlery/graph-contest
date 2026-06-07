CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -DNDEBUG -Wall -Wno-sign-compare -Wno-unused-variable -fno-stack-protector
LDFLAGS  ?=

SRC      := src/main.cpp
BIN      := sakgd

SRC1     := src/approach1_lns.cpp
BIN1     := approach1

# Baseline control builds: pre-k-critical-selection snapshot (original Phase-2
# vertex selection). Used only for A/B comparison against the new default.
SRCB     := src/main_baseline.cpp
BINB     := sakgd_baseline
SRCB1    := src/approach1_lns_baseline.cpp
BINB1    := approach1_baseline

.PHONY: all clean run debug approach1 baseline

all: $(BIN) $(BIN1)

$(BIN): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

$(BIN1): $(SRC1)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

# `make baseline` builds the original-selection control binaries.
baseline: $(BINB) $(BINB1)

$(BINB): $(SRCB)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

$(BINB1): $(SRCB1)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

debug: CXXFLAGS := -std=c++17 -O0 -g -Wall -Wno-sign-compare -Wno-unused-variable -fsanitize=address,undefined
debug: $(BIN) $(BIN1)

run: $(BIN)
	./$(BIN) -i data/input.json -o data/output.json -t 60 -p1 10

clean:
	rm -f $(BIN) $(BIN1) $(BINB) $(BINB1)
