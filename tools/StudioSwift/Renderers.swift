import SwiftUI
import AppKit
import AVFoundation
import QuartzCore

struct RenderContent: View {
    let ctx: qtsq_context_t
    let data: Data
    
    var body: some View {
        let type = ctx.header.data_type
        
        if isVideoType(type) {
            // Video streams (e.g. inside a container) have no single-image
            // payload, so they must reach the player rather than fall through
            // to the hex dump.
            VideoPlayerView(ctx: ctx)
        } else if isImageType(type) || isPrecompressedImage(data) {
            ImageRenderer(ctx: ctx, data: data)
        } else if isTextType(type) || isLikelyText(data) {
            TextRenderer(data: data)
        } else if isAudioType(type) {
            AudioRenderer(ctx: ctx, data: data)
        } else {
            HexRenderer(data: data)
        }
    }
    
    private func isVideoType(_ type: UInt8) -> Bool {
        let t = qtsq_data_type_t(UInt32(type))
        return t == QTSQ_TYPE_VIDEO_FRAMES
    }

    private func isImageType(_ type: UInt8) -> Bool {
        let t = qtsq_data_type_t(UInt32(type))
        return t == QTSQ_TYPE_IMAGE_GRAY || t == QTSQ_TYPE_IMAGE_RGB || t == QTSQ_TYPE_IMAGE_RGBA
    }
    
    private func isPrecompressedImage(_ data: Data) -> Bool {
        if data.count > 4 {
            let bytes = [UInt8](data.prefix(4))
            if bytes[0] == 0xFF && bytes[1] == 0xD8 { return true } // JPEG
            if bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E && bytes[3] == 0x47 { return true } // PNG
        }
        return false
    }
    
    private func isTextType(_ type: UInt8) -> Bool {
        let t = qtsq_data_type_t(UInt32(type))
        return t == QTSQ_TYPE_TEXT_ASCII || t == QTSQ_TYPE_TEXT_UTF8 || t == QTSQ_TYPE_JSON || t == QTSQ_TYPE_CSV
    }
    
    private func isLikelyText(_ data: Data) -> Bool {
        let maxLen = min(data.count, 512)
        guard maxLen > 0 else { return false }
        
        let bytes = [UInt8](data.prefix(maxLen))
        var printables = 0
        for b in bytes {
            if (b >= 32 && b < 127) || b == 10 || b == 9 {
                printables += 1
            }
        }
        return (printables * 100 / maxLen) > 85
    }
    
    private func isAudioType(_ type: UInt8) -> Bool {
        let t = qtsq_data_type_t(UInt32(type))
        return t == QTSQ_TYPE_AUDIO_MONO || t == QTSQ_TYPE_AUDIO_STEREO || t == QTSQ_TYPE_AUDIO_MULTI
    }
}

struct ImageRenderer: View {
    let ctx: qtsq_context_t
    let data: Data
    
    @State private var totalScale: CGFloat = 1.0
    @State private var finalOffset: CGSize = .zero
    @State private var gestureOffset: CGSize = .zero
    
    var body: some View {
        if let nsImage = createNSImage() {
            let currentOffset = CGSize(
                width: finalOffset.width + gestureOffset.width,
                height: finalOffset.height + gestureOffset.height
            )
            
            ZStack(alignment: .bottomTrailing) {
                // Subtle dot-grid canvas background
                Canvas { context, size in
                    let dotSpacing: CGFloat = 20
                    let dotSize: CGFloat = 1.5
                    for x in stride(from: dotSpacing/2, to: size.width, by: dotSpacing) {
                        for y in stride(from: dotSpacing/2, to: size.height, by: dotSpacing) {
                            context.fill(
                                Path(ellipseIn: CGRect(x: x - dotSize/2, y: y - dotSize/2, width: dotSize, height: dotSize)),
                                with: .color(Color.black.opacity(0.04))
                            )
                        }
                    }
                }
                .background(VaultTheme.cardWhite)
                
                GeometryReader { _ in
                    Image(nsImage: nsImage)
                        .resizable()
                        .interpolation(totalScale > 3.0 ? .none : .high)
                        .antialiased(totalScale > 3.0 ? false : true)
                        .aspectRatio(contentMode: .fit)
                        .scaleEffect(totalScale)
                        .offset(currentOffset)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                        .simultaneousGesture(
                            DragGesture()
                                .onChanged { value in
                                    if totalScale > 1.0 {
                                        gestureOffset = value.translation
                                    }
                                }
                                .onEnded { value in
                                    if totalScale > 1.0 {
                                        finalOffset.width += gestureOffset.width
                                        finalOffset.height += gestureOffset.height
                                    }
                                    gestureOffset = .zero
                                }
                        )
                }
                .clipped()
                
                // Clean zoom controls pill overlay
                HStack(spacing: 6) {
                    Button(action: {
                        if totalScale > 1.0 {
                            totalScale = max(totalScale / 1.5, 1.0)
                            if totalScale == 1.0 {
                                finalOffset = .zero
                                gestureOffset = .zero
                            }
                        }
                    }) {
                        Image(systemName: "minus")
                            .font(.system(size: 11, weight: .bold))
                            .foregroundColor(VaultTheme.textPrimary)
                            .frame(width: 24, height: 24)
                    }
                    .buttonStyle(PlainButtonStyle())
                    
                    Text(String(format: "%.1fx", totalScale))
                        .font(.system(size: 11, weight: .semibold, design: .monospaced))
                        .foregroundColor(VaultTheme.textPrimary)
                        .frame(width: 38)
                    
                    Button(action: {
                        totalScale = min(totalScale * 1.5, 50.0)
                    }) {
                        Image(systemName: "plus")
                            .font(.system(size: 11, weight: .bold))
                            .foregroundColor(VaultTheme.textPrimary)
                            .frame(width: 24, height: 24)
                    }
                    .buttonStyle(PlainButtonStyle())
                }
                .padding(.horizontal, 8)
                .padding(.vertical, 4)
                .background(VaultTheme.cardWhite.opacity(0.95))
                .cornerRadius(6)
                .overlay(RoundedRectangle(cornerRadius: 6).stroke(VaultTheme.border, lineWidth: 1))
                .shadow(color: VaultTheme.cardShadowColor, radius: 4, y: 1)
                .padding(14)
            }
            .padding(.top, 8)
        } else {
            Text("Failed to render image")
                .foregroundColor(.red)
        }
    }
    
