# Android binding

`brhbp_jni.cc` plus `HbpEncoder.kt`. Two decisions keep the hot path off the
JNI boundary:

* **Output is a file descriptor, not a callback.** Kotlin owns the socket and
  passes its fd in once; native writes encoded bytes straight to it. Nothing
  comes back across JNI.
* **Input is a band, not a row.** A direct `ByteBuffer` holding 64 rows means
  one crossing per band instead of 64, and the renderer and encoder share one
  allocation with no copy.

Per-job crossings are create / begin / beginPage / endPage / end / destroy, and
one per band. At A4/600 that is ~107 band calls per page.

## Cancellation

`requestCancel()` is safe from any thread and only raises a flag. The *writing*
thread's next call fails with `CANCELLED`, and that thread calls `abort()`,
which closes the raster escape and ends the job **without ejecting the page in
flight**. Only the writing thread ever touches the socket, so no locking.

`abort()` abandons what has not been sent; it cannot recall pages the device has
already committed. For those, use the IPP cancel on port 631 (`ippcancel.h`).
Build both into the app before it can send a second byte -- there is no
back-channel on 9100 and no PJL cancel command, so an app without this has no
way to stop a job except by killing power.

## Building

Point `Android.mk`/CMake at `../brhbp.cc` and `brhbp_jni.cc`; there are no
dependencies beyond libc. Rendering is separate -- link MuPDF or PDFium and
have it fill the direct `ByteBuffer` one band at a time, as `brhbp_pdf.cc`
does on the desktop.
