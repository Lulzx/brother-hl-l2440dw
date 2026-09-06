# Driver-free printing to a Brother HL-L2440DW (simulated)

The HL-L2440DW has no PCL or PostScript interpreter. It is a host-based engine
that only accepts Brother's compressed 1-bit raster wrapped in PJL. This repo
contains the reverse-engineered description of that stream (`FORMAT.md`), a
~400-line dependency-free C program that produces it from any PDF, and a
software model of the printer that decodes the stream and renders what would
come out of the paper tray. Everything is verified in simulation; no hardware
was used.

```
brpdf.c        PBM pages -> Brother HBP job (PJL + mode-1030 raster)
brsim.py       printer model: decodes jobs to PNG, or listens on tcp/9100 like the real device
FORMAT.md      the wire protocol
ref/           brlaser's original encoder + harness for byte-for-byte conformance
test/          sample PDF and the end-to-end test script
```

## Print a PDF

```sh
make                                   # builds ./brpdf (C99, no libraries)
mutool draw -F pbm -r 600 -o - doc.pdf | ./brpdf -p A4 > job.prn
```

Any rasteriser that emits raw PBM works (`pdftoppm -mono`, Ghostscript
`-sDEVICE=pbmraw`). Options: `-p` paper, `-r 300|600|1200`, `-c` copies,
`-d` duplex, `-e` toner save, `-t` tray, `-m` media type, `-j` job name.

To send to a real printer you would do `nc <printer-ip> 9100 < job.prn` or
`lp -d <raw-queue> -o raw job.prn`. To send to the simulator instead:

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

No real HL-L2440DW was involved. Confidence rests on (a) Brother's spec
confirming the model is host-based, (b) byte-identical output to brlaser,
(c) brlaser user reports on this exact model family. Duplex ordering and the
even-page rotation are the least certain part (see FORMAT.md section 6).
The printer also does AirPrint/IPP Everywhere, which is an alternative
driver-free route that avoids the proprietary format entirely.
