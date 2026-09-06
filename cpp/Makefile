CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

# brhbp_pdf needs MuPDF; skipped automatically if it is not installed.
MUPDF_PREFIX ?= /opt/homebrew
HAVE_MUPDF := $(wildcard $(MUPDF_PREFIX)/include/mupdf/fitz.h)

all: brhbp_tool brhbp_bench $(if $(HAVE_MUPDF),brhbp_pdf,)
	$(if $(HAVE_MUPDF),,@echo "note: MuPDF not found at $(MUPDF_PREFIX); skipping brhbp_pdf")

brhbp_tool: brhbp_tool.cc brhbp.cc brhbp.h
	$(CXX) $(CXXFLAGS) -o $@ brhbp_tool.cc brhbp.cc

brhbp_bench: brhbp_bench.cc brhbp.cc brhbp.h
	$(CXX) $(CXXFLAGS) -o $@ brhbp_bench.cc brhbp.cc

test: brhbp_tool
	./conformance.sh

bench: brhbp_bench
	./brhbp_bench 600 30
	./brhbp_bench 1200 15

brhbp_pdf: brhbp_pdf.cc brhbp.cc brhbp.h
	$(CXX) $(CXXFLAGS) -I$(MUPDF_PREFIX)/include -o $@ brhbp_pdf.cc brhbp.cc \
	  -L$(MUPDF_PREFIX)/lib -lmupdf -lmupdf-third

clean:
	rm -f brhbp_tool brhbp_bench brhbp_pdf

.PHONY: all test bench clean
