import BrhbpBridge
import BrhbpKit
import Observation
import SwiftUI
import UniformTypeIdentifiers

@MainActor
@Observable
final class PrintModel {
    // Document
    var documentURL: URL?
    var pageCount = 0
    var previews: [Int: CGImage] = [:]
    var selectedPage = 0

    // Destination
    var host = "192.168.1.17"
    var port: UInt16 = 9100

    // Settings
    var paper: Paper = .a4
    var dpi: Int32 = 600
    var copies: Int32 = 1
    var duplex = false
    var tonerSave = false
    var halftone: Halftone = .ordered

    // Live state
    var device = DeviceSnapshot()
    var isPrinting = false
    var statusLine = "Choose a document."
    var currentPage = 0
    var cancelling = false

    private var engine: PrintEngine?
    private var doc: OpaquePointer?

    /// `deinit` on a `@MainActor` type cannot touch isolated state, and
    /// `@Observable` will not accept `nonisolated` on a stored var. A box
    /// sidesteps both: the reference is immutable, and `Task.cancel()` is
    /// safe from anywhere.
    @ObservationIgnored private let poll = TaskBox()

    private final class TaskBox: @unchecked Sendable {
        var task: Task<Void, Never>?
    }

    var canPrint: Bool { documentURL != nil && !isPrinting && pageCount > 0 }

    // MARK: - Document

    func load(url: URL) {
        closeDoc()
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }

        guard let d = brhbp_doc_open(url.path(percentEncoded: false)) else {
            statusLine = "Could not open that file."
            return
        }
        doc = d
        documentURL = url
        pageCount = Int(brhbp_doc_pages(d))
        previews = [:]
        selectedPage = 0
        statusLine = "\(pageCount) page\(pageCount == 1 ? "" : "s") ready."
        renderPreview(page: 0)
    }

    /// Previews come from the same renderer the print path uses, so what is on
    /// screen is what the encoder will see -- just at screen scale. At 110 dpi
    /// a page costs about 13 ms, which is why this is not cached aggressively.
    func renderPreview(page: Int) {
        guard let d = doc, page >= 0, page < pageCount, previews[page] == nil else { return }
        var w: Int32 = 0, h: Int32 = 0
        var gray: UnsafeMutablePointer<UInt8>?
        guard brhbp_doc_preview(d, Int32(page), 110.0 / 72.0, &w, &h, &gray), let g = gray
        else { return }
        defer { brhbp_free(g) }
        let count = Int(w) * Int(h)
        guard count > 0, let provider = CGDataProvider(data: Data(bytes: g, count: count) as CFData)
        else { return }
        previews[page] = CGImage(
            width: Int(w), height: Int(h), bitsPerComponent: 8, bitsPerPixel: 8,
            bytesPerRow: Int(w), space: CGColorSpaceCreateDeviceGray(),
            bitmapInfo: CGBitmapInfo(rawValue: 0), provider: provider,
            decode: nil, shouldInterpolate: true, intent: .defaultIntent)
    }

    private func closeDoc() {
        if let d = doc { brhbp_doc_close(d); doc = nil }
    }

    // MARK: - Status

    func startPolling() {
        poll.task?.cancel()
        let h = host
        poll.task = Task { [weak self] in
            while !Task.isCancelled {
                let snap = await Device.poll(host: h)
                await MainActor.run { self?.device = snap }
                try? await Task.sleep(for: .seconds(3))
            }
        }
    }

    func stopPolling() { poll.task?.cancel(); poll.task = nil }

    // MARK: - Printing

    func print() {
        guard let url = documentURL, !isPrinting else { return }
        isPrinting = true
        cancelling = false
        currentPage = 0
        statusLine = "Connecting…"

        let e = PrintEngine()
        engine = e
        let path = url.path(percentEncoded: false)
        // Snapshot the settings: the task must not read back through `self`.
        let (h, prt, pap, res, cps) = (host, port, paper, dpi, copies)
        let (dup, save, half) = (duplex, tonerSave, halftone)

        // One weak reference, captured once. Nesting two `[weak self]` lists
        // makes the outer one a captured var, which Swift 6 rejects.
        let handler: @Sendable (PrintProgress) -> Void = { [weak self] progress in
            Task { @MainActor in self?.apply(progress) }
        }
        let done: @Sendable () -> Void = { [weak self] in
            Task { @MainActor in
                self?.isPrinting = false
                self?.cancelling = false
                self?.engine = nil
            }
        }

        Task {
            await e.run(documentPath: path, host: h, port: prt, paper: pap,
                        dpi: res, copies: cps, duplex: dup,
                        tonerSave: save, halftone: half, onProgress: handler)
            done()
        }
    }

    private func apply(_ p: PrintProgress) {
        switch p {
        case .connecting:
            statusLine = "Connecting…"
        case .page(let i, let n):
            currentPage = i
            statusLine = "Sending page \(i) of \(n)…"
        case .finished(let pages, _):
            statusLine = "Sent \(pages) page\(pages == 1 ? "" : "s"). The printer is working through them."
        case .cancelled(let committed):
            statusLine = committed == 0
                ? "Cancelled before anything was sent."
                : "Cancelled. \(committed) page\(committed == 1 ? "" : "s") had already been sent and will still print."
        case .failed(let why):
            statusLine = why
        }
    }

    /// Stop sending. Pages already handed over cannot be un-sent from here --
    /// `cancelOnDevice` is the only thing that can reach those.
    func cancel() {
        guard isPrinting else { return }
        cancelling = true
        statusLine = "Stopping…"
        engine?.requestCancel()
    }

    func cancelOnDevice() {
        let h = host
        Task { @MainActor in
            statusLine = "Asking the printer to drop the job…"
            let ok = await Device.cancelOnDevice(host: h)
            statusLine = ok
                ? "The printer accepted the cancel."
                : "The printer refused the cancel. Use the panel button."
        }
    }

    deinit { poll.task?.cancel() }
}
