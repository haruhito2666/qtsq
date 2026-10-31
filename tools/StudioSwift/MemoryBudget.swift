import Foundation

/// App-wide memory budget shared by encode and decode flows.
///
/// The library enforces its own ceiling for a single encode/decode working
/// set (qtsq_memory_limit_bytes, default 5 GB). This adds the layer the
/// library cannot see: every QTSQFile the app holds keeps a sealed copy plus,
/// once unlocked, a decrypted copy, so a handful of large videos can exceed
/// the per-operation budget on their own. Callers reserve before allocating
/// and release when the context is freed.
enum QTSQMemoryBudget {
    private static let lock = NSLock()
    private static var reserved: UInt64 = 0

    /// Total ceiling for everything QTSQ holds at once.
    static var limitBytes: UInt64 {
        UInt64(qtsq_memory_limit_bytes())
    }

    static var reservedBytes: UInt64 {
        lock.lock(); defer { lock.unlock() }
        return reserved
    }

    static var limitDescription: String {
        ByteCountFormatter.string(fromByteCount: Int64(limitBytes), countStyle: .file)
    }

    /// Reserves `bytes`. Returns false (and reserves nothing) if that would
    /// push the app past its share of the ceiling.
    @discardableResult
    static func reserve(_ bytes: UInt64) -> Bool {
        lock.lock(); defer { lock.unlock() }
        // Leave headroom for decode buffers and the display pipeline so the
        // process as a whole stays under the ceiling, not just the contexts.
        let ceiling = limitBytes / 2
        if bytes > ceiling || reserved + bytes > ceiling {
            return false
        }
        reserved += bytes
        return true
    }

    static func release(_ bytes: UInt64) {
        lock.lock(); defer { lock.unlock() }
        reserved = bytes > reserved ? 0 : reserved - bytes
    }

    /// Reservation that releases itself when the token goes out of scope.
    final class Token {
        private let bytes: UInt64
        init?(_ bytes: UInt64) {
            guard QTSQMemoryBudget.reserve(bytes) else { return nil }
            self.bytes = bytes
        }
        deinit { QTSQMemoryBudget.release(bytes) }
    }
}
