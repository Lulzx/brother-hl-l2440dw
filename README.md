# Native HBP printing to a Brother HL-L2440DW

The HL-L2440DW has no PCL or PostScript interpreter. It is a host-based engine:
its native path is Brother's compressed 1-bit raster ("HBP") wrapped in PJL,
which the host must produce in full. This repo contains the reverse-engineered
description of that stream (`FORMAT.md`), a ~400-line dependency-free C program
that produces it from any PDF, and a software model of the printer that decodes
the stream and renders what would come out of the paper tray. The encoder is
verified byte-for-byte against brlaser, in simulation, and on the hardware
itself.

**This is not the only way to print to this device, and for most purposes it is
not the easiest.** The printer is also Mopria 2.1 certified and speaks IPP
Everywhere, so a stock CUPS queue drives it with no driver at all. The reason to
use the HBP path is resolution and control, not necessity -- see *Two ways to
print* below for the measured trade-off.

```
brpdf.c        PBM pages -> Brother HBP job (PJL + mode-1030 raster)
brprint        PDF -> printer: rasterise, encode, push to tcp/9100
brsim.py       printer model: decodes jobs to PNG, or listens on tcp/9100 like the real device
tools/probe.py interrogate the real printer over PJL + SNMP + IPP
tools/testpage.py  calibration sheet for measuring the engine's true geometry
FORMAT.md      the wire protocol
ref/           brlaser's original encoder + harness for byte-for-byte conformance
test/          sample PDF and the end-to-end test script
```

## Two ways to print

### The easy one: a driverless CUPS queue

The printer is Mopria 2.1 certified and advertises `image/urf` and
`image/pwg-raster`, so CUPS drives it with no driver and no vendor software:

```sh
lpadmin -p brother -E -v ipp://192.168.1.17/ipp/print -m everywhere
lp -d brother doc.pdf
```

That is the whole setup. CUPS generates a PPD from the printer's own IPP
attributes, including its model name. Note the queue must be added *by IP*: the
unit does not answer mDNS (see below), so it never appears in a printer picker
on its own. brlaser is a second, classic CUPS route -- it is a CUPS filter, and
`ref/` is built from its encoder.

### The catch, measured

CUPS's generated PPD collapses the printer's resolutions into three quality
tiers and there is no 600 dpi among them:

    *DefaultResolution: 300dpi
    *cupsPrintQuality Draft:  HWResolution [300 150]
    *cupsPrintQuality Normal: HWResolution [300 300]
    *cupsPrintQuality High:   HWResolution [1200 1200]

The printer meanwhile reports `printer-resolution-default = 600dpi` and
`pwg-raster-document-resolution-supported = 600dpi` -- 600 is both its default
and the *only* PWG raster resolution it claims to want. A default `lp` job
therefore hands the engine 300 dpi and lets the firmware scale it, against an
engine whose SNMP `prtMarkerAddressability` is 600 dpi in the feed direction.
Confirmed by running the filter chain by hand: `rastertopwg` logs
`HWResolution = [300 300]` and emits a 2550x3300 page.

Same PDF page, same physical size, both printed on the hardware:

| | driverless CUPS | `brprint` (HBP) |
|---|---|---|
| Setup | one `lpadmin` line | `make` |
| Raster sent | 2550x3300 = 8.5x11in **@300dpi** | 5100x6600 = 8.5x11in **@600dpi** |
| Host intermediate | 7.8 MB, 8-bit gray | 4.2 MB, 1-bit PBM |
| On the wire | 409,611 B (PWG) | 462,783 B (HBP) |
| Duplex back side | `normal`, no rotation | 180-degree pre-rotation |

Near-identical bytes on the wire, but the HBP stream carries 4x the pixels for
them: mode-1030 delta coding on 1-bit data fits text far better than PWG on
8-bit gray.

So: use the CUPS queue for everyday printing. Use this repo when you want the
engine's native 600 dpi, exact control of halftoning and placement, or a print
path with no spooler, no PPD and no CUPS at all -- one C99 binary and a socket.
Whether the driverless path can be pushed to 600 dpi by overriding
`printer-resolution` (which the printer does list in
`job-creation-attributes-supported`) is untested.

## Print a PDF

```sh
make                                   # builds ./brpdf (C99, no libraries)
mutool draw -F pbm -r 600 -o - doc.pdf | ./brpdf -p A4 > job.prn
```

