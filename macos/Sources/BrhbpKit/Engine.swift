import BrhbpBridge
import Foundation
import Network

/// Paper sizes, in the order the C bridge expects.
public enum Paper: Int32, CaseIterable, Identifiable, Sendable {
    case a4, letter, legal, a5, a6, b5, b6, executive, c5, dl, monarch
    public var id: Int32 { rawValue }
    public var label: String {
        switch self {
        case .a4: "A4";          case .letter: "US Letter"
        case .legal: "US Legal";  case .a5: "A5"
        case .a6: "A6";           case .b5: "B5"
        case .b6: "B6";           case .executive: "Executive"
        case .c5: "C5";           case .dl: "DL"
        case .monarch: "Monarch"
        }
    }
}

public enum Halftone: Int32, CaseIterable, Identifiable, Sendable {
    case ordered, threshold, diffusion
    public var id: Int32 { rawValue }
    public var label: String {
        switch self {
        case .ordered: "Ordered dither"
        case .threshold: "Threshold"
        case .diffusion: "Error diffusion"
        }
    }
    public var detail: String {
        switch self {
        case .ordered: "Stateless. Good for text and mixed pages."
        case .threshold: "Fastest. Best when the source is already halftoned."
        case .diffusion: "Best tone on photographs. Bands must go out in order."
        }
    }
}

public enum DeviceState: Int32, Sendable {
    case unknown = 0, other = 1, idle = 3, printing = 4, warmup = 5
    public var label: String {
        switch self {
        case .idle: "Ready";      case .printing: "Printing"
        case .warmup: "Warming up"; case .other: "Busy"
        case .unknown: "Unknown"
        }
    }
}

public struct DeviceSnapshot: Sendable, Equatable {
    public init() {}
    public var reachable = false
    public var state: DeviceState = .unknown
    public var errors: UInt32 = 0
    public var impressions: Int64 = -1

    /// Bits of hrPrinterDetectedErrorState that a user can act on.
    public var faults: [String] {
        var out: [String] = []
        // Already normalised LSB-first by the bridge; SNMP itself numbers
        // these MSB-first, which is easy to get backwards and silent when you
        // do -- a real 0x06 (jam + offline) decodes as "no paper, low toner".
        let bits: [(UInt32, String)] = [
            (1 << 0, "Paper low"),   (1 << 1, "Out of paper"),
            (1 << 2, "Toner low"),   (1 << 3, "Out of toner"),
            (1 << 4, "Cover open"),  (1 << 5, "Paper jam"),
            (1 << 6, "Offline"),     (1 << 7, "Service required"),
            (1 << 8, "Input tray missing"),  (1 << 9, "Output tray missing"),
            (1 << 10, "Toner cartridge missing"),
            (1 << 11, "Output tray nearly full"), (1 << 12, "Output tray full"),
            (1 << 13, "Input tray empty"), (1 << 14, "Maintenance overdue"),
        ]
        for (bit, name) in bits where errors & bit != 0 { out.append(name) }
        return out
    }
}

public enum PrintProgress: Sendable {
    case connecting
    case page(index: Int, of: Int)
    case finished(pages: Int, bytes: Int)
    case cancelled(pagesCommitted: Int)
    case failed(String)
}