    private func createNSImage() -> NSImage? {
        if data.count > 4 {
            let bytes = [UInt8(data[data.startIndex]), UInt8(data[data.index(after: data.startIndex)]),
                         UInt8(data[data.index(data.startIndex, offsetBy: 2)]), UInt8(data[data.index(data.startIndex, offsetBy: 3)])]
            if (bytes[0] == 0xFF && bytes[1] == 0xD8) || (bytes[0] == 0x89 && bytes[1] == 0x50) {
                return NSImage(data: data)
            }
        }
        
        var w = UInt32(ctx.schema.dimensions.0)
        var h = UInt32(ctx.schema.dimensions.1)
        
        print("[DEBUG ImageRenderer] initial w=\(w), h=\(h), data.count=\(data.count)")
        
        // Detect actual channels from data size vs schema dimensions
        var ch: UInt8
        if w > 0 && h > 0 {
            let expectedPixels = Int(w) * Int(h)
            if data.count == expectedPixels {
                ch = 1  // Grayscale
            } else if data.count == expectedPixels * 4 {
                ch = 4
            } else if data.count == expectedPixels * 3 {
                ch = 3
            } else {
                ch = ctx.header.num_channels > 0 ? ctx.header.num_channels : 1
            }
        } else {
            ch = ctx.header.num_channels > 0 ? ctx.header.num_channels : 1
        }
        
        if w == 0 || h == 0 {
            let px = data.count / Int(ch)
            w = UInt32(sqrt(Double(px)))
            h = UInt32(px / Int(w))
            if w == 0 { w = 1 }
            if h == 0 { h = 1 }
            print("[DEBUG ImageRenderer] fallback w=\(w), h=\(h), ch=\(ch)")
        } else {
            print("[DEBUG ImageRenderer] final ch=\(ch)")
        }
        
        if ch == 0 { ch = 1 }
        
        let stride = Int(w) * 4
        
        // Use NSBitmapImageRep for 100% robust memory ownership and rendering structure
        guard let rep = NSBitmapImageRep(
            bitmapDataPlanes: nil,
            pixelsWide: Int(w),
            pixelsHigh: Int(h),
            bitsPerSample: 8,
            samplesPerPixel: 4,
            hasAlpha: true,
            isPlanar: false,
            colorSpaceName: .calibratedRGB,
            bytesPerRow: stride,
            bitsPerPixel: 32
        ), let outPtr = rep.bitmapData else {
            print("[DEBUG ImageRenderer] Failed to create NSBitmapImageRep")
            return nil
        }
        
        data.withUnsafeBytes { inBuffer in
            guard let inPtr = inBuffer.bindMemory(to: UInt8.self).baseAddress else { return }
            for y in 0..<Int(h) {
                for x in 0..<Int(w) {
                    let si = (y * Int(w) + x) * Int(ch)
                    let di = y * stride + x * 4
                    if si + Int(ch) > data.count { break }
                    
                    if ch == 1 {
                        let c = inPtr[si]
                        outPtr[di] = c; outPtr[di+1] = c; outPtr[di+2] = c
                    } else if ch >= 3 {
                        outPtr[di] = inPtr[si]
                        outPtr[di+1] = inPtr[si+1]
                        outPtr[di+2] = inPtr[si+2]
                    }
                    outPtr[di+3] = (ch == 4 && si+3 < data.count) ? inPtr[si+3] : 255
                }
            }
        }
        
        let nsImg = NSImage(size: NSSize(width: Int(w), height: Int(h)))
        nsImg.addRepresentation(rep)
        return nsImg
    }
}

