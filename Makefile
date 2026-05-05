CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -DNDEBUG -Wall -Wno-sign-compare -Wno-unused-variable -fno-stack-protector
LDFLAGS  ?=

SRC      := src/main.cpp
BIN      := sakgd

.PHONY: all clean run debug

all: $(BIN)

$(BIN): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

debug: CXXFLAGS := -std=c++17 -O0 -g -Wall -Wno-sign-compare -Wno-unused-variable -fsanitize=address,undefined
debug: $(BIN)

run: $(BIN)
	./$(BIN) -i data/input.json -o data/output.json -t 60 -p1 10

clean:
	rm -f $(BIN)
