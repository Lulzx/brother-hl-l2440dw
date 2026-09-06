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
    /// Remembered between launches. No address is baked into the source; the
    /// first run picks up BRPRINTER if it is set, otherwise the field starts
    /// empty and whatever is typed is kept.
    var host: String = UserDefaults.standard.string(forKey: "printerHost")
        ?? ProcessInfo.processInfo.environment["BRPRINTER"]
        ?? "" {
        didSet { UserDefaults.standard.set(host, forKey: "printerHost") }
    }
    var port: UInt16 = 9100

    // Settings
    var paper: Paper = .a4
    var dpi: Int32 = 600
    var copies: Int32 = 1
    var duplex: DuplexMode = .off
    var tonerSave = false
    var halftone: Halftone = .ordered

    // Live state
    var device = DeviceSnapshot()
    var isPrinting = false
    var statusLine = "Choose a document."
    var currentPage = 0
    var cancelling = false

    private var engine: PrintEngine?
    @ObservationIgnored private let renderer = PreviewPool()
    private var inFlight: Set<Int> = []

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
        let scoped = url.startAccessingSecurityScopedResource()
        let path = url.path(percentEncoded: false)
        previews = [:]
        inFlight = []
        selectedPage = 0
        documentURL = url
        statusLine = "Opening\u{2026}"

        Task { [renderer] in
            let n = await renderer.open(path: path)
            if scoped { url.stopAccessingSecurityScopedResource() }
            await MainActor.run {
                self.pageCount = n
                self.statusLine = n == 0
                    ? "Could not open that file."
                    : "\(n) page\(n == 1 ? "" : "s") ready."
                // Belt and braces: kick off the first screenful directly
                // rather than waiting for the grid to ask. Cheap -- a page is
                // a few milliseconds -- and it means the window is never
                // showing spinners it has not actually started work for.
                for p in 0..<min(n, 12) { self.renderPreview(page: p) }
            }
        }
    }

    /// Previews come from the same renderer the print path uses, at 110 dpi
    /// instead of 600, so what is on screen is what the encoder will see.
    ///
    /// Rendering happens on the `PreviewRenderer` actor, never on the main
    /// thread: a page costs 3-50 ms and a screenful is a dozen pages, which is
    /// long enough to stall the window if done inline.
    func renderPreview(page: Int) {
        guard previews[page] == nil, !inFlight.contains(page), page < pageCount else { return }
        inFlight.insert(page)
        Task { [renderer] in
            let bmp = await renderer.render(page: page)
            await MainActor.run {
                self.inFlight.remove(page)
                guard let bmp, let image = Self.makeImage(bmp) else { return }
                self.previews[page] = image
            }
        }
    }

    private static func makeImage(_ b: PreviewBitmap) -> CGImage? {
        guard let provider = CGDataProvider(data: Data(b.gray) as CFData) else { return nil }
        return CGImage(width: b.width, height: b.height,
                       bitsPerComponent: 8, bitsPerPixel: 8, bytesPerRow: b.width,
                       space: CGColorSpaceCreateDeviceGray(),
                       bitmapInfo: CGBitmapInfo(rawValue: 0), provider: provider,
                       decode: nil, shouldInterpolate: true, intent: .defaultIntent)
    }

    // MARK: - Status

    /// One long-lived loop that re-reads the address every tick, so editing
    /// the field takes effect without restarting anything, and an empty field
    /// simply idles instead of polling nothing.
    func startPolling() {
        poll.task?.cancel()
        poll.task = Task { [weak self] in
            while !Task.isCancelled {
                let h = await MainActor.run { self?.host ?? "" }
                if h.isEmpty {
                    await MainActor.run { self?.device = DeviceSnapshot() }
                } else {
                    let snap = await Device.poll(host: h)
                    await MainActor.run { self?.device = snap }
                }
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
