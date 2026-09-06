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
600 dpi tier and defaults to 300. That cap is CUPS's, not the printer's: a
600 dpi URF submitted directly over IPP is accepted and printed. See the
README, *Two ways to print*.

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

### Do not send compressed or otherwise arbitrary bytes on 9100

The engine does not sniff, frame or validate what arrives on this port. Once
`ENTER LANGUAGE = PCL` has been issued it feeds every following byte through
the PCL-syntax parser, and **`\f` (0x0c) ejects a sheet** wherever it appears.
Band lengths are ASCII decimal read inline, so there is no length field the
parser can use to skip over a region it does not understand: it cannot resync,
it just keeps interpreting.

Any high-entropy payload therefore contains roughly one form feed per 256
bytes and turns into that many pages. Verified the expensive way on an
HL-L2440DW: a 54,589-byte gzip stream, wrapped in `@PJL JOB`/`@PJL EOJ` and
sent to 9100 to see whether the firmware would accept compression, contained
250 `0x0c` bytes and began printing 250 pages. The impression counter advanced
three sheets within the first second. There is no software stop -- the device
answers no back-channel (section 6b), and it was only halted by pulling the
power.

The vendor Android app *does* gzip its payload, but it can: it sends PWG
Raster as a single opaque blob that the firmware hands to a raster decoder,
never to the PCL parser. Mode-1030 has no such envelope. The two are not
interchangeable, and there is no PJL variable to enable compression either --
`INQUIRE` returns `"?"` for `COMPRESS`, `COMPRESSION`, `GZIP`, `DATACOMPRESS`,
`RASTERCOMPRESS`, `DEFLATE` and `ENCODING`, the same as an invented name.

If you need to experiment on this port, do it against `brsim.py` first. It
decodes the same stream and costs no paper.

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
itself before cropping the margins.

**Confirmed on real hardware for long-edge duplex.** A brpdf duplex job printed
on an HL-L2440DW (firmware Ver.1.24) advanced `prtMarkerLifeCount` by exactly 2
on a single sheet, and on the calibration sheet the back side reads upright when
the page is turned about its long edge -- so the 180-degree pre-rotation is
correct for this engine and is the right default. See section 1b.

Do not be misled by the printer's IPP `pwg-raster-document-sheet-back = normal`,
which says the back image should *not* be rotated: that describes only the
AirPrint/PWG path, where the firmware rasterises for itself and compensates
internally. It does not apply to the HBP path documented here.

Two things remain open. Short-edge binding (`BINDING = SHORTEDGE`) is untested
and brpdf does not emit it. And brlaser issue #212 reports garbage back pages on
an HL-L2400DWE, a model not tested here -- that may be a firmware difference
within the generation, so the result above should be read as confirmed for the
HL-L2440DW rather than for the whole family.

## 6b. Status readback: what the device will and will not tell you

Tested against firmware Ver.1.24 with `@PJL ECHO <<VAR>>` delimiters so every
reply is attributable, and `BOGUSVARNAME3` as a negative control.

**PJL unsolicited status does not exist on this firmware.** Both `USTATUS` and
`USTATUSOFF` answer `"?"`, i.e. unknown, exactly like the invented control
name. There is therefore no way to have the printer push `@PJL USTATUS JOB` /
`PAGE` / `DEVICE` messages back during a job, which is the mechanism a PJL
driver would normally use to follow progress. Confirmed behaviourally as well:
holding the 9100 socket open for 45 seconds across two complete print jobs
returned **zero** back-channel bytes.

So on 9100 the device is effectively write-only during printing. Status has to
come from a second transport, and SNMP works well for it -- SNMP, IPP and 9100
can be used concurrently, unlike two simultaneous 9100 connections.

Sampling SNMP once a second across a job gives clean transitions:

| OID | Meaning | Observed |
|---|---|---|
| `1.3.6.1.2.1.25.3.5.1.1.1` | `hrPrinterStatus` | `idle` -> `printing` -> `idle` |
| `1.3.6.1.2.1.25.3.5.1.2.1` | `hrPrinterDetectedErrorState` | `"00 "` throughout (no error) |
| `1.3.6.1.2.1.43.10.2.1.4.1.1` | `prtMarkerLifeCount` | increments as each sheet lands |

`hrPrinterStatus` goes to `printing` about 1.2 s after the job is written to the
socket and back to `idle` when the sheet is out, so a poll loop on those three
OIDs is a workable substitute for the missing PJL back-channel: it gives
start, finish, per-sheet progress and error state.

Variables that *are* supported (silent to `INQUIRE`), beyond those in section
1b: `RESOLUTIONX`, `RESOLUTIONY`, `DENSITY`, `PRINTQUALITY`, `IMAGEADAPT`,
`TIMEOUT`, `AUTOCONT`, `JOBOFFSET`, `HOLD`, `CPLOCK`, `POWERSAVE`. Not
supported: `USTATUS`, `USTATUSOFF`, `TONERSAVE`, `SLEEP`, `PAGEPROTECT`.

