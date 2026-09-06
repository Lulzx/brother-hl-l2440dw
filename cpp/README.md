# Streaming C++ encoder interface

`brhbp.h` is an interface sketch, not an implementation. It exists to pin down
the shape a fast client needs, which is the opposite of how the vendor apps
behave: render and spool every page first, then start printing.

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