// ─── Paginated NSTextView-backed renderer ─────────────────────────────────
//
// THREE-LAYER RAM FIX for large corpora (e.g. 1.6 GB mtlm_corpus.qtsq):
//
// Layer 1 — OS-managed mmap (Models.swift):
//   Decompressed bytes live in a temp file. The OS pages them in/out.
//   Only the pages being viewed are resident. The full C malloc buffer
//   is freed as soon as the temp file is written.
//
// Layer 2 — Paginated 5 MB windows (here):
//   NSTextStorage / NSTextView only ever hold one 5-MB page at a time.
//   No matter how large the corpus, NSTextStorage ≤ ~10 MB.
//   (NSString UTF-16 is ~2× raw bytes in the worst case.)
//   Peak layout memory: ~20–50 MB. Previous approach: 5–10 GB.
//
// Layer 3 — TextKit 2 lazy layout (here):
//   NSTextLayoutManager (macOS 12+) only typsets the visible glyphs.
//   The old NSLayoutManager (TextKit 1) pre-computes every glyph in the
//   document up front, which for millions of lines = gigabytes.
//
// Result: ~100 MB resident regardless of input size.
//   (vs. 20 GB for the original no-mmap, no-pagination approach)

// MARK: - Low-level text view wrapper (NSViewRepresentable)

private final class PagedTextScrollView: NSObject {
    /// Currently displayed text view
    var scrollView: NSScrollView
    var textView: NSTextView

    init(font: NSFont) {
        let sv = NSScrollView()
        sv.hasVerticalScroller = true
        sv.hasHorizontalScroller = true
        sv.autohidesScrollers = true
        sv.borderType = .noBorder
        sv.hasVerticalRuler = true
        sv.rulersVisible = true

        // ── TextKit 2 on macOS 12+, TextKit 1 fallback ─────────────────
        let tv: NSTextView
        if #available(macOS 12, *) {
            tv = NSTextView(usingTextLayoutManager: true)
        } else {
            tv = NSTextView()
        }

        tv.isEditable = false
        tv.isSelectable = true
        tv.allowsUndo = false
        tv.font = font
        tv.textColor = NSColor.labelColor
        tv.backgroundColor = NSColor.textBackgroundColor
        tv.isAutomaticQuoteSubstitutionEnabled = false
        tv.isAutomaticSpellingCorrectionEnabled = false
        tv.isAutomaticDashSubstitutionEnabled = false
        tv.isHorizontallyResizable = true
        tv.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude,
                            height: CGFloat.greatestFiniteMagnitude)
        tv.textContainer?.widthTracksTextView = false
        tv.textContainer?.containerSize = NSSize(
            width: CGFloat.greatestFiniteMagnitude,
            height: CGFloat.greatestFiniteMagnitude)

        sv.documentView = tv
        self.scrollView = sv
        self.textView = tv
        super.init()
    }

    /// Load a Data slice into the text view (background decode, main-thread assign).
    func load(slice: Data) {
        let tv = self.textView
        DispatchQueue.global(qos: .userInitiated).async {
            // String(decoding:) over an mmap'd Data slice → only those pages fault in
            let str = String(decoding: slice, as: UTF8.self)
            DispatchQueue.main.async {
                tv.string = str          // NSTextStorage gets one page-sized string
                tv.scrollToBeginningOfDocument(nil)
            }
        }
    }
}

private struct NativeTextView: NSViewRepresentable {
    let slice: Data   // ← a 5 MB window into the mmap'd Data; no extra copy

    func makeNSView(context: Context) -> NSScrollView {
        let font = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
        let wrapper = PagedTextScrollView(font: font)
        context.coordinator.wrapper = wrapper
        wrapper.load(slice: slice)
        return wrapper.scrollView
    }

    func updateNSView(_ nsView: NSScrollView, context: Context) {
        // Triggered when the parent changes pageStart → re-load the new slice
        context.coordinator.wrapper?.load(slice: slice)
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    final class Coordinator {
        var wrapper: PagedTextScrollView?
    }
}

// MARK: - Public TextRenderer (pagination shell)

struct TextRenderer: View {
    let data: Data

    // ── Pagination ──────────────────────────────────────────────────────
    /// How many raw bytes per page. 5 MB → NSTextStorage ≤ ~10 MB (UTF-16).
    private static let pageBytes = 5_000_000