Any rasteriser that emits raw PBM works (`pdftoppm -mono`, Ghostscript
`-sDEVICE=pbmraw`). Options: `-p` paper, `-r 300|600|1200`, `-c` copies,
`-d` duplex, `-e` toner save, `-t` tray, `-m` media type, `-j` job name.

## The printer on this network

The target device is wired in as the default: a **Brother HL-L2440DW at
`192.168.1.17`** (`BRWC8A3E8DC3C17`, MAC `c8:a3:e8:dc:3c:17`), ports 9100, 631,
515, 80 and 443 open. Override with `BRPRINTER` / `PRINTER` anywhere below.

```sh
make probe                             # everything the printer will say about itself
./brprint --status                     # short version (via IPP)
./brprint doc.pdf                      # rasterise, encode, print
./brprint -d -p A4 -e doc.pdf          # duplex A4, toner save
./brprint -o job.prn doc.pdf           # build the job, send nothing
make print PRINTER=192.168.1.42        # or drive it from the Makefile
```

It is *not* discoverable over Bonjour -- nothing answers `_ipp._tcp` or
`_pdl-datastream._tcp` -- so it will not show up in the macOS printer picker on
its own. It *does* answer PJL on 9100, but only if the client keeps its side of
the socket open; `nc` half-closes and so sees nothing. See `FORMAT.md` section 1.

To send by hand instead: `nc 192.168.1.17 9100 < job.prn`, or
`lp -d <raw-queue> -o raw job.prn` against a CUPS raw queue. To send to the
simulator (`./brprint --sim doc.pdf`, or by hand):

```sh
python3 brsim.py listen --port 9100 -o spool &     # pretends to be the printer
nc localhost 9100 < job.prn                        # "print"
open spool/job-*/page-001-front-preview.png        # what the sheet would look like
```

Or render an existing job file directly: `python3 brsim.py decode job.prn -o out -v`.

## Verification

`make test` runs `test/roundtrip.sh`:

1. Rasterises the sample PDF with mutool.
2. Encodes it with `brpdf` and with brlaser's unmodified encoder (compiled into
   `ref/refenc`); the two job files must be byte-identical. brlaser is the
   driver that the field reports confirm the HL-L2400/2440/2460 accept.
3. Decodes the job with the printer model and checks every pixel against the
   source bitmap cropped to the printable area.
4. Checks duplex back pages are the exact 180-degree rotation, and runs
   copies, toner save, 1200 dpi and A4 variants.
5. Starts the network emulator, queries `@PJL INFO ID`, sends the job over TCP
   and checks it renders with zero warnings.

Measured on the 3-page Letter sample at 600 dpi: 12.6 MB of bitmap becomes a
1.35 MB job in about 60 ms.

## What is not verified

The printer has been interrogated over PJL, SNMP and IPP (`make probe`,
findings in `FORMAT.md` section 1b). The device's own IEEE-1284 ID is
`CMD:PJL,HBP,URF`: no PCL and no PostScript interpreter, confirmed first-hand,
but also `URF` -- the AirPrint raster that makes the driverless path above
work.

A `brpdf` duplex job has also been printed on the real device (HL-L2440DW,
firmware Ver.1.24) and was accepted cleanly -- the impression counter advanced
by exactly 2 on one sheet. The stream format is therefore confirmed against
hardware, not just against brlaser and the simulator.

The calibration sheet (`make cal.prn`) settled the duplex question: the back
side reads upright when the sheet is turned about its long edge, so brlaser's
180-degree back-page rotation is correct for this engine and `brpdf -d`'s
default is right. The printer's IPP `pwg-raster-document-sheet-back = normal`
describes only its AirPrint path.

What is still unconfirmed is the *margin*. brlaser and `brpdf` place the first
raster pixel at 8pt from the sides and 16pt from the top; the printer reports
12pt uniform over both SNMP and IPP. The test sheet printed without visible
defect, which fits brlaser's model, but the corner marks were not measured, so
a clip of up to ~1.4mm at the extreme edges is not ruled out. Note the test
suite cannot settle this either way -- `brpdf` and `brsim.py` were written from
the same assumption and so agree with each other regardless.

To close it: `make cal.prn && nc 192.168.1.17 9100 < cal.prn`, then measure
paper edge to the solid corner L. 2.82mm means brlaser is right; 4.23mm, or a
sliced-off L, means the firmware is and `brpdf` is losing content at the edges.
The 12pt figure may well be only what the IPP/AirPrint stack guarantees rather
than the engine's physical limit.
