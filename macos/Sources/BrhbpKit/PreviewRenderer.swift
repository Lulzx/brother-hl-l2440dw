import BrhbpBridge
import Foundation

public struct PreviewBitmap: Sendable {
    public let page: Int
    public let width: Int
    public let height: Int
    public let gray: [UInt8]      // 8-bit, one byte per pixel, row-major
}

/// Renders page previews off the main thread.
///
/// An actor rather than a queue because MuPDF's `fz_context` is not
/// thread-safe: this owns its own document handle, separate from the one the
/// print path uses, and serialises every call into it. Previews therefore
/// cannot interfere with a job in flight.
public actor PreviewRenderer {
    private var doc: OpaquePointer?

    public init() {}

    /// Returns the page count, or 0 if the document could not be opened.
    public func open(path: String) -> Int {
        close()
        guard let d = brhbp_doc_open(path) else { return 0 }
        doc = d
        return Int(brhbp_doc_pages(d))
    }

    public func render(page: Int, dpi: Double = 110) -> PreviewBitmap? {
        guard let d = doc else { return nil }
        var w: Int32 = 0, h: Int32 = 0
        var gray: UnsafeMutablePointer<UInt8>?
        guard brhbp_doc_preview(d, Int32(page), Float(dpi / 72.0), &w, &h, &gray),
              let g = gray else { return nil }
        defer { brhbp_free(g) }
        let count = Int(w) * Int(h)
        guard count > 0 else { return nil }
        return PreviewBitmap(page: page, width: Int(w), height: Int(h),
                             gray: [UInt8](UnsafeBufferPointer(start: g, count: count)))
    }

    public func close() {
        if let d = doc { brhbp_doc_close(d); doc = nil }
    }

    // No deinit: a nonisolated deinit cannot touch actor state under Swift 6.
    // Callers close explicitly, and the handle is freed when `open` is called
    // again, so the only leak window is process exit.
}