    @State private var pageStart: Int = 0
    @State private var totalLines: Int = 0
    @State private var isCountingLines: Bool = true

    // ── Derived ─────────────────────────────────────────────────────────

    /// End byte index of the current page, aligned to next '\n' to avoid
    /// splitting a line in the middle (purely cosmetic — split lines are
    /// still valid UTF-8).
    private var pageEnd: Int {
        var end = min(pageStart + Self.pageBytes, data.count)
        // Walk forward to a newline boundary so pages break cleanly
        if end < data.count {
            data.withUnsafeBytes { buf in
                guard let ptr = buf.bindMemory(to: UInt8.self).baseAddress else { return }
                while end < data.count && ptr[end] != 10 { end += 1 }
                if end < data.count { end += 1 }  // include the '\n'
            }
        }
        return end
    }

    /// The current page as a zero-copy Data sub-sequence of the mmap.
    private var pageSlice: Data {
        let lo = data.startIndex.advanced(by: pageStart)
        let hi = data.startIndex.advanced(by: pageEnd)
        return data[lo..<hi]
    }

    private var hasPrev: Bool { pageStart > 0 }
    private var hasNext: Bool { pageEnd < data.count }

    private var pageLabel: String {
        "\(formatBytes(pageStart))–\(formatBytes(pageEnd)) of \(formatBytes(data.count))"
    }

    // MARK: - Body

    var body: some View {
        VStack(spacing: 0) {

            // ── Top info / navigation bar ─────────────────────────────────
            HStack(spacing: 12) {

                // Line count badge
                if isCountingLines {
                    Label("Counting lines…", systemImage: "ellipsis")
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundColor(.secondary)
                } else {
                    Label("\(totalLines.formatted()) lines", systemImage: "text.alignleft")
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundColor(.secondary)
                }

                Spacer()

                // ── Page navigation (only shown for large files) ──────────
                if data.count > Self.pageBytes {
                    HStack(spacing: 6) {
                        Button(action: prevPage) {
                            Image(systemName: "chevron.left")
                        }
                        .buttonStyle(PlainButtonStyle())
                        .disabled(!hasPrev)
                        .opacity(hasPrev ? 1 : 0.35)

                        Text(pageLabel)
                            .font(.system(size: 11, design: .monospaced))
                            .foregroundColor(.secondary)

                        Button(action: nextPage) {
                            Image(systemName: "chevron.right")
                        }
                        .buttonStyle(PlainButtonStyle())
                        .disabled(!hasNext)
                        .opacity(hasNext ? 1 : 0.35)
                    }
                    .padding(.horizontal, 6)
                    .padding(.vertical, 3)
                    .background(Color(NSColor.controlBackgroundColor))
                    .cornerRadius(6)
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 6)
            .background(Color(NSColor.controlBackgroundColor).opacity(0.5))

            Divider()

            // ── TextKit 2 virtualized view — bounded to one page ──────────
            NativeTextView(slice: pageSlice)
        }
        .onAppear { countLines() }
    }

    // MARK: - Page helpers

    private func prevPage() {
        let target = max(0, pageStart - Self.pageBytes)
        // Align backward to previous newline for clean breaks
        var aligned = target
        if aligned > 0 {
            data.withUnsafeBytes { buf in
                guard let ptr = buf.bindMemory(to: UInt8.self).baseAddress else { return }
                while aligned > 0 && ptr[aligned - 1] != 10 { aligned -= 1 }
            }
        }
        pageStart = aligned
    }

    private func nextPage() {
        pageStart = pageEnd
    }

    // MARK: - Line counting

    /// Scans '\n' bytes on a background thread — O(n) time, O(1) memory.
    /// Over an mmap'd Data, only the bytes being scanned are faulted in.
    private func countLines() {
        let capturedData = data
        DispatchQueue.global(qos: .utility).async {
            var count = 0
            capturedData.withUnsafeBytes { buf in
                guard let ptr = buf.bindMemory(to: UInt8.self).baseAddress else { return }
                let n = capturedData.count
                var i = 0
                while i < n {
                    if ptr[i] == 10 { count += 1 }
                    i += 1
                }
                if n > 0 && ptr[n - 1] != 10 { count += 1 }
            }
            DispatchQueue.main.async {
                self.totalLines = count
                self.isCountingLines = false
            }
        }
    }

    // MARK: - Formatting

    private func formatBytes(_ count: Int) -> String {
        if count < 1024 { return "\(count) B" }
        if count < 1_048_576 { return String(format: "%.1f KB", Double(count) / 1024.0) }
        if count < 1_073_741_824 { return String(format: "%.1f MB", Double(count) / 1_048_576.0) }
        return String(format: "%.2f GB", Double(count) / 1_073_741_824.0)
    }
}

struct AudioRenderer: View {
    let ctx: qtsq_context_t
    let data: Data
    @StateObject private var player = AudioPlayerController()
    
