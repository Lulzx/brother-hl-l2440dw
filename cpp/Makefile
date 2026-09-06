CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

# brhbp_pdf needs MuPDF; skipped automatically if it is not installed.
MUPDF_PREFIX ?= /opt/homebrew
HAVE_MUPDF := $(wildcard $(MUPDF_PREFIX)/include/mupdf/fitz.h)

all: brhbp_tool brhbp_bench cancel_test ippcancel_tool status_tool snmp_encode_test \
     $(if $(HAVE_MUPDF),brhbp_pdf,)
	$(if $(HAVE_MUPDF),,@echo "note: MuPDF not found at $(MUPDF_PREFIX); skipping brhbp_pdf")

brhbp_tool: brhbp_tool.cc brhbp.cc brhbp.h
	$(CXX) $(CXXFLAGS) -o $@ brhbp_tool.cc brhbp.cc

brhbp_bench: brhbp_bench.cc brhbp.cc brhbp.h
	$(CXX) $(CXXFLAGS) -o $@ brhbp_bench.cc brhbp.cc

cancel_test: cancel_test.cc brhbp.cc brhbp.h
	$(CXX) $(CXXFLAGS) -o $@ cancel_test.cc brhbp.cc

ippcancel_tool: ippcancel_tool.cc ippcancel.cc ippcancel.h
	$(CXX) $(CXXFLAGS) -o $@ ippcancel_tool.cc ippcancel.cc

status_tool: status_tool.cc status.cc status.h
	$(CXX) $(CXXFLAGS) -o $@ status_tool.cc status.cc

snmp_encode_test: snmp_encode_test.cc status.cc status.h
	$(CXX) $(CXXFLAGS) -o $@ snmp_encode_test.cc status.cc

# JNI shared library. Needs a JDK for jni.h; skipped when JAVA_HOME is unset.
JAVA_HOME ?= $(shell /usr/libexec/java_home 2>/dev/null)
jni:
	@test -n "$(JAVA_HOME)" || { echo "no JDK found; skipping JNI"; exit 0; }
	$(CXX) $(CXXFLAGS) -fPIC -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/darwin -I. \
	  -shared -o android/libbrhbp.dylib android/brhbp_jni.cc brhbp.cc

test: brhbp_tool cancel_test snmp_encode_test
	./conformance.sh
	./cancel_check.sh
	./snmp_check.sh

bench: brhbp_bench
	./brhbp_bench 600 30
	./brhbp_bench 1200 15

brhbp_pdf: brhbp_pdf.cc brhbp.cc brhbp.h
	$(CXX) $(CXXFLAGS) -I$(MUPDF_PREFIX)/include -o $@ brhbp_pdf.cc brhbp.cc \
	  -L$(MUPDF_PREFIX)/lib -lmupdf -lmupdf-third

clean:
	rm -f brhbp_tool brhbp_bench brhbp_pdf cancel_test ippcancel_tool \
	      status_tool snmp_encode_test android/libbrhbp.dylib

.PHONY: all test bench clean jni
