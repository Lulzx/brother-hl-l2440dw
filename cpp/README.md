# Streaming C++ encoder interface

A working streaming encoder for the Brother HBP raster format. `brhbp.h` is the
interface, `brhbp.cc` the implementation, `brhbp_tool` a CLI that mirrors
`brpdf` so the two can be diffed, and `brhbp_bench` an in-process benchmark.

    make -C cpp        # build
    make -C cpp test   # 18 checks: 11 conformance, 3 cancel, 4 SNMP encoding
    make -C cpp bench
    make -C cpp jni    # Android shared library (needs a JDK)

`brhbp_pdf` is the end-to-end path: PDF in, job out, no page bitmap anywhere.
MuPDF renders one 64-row band straight into a band-sized pixmap, the band is
halftoned to 1 bit, fed to the encoder and dropped. It builds only if MuPDF is
installed; the rest does not need it.

    ./cpp/brhbp_pdf -p A4 -r 600 doc.pdf > job.prn

## Correctness

Output is **byte-identical to `brpdf`** for every case in `conformance.sh`:
both resolutions either side of the default, multi-page, duplex with and
without back-page rotation, copies, toner save, and photographic content that
defeats the delta coder. `brpdf` is in turn byte-identical to brlaser, so the
C++ encoder inherits that verification rather than asserting its own.

Every optimisation in `brhbp.cc` changes only how fast the same answer is
found, never the answer. The three hot loops -- common-prefix skip,
common-suffix trim, run length -- are word-at-a-time with `__builtin_ctzll`
instead of byte-at-a-time, with scalar fallbacks for big-endian. The
substitute-length scan is a 3-wide sliding window and stays scalar.

## Measured

Encoder alone, A4:

    @600 dpi   1.10 ms/page   911 pages/s   encoder working set 64 KB
    @1200 dpi  2.61 ms/page   383 pages/s   encoder working set 64 KB

End to end through `brhbp_pdf`, 3-page Letter PDF -- open, render, halftone,
encode:

    dpi     ms/page    peak RSS
    300        34.5    12,144 KB
    600        69.2    12,320 KB
    1200      150.7    12,688 KB

MuPDF's own baseline is 9,568 KB of that, so the pipeline above it is ~2.6 MB
at 300 dpi and ~3.1 MB at 1200. Sixteen times the pixels for 4.5% more memory:
nothing in the path scales with page area, only with band width. The vendor
Android app allocates a 139 MB ARGB_8888 bitmap for one A4 page at 600 dpi.

Correctness of the rendered path was checked against MuPDF's own 1-bit output
on the same page: 5.673% ink coverage versus 5.685%.

Halftoning is an ordered (Bayer) dither by default, and `-t` selects a plain
threshold. Ordered dither is stateless per pixel, so a band's output does not
depend on the band above it -- which is what keeps bands independent. Error
diffusion would give better tone on photographs but carries state across the
band boundary and would couple them.

## Why it is shaped this way

The design follows one property of the wire format. Bands are self-contained --
a band never references data in a previous band -- so a page can be emitted 64
rows at a time and the rows thrown away immediately.

    A4 @600 dpi   full page ARGB_8888   139 MB
                  full page 1-bit         4.4 MB
                  this encoder           ~55 KB

Measured on the target hardware: a page sent on its own printed 14.3 s later
while the following page had not been transmitted at all. The engine spends
~14 s per sheet, so every page after the first can be rendered inside time that
is already being spent. Time to first sheet stops scaling with page count.

    batch (2 s/page)     streaming
    1 page    2 s          2 s
    10 pages  20 s         2 s
    50 pages  100 s        2 s

`example_stream.cc` shows the loop. Note what is absent from it: no page
bitmap, no temp files, no second pass.

Two things to keep in mind when implementing:

* One socket for the whole document. Port 9100 accepts a single connection at
  a time and refuses the next for a second or two after close, so a connection
  per page is a guaranteed stall.