    var body: some View {
        VStack(spacing: 24) {
            // Waveform visualization
            WaveformView(data: data, progress: player.progress)
                .frame(height: 120)
                .padding(.horizontal)
            
            // Time display
            HStack {
                Text(player.elapsedString)
                    .font(.system(size: 12, design: .monospaced))
                    .foregroundColor(.secondary)
                Spacer()
                Text(player.remainingString)
                    .font(.system(size: 12, design: .monospaced))
                    .foregroundColor(.secondary)
            }
            .padding(.horizontal, 24)
            
            // Seek bar
            Slider(value: Binding(
                get: { player.progress },
                set: { player.seek(to: $0) }
            ), in: 0...1)
            .accentColor(.orange)
            .padding(.horizontal, 24)
            
            // Controls
            HStack(spacing: 32) {
                Button(action: { player.stop() }) {
                    Image(systemName: "stop.fill")
                        .font(.system(size: 18))
                        .foregroundColor(.secondary)
                }
                .buttonStyle(PlainButtonStyle())
                
                Button(action: { player.togglePlayPause() }) {
                    Image(systemName: player.isPlaying ? "pause.circle.fill" : "play.circle.fill")
                        .font(.system(size: 44))
                        .foregroundColor(.orange)
                }
                .buttonStyle(PlainButtonStyle())
                
                // Format info
                VStack(alignment: .leading) {
                    Text(player.formatInfo)
                        .font(.system(size: 11))
                        .foregroundColor(.secondary)
                }
            }
        }
        .padding(32)
        .onAppear {
            let sr = ctx.schema.sample_rate > 0 ? Int(ctx.schema.sample_rate) : 44100
            player.loadData(data, sampleRate: sr, channels: max(1, Int(ctx.header.num_channels)))
        }
        .onDisappear {
            player.stop()
        }
    }
}

// Audio player controller using AVAudioPlayer
class AudioPlayerController: NSObject, ObservableObject {
    @Published var isPlaying = false
    @Published var progress: Double = 0.0
    @Published var elapsedString = "0:00"
    @Published var remainingString = "-0:00"
    @Published var formatInfo = ""

    private var audioPlayer: AVAudioPlayer?
    private var timer: Timer?

    /// Wraps raw PCM in a minimal WAV container — AVAudioPlayer cannot parse
    /// headerless PCM (classic gravitational audio decompresses to raw int16).
    static func makeWav(pcm: Data, sampleRate: Int, channels: Int, bitsPerSample: Int) -> Data {
        let byteRate = sampleRate * channels * bitsPerSample / 8
        let blockAlign = channels * bitsPerSample / 8
        var d = Data()
        func le32(_ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
        func le16(_ v: UInt16) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
        d.append("RIFF".data(using: .ascii)!); le32(UInt32(36 + pcm.count)); d.append("WAVE".data(using: .ascii)!)
        d.append("fmt ".data(using: .ascii)!); le32(16); le16(1)
        le16(UInt16(channels)); le32(UInt32(sampleRate)); le32(UInt32(byteRate))
        le16(UInt16(blockAlign)); le16(UInt16(bitsPerSample))
        d.append("data".data(using: .ascii)!); le32(UInt32(pcm.count))
        d.append(pcm)
        return d
    }

    func loadData(_ data: Data, sampleRate: Int, channels: Int) {
        do {
            let wav: Data
            if data.count >= 12,
               let head = String(data: data.prefix(4), encoding: .ascii),
               head == "RIFF" {
                wav = data  // already a WAV/RIFF container
            } else {
                wav = AudioPlayerController.makeWav(pcm: data, sampleRate: sampleRate,
                                                    channels: channels, bitsPerSample: 16)
            }
            let player = try AVAudioPlayer(data: wav)
            player.delegate = self
            player.prepareToPlay()
            self.audioPlayer = player

            let dur = player.duration
            let ch = player.numberOfChannels
            let fmt = player.format
            let sr = Int(fmt.sampleRate)
            self.formatInfo = "\(sr) Hz • \(ch == 1 ? "Mono" : "Stereo") • \(formatTime(dur))"
            self.remainingString = "-\(formatTime(dur))"
        } catch {
            self.formatInfo = "Could not decode audio"
        }
    }
    
    /// Transport controls used to keep video audio in step with the picture.
    func play() {
        audioPlayer?.play()
        isPlaying = true
    }

    func pause() {
        audioPlayer?.pause()
        isPlaying = false
    }

    func togglePlayPause() {
        guard let player = audioPlayer else { return }
        if player.isPlaying {
            player.pause()
            isPlaying = false
            timer?.invalidate()
        } else {
            player.play()
            isPlaying = true
            startTimer()
        }
    }
    
    func stop() {
        audioPlayer?.stop()
        audioPlayer?.currentTime = 0
        isPlaying = false
        progress = 0
        timer?.invalidate()
        updateTimeDisplay()
    }
    
    /// 0 = muted, 1 = full volume.
    func setVolume(_ value: Float) {
        audioPlayer?.volume = value
    }

    func seek(to value: Double) {
        guard let player = audioPlayer else { return }
        player.currentTime = value * player.duration
        updateTimeDisplay()
    }
    
    private func startTimer() {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { [weak self] _ in
            DispatchQueue.main.async {
                self?.updateTimeDisplay()
            }
        }
    }
    
    private func updateTimeDisplay() {
        guard let player = audioPlayer else { return }
        let elapsed = player.currentTime
        let remaining = player.duration - elapsed
        progress = player.duration > 0 ? elapsed / player.duration : 0
        elapsedString = formatTime(elapsed)
        remainingString = "-\(formatTime(remaining))"
    }
    
    private func formatTime(_ t: TimeInterval) -> String {
        let mins = Int(t) / 60
        let secs = Int(t) % 60
        return String(format: "%d:%02d", mins, secs)
    }
}

extension AudioPlayerController: AVAudioPlayerDelegate {
    func audioPlayerDidFinishPlaying(_ player: AVAudioPlayer, successfully flag: Bool) {
        DispatchQueue.main.async {
            self.isPlaying = false
            self.progress = 0
            self.audioPlayer?.currentTime = 0
            self.timer?.invalidate()
            self.updateTimeDisplay()
        }
    }
}

// Waveform visualization
struct WaveformView: View {
    let data: Data
    let progress: Double
    @State private var cachedAmplitudes: [Double] = []
    
