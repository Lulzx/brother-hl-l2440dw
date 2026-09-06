package dev.brhbp

import android.os.ParcelFileDescriptor
import java.net.Socket
import java.nio.ByteBuffer
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Kotlin face of the streaming HBP encoder.
 *
 * Kotlin owns the socket; native owns the encoding. The fd is handed across
 * once at construction and encoded bytes never come back over JNI. Rows go
 * down a band at a time in a direct ByteBuffer, so the renderer and the
 * encoder share one allocation with no copy.
 *
 * One socket per document, not per page: port 9100 accepts a single connection
 * at a time and refuses the next for a second or two after close.
 */
class HbpEncoder private constructor(
    private val handle: Long,
    private val pfd: ParcelFileDescriptor,
) : AutoCloseable {

    /** width_px, rows, stride, origin_x, origin_y of the printable area. */
    val geometry: IntArray = nativeGeometry(handle)
    val widthPx get() = geometry[0]
    val rows    get() = geometry[1]
    val stride  get() = geometry[2]

    private val cancelled = AtomicBoolean(false)

    fun begin()      = nativeBegin(handle)
    fun beginPage()  = nativeBeginPage(handle)
    fun endPage()    = nativeEndPage(handle)
    fun end()        = nativeEnd(handle)

    /** [buf] must be direct and hold [nRows] * [stride] bytes. */
    fun writeBand(buf: ByteBuffer, nRows: Int): Boolean {
        require(buf.isDirect) { "band buffer must be a direct ByteBuffer" }
        return nativeWriteBand(handle, buf, nRows)
    }

    /**
     * Safe to call from any thread, including the UI thread. Only raises a
     * flag: the next [writeBand] or [beginPage] on the writing thread fails,
     * and that thread calls [abort]. Nothing else may touch the socket.
     */
    fun requestCancel() {
        if (cancelled.compareAndSet(false, true)) nativeRequestCancel(handle)
    }

    /**
     * Close the raster escape and end the job without ejecting the page in
     * flight. Call on the writing thread after a call has failed.
     *
     * This abandons what has not been sent. It cannot recall pages the device
     * already committed -- for those, use the IPP cancel on port 631.
     */
    fun abort() = nativeAbort(handle)

    fun status() = Status.entries.getOrElse(nativeStatus(handle)) { Status.SINK_ERROR }

    override fun close() {
        nativeDestroy(handle)
        pfd.close()
    }

    enum class Status { OK, SINK_ERROR, BAD_ARGUMENT, TOO_MANY_ROWS, CANCELLED }

    enum class Paper { A4, LETTER, LEGAL, A5, A6, B5, B6, EXECUTIVE, C5, DL, MONARCH }

    companion object {
        init { System.loadLibrary("brhbp") }

        fun open(
            socket: Socket,
            paper: Paper = Paper.A4,
            dpi: Int = 600,
            copies: Int = 1,
            duplex: Boolean = false,
            tonerSave: Boolean = false,
            jobName: String = "job",
        ): HbpEncoder {
            val pfd = ParcelFileDescriptor.fromSocket(socket)
            val h = nativeCreate(pfd.fd, paper.ordinal, dpi, copies, duplex, tonerSave, jobName)
            check(h != 0L) { "failed to create encoder" }
            return HbpEncoder(h, pfd)
        }

        @JvmStatic private external fun nativeCreate(
            fd: Int, paper: Int, dpi: Int, copies: Int,
            duplex: Boolean, tonerSave: Boolean, jobName: String): Long
        @JvmStatic private external fun nativeGeometry(h: Long): IntArray
        @JvmStatic private external fun nativeBegin(h: Long): Boolean
        @JvmStatic private external fun nativeBeginPage(h: Long): Boolean
        @JvmStatic private external fun nativeWriteBand(h: Long, buf: ByteBuffer, nRows: Int): Boolean
        @JvmStatic private external fun nativeEndPage(h: Long): Boolean
        @JvmStatic private external fun nativeEnd(h: Long): Boolean
        @JvmStatic private external fun nativeRequestCancel(h: Long)
        @JvmStatic private external fun nativeAbort(h: Long): Boolean
        @JvmStatic private external fun nativeStatus(h: Long): Int
        @JvmStatic private external fun nativeDestroy(h: Long)
    }
}
