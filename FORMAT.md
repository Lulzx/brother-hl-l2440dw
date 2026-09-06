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

The printer also supports AirPrint/Mopria (`URF`, PWG raster over IPP) and is
Mopria 2.1 certified. That is a second, standards-based, driver-free path: a
stock CUPS `-m everywhere` queue drives the device with no driver at all. It is
not the Brother pipeline and is not documented here, but it is the easier route
for everyday printing -- with the caveat that CUPS's generated PPD offers no
600 dpi tier and defaults to 300. See the README, *Two ways to print*.

Transport: raw TCP port 9100 (JetDirect), USB bulk, or IPP with the job body
sent as `application/octet-stream`. The printer does not acknowledge raw jobs;
PJL `INFO`/`INQUIRE` queries get a text reply terminated with a form feed.

Confirmed on real hardware -- an HL-L2440DW, firmware `Ver.1.23` and `Ver.1.24`,
model ID `84U-M27`, network board `Brother NC-9300w` -- at 192.168.1.17:

    @PJL INFO ID  ->  "Brother HL-L2440DW:84U-M27:Ver.1.23"\f
    @PJL ECHO x   ->  @PJL ECHO x\f

The reply arrives immediately on the same 9100 connection, with or without a
trailing UEL, and `\n` works as well as `\r\n`. Two caveats found the hard way:
the client must NOT half-close its side of the socket (`shutdown(SHUT_WR)`
after the query yields zero bytes -- which is why `nc` appears to show no
back-channel at all), and the device accepts only one connection on 9100 at a
time, refusing the next for a second or two after the previous one closes.

The device's own IEEE-1284 ID, read over IPP, is
`MFG:Brother;CMD:PJL,HBP,URF;MDL:HL-L2440DW;CLS:PRINTER;CID:Brother Laser Type1`
-- confirming PJL + HBP + AirPrint raster and no PCL or PostScript interpreter.

The unit does not advertise itself over mDNS: browsing `_ipp._tcp`,
`_ipps._tcp`, `_pdl-datastream._tcp` and `_printer._tcp` on the LAN returns
nothing, even though ports 80, 443, 515, 631 and 9100 are all open. It will not
appear in a Bonjour printer picker; address it by IP.

## 1b. What the hardware itself reports

Read off the unit at 192.168.1.17 (firmware `Ver.1.24`, model ID `84U-M27`,
network board `Brother NC-9300w`) with `tools/probe.py`. Checked on both
`Ver.1.23` and `Ver.1.24`: the update changed none of the values below, and in
particular did not resolve either contradiction. Everything here is
first-party -- the device said it -- as opposed to section 1, which is inferred
from brlaser and from Brother's published spec.

| Fact | Source | Value |
|---|---|---|
| Languages | IPP `printer-device-id` | `CMD:PJL,HBP,URF` -- no PCL, no PostScript |
| Resolutions | IPP + PJL `RESOLUTION` enum | 300, 600, 1200 (PJL also lists 900, HQ1200, TR1200) |
| Engine addressability | SNMP `prtMarkerAddressability` | 600 dpi feed x 2400 dpi cross-feed |
| Duplex | IPP `sides-supported` | one-sided, two-sided-long-edge, two-sided-short-edge |
| Copies | IPP + PJL `COPIES` | 1-999 |
| Unprintable margin | SNMP `prtMarkerMargins`, IPP `media-*-margin-supported` | 4233 um = 4.23 mm = **12.0 pt, uniform on all four sides** |
| Back-side transform | IPP `pwg-raster-document-sheet-back` | **`normal`** (do not rotate the back image) |

Two of those contradict the model this repo is built on. Neither can be settled
from the network; a calibration sheet (`tools/testpage.py`) was printed on the
real device to resolve them, with the following results.

