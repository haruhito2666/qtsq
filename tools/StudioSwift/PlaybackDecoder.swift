import Foundation
import AppKit
import QuartzCore

/// One decoded frame as an immutable snapshot: the pixel data is converted
/// into the bitmap representation's own storage, so there is no intermediate
/// buffer and no second copy.
final class PlaybackFrame {
    let image: NSImage
    let index: UInt32

    init(width: Int, height: Int, rgb: UnsafePointer<UInt8>, pixelCount: Int, index: UInt32) {
        let rep = NSBitmapImageRep(
            bitmapDataPlanes: nil,
            pixelsWide: width,
            pixelsHigh: height,
            bitsPerSample: 8,
            samplesPerPixel: 3,
            hasAlpha: false,
            isPlanar: false,
            colorSpaceName: .deviceRGB,
            bytesPerRow: width * 3,
            bitsPerPixel: 24
        )!
        if let dst = UnsafeMutablePointer<UInt8>(rep.bitmapData) {
            dst.update(from: rgb, count: pixelCount * 3)
        }
        self.index = index
        self.image = NSImage(size: NSSize(width: width, height: height))
        self.image.addRepresentation(rep)
    }
}

/// Sequential decode thread for pixel playback.
///
/// Decoding is decoupled from presentation: this thread decodes ahead of the
/// playhead into a bounded buffer, and the UI pulls whichever frame matches
/// its wall clock. The two properties that matter for smooth playback:
///
///  * Lookahead must exceed the worst-case single-frame decode time, or an
///    I-frame spike drains the buffer and the display repeats frames.
///  * The buffer must hold more frames than the lookahead, so steady-state
///    decoding never has to evict a frame the playhead has not reached.
final class PlaybackDecoder: @unchecked Sendable {
    private let queue = DispatchQueue(label: "qtsq.video.decode", qos: .userInteractive)
    private let cv = NSCondition()
    private var decoder: OpaquePointer?
    private var totalFrames: UInt32 = 0
    private var width = 0
    private var height = 0
    private let gopSize: UInt32
    private let lookahead: Int
    private let maxReady: Int

    private var cursor: UInt32 = 0
    private var playhead: UInt32 = 0
    private var playheadServed = false
    private var playheadWrapped = false
    private var pendingSeek: UInt32?
    private var playing = false
    private var running = true
    private var readyFrames: [UInt32: PlaybackFrame] = [:]

    /// Diagnostics for the smoothness harness.
    private(set) var decodedCount: UInt64 = 0
    private(set) var stalledTicks: UInt64 = 0

    init(decoder: OpaquePointer,
         totalFrames: UInt32,
         width: Int,
         height: Int,
         gopSize: UInt32) {
        self.decoder = decoder
        self.totalFrames = max(1, totalFrames)
        self.width = max(1, width)
        self.height = max(1, height)
        self.gopSize = max(1, gopSize)
        /* Bound the frame buffer by bytes, not just by count: a 4K frame is
         * ~25 MB, so 8 of them is 200 MB, but an 8K frame is ~100 MB and the
         * same count would be 800 MB. Lookahead must stay under the buffer
         * depth, or decoding outruns the buffer and evicts frames ahead of
         * the playhead that it will never decode again. */
        let frameBytes = self.width * self.height * 3
        let budget = 256 * 1024 * 1024
        let affordable = max(3, budget / max(1, frameBytes))
        self.maxReady = min(Int(self.totalFrames), 8, affordable)
        self.lookahead = max(2, self.maxReady - 2)
        queue.async { [weak self] in self?.run() }
    }

    func start(at index: UInt32) {
        cv.lock()
        playhead = min(index, totalFrames - 1)
        pendingSeek = playhead
        playheadServed = false
        running = true
        playing = true
        cv.broadcast()
        cv.unlock()
    }

    func setPlaying(_ value: Bool) {
        cv.lock()
        playing = value
        if !value {
            // Paused: keep only what the playhead still needs. Frees ~300 MB
            // of 4K bitmaps that would otherwise sit buffered while stopped.
            let head = playhead
            while readyFrames.count > 1 {
                guard let victim = readyFrames.keys.first(where: { $0 != head }) else { break }
                readyFrames.removeValue(forKey: victim)
            }
        }
        cv.signal()
        cv.unlock()
    }

    func setPlayhead(_ index: UInt32) {
        cv.lock()
        let target = min(index, totalFrames - 1)
        if target < playhead { playheadWrapped = true }
        if target != playhead { playheadServed = false }
        playhead = target
        cv.signal()
        cv.unlock()
    }

    func seek(to index: UInt32) {
        cv.lock()
        let target = min(index, totalFrames - 1)
        playhead = target
        pendingSeek = target
        playheadServed = false
        cv.broadcast()
        cv.unlock()
    }