/// Owns the socket and the C encoder for one job. An actor because the C
/// handle is not thread-safe: everything except `requestCancel` is serialised
/// here, and cancellation is the one operation the C side makes safe to call
/// from anywhere.
public actor PrintEngine {
    private var job: OpaquePointer?
    private var fd: Int32 = -1
    private var cancelled = false

    /// Safe from any task. Only raises a flag inside the encoder; the writing
    /// task then sees its next call fail and calls `abort`.
    nonisolated(unsafe) private var handleForCancel: UnsafeMutableRawPointer?

    public init() {}

    public nonisolated func requestCancel() {
        if let h = handleForCancel {
            brhbp_request_cancel(OpaquePointer(h))
        }
    }

    public func run(
        documentPath: String,
        host: String,
        port: UInt16,
        paper: Paper,
        dpi: Int32,
        copies: Int32,
        duplex: Bool,
        tonerSave: Bool,
        halftone: Halftone,
        maxPages: Int32 = 0,
        onProgress: @Sendable @escaping (PrintProgress) -> Void
    ) async {
        onProgress(.connecting)

        guard let doc = brhbp_doc_open(documentPath) else {
            onProgress(.failed("Could not open the document.")); return
        }
        defer { brhbp_doc_close(doc) }
        let pages = Int(brhbp_doc_pages(doc))
        guard pages > 0 else { onProgress(.failed("The document has no pages.")); return }

        // One socket for the whole document: port 9100 accepts a single
        // connection at a time and refuses the next for a second or two.
        guard let sock = try? Self.connect(host: host, port: port) else {
            onProgress(.failed("Could not reach \(host):\(port).")); return
        }
        fd = sock
        defer { if fd >= 0 { close(fd); fd = -1 } }

        guard let j = brhbp_open_fd(fd, paper.rawValue, dpi, copies, duplex, tonerSave, "brhbp")
        else { onProgress(.failed("Could not start the job.")); return }
        job = j
        handleForCancel = UnsafeMutableRawPointer(j)
        defer { brhbp_close(j); job = nil; handleForCancel = nil }

        let g = brhbp_geometry(j)
        let stride = Int(g.stride)
        let bandRows: Int32 = 64
        var band = [UInt8](repeating: 0, count: stride * Int(bandRows))

        guard brhbp_begin(j) else { onProgress(.failed("The printer closed the connection.")); return }

        var committed = 0
        let limit = maxPages > 0 ? min(pages, Int(maxPages)) : pages
        for p in 0..<limit {
            guard brhbp_begin_page(j) else { break }
            onProgress(.page(index: p + 1, of: limit))

            var y: Int32 = 0
            var pageOK = true
            while y < g.rows {
                let n = min(bandRows, g.rows - y)
                let rendered = band.withUnsafeMutableBufferPointer { buf in
                    brhbp_doc_render_band(doc, Int32(p), dpi, paper.rawValue,
                                          y, n, halftone.rawValue, buf.baseAddress!)
                }
                guard rendered else { pageOK = false; break }
                let wrote = band.withUnsafeBufferPointer { buf in
                    brhbp_write_rows(j, buf.baseAddress!, n)
                }
                guard wrote else { pageOK = false; break }
                y += n
                await Task.yield()
            }
            guard pageOK, brhbp_end_page(j) else { break }
            committed += 1
            // The sheet is on the wire now; the engine starts on it while the
            // loop renders the next page.
        }

        if brhbp_status_code(j) == 4 {          // kCancelled
            _ = brhbp_abort(j)
            onProgress(.cancelled(pagesCommitted: committed))
            return
        }
        guard brhbp_end(j) else { onProgress(.failed("The job did not close cleanly.")); return }
        onProgress(.finished(pages: committed, bytes: 0))
    }

    private static func connect(host: String, port: UInt16) throws -> Int32 {
        var hints = addrinfo(ai_flags: 0, ai_family: AF_UNSPEC, ai_socktype: SOCK_STREAM,
                             ai_protocol: 0, ai_addrlen: 0, ai_canonname: nil,
                             ai_addr: nil, ai_next: nil)
        var res: UnsafeMutablePointer<addrinfo>?
        guard getaddrinfo(host, String(port), &hints, &res) == 0, let list = res else {
            throw POSIXError(.EHOSTUNREACH)
        }
        defer { freeaddrinfo(list) }
        var a: UnsafeMutablePointer<addrinfo>? = list
        while let cur = a {
            let s = socket(cur.pointee.ai_family, cur.pointee.ai_socktype, cur.pointee.ai_protocol)
            if s >= 0 {
                var tv = timeval(tv_sec: 10, tv_usec: 0)
                setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
                if Darwin.connect(s, cur.pointee.ai_addr, cur.pointee.ai_addrlen) == 0 { return s }
                close(s)
            }
            a = cur.pointee.ai_next
        }
        throw POSIXError(.ECONNREFUSED)
    }
}

/// Status polling and out-of-band cancel. Free functions: they hold no state.
public enum Device {
    public static func poll(host: String, community: String = "public") async -> DeviceSnapshot {
        // The C call blocks on a UDP round trip, so it goes off the current
        // executor rather than parking a cooperative thread.
        await Task.detached(priority: .utility) {
            let s = brhbp_poll_status(host, community, 1500)
            var snap = DeviceSnapshot()
            snap.reachable = s.ok
            snap.state = DeviceState(rawValue: s.state) ?? .unknown
            snap.errors = s.errors
            snap.impressions = s.life_count
            return snap
        }.value
    }

    /// Ask the device to drop what it is holding. `PrintEngine.requestCancel`
    /// only stops what has not been sent; this is the only way to reach a job
    /// the printer has already committed.
    public static func cancelOnDevice(host: String) async -> Bool {
        let user = NSUserName()
        return await Task.detached(priority: .userInitiated) {
            let a = brhbp_ipp_cancel_current(host, 631, "/ipp/print", user)
            let b = brhbp_ipp_purge(host, 631, "/ipp/print", user)
            return a == 0 || b == 0
        }.value
    }
}
