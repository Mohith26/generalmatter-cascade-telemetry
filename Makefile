CXX = clang++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Werror -pthread

PY = .venv/bin/python
PYTEST = .venv/bin/pytest

BIN = build
RT_HEADERS = $(wildcard rt/*.hpp)

all: $(BIN)/rt $(BIN)/rt_tests

$(BIN):
	mkdir -p $(BIN)

$(BIN)/rt: rt/main.cpp $(RT_HEADERS) | $(BIN)
	$(CXX) $(CXXFLAGS) rt/main.cpp -o $@

$(BIN)/rt_tests: rt/tests.cpp $(RT_HEADERS) | $(BIN)
	$(CXX) $(CXXFLAGS) rt/tests.cpp -o $@

venv:
	python3 -m venv .venv && .venv/bin/pip install numpy pytest

test: all
	$(BIN)/rt_tests
	$(PYTEST) --color=no -q

bench: all
	mkdir -p results
	$(PY) -m cascade.validate
	$(BIN)/rt bench --out results
	$(BIN)/rt eval --out results
	$(BIN)/rt replay --out results

clean:
	rm -rf $(BIN)

.PHONY: all venv test bench clean