    /// Newest decoded frame at or before `target`, removed from the buffer.
    /// Returns nil when the decoder has not reached that point, in which case
    /// the caller keeps showing the previous frame.
    func takeReadyFrame(atOrBefore target: UInt32) -> PlaybackFrame? {
        cv.lock()
        defer { cv.unlock() }
        var best: UInt32?
        for index in readyFrames.keys where index <= target && (best == nil || index > best!) {
            best = index
        }
        guard let chosen = best else { return nil }
        return readyFrames.removeValue(forKey: chosen)
    }

    func stats() -> (decoded: UInt64, stalled: UInt64, buffered: Int) {
        cv.lock()
        defer { cv.unlock() }
        return (decodedCount, stalledTicks, readyFrames.count)
    }

    /// Stops the decode thread and waits for it to exit before the owner frees
    /// the decoder, so no frame can be read after free.
    func stop() -> OpaquePointer? {
        cv.lock()
        running = false
        playing = false
        pendingSeek = nil
        readyFrames.removeAll()
        cv.broadcast()
        cv.unlock()
        queue.sync {}
        cv.lock()
        let dec = decoder
        decoder = nil
        cv.unlock()
        return dec
    }

    private func keyframeAtOrBefore(_ index: UInt32) -> UInt32 {
        guard let dec = decoder else { return index }
        var kf = index
        let floor = index > gopSize ? index - gopSize : 0
        while true {
            let type = qtsq_video_decoder_get_frame_type(dec, kf)
            if type == UInt8(ascii: "K") || type == UInt8(ascii: "I") || type == UInt8(ascii: "G") {
                return kf
            }
            if kf == floor { return kf }
            kf -= 1
        }
    }

    private func distance(from a: UInt32, to b: UInt32) -> UInt32 {
        b >= a ? b - a : totalFrames - a + b
    }

    private func run() {
        cv.lock()
        while running {
            guard let dec = decoder else { break }
            if let target = pendingSeek {
                pendingSeek = nil
                readyFrames.removeAll()
                cursor = keyframeAtOrBefore(target)
            }
            if playheadWrapped {
                playheadWrapped = false
                readyFrames.removeAll()
                cursor = 0
            }
            /* Never race past the end of the clip: wrapping the cursor on our
             * own would publish frames from the start of the video while the
             * playhead is still near the end, which showed up as a visible
             * jump/glitch on the last second. Only the playhead's own wrap
             * (reported via playheadWrapped) rewinds the cursor. */
            if cursor >= totalFrames {
                cv.wait()
                continue
            }
            // Reach the playhead (first frame, seek, or catching up) …
            let reachesPlayhead = distance(from: cursor, to: playhead) <= gopSize
            let needServe = !playheadServed && reachesPlayhead
            // … and keep a runway ahead of it while playing.
            let prefetch = playing
                && distance(from: playhead, to: cursor) <= UInt32(lookahead)
            if !needServe && !prefetch {
                cv.wait()
                continue
            }
            let index = cursor
            cv.unlock()
            var produced: PlaybackFrame?
            /* AppKit autoreleases the bitmap internals. A GCD queue has no
             * runloop, so without an explicit pool every decoded frame's
             * ~25 MB bitmap is retained forever — a 20 s 4K clip reached
             * 11 GB, and the memory pressure caused the visible stutter.
             * Both the decode and the frame build must run inside a pool. */
            autoreleasepool {
                var pixels: UnsafeMutablePointer<UInt8>? = nil
                var w: UInt32 = 0
                var h: UInt32 = 0
                let rc = qtsq_video_decoder_get_frame(dec, index, &pixels, &w, &h)
                if rc == 0, let rgb = pixels,
                   w == UInt32(self.width), h == UInt32(self.height) {
                    produced = PlaybackFrame(width: self.width, height: self.height,
                                             rgb: UnsafePointer(rgb),
                                             pixelCount: self.width * self.height,
                                             index: index)
                } else {
                    NSLog("[QTSQ Player] decode failed rc=%d at frame %u", rc, index)
                }
            }
            cv.lock()
            if !running { break }
            if let frame = produced {
                readyFrames[index] = frame
                decodedCount += 1
                if index == playhead { playheadServed = true }
                // Never evict the playhead or the frame just decoded: doing so
                // latched playheadServed with nothing left to display, which
                // pinned the view on "Loading..." forever.
                while readyFrames.count > maxReady {
                    let victim = readyFrames.keys
                        .filter { $0 != playhead && $0 != index }
                        .min()
                    guard let v = victim else { break }
                    readyFrames.removeValue(forKey: v)
                }
            }
            cursor = index &+ 1
        }
        cv.unlock()
    }
}