* **Duplex back-side rotation -- RESOLVED, brlaser is right.** brlaser's PPD
  says `Duplex rotated`, so CUPS pre-rotates back pages 180 degrees and
  `brpdf -d` reproduces that; the printer's IPP stack advertises
  `pwg-raster-document-sheet-back = normal`. On the printed sheet the back side
  reads upright when the page is turned about its long edge, so the
  180-degree pre-rotation is correct for the HBP path and `brpdf -d`'s default
  stands. The IPP `normal` describes only the AirPrint/PWG path, where the
  firmware rasterises for itself and compensates internally. Section 6's
  long-standing caveat is now discharged for long-edge duplex. Short-edge
  (`BINDING=SHORTEDGE`) remains untested -- `brpdf` does not emit it.

* **Unprintable margin -- still open, but no longer suspected.** brlaser's PPD
  declares `HWMargins 8 8 8 16` (left/bottom/right/top in points) and `brpdf`
  places the first raster pixel at (8pt, 16pt); the firmware reports 12pt
  uniform over both SNMP and IPP. The calibration sheet printed without visible
  defect, which is consistent with brlaser's model, but the corner marks were
  not measured to the millimetre -- so a clip of up to ~1.4mm at the extreme
  edges is not excluded. The likeliest reading is that the firmware's 12pt is
  the conservative area the IPP/AirPrint stack guarantees rather than the
  engine's physical limit. To close this properly, print `make cal.prn` and
  measure paper edge to the solid corner L: 2.82mm confirms brlaser, 4.23mm
  (or a sliced-off L) confirms the firmware.

### The engine accepts the stream

A `brpdf`-generated duplex job (the `tools/testpage.py` calibration sheet, A4 at
600 dpi, 143831 bytes) was sent to tcp/9100 and printed. `prtMarkerLifeCount`
went 1889 -> 1891 and the drum counter fell by the same 2, i.e. exactly two
impressions on one sheet, with no error and no operator intervention. So the
PJL envelope, the PCL-flavoured page header and the mode-1030 raster in this
document are all accepted by the real engine, and `@PJL SET DUPLEX = ON` really
does produce a two-sided sheet rather than two simplex pages.

That result says nothing about where on the paper the image landed; see the
margin and back-side rows above, which only a ruler can settle.

### PJL variables the printer really has

`@PJL INFO VARIABLES` returns a full settings table, but it is **generic Brother
firmware boilerplate, not a model description**: it advertises A3 and LEDGER
paper, TRAY3-6, a staple finisher, proof/store job retention and PCL/PostScript
personalities, none of which this printer has. Do not treat it as ground truth.

`@PJL INQUIRE <var>` is the sharper instrument. It answers `"?"` for a variable
the firmware does not know -- verified against a deliberately invented name --
and stays silent for one it does know, so silence means supported. Applied to
every line `brpdf` emits:

    RAS1200MODE   -> 0     supported (and absent from INFO VARIABLES)
    PAGEPROTECT   -> "?"   NOT SUPPORTED
    BOGUSVARNAME  -> "?"   (negative control)
    SOURCETRAY, MEDIATYPE, PAPER, RESOLUTION, ECONOMODE,
    DUPLEX, BINDING, COPIES, ORIENTATION -> silent, i.e. supported

So `@PJL SET PAGEPROTECT = AUTO` is a dead line -- the printer discards it.
brlaser emits it too (`job.cc:74`), and `brpdf` reproduces brlaser byte for
byte on purpose, so it stays: 27 wasted bytes per job is a better trade than
losing the conformance test that anchors the whole encoder.

Value names are a separate question, still unresolved. The firmware's own
enumerations are `MEDIATYPE={REGULAR,THICK,THICK2,THIN,BOND,ENVELOPES,...}` and
`MEDIASOURCE={AUTOSELECT,TRAY1,TRAY2,...}`, whereas brlaser (and therefore
`brpdf -m` / `-t`) sends `PLAIN`, `THICKER`, `ENV`, `AUTO`, `T1`, `MP`,
`MANUAL`. PJL reports no error for a bad value, so this cannot be tested from
the network; the likely outcome is that the unrecognised ones are ignored and
the tray/media default is used.

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
