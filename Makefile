CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra
PDF     ?= test/sample.pdf
# The target printer. Override on the command line or in the environment.
# Set BRPRINTER (or PRINTER) to your printer's address.
PRINTER ?= $(BRPRINTER)
# Targets that talk to the device check this rather than failing obscurely.
require-printer:
	@test -n "$(PRINTER)" || { \
	  echo "set BRPRINTER or PRINTER, e.g. make print PRINTER=10.0.0.5"; exit 2; }
PORT    ?= 9100
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

# Send the job to the real printer's raw port.  Nothing Brother-supplied involved.
print: job.prn
	nc -w 20 $(PRINTER) $(PORT) < job.prn

# Ask the printer who and how it is.  The device accepts data on 9100 but never
# answers PJL there (no back-channel), so identity/state come from its IPP port.
status:
	@ipptool -tv ipp://$(PRINTER)/ipp/print get-printer-attributes.test \
	  | grep -E 'printer-(make-and-model|name|state|state-reasons|uri-supported|is-accepting-jobs)'

# Ask the hardware what it is, over PJL + SNMP + IPP.
probe:
	python3 tools/probe.py $(PRINTER)

# Build the geometry calibration sheet (see tools/testpage.py); does not print.
CALFLAGS ?= -d
cal.prn: brpdf tools/testpage.py
	python3 tools/testpage.py --paper $(PAPER) --dpi $(DPI) | ./brpdf -p $(PAPER) -r $(DPI) $(CALFLAGS) -j cal > $@

# Preview it the way the printer would render it.
cal-preview: cal.prn
	python3 brsim.py decode cal.prn -o cal-out -v

# Full conformance + round-trip test suite.
cpp:
	$(MAKE) -C cpp

cpp-test: brpdf
	$(MAKE) -C cpp test

cpp-bench:
	$(MAKE) -C cpp bench

test: brpdf ref/refenc
	sh test/roundtrip.sh $(PDF)

clean:
	rm -rf brpdf ref/refenc job.prn cal.prn out cal-out test/out* test/spool test/*.prn test/*.pbm

.PHONY: all require-printer cpp cpp-test cpp-bench simulate print status probe cal-preview test clean
