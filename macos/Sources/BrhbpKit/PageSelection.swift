import Foundation

/// Page selection, shared by the text field and the CLI so the two can never
/// disagree about what "2-4,7" means.
///
/// Pages are 0-indexed internally and 1-indexed in text, because every printer
/// dialog in existence is 1-indexed and the conversion belongs in one place.
public enum PageSelection {
    /// Parses `"1-3,5,8-"` into a page set. Returns nil if the text is
    /// malformed, and an empty string means "all pages" rather than "none" --
    /// an empty field should not silently print nothing.
    public static func parse(_ text: String, pageCount: Int) -> Set<Int>? {
        let trimmed = text.trimmingCharacters(in: .whitespaces)
        guard pageCount > 0 else { return [] }
        if trimmed.isEmpty { return Set(0..<pageCount) }

        var out: Set<Int> = []
        for chunk in trimmed.split(separator: ",") {
            let part = chunk.trimmingCharacters(in: .whitespaces)
            if part.isEmpty { continue }
            let bits = part.split(separator: "-", omittingEmptySubsequences: false)
            switch bits.count {
            case 1:
                guard let n = Int(bits[0]), (1...pageCount).contains(n) else { return nil }
                out.insert(n - 1)
            case 2:
                // "3-" means 3 to the end, "-3" means 1 to 3.
                let lo = bits[0].isEmpty ? 1 : Int(bits[0]) ?? -1
                let hi = bits[1].isEmpty ? pageCount : Int(bits[1]) ?? -1
                guard lo >= 1, hi >= lo, hi <= pageCount else { return nil }
                out.formUnion((lo - 1)...(hi - 1))
            default:
                return nil
            }
        }
        return out
    }

    /// Renders a page set back to the shortest equivalent text, so clicking
    /// thumbnails produces something a person would have typed.
    public static func format(_ pages: Set<Int>, pageCount: Int) -> String {
        if pages.isEmpty { return "" }
        if pages.count == pageCount && pageCount > 0 { return "" }   // "all"
        var out: [String] = []
        var run: [Int] = []
        func flush() {
            guard let first = run.first, let last = run.last else { return }
            out.append(first == last ? "\(first + 1)" : "\(first + 1)-\(last + 1)")
            run = []
        }
        for p in pages.sorted() {
            if let last = run.last, p == last + 1 { run.append(p) } else { flush(); run = [p] }
        }
        flush()
        return out.joined(separator: ",")
    }
}
