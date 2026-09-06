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

    /// `maxEdge` is the longer side in pixels. Sizing to the thumbnail rather
    /// than to a fixed dpi is what keeps a large-format drawing from costing
    /// twenty times an A4 page to show at the same size on screen.
    public func render(page: Int, maxEdge: Int = 700) -> PreviewBitmap? {
        guard let d = doc else { return nil }
        var w: Int32 = 0, h: Int32 = 0
        var gray: UnsafeMutablePointer<UInt8>?
        guard brhbp_doc_preview_fit(d, Int32(page), Int32(maxEdge), &w, &h, &gray),
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

/// A small pool of independent renderers.
///
/// `PreviewRenderer` is an actor, so a single one serialises every page. That
/// is fine for text (a few ms each) and obviously wrong for scanned documents,
/// where a page costs 40 ms and a screenful is a dozen pages. MuPDF's
/// `fz_context` cannot be shared across threads, so the way to go wider is
/// more contexts, not locks: each worker opens the document independently.
///
/// Pages are assigned `page % count`, which needs no coordination at all --
/// the pool itself holds no mutable state and is therefore plainly `Sendable`.
public final class PreviewPool: Sendable {
    private let workers: [PreviewRenderer]

    public init(count: Int = min(6, ProcessInfo.processInfo.activeProcessorCount)) {
        workers = (0..<max(1, count)).map { _ in PreviewRenderer() }
    }

    /// Opens the document on every worker. Returns the page count.
    public func open(path: String) async -> Int {
        await withTaskGroup(of: Int.self) { group in
            for w in workers { group.addTask { await w.open(path: path) } }
            var n = 0
            for await r in group { n = max(n, r) }
            return n
        }
    }

    public func render(page: Int, maxEdge: Int = 700) async -> PreviewBitmap? {
        await workers[page % workers.count].render(page: page, maxEdge: maxEdge)
    }

    public func close() async {
        for w in workers { await w.close() }
    }
}
