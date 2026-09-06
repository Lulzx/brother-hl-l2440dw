CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra
PDF     ?= test/sample.pdf
PAPER   ?= LETTER
DPI     ?= 600

all: brpdf

brpdf: brpdf.c
	$(CC) $(CFLAGS) -o $@ $<

# Reference encoder built from brlaser's unmodified sources (for conformance tests).
ref/refenc: ref/refenc.cc ref/brlaser/job.cc ref/brlaser/line.cc ref/brlaser/job.h ref/brlaser/line.h ref/brlaser/block.h
	c++ -std=c++11 -O2 -I ref -o $@ ref/refenc.cc ref/brlaser/job.cc ref/brlaser/line.cc

# PDF -> printer job, no Brother software involved.
job.prn: brpdf $(PDF)
	mutool draw -F pbm -r $(DPI) -o - $(PDF) | ./brpdf -p $(PAPER) -r $(DPI) > $@

# Render the job the way the printer would.
simulate: job.prn
	python3 brsim.py decode job.prn -o out -v

# Full conformance + round-trip test suite.
test: brpdf ref/refenc
	sh test/roundtrip.sh $(PDF)

clean:
	rm -rf brpdf ref/refenc job.prn out test/out* test/spool test/*.prn test/*.pbm

.PHONY: all simulate test clean
