# How Brother Mobile Connect 1.23.6 processes a document (Android)

Static analysis of the shipped APK -- `jadx` over the three DEX files, `nm -D`
over the native libraries. Interoperability research on a device and an app we
own. Nothing here required defeating any protection: the classes are Kotlin
compiled to DEX with ordinary name obfuscation.

## The pipeline

`com.brooklyn.bloomsdk` is Brother's internal SDK ("bloom"). Printing runs
through `print/pipeline/` as five stages:

    RasterizeStage -> LayoutStage -> BuildStage -> PreviewStage -> TransferStage

The stage base classes give the data flow away immediately:

    RasterizeStage extends common.h<File>
    LayoutStage    extends o<File, File>
    BuildStage     extends o<File, File>
    PreviewStage   extends g<File, File>
    l (transfer)   extends g<File, File>

Every stage takes a `java.io.File` and produces a `java.io.File`. Each page is
written to disk and re-read at every step.

## Why it is slow

**1. Full-page ARGB_8888 bitmaps.** `pipeline/stage/a.java` fixes the rasterize
settings:

    Bitmap.Config config = Bitmap.Config.ARGB_8888;
    Bitmap.CompressFormat compressFormat = Bitmap.CompressFormat.PNG;
    this.f8758g = 100;                 // quality

The app's own page-size table (`print/caps/b.java`) defines A4 as 4960 x 7014
at 600 dpi. At 4 bytes per pixel that is a **139 MB bitmap per page**, for a
printer whose native input is 1 bit per pixel -- 4.4 MB. The pipeline carries
32x more data than the device can use.

**2. PNG at quality 100, per page, per stage.** Deflating a page of that size
costs roughly 0.24 s for text and 1.8 s for photographic content measured on a
desktop M-series CPU; a phone is several times slower, and it happens at more
than one stage. `PreviewerImpl` additionally writes a `preview_image.jpg` per
page. This is the visible "doing something with every page".