`RESOLUTIONX` and `RESOLUTIONY` are the interesting pair -- the firmware models
the two axes independently, which matches the asymmetric engine (section 7) and
is not something brlaser or brpdf ever sets.

## 6c. Consumables, as the device reports them

From `prtMarkerSuppliesTable` (`1.3.6.1.2.1.43.11.1.1`), firmware Ver.1.24:

| Supply | Max capacity (`.8`) | Level (`.9`) | Behaviour |
|---|---|---|---|
| `Black Toner Cartridge` | `-2` (unknown) | `-3` | no level reported at all |
| `Drum Unit` | `15000` | `13103` | counts **down**, one per impression |

Two things worth recording.

The drum is a plain page countdown: 15000 at full, and it fell by exactly 2
across the two sheets printed while measuring this. Nothing about it is
mysterious and it can be read remotely at any time.

The toner cartridge reports **no measurable level over SNMP** -- `-2` is
"unknown" capacity and `-3` is the Printer-MIB code for a supply that is known
to be present but whose level the device cannot quantify. Whatever percentage a
host-side utility displays for toner is therefore not coming from the
cartridge over this interface; it is an estimate computed somewhere else. That
is worth knowing before treating any toner-remaining figure as a measurement.

## 7. Resolution modes

| PJL | Raster | Notes |
|---|---|---|
| `RESOLUTION = 300`, `RAS1200MODE = FALSE` | 300 x 300 | draft |
| `RESOLUTION = 600`, `RAS1200MODE = FALSE` | 600 x 600 | default |
| `RESOLUTION = 600`, `RAS1200MODE = TRUE` | 1200 x 1200 | "HQ1200"; see below |

### The engine is not square

`prtMarkerAddressability` reports **600 dpi in the feed direction and 2400 dpi
in the cross-feed direction** (`...43.10.2.1.9.1.1 = 600`, `.10.1.1 = 2400`).
That is 600 laser scanlines per inch down the page and 2400 modulation steps
per inch across it, so a literal 1200 x 1200 addressable grid is not something
this hardware has. Whatever "1200 dpi" means here, it is a mapping onto a
600 x 2400 engine, not a doubling of both axes.

The firmware's own `RESOLUTION` enum offers six values -- `300, 600, 900, 1200,
HQ1200, TR1200` -- and it separately knows `RESOLUTIONX` and `RESOLUTIONY` as
independent settables (section 6b). brlaser, and therefore brpdf, uses none of
that: it sets `RAS1200MODE = TRUE`, sends `RESOLUTION = 600`, and encodes the
bitmap exactly as at 600 dpi with twice the rows and twice the stride. That is
an inherited assumption, not a measured fact.

**What has been established on hardware (firmware Ver.1.24).** A brpdf 1200 dpi
job -- A4, 13633 rows of 1207 bytes, i.e. 2x in both axes versus the same page
at 600 dpi -- was accepted and printed, one impression, no error, no operator
intervention, `hrPrinterDetectedErrorState` clear throughout. So the engine does
not reject the doubled geometry, and brlaser's assumption is at least not
catastrophically wrong.

**Corroboration from Brother's own app.** The Android app carries a byte-code
table mapping resolution codes to `dpiX` x `dpiY` pairs (`print/d.java`), and
code 4 is **600 x 2400** -- the exact figure this engine reports over SNMP. The
table also holds 1200x1200, 1200x2400, 600x1200 and 1200x600. So asymmetric
modes are first-class in Brother's protocol, resolution is a pair rather than a
scalar, and "1200 dpi" on this family may well be one of the asymmetric codes
rather than a square grid. That would explain why 1200 mode costs roughly 2.7x
the data rather than the 4x a true doubling of both axes would give. See
VENDOR-APP.md.

**What is still open.** Whether the extra data becomes real resolution on paper,
and whether it does so equally in both axes. `tools/restest.py` generates the
page that answers this: gratings specified in line pairs per inch (so the same
physical target is drawn at either resolution), each drawn twice -- once with
vertical bars, which vary along the 2400 dpi cross-feed axis, and once with
horizontal bars, which vary along the 600 dpi feed axis. If the engine is
really 600 x 2400, the vertical blocks should stay resolved to a much finer
pitch than the horizontal ones, and the 300 lp/in block should be stripes on
the 1200 sheet where it is uniform grey on the 600 sheet. Equal behaviour in
both axes would mean the addressability figures do not describe the raster
path. This needs a loupe; it cannot be settled over the network.

## 8. Sources

* brlaser (Peter De Wachter, GPL-2): `src/job.cc`, `src/line.cc`, `src/block.h`,
  `src/brdecode.cc`, `brlaser.drv.in`. Copied to `ref/brlaser/` for the
  conformance harness.
* Brother online specification for HL-L2400D/DW, HL-L2440DW, HL-L2445DW,
  HL-L2460DN/DW/DWXL, HL-L2461DN/DW, HL-L2865DW (emulation table).
* brlaser GitHub issues #212, #216, #222 (field reports for the HL-L24x0 family).
