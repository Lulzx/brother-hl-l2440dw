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

## Not established here

Where the final on-wire raster is produced. `p107s1/a.java`'s byte transform
`m(byte[], boolean)` is the identity function and its `f()` returns false, so
that codec sends page bytes unmodified; the app references port 631 far more
often than 9100 (292 vs 36 occurrences), so its primary path is likely IPP
rather than the HBP stream this repo documents. Confirming the exact wire
format would need a packet capture, not static analysis.