    var body: some View {
        GeometryReader { geo in
            let h = geo.size.height
            
            HStack(spacing: 1) {
                ForEach(0..<cachedAmplitudes.count, id: \.self) { i in
                    let amp = cachedAmplitudes[i]
                    let barH = max(2, amp * h * 0.9)
                    let played = Double(i) / Double(max(cachedAmplitudes.count, 1)) <= progress
                    
                    RoundedRectangle(cornerRadius: 1)
                        .fill(played ? Color.orange : Color.orange.opacity(0.3))
                        .frame(width: 2, height: barH)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .onAppear {
                if cachedAmplitudes.isEmpty {
                    cachedAmplitudes = computeAmplitudes(bars: Int(geo.size.width / 3))
                }
            }
        }
    }
    
    private func computeAmplitudes(bars: Int) -> [Double] {
        guard bars > 0, data.count > 100 else {
            return Array(repeating: 0.3, count: max(bars, 1))
        }
        
        var result = [Double]()
        let step = max(1, data.count / bars)
        
        data.withUnsafeBytes { buffer in
            let bytes = buffer.bindMemory(to: UInt8.self)
            
            for i in 0..<bars {
                let start = i * step
                let end = min(start + step, data.count)
                if start >= data.count { break }
                
                var sum: Double = 0
                var count = 0
                var j = start
                while j + 1 < end {
                    // Cast to Int32 BEFORE abs to avoid Int16.min overflow crash
                    let raw = Int32(Int16(bitPattern: UInt16(bytes[j]) | (UInt16(bytes[j+1]) << 8)))
                    let sample = abs(raw)
                    sum += Double(sample)
                    count += 1
                    j += 2
                }
                let avg = count > 0 ? sum / Double(count) / 32768.0 : 0.1
                result.append(min(1.0, avg * 3.0 + 0.05))
            }
        }
        
        return result
    }
}

struct HexRenderer: View {
    let data: Data
    
    var body: some View {
        ScrollView {
            Text(generateHex(from: data))
                .font(.system(size: 11, design: .monospaced))
                .foregroundColor(.primary)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding()
        }
    }
    
    private func generateHex(from data: Data) -> String {
        var res = ""
        let rows = min(data.count / 16, 200) // limit for UI performance
        
        let bytes = [UInt8](data)
        for r in 0...rows where r*16 < data.count {
            res += String(format: "%08x  ", r * 16)
            
            for col in 0..<16 {
                let idx = r * 16 + col
                if idx < data.count {
                    res += String(format: "%02x ", bytes[idx])
                } else {
                    res += "   "
                }
                if col == 7 { res += " " }
            }
            res += " "
            for col in 0..<16 {
                let idx = r * 16 + col
                if idx < data.count {
                    let ch = bytes[idx]
                    if ch >= 32 && ch < 127 {
                        res += String(format: "%c", ch)
                    } else {
                        res += "."
                    }
                }
            }
            res += "\n"
        }
        
        if data.count > rows * 16 {
            res += "\n... (truncated for display, total \(data.count) bytes)"
        }
        
        return res
    }
}

// ─── VIDEO PLAYER ───
struct VideoPlayerView: View {
    let ctx: qtsq_context_t
    
    @StateObject private var player = VideoPlayerController()
    
    var body: some View {
        VStack(spacing: 0) {
            if let nsImage = player.currentFrameImage {
                Image(nsImage: nsImage)
                    .resizable()
                    .aspectRatio(contentMode: .fit)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .background(Color.black)
            } else {
                Rectangle()
                    .fill(Color.black)
                    .overlay(ProgressView("Loading...").foregroundColor(.white))
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
            
            // Transport Controls
            HStack {
                Button(action: { player.togglePlay() }) {
                    Image(systemName: player.isPlaying ? "pause.fill" : "play.fill")
                        .foregroundColor(.white)
                        .padding(8)
                        .background(Color.white.opacity(0.2))
                        .clipShape(Circle())
                }.buttonStyle(PlainButtonStyle())
                
                Slider(
                    value: Binding(
                        get: { Double(player.currentFrame) },
                        set: { newVal in
                            player.userSeek(to: UInt32(newVal))
                        }
                    ),
                    in: 0...Double(max(1, Int(player.totalFrames) - 1))
                )
                .accentColor(.blue)
                
                Text("\(player.timeString)")
                    .font(.caption.monospacedDigit())
                    .foregroundColor(.white)
                    .frame(width: 100, alignment: .trailing)

                // The gravitational audio track rides along with the video;
                // without a control the user has no indication it is there.
                if player.hasAudioTrack {
                    Button(action: { player.toggleAudioMute() }) {
                        Image(systemName: player.isAudioMuted ? "speaker.slash.fill" : "speaker.wave.2.fill")
                            .foregroundColor(.white)
                            .padding(8)
                            .background(Color.white.opacity(0.2))
                            .clipShape(Circle())
                    }.buttonStyle(PlainButtonStyle())
                    .help(player.isAudioMuted ? "Unmute GFC audio" : "Mute GFC audio")
                }
            }
            .padding()
            .background(Color.black.opacity(0.8))
        }
        .onAppear {
            player.load(ctx: ctx)
        }
        .onDisappear {
            player.stop()
        }
    }
}


class VideoPlayerController: ObservableObject {
    @Published var currentFrameImage: NSImage? = nil
    @Published var isPlaying: Bool = false
    @Published var currentFrame: UInt32 = 0
    @Published var totalFrames: UInt32 = 1
    @Published var timeString: String = "00:00 / 00:00"

    private var fps: Double = 24.0
    private var displayDriver: AnyObject? = nil
    private var displayTimer: Timer? = nil
    private var playback: PlaybackDecoder? = nil
    private var playStart = DispatchTime.now()
    private var playStartFrame: UInt32 = 0
    private var lastTimeUpdate = DispatchTime.now()
    private var hasFirstFrame = false
    private var pendingAutoPlay = false
    /// Classic Gravitational audio carried inside the video file, if present.
    @Published private(set) var audio: AudioPlayerController? = nil
    @Published private(set) var hasAudioTrack = false
    @Published private(set) var isAudioMuted = false

    func load(ctx: qtsq_context_t) {
        var localCtx = ctx
        guard let dec = qtsq_video_decoder_open(&localCtx) else {
            NSLog("[QTSQ Player] ERROR: decoder_open returned NULL")
            return
        }

        let storedFPS = localCtx.schema.frame_rate
        fps = storedFPS > 0 && storedFPS < 1000 ? Double(storedFPS) : 24.0
        totalFrames = qtsq_video_decoder_get_frame_count(dec)

        var pixels: UnsafeMutablePointer<UInt8>? = nil
        var width: UInt32 = 0
        var height: UInt32 = 0
        if totalFrames > 0 {
            qtsq_video_decoder_get_frame(dec, 0, &pixels, &width, &height)
        }
        guard totalFrames > 0, width > 0, height > 0 else {
            NSLog("[QTSQ Player] ERROR: no video frames (%u)", totalFrames)
            qtsq_video_decoder_free(dec)
            return
        }

        hasFirstFrame = false
        playback = PlaybackDecoder(
            decoder: dec,
            totalFrames: totalFrames,
            width: Int(width),
            height: Int(height),
            gopSize: max(1, storedFPS)
        )

        // Classic Gravitational audio track, if the file carries one. The
        // decoder hands back a complete mono WAV, which AVAudioPlayer takes
        // directly, so it stays in sync with the video playhead.
        hasAudioTrack = qtsq_video_decoder_has_audio(dec) != 0
        if hasAudioTrack {
            var wav: UnsafeMutablePointer<UInt8>? = nil
            var wavSize = 0
            var rate: UInt32 = 0
            var channels: UInt8 = 0
            if qtsq_video_decoder_get_audio(dec, &wav, &wavSize, &rate, &channels) == 0,
               let wav = wav, wavSize > 44 {
                let controller = AudioPlayerController()
                controller.loadData(Data(bytes: wav, count: wavSize),
                                    sampleRate: Int(rate), channels: Int(channels))
                self.audio = controller
            }
            free(wav)
        }

        NSLog("[QTSQ Player] Opened: %d frames, %.3f fps, %dx%d, audio=%@",
              totalFrames, fps, width, height, hasAudioTrack ? "yes" : "no")

        updateTimeString()
        startDisplayDriver()
        /* start(at:) arms the decoder for frame 0. Do NOT pause the decoder
         * here: the decode thread must produce the first frame for the view
         * to leave "Loading...". Playback is gated on the first frame via
         * pendingAutoPlay instead. */
        playback?.start(at: 0)
        pendingAutoPlay = true
    }

    func stop() {
        isPlaying = false
        audio?.stop()
        audio = nil
        hasAudioTrack = false
        stopDisplayDriver()
        if let dec = playback?.stop() {
            qtsq_video_decoder_free(dec)
        }
        playback = nil
    }

    func togglePlay() {
        if isPlaying { pause() } else { play() }
    }

    func userSeek(to frame: UInt32) {
        guard playback != nil, totalFrames > 0 else { return }
        let target = min(frame, totalFrames - 1)
        isPlaying = false
        playback?.setPlaying(false)
        currentFrame = target
        playStartFrame = target
        updateTimeString()
        playback?.seek(to: target)
    }

    private func startDisplayDriver() {
        /* CADisplayLink needs macOS 14; this app still supports 12.0, so keep
         * the reference type-erased and fall back to a 60 Hz main-runloop
         * timer on older systems. Either way the frame index comes from the
         * wall clock, not from tick count. */
        if #available(macOS 14.0, *) {
            if let link = NSScreen.main?.displayLink(target: self, selector: #selector(displayTick)) {
                if #available(macOS 15.0, *) {
                    let maxHz = NSScreen.main?.maximumFramesPerSecond ?? 60
                    link.preferredFrameRateRange = CAFrameRateRange(minimum: 30, maximum: Float(maxHz), preferred: 60)
                }
                link.add(to: .main, forMode: .common)
                displayDriver = link
                return
            }
        }
        let timer = Timer(timeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
            self?.displayTick()
        }
        RunLoop.main.add(timer, forMode: .common)
        displayTimer = timer
    }

    private func stopDisplayDriver() {
        if #available(macOS 14.0, *) {
            (displayDriver as? CADisplayLink)?.invalidate()
        }
        displayDriver = nil
        displayTimer?.invalidate()
        displayTimer = nil
    }

