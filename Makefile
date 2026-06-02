CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -DNDEBUG -Wall -Wno-sign-compare -Wno-unused-variable -fno-stack-protector
LDFLAGS  ?=

SRC      := src/main.cpp
BIN      := sakgd

SRC1     := src/approach1_lns.cpp
BIN1     := approach1

.PHONY: all clean run debug approach1

all: $(BIN) $(BIN1)

$(BIN): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

$(BIN1): $(SRC1)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

debug: CXXFLAGS := -std=c++17 -O0 -g -Wall -Wno-sign-compare -Wno-unused-variable -fsanitize=address,undefined
debug: $(BIN) $(BIN1)

run: $(BIN)
	./$(BIN) -i data/input.json -o data/output.json -t 60 -p1 10

clean:
	rm -f $(BIN) $(BIN1)
