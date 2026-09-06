# Brother HL-L2440DW print pipeline, reverse-engineered

## 1. What the printer actually speaks

Brother's own specification sheet for the HL-L2400 family lists PCL6 emulation
only for the HL-L2460DN/DW/DWXL, HL-L2461DN/DW and HL-L2865DW. The HL-L2400D,
HL-L2400DW, HL-L2440DW and HL-L2445DW have no page description language at all.
They are *host-based* ("GDI") engines: the host must rasterise every page to a
1-bit-per-pixel bitmap and ship it in Brother's proprietary compressed raster
format. Brother's USB device ID for this class advertises `CMD:PJL,HBP` (HBP =
Host Based Printing). The same engine family (HL-L2300D … HL-L2390DW, DCP-L25xx,
MFC-L27xx) is driven by the open-source brlaser CUPS filter, and users report
brlaser working on the HL-L2400DW/HL-L2440DW/HL-L2460DW (GitHub issues #212,
#216, #222), so the stream below is known to be accepted by this hardware.

The printer also supports AirPrint/Mopria (`URF`, PWG raster over IPP). That is
a second, standards-based, driver-free path, but it is not the Brother pipeline
and is out of scope here.

Transport: raw TCP port 9100 (JetDirect), USB bulk, or IPP with the job body
sent as `application/octet-stream`. The printer does not acknowledge raw jobs;
PJL `INFO`/`INQUIRE` queries get a text reply terminated with a form feed.

## 2. Job envelope (PJL)

```
<128 x 0x00>                                  wake-up padding
ESC % - 1 2 3 4 5 X @PJL \n                   UEL + start of PJL
@PJL JOB NAME="<ascii, no quotes/backslash, <=79 chars>" \n
ESC % - 1 2 3 4 5 X @PJL \n                   (per page header; brlaser emits it once per distinct settings)
@PJL SET RAS1200MODE = FALSE|TRUE \n          TRUE => 1200x1200 raster, RESOLUTION stays 600
@PJL SET RESOLUTION = 300|600 \n
@PJL SET ECONOMODE = OFF|ON \n                toner save
@PJL SET SOURCETRAY = AUTO|T1|T2|T3|MP|MANUAL \n
@PJL SET MEDIATYPE = PLAIN|THIN|THICK|THICKER|BOND|TRANS|ENV|ENV-THICK|ENV-THIN \n
@PJL SET PAPER = A4|LETTER|LEGAL|A5|A6|B5|B6|EXECUTIVE|C5|DL|MONARCH \n
@PJL SET PAGEPROTECT = AUTO \n
@PJL SET ORIENTATION = PORTRAIT \n
@PJL ENTER LANGUAGE = PCL \n                  "PCL" here is only the escape-sequence syntax
... page data (section 3) ...
ESC % - 1 2 3 4 5 X @PJL \n
@PJL EOJ NAME="<same name>" \n
ESC % - 1 2 3 4 5 X \n
```

## 3. Page data (PCL-syntax wrapper)

| Bytes | Meaning |
|---|---|
| `ESC E` | Printer reset, once after ENTER LANGUAGE |
| `ESC & l <n> X` | Number of copies |
| `ESC & l 2 S` | Long-edge duplex (omit for simplex) |
| `ESC * b 1030 m` | Select Brother compression mode 1030. Lower-case `m`: the escape sequence continues |
| `<len> w <band>` | One band. `len` is ASCII decimal = band size in bytes (max 16350 + 2). Repeated for every band, still inside the same escape sequence |
| `1030 M` | Upper-case terminator closing the escape sequence (harmless re-selection of mode 1030) |
| `\f` | Form feed = eject the page |

There is no width, height, or cursor command. Position is implicit: the first
row lands at the top-left corner of the printable area, which sits 8 pt from
the left edge and 16 pt from the top (right and bottom borders are 8 pt). At
600 dpi that is 66 px in and 133 px down. Rows may be any length; trailing
white is simply omitted. Anything wider than the paper is clipped by the engine.

## 4. Band structure

```
band := uint16_be  n_rows          (encoders use 64 or, in Brother's driver, 128 rows)
        row * n_rows
row  := 0xFF                       an all-white row; also resets the reference row to white
      | uint8 n_edits (0..254)     edits applied to the previous row ("reference")
        edit * n_edits
```

Each row is described as edits to the *previous decoded row*. The decoder keeps
one line buffer; every edit first skips `offset` unchanged bytes, then writes
bytes. Bytes after the last edit are inherited from the reference unchanged.
The first row of a page and the first row of every band are encoded with no
reference (one substitute covering the whole row, or `0xFF`), so a band never
depends on the previous band.

```
edit := substitute | repeat

substitute (bit7 = 0):   [0 o o o o c c c]  overflow(offset-15)  overflow(count-7)  <count+1 literal bytes>
    o = min(offset,15), c = min(count,7) where count = literals-1

repeat (bit7 = 1):       [1 o o c c c c c]  overflow(offset-3)   overflow(count-31) <value>
    o = min(offset,3), c = min(count,31) where count = run_length-2  (run_length >= 2)

overflow(v): omitted if v < 0; a single byte if v < 255; otherwise (v/255) bytes of 0xFF followed by v%255.
    Decoder: sum bytes until one that is not 0xFF.
```

Byte order within a row is MSB-first, 1 = black, identical to PBM (P4).

## 5. Encoder heuristics (brlaser, reproduced byte-for-byte in brpdf.c)

For a row vs its reference: strip the common suffix; then loop: skip common
prefix (becomes `offset`), and choose a substitute of length
`substitute_length` (stop when two consecutive bytes match the reference again,
or when a run of three equal bytes begins) else a repeat of the current run. At
most 254 edits per row; the 254th dumps the remainder as one substitute. Bands
are flushed every 64 rows or when the next row would push the band past 16350
bytes; the following row is then re-encoded without a reference.

## 6. Duplex

The engine flips on the long edge and prints the back side as received, so the
host must send even pages rotated 180 degrees (this is the `Duplex rotated`
attribute in brlaser's PPD, normally done by CUPS). brpdf does the rotation
itself before cropping the margins. Untested on real hardware here; brlaser
issue #212 reports garbage back pages on an HL-L2400DWE, which may point to a
firmware difference in this generation, so treat duplex as experimental.

## 7. Resolution modes

| PJL | Raster | Notes |
|---|---|---|
| `RESOLUTION = 300`, `RAS1200MODE = FALSE` | 300 x 300 | draft |
| `RESOLUTION = 600`, `RAS1200MODE = FALSE` | 600 x 600 | default |
| `RESOLUTION = 600`, `RAS1200MODE = TRUE` | 1200 x 1200 | "HQ1200"; roughly 2.7x the data |

## 8. Sources

* brlaser (Peter De Wachter, GPL-2): `src/job.cc`, `src/line.cc`, `src/block.h`,
  `src/brdecode.cc`, `brlaser.drv.in`. Copied to `ref/brlaser/` for the
  conformance harness.
* Brother online specification for HL-L2400D/DW, HL-L2440DW, HL-L2445DW,
  HL-L2460DN/DW/DWXL, HL-L2461DN/DW, HL-L2865DW (emulation table).
* brlaser GitHub issues #212, #216, #222 (field reports for the HL-L24x0 family).
