CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

all: brhbp_tool brhbp_bench

brhbp_tool: brhbp_tool.cc brhbp.cc brhbp.h
	$(CXX) $(CXXFLAGS) -o $@ brhbp_tool.cc brhbp.cc

brhbp_bench: brhbp_bench.cc brhbp.cc brhbp.h
	$(CXX) $(CXXFLAGS) -o $@ brhbp_bench.cc brhbp.cc

test: brhbp_tool
	./conformance.sh

bench: brhbp_bench
	./brhbp_bench 600 30
	./brhbp_bench 1200 15

clean:
	rm -f brhbp_tool brhbp_bench

.PHONY: all test bench clean
