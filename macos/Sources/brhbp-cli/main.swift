// brhbp-cli -- the app's engine, without the app.
//
// Same PrintEngine actor, same bridge, same band renderer the UI drives; only
// the buttons are missing. Exists so the print path can be exercised and
// watched from a terminal.
import BrhbpKit
import Foundation

let args = CommandLine.arguments
guard args.count >= 2 else {
    print("""
    usage: brhbp-cli DOCUMENT.pdf [--host H] [--paper A4|LETTER] [--dpi N]
                     [--copies N] [--duplex] [--toner-save]
                     [--halftone ordered|threshold|diffusion] [--pages N]
                     [--status] [--cancel-after SECONDS]
    """)
    exit(2)
}

// args[1] is the document unless it is a flag, so `--status` works alone.
var path = args[1].hasPrefix("--") ? "" : args[1]
var host = "192.168.1.17"
var paper = Paper.a4
var dpi: Int32 = 600
var copies: Int32 = 1
var duplex = false, tonerSave = false, statusOnly = false
var halftone = Halftone.ordered
var maxPages: Int32 = 0
var cancelAfter: Double = 0
var previewCount = 0

var i = path.isEmpty ? 1 : 2
while i < args.count {
    switch args[i] {
    case "--host":       i += 1; host = args[i]
    case "--paper":      i += 1; paper = args[i].uppercased() == "LETTER" ? .letter : .a4
    case "--dpi":        i += 1; dpi = Int32(args[i]) ?? 600
    case "--copies":     i += 1; copies = Int32(args[i]) ?? 1
    case "--pages":      i += 1; maxPages = Int32(args[i]) ?? 0
    case "--duplex":     duplex = true
    case "--toner-save": tonerSave = true
    case "--status":     statusOnly = true
    case "--previews":   i += 1; previewCount = Int(args[i]) ?? 0
    case "--cancel-after": i += 1; cancelAfter = Double(args[i]) ?? 0
    case "--halftone":
        i += 1
        halftone = args[i] == "threshold" ? .threshold : (args[i] == "diffusion" ? .diffusion : .ordered)
    default: FileHandle.standardError.write("unknown option \(args[i])\n".data(using: .utf8)!); exit(2)
    }
    i += 1
}
if !statusOnly && path.isEmpty {
    FileHandle.standardError.write("no document given\n".data(using: .utf8)!)
    exit(2)
}

// Top-level code is concurrently executed under Swift 6, so anything the
// tasks below call has to be Sendable. Formatting from the value each time
// keeps it stateless rather than sharing a DateFormatter.
@Sendable func stamp() -> String {
    Date.now.formatted(.dateTime.hour().minute().second().secondFraction(.fractional(2)))
}

// Exercises the same PreviewRenderer actor the UI drives, so the async path
// can be checked without a window.
if previewCount > 0 {
    let r = PreviewRenderer()
    let n = await r.open(path: path)
    print("pages: \(n)")
    guard n > 0 else { exit(1) }
    let t0 = Date.now
    var ok = 0
    await withTaskGroup(of: PreviewBitmap?.self) { group in
        for p in 0..<min(n, previewCount) {
            group.addTask { await r.render(page: p) }
        }
        for await bmp in group where bmp != nil { ok += 1 }
    }
    let ms = Date.now.timeIntervalSince(t0) * 1000
    print("rendered \(ok)/\(min(n, previewCount)) previews in \(Int(ms)) ms "
          + "(\(Int(ms) / max(ok, 1)) ms each)")
    await r.close()
    exit(ok == min(n, previewCount) ? 0 : 1)
}

if statusOnly {
    let s = await Device.poll(host: host)
    print("\(stamp())  \(s.reachable ? s.state.label : "unreachable")  " +
          "faults=\(s.faults.isEmpty ? "none" : s.faults.joined(separator: ", "))  " +
          "impressions=\(s.impressions)")
    exit(0)
}

let before = await Device.poll(host: host)
print("\(stamp())  before: \(before.state.label), faults=\(before.faults.isEmpty ? "none" : before.faults.joined(separator: ", ")), impressions=\(before.impressions)")
guard before.reachable else { print("printer unreachable"); exit(1) }
guard before.faults.isEmpty else { print("refusing to print: \(before.faults.joined(separator: ", "))"); exit(1) }

let engine = PrintEngine()

// A watchdog that always exists. Sending to this port without a way to stop is
// exactly how a runaway happens.
if cancelAfter > 0 {
    Task {
        try? await Task.sleep(for: .seconds(cancelAfter))
        print("\(stamp())  watchdog: requesting cancel")
        engine.requestCancel()
    }
}

// Watch the device while the job streams. SNMP works during a print; a second
// connection to port 9100 does not.
let watcher = Task {
    var last: Int64 = before.impressions
    while !Task.isCancelled {
        let s = await Device.poll(host: host)
        if s.impressions != last {
            print("\(stamp())  sheet out -> impressions=\(s.impressions)  (\(s.state.label))")
            last = s.impressions
        } else if !s.faults.isEmpty {
            print("\(stamp())  FAULT: \(s.faults.joined(separator: ", "))")
        }
        try? await Task.sleep(for: .seconds(2))
    }
}

await engine.run(documentPath: path, host: host, port: 9100, paper: paper,
                 dpi: dpi, copies: copies, duplex: duplex, tonerSave: tonerSave,
                 halftone: halftone, maxPages: maxPages) { p in
    switch p {
    case .connecting:            print("\(stamp())  connecting")
    case .page(let i, let n):    print("\(stamp())  sending page \(i)/\(n)")
    case .finished(let n, _):    print("\(stamp())  sent \(n) page(s)")
    case .cancelled(let n):      print("\(stamp())  cancelled; \(n) page(s) had already been sent")
    case .failed(let why):       print("\(stamp())  failed: \(why)")
    }
}

try? await Task.sleep(for: .seconds(25))
watcher.cancel()
let after = await Device.poll(host: host)
print("\(stamp())  after: \(after.state.label), impressions=\(after.impressions) (+\(after.impressions - before.impressions))")