**3. Nothing streams.** `l.java` (transfer) reads a whole page into a byte
array before sending any of it:

    public final byte[] n(File page) {
        long length = page.length();
        if (length > 2147483647L)
            throw new OutOfMemoryError("File " + page + " is too big ...");
        byte[] bArrCopyOf = new byte[i];
        ...

**4. Out-of-memory is a designed-for condition, not an edge case.**
`OutOfMemoryError` is caught in `stage/b.java` (three sites), `stage/i.java`,
and thrown explicitly twice in `l.java`. `p107s1/a.java` converts it into a
`PrintExecutionException`.

**5. The fast path is disabled exactly when you need it.** There are two
controllers -- `PipelineControllerParallelImpl` and
`PipelineControllerSequentialImpl` -- and the choice is made in
`com/brother/mfc/mobileconnect/model/print/i.java`:

    if (Build.VERSION.SDK_INT < 26 || !pageSize.q(caps.b.f8606v)) return false;
    Pair pixels = pageSize.o(res.a(), res.b());
    Pair ref    = caps.b.f8607w.o(600, 600);
    return pixels.second * pixels.first <= ref.second * ref.first;

with `f8606v` = A3 and `f8607w` = A4 (4960 x 7014). Decoded, PARALLEL requires
all of:

* Android 8.0 or newer;
* paper strictly smaller than A3;
* page area at the job's resolution <= A4-at-600dpi = **34,789,440 px**.

A4 at 600 dpi is 34,789,440 px -- it passes by exactly zero margin. A4 at 1200
dpi is 139,157,760 px, four times over, so **every 1200 dpi job falls back to
the sequential pipeline**, as does anything at or above A3. The quality setting
that most needs throughput is the one that disables the concurrent path.

## What the native libraries do

    libpdfium.so        PDF rasterisation (the Chrome engine)
    libbraltpdf.so      JNI wrapper -> com.brooklyn.bloomsdk.pdfrenderercompat
    libBsNetDevAccs.so  SNMP/BER device discovery (brtcl::CTSnmpMsg, CTBERformat)
    libBsDeviceAccs.so  device access
    libBsUsbDevAccs.so  USB transport
    libScanSDK.so       scanning

`nm -D` finds **no** raster compression, halftoning or dithering symbols in any
of them. The heavy per-page image work is in managed Kotlin on the JVM, not in
native code. PDFium itself is fast; everything wrapped around it is not.

## Resolution modes

`print/d.java` carries Brother's byte-code table for resolution. It is a pair,
not a scalar: `p085o1/e.java` stores `dpiX` and `dpiY` as separate
`@SerializedName` fields.

| Code | dpiX x dpiY | Code | dpiX x dpiY |
|---|---|---|---|
| 1 | 300 x 300 | 10 | 300 x 1200 |
| 2 | 600 x 300 | 11 | 300 x 2400 |
| 3 | 600 x 600 | 12 | 300 x 6000 |
| **4** | **600 x 2400** | 13 | 600 x 1200 |
| 5 | 1200 x 1200 | 14 | 600 x 6000 |
| 6 | 1200 x 2400 | 15 | 1200 x 300 |
| 7 | 1200 x 600 | 48 | 1200 x 6000 |
| 9 | 300 x 600 | | *(8 absent)* |

Code 4 is **600 x 2400**, which is exactly the `prtMarkerAddressability` this
printer reports over SNMP. That is independent confirmation from Brother's own
code that the asymmetric engine is real and a first-class mode, not an SNMP
reporting quirk -- and it matches the firmware exposing `RESOLUTIONX` and
`RESOLUTIONY` as independent settables. Neither brlaser nor brpdf ever sets
them. See FORMAT.md section 7.

One oddity worth verifying against smali before believing it: `e.equals()`
decompiles as `this.dpiX == other.dpiY && this.dpiY == other.dpiY`, comparing X
against the other object's Y. Harmless for square modes, wrong for every
asymmetric one. It may be a jadx artifact.

## What the app gets right

The findings above are one-sided by construction -- they are the answer to "why
is this slow". In fairness:

* **PDFium for rendering.** The engine Chrome uses: native, fast, well
  maintained. The correct choice, and the right one to keep.
* **The pipeline is well built.** Five typed stages with cancellation,
  progress callbacks, interruption handling and preview generation wired in as
  a stage rather than bolted on.
* **Memory was considered, not ignored.** `PipelineControllerParallelImpl`
  exists and the sequential fallback is a deliberate guard with a real budget.
  `OutOfMemoryError` handling at six sites is defensive engineering.
* **PWG Raster is a standard**, so one code path serves every model instead of
  a proprietary encoder per engine family.

And that last point is the actual explanation. `SupportModelFixed.json` lists
**314 models** across `LASER`, `INKJET`, `LED` and `THERMAL`, with a further
872 explicitly unsupported. ARGB_8888 is the one buffer format that serves
colour inkjet photographs, label printers and mono lasers alike; the disk
round-trips give cancellation points and preview reuse across five stages.

Those are reasonable choices for a 314-model general-purpose app. Anything in
this repo is faster because it targets one raster format on one engine family
and refuses to be general -- it deletes requirements the vendor cannot delete.
That is a narrower problem, not better engineering, and it is worth saying so
plainly rather than claiming a 30x speedup without naming what was given up.

Two things remain defects rather than trade-offs. PNG at quality 100 for
*intermediate* files nobody ever looks at is lossless recompression of data
about to be discarded. And the parallel path switching off at exactly 1200 dpi
disables concurrency precisely where throughput matters most.

## The wire format: PWG Raster over port 9100

Resolved statically; no capture was needed, and the earlier guess that this
was an IPP path was wrong.

**Transport is raw TCP 9100**, not IPP. `print/transfer/b.java` is
`Port9100TransfererImpl` and constructs `network.b(byName, 9100)`. There is no
`ipp://` or `ipps://` URI anywhere in the app.

**Payload is PWG Raster**, not Brother's mode-1030. `p107s1/f.java` builds it
directly:

    this.f17888h = ByteBuffer.allocate(1796).array();     // PWG page header
    byte[] bytes = "PwgRaster".getBytes(charset);         // PWG magic
    PwgColorSpace pwgColorSpace = printParameter.l() == PrintColor.COLOR
                                  ? PwgColorSpace.SRGB : PwgColorSpace.SGRAY;

1796 bytes is the PWG Raster page header size, and the `Pair<Integer,Integer>`
fields scattered through the constructor are its byte offsets -- `(372,375)`
width, `(376,379)` height, `(388,391)` bits-per-pixel -- matching the spec.
Nothing in any codec contains the string `1030` or emits an ESC byte, so the
mode-1030 line-delta compression this repo documents is not used at all.

**Optional gzip.** `p107s1/g.java` overrides the byte transform with a
`GZIPOutputStream` kept open across pages; `p107s1/a.java` and `f.java` return
the identity instead. So the stream is PJL `JOB`/`EOJ` wrapping PWG Raster,
gzipped on the models that enable it.

### What that costs

The colour space is `SGRAY` (8 bits per pixel) or `SRGB` (24), against the
1 bit per pixel the engine actually consumes:

| A4 @600 dpi | Raw page |
|---|---|
| PWG `SRGB`, colour mode | 104.4 MB |
| PWG `SGRAY`, mono | 34.8 MB |
| HBP mode-1030, native | 4.3 MB |

On the wire the comparison goes the *other* way once gzip is counted, and it
is worth being straight about it. Same page, A4 at 600 dpi, both pipelines run
end to end:

| Stream | Bytes |
|---|---|
| PWG raster, raw | 908,023 |
| HBP mode-1030 | 465,174 |
| **PWG raster + gzip (what the app sends)** | **54,589** |

mode-1030 is a delta scheme with no entropy coder; gzip is a real one. On a
mostly-white page gzip beats it by 8.5x. Note this only applies on models where
the app enables its gzip layer -- `p107s1/g.java` gates it behind a per-model
flag, and the other codecs return the identity.

That difference does not matter in practice, which is the point. At 100 Mbit
the gap is 37 ms versus 4 ms, against an engine that spends ~14,000 ms per
sheet. Wire size is not the constraint; host cost is.

Worth noting the gzip step is *not* the bottleneck: compressing a full page of
8-bit gray takes ~0.06 s for text on a desktop CPU. The cost is everywhere
else -- the 139 MB ARGB_8888 bitmaps, the PNG temp files at quality 100, and
the four disk round-trips.