* Parallel band encoding is possible and `EncodeBand` is there for it, but it
  changes where band boundaries land on dense pages, which breaks byte-identity
  with brlaser and therefore the conformance test. Keep the serial path for
  anything you want to verify against the reference encoder.

Halftoning is deliberately outside this interface: the encoder takes 1-bit rows
and the caller owns the screening. Worth knowing that the choice interacts with
threading -- error diffusion carries state along the row and into the next one,
so it is serial down the page, while ordered or clustered-dot dithering is
stateless per pixel and parallelises with the bands.


## Cancellation

There is no back-channel on port 9100 and no PJL cancel command, so a job can
only be abandoned, never recalled. Two layers, and an app needs both before it
sends its first byte.

**In-band** (`JobEncoder`). `RequestCancel()` is safe from any thread and only
raises a flag; the *writing* thread's next call fails with `kCancelled` and
that thread calls `Abort()`. Only the writer touches the sink, so no locking.

`Abort()` is not just a UEL, and this is the part worth knowing. Bands live
inside one open `ESC * b 1030 m` escape which the parser keeps reading as
`<digits><letter>` pairs until a letter arrives in upper case. A UEL sent into
that state is swallowed as parameters -- `-12345` matches the number field and
`X` looks like the terminator -- so everything after it is parsed as PCL. The
simulator reports exactly that: three bogus `ESC*b` commands and 30 stray
bytes. `Abort()` therefore closes the raster sequence with `1030M` first, with
**no form feed**, so the page in flight is abandoned rather than ejected, and
only then sends UEL and `@PJL EOJ`.

`cancel_check.sh` judges this with the printer model rather than by assertion:

    control: three pages, no cancel                      3 pages, 0 warnings
    cancel mid page 2 + Abort(): page 2 abandoned        1 page,  0 warnings
    naive stop, no Abort(): leaves an open escape        1 page,  1 warning

**Out-of-band** (`ippcancel.h`). `Abort()` cannot touch what the device has
already committed. IPP on port 631 can: `CancelCurrentJob()`, `CancelJob(id)`
and `PurgeJobs()`, dependency-free -- IPP is an HTTP POST with a binary body.
Encoding was checked against a local `ippeveprinter`, which returns the same
IPP-level response to our requests as to `ipptool`'s. The operations themselves
still need the real printer; `ippeveprinter` does not implement them.

## Status

`status.h` polls three OIDs over SNMP, which is the only channel that reports
progress and, unlike a second 9100 connection, works *during* a job:

    hrPrinterStatus              idle / printing / warmup
    hrPrinterDetectedErrorState  no paper, jam, door open, low toner, ...
    prtMarkerLifeCount           increments once per impression

The request encoder is verified byte-identical to net-snmp's for all four OIDs,
with no printer involved: `snmpget` is aimed at a local UDP socket that
captures the packet, its request-id is read back out, and our encoder is asked
for the same one.

**Error bits are MSB-first**, and this was a real bug rather than a
hypothetical. SNMP `BITS` put bit 0 in the 0x80 position of the first octet.
Decoding LSB-first is silent -- the value still parses, into the wrong faults.
A genuine `0x06` from the device meant `jammed` + `offline` after power was
pulled mid-print, and read as "no paper, low toner" instead. Plausible enough
that only the owner saying "there is plenty of paper" caught it. `status.cc`
now reverses each octet on the way in, so `ErrorBit` stays readable LSB-first
in the API, and both octets are decoded rather than just the first.

## Halftoning

    default   ordered Bayer 8x8   stateless, bands independent
    -t        plain threshold     fastest, right for already-halftoned input
    -E        Floyd-Steinberg     best tone, see below

Error diffusion carries residue into the row below, so it has to cross band
boundaries. Keeping one error row outside the band loop lets it stream -- bands
still go out one at a time, they just have to go out *in order*. That rules out
encoding bands in parallel, which ordered dither would allow. It is opt-in for
that reason, not because of quality.