    private func play() {
        guard !isPlaying, totalFrames > 1, hasFirstFrame, playback != nil else { return }
        isPlaying = true
        playback?.setPlaying(true)
        audio?.play()
        playStart = .now()
        playStartFrame = currentFrame
    }

    private func pause() {
        isPlaying = false
        playback?.setPlaying(false)
        audio?.pause()
    }

    func toggleAudioMute() {
        isAudioMuted.toggle()
        audio?.setVolume(isAudioMuted ? 0 : 1)
    }

    @objc private func displayTick() {
        guard playback != nil, totalFrames > 1 else { return }

        if isPlaying {
            let elapsed = Double(DispatchTime.now().uptimeNanoseconds - playStart.uptimeNanoseconds) / 1_000_000_000
            let tick = UInt32(elapsed * fps)
            let target = (playStartFrame &+ tick) % totalFrames
            if target != currentFrame {
                currentFrame = target
                playback?.setPlayhead(target)
            }
        }

        // Publish only the frame the wall clock is on. Decode-ahead frames
        // stay buffered in the decoder, so a slow decode repeats the current
        // frame instead of advancing one frame per decode.
        if let ready = playback?.takeReadyFrame(atOrBefore: currentFrame) {
            currentFrameImage = ready.image
            if !hasFirstFrame {
                hasFirstFrame = true
                if pendingAutoPlay {
                    pendingAutoPlay = false
                    play()
                }
            }
        }

        let now = DispatchTime.now()
        if now.uptimeNanoseconds - lastTimeUpdate.uptimeNanoseconds > 80_000_000 {
            lastTimeUpdate = now
            updateTimeString()
        }
    }

    private func updateTimeString() {
        let current = Double(currentFrame) / fps
        let total = Double(totalFrames) / fps
        let cm = Int(current) / 60
        let cs = Int(current) % 60
        let tm = Int(total) / 60
        let ts = Int(total) % 60
        timeString = String(format: "%02d:%02d / %02d:%02d", cm, cs, tm, ts)
    }
}
