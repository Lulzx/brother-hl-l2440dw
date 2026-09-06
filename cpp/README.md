# Streaming C++ encoder interface

A working streaming encoder for the Brother HBP raster format. `brhbp.h` is the
interface, `brhbp.cc` the implementation, `brhbp_tool` a CLI that mirrors
`brpdf` so the two can be diffed, and `brhbp_bench` an in-process benchmark.

    make -C cpp        # build
    make -C cpp test   # 11 byte-for-byte cases against brpdf
    make -C cpp bench

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

    A4 @600 dpi   1.10 ms/page   911 pages/s   encoder working set 64 KB
    A4 @1200 dpi  2.61 ms/page   383 pages/s   encoder working set 64 KB

The working set is flat across resolutions because nothing scales with page
size: one reference row, one encoded row, one band.

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
