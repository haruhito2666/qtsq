import SwiftUI
import AVFoundation

struct SpectralAudioPlayerView: View {
    let ctx: qtsq_context_t
    @StateObject private var engine = SpectralAudioEngine()
    
    var body: some View {
        let isPhoton = ctx.header.strategy == 0x08
        let isLossyFloat = ctx.header.strategy == 0x01 && ctx.header.sample_depth == 32
        let isLossy = (ctx.header.flags & 0x0002) != 0
        let badgeText = isPhoton ? "PHOTON AUDIO HQ"
                      : isLossyFloat ? "MDCT2 · LOSSY"
                      : "SPECTRAL DICTIONARY"
        let badgeIcon = isPhoton ? "waveform" : "waveform.badge.magnifyingglass"
        
        VStack(spacing: 0) {
            // ── Badge ──
            HStack {
                HStack(spacing: 6) {
                    Image(systemName: badgeIcon)
                        .font(.system(size: 11, weight: .semibold))
                    Text(badgeText)
                        .font(.system(size: 10, weight: .heavy))
                        .tracking(1.0)
                }
                .foregroundColor(VaultTheme.emberOrange)
                .padding(.horizontal, 10)
                .padding(.vertical, 4)
                .background(VaultTheme.emberOrangeBg)
                .cornerRadius(6)
                .overlay(
                    RoundedRectangle(cornerRadius: 6)
                        .stroke(VaultTheme.emberOrange.opacity(0.3), lineWidth: 1)
                )
                
                // Stream integrity chip. MDCT2 stereo audio is LOSSY
                // (QTSQ_FLAG_LOSSY) -- claiming "Lossless Stream" for it
                // would be false.
                HStack(spacing: 4) {
                    Circle()
                        .fill(isLossy ? VaultTheme.emberOrange : VaultTheme.integrityGreen)
                        .frame(width: 5, height: 5)
                    Text(isLossy ? "Lossy Stream · MDCT2 Stereo" : "Lossless Stream")
                        .font(.system(size: 10, weight: .medium))
                        .foregroundColor(VaultTheme.textSecondary)
                }
                .padding(.leading, 8)
                
                Spacer()
                
                if engine.isDecoding {
                    HStack(spacing: 4) {
                        ProgressView()
                            .controlSize(.small)
                        Text("Decoding stream…")
                            .font(.system(size: 11))
                            .foregroundColor(VaultTheme.textSecondary)
                    }
                }
            }
            .padding(.horizontal, 24)
            .padding(.top, 16)
            
            Spacer().frame(height: 20)
            
            // ── Waveform ──
            SpectralWaveformView(
                samples: engine.waveformSamples,
                progress: engine.progress
            )
            .frame(height: 100)
            .padding(.horizontal, 24)
            
            Spacer().frame(height: 16)
            
            // ── Lossless Indicator (Photon only) ──
            if isPhoton && engine.isReady {
                HStack(spacing: 6) {
                    Circle()
                        .fill(engine.isLosslessPath ? Color.green : Color.yellow)
                        .frame(width: 6, height: 6)
                    Text(engine.isLosslessPath ? "32-bit Float Lossless" : "Resampled")
                        .font(.system(size: 10, weight: .semibold))
                        .foregroundColor(engine.isLosslessPath ? .green : .yellow)
                    if !engine.losslessDetail.isEmpty {
                        Text("• \(engine.losslessDetail)")
                            .font(.system(size: 9))
                            .foregroundColor(.secondary)
                    }
                }
                .padding(.horizontal, 28)
                
                if engine.spatialAtomCount > 0 {
                    HStack(spacing: 6) {
                        Image(systemName: "view.3d")
                            .font(.system(size: 10))
                        Text("True Spatial Audio • \(engine.spatialAtomCount) Atoms mapped")
                            .font(.system(size: 10, weight: .bold))
                            .foregroundColor(.indigo)
                    }
                    .padding(.horizontal, 28)
                    .padding(.top, 2)
                }
            }
            
            Spacer().frame(height: 4)
            
            // ── Time ──
            HStack {
                Text(engine.elapsedString)
                    .font(.system(size: 12, weight: .medium, design: .monospaced))
                    .foregroundColor(isPhoton ? .purple : .orange)
                Spacer()
                Text(engine.durationString)
                    .font(.system(size: 12, weight: .medium, design: .monospaced))
                    .foregroundColor(.secondary)
            }
            .padding(.horizontal, 28)
            
            // ── Seek ──
            Slider(
                value: Binding(
                    get: { engine.progress },
                    set: { engine.seek(to: $0) }
                ),
                in: 0...1
            )
            .accentColor(isPhoton ? .purple : .orange)
            .padding(.horizontal, 24)
            .padding(.vertical, 4)
            
            Spacer().frame(height: 12)
            
            // ── Controls ──
            HStack(spacing: 32) {
                // Stop
                Button(action: { engine.stop() }) {
                    Image(systemName: "stop.fill")
                        .font(.system(size: 16))
                        .foregroundColor(.secondary)
                        .frame(width: 40, height: 40)
                        .background(Color.primary.opacity(0.06))
                        .cornerRadius(12)
                }
                .buttonStyle(PlainButtonStyle())
                
                // Play/Pause
                Button(action: { engine.togglePlayPause() }) {
                    ZStack {
                        Circle()
                            .fill(
                                LinearGradient(colors: isPhoton
                                    ? [Color.purple, Color.indigo.opacity(0.7)]
                                    : [Color.orange, Color.red.opacity(0.7)],
                                               startPoint: .topLeading, endPoint: .bottomTrailing)
                            )
                            .frame(width: 56, height: 56)
                            .shadow(color: (isPhoton ? Color.purple : Color.orange).opacity(0.4), radius: 10, y: 4)
                        
                        Image(systemName: engine.isPlaying ? "pause.fill" : "play.fill")
                            .font(.system(size: 22, weight: .bold))
                            .foregroundColor(.white)
                            .offset(x: engine.isPlaying ? 0 : 2)
                    }
                }
                .buttonStyle(PlainButtonStyle())
                .disabled(!engine.isReady)
                
                // Info
                VStack(alignment: .leading, spacing: 2) {
                    Text(engine.formatInfo)
                        .font(.system(size: 11, weight: .medium))
                        .foregroundColor(.secondary)
                    Text(engine.compressionInfo)
                        .font(.system(size: 10))
                        .foregroundColor(.secondary.opacity(0.7))
                }
            }
            
            Spacer().frame(height: 20)
        }
        .onAppear {
            engine.decode(ctx: ctx)
        }
        .onDisappear {
            engine.stop()
        }
    }
}

// ═══════════════════════════════════════════════════════════
//  SpectralAudioEngine — 3-Second Jitter Buffer Playback
//
//  Uses AVAudioPlayerNode with pre-rendered 3-second chunks
//  queued on a background thread. Eliminates all real-time
//  synthesis from the audio thread, preventing crackling.
// ═══════════════════════════════════════════════════════════

class SpectralAudioEngine: ObservableObject {
    @Published var isPlaying = false
    @Published var isDecoding = false
    @Published var isReady = false
    @Published var progress: Double = 0
    @Published var elapsedString = "0:00"
    @Published var durationString = "0:00"
    
    @Published var formatInfo = ""
    @Published var compressionInfo = ""
    @Published var waveformSamples: [Float] = []
    @Published var isLosslessPath = false
    @Published var losslessDetail = ""
    @Published var spatialAtomCount: Int = 0
    
    // Core Audio Components
    private var audioEngine: AVAudioEngine?
    private var audioFormat: AVAudioFormat?
    private let playerNode = AVAudioPlayerNode()
    
    // Pre-rendered full song buffer (decoded once, played instantly)
    private var fullBuffer: AVAudioPCMBuffer?
    
    // Raw decoded pointers (kept alive for waveform building, freed after pre-render)
    private var rawPcmPtr: UnsafeMutablePointer<Float>? = nil
    private var rawAtomsPtr: UnsafeMutablePointer<photon_atom_t>? = nil
    
    private var totalFrames: Int = 0
    private var rawPcmChannels: Int = 2
    private var sampleRate: Double = 44100
    private var timer: Timer?
    private var duration: TimeInterval = 0
    
    // Time tracking — uses playerNode.lastRenderTime for accuracy
    private var seekOffsetFrames: AVAudioFramePosition = 0
    private var playStartHostTime: UInt64 = 0
    
    // MARK: - Decode
    
    func decode(ctx: qtsq_context_t) {
        guard !isDecoding else { return }
        isDecoding = true
        
        var localCtx = ctx
        let isPhoton = localCtx.header.strategy == 0x08
        let isLossyFloat = localCtx.header.strategy == 0x01 && localCtx.header.sample_depth == 32
        let qtsqSize = localCtx.header.original_size
        
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            
            var rawData: UnsafeMutablePointer<UInt8>? = nil
            var rawSize: Int = 0
            
            let channels = localCtx.header.num_channels
            let sr = localCtx.schema.sample_rate
            let sampleRate = sr > 0 ? sr : 44100
            let ch = max(1, UInt16(channels))
            
            let res: Int32
            var atomCountOut: Int = 0
            
            if isPhoton {
                var rawPCM: UnsafeMutablePointer<Float>? = nil
                var rawAtoms: UnsafeMutablePointer<photon_atom_t>? = nil
                var totalSamples: Int = 0
                
                res = qtsq_decompress_audio_hq(&localCtx, &rawPCM, &totalSamples, &rawAtoms, &atomCountOut)
                
                if res == QTSQ_OK && rawPCM != nil {
                    let totalFr = totalSamples / Int(ch)
                    let isSpatial = atomCountOut > 0
                    let outCh = isSpatial ? 6 : Int(ch)
                    
                    // ── Pre-render the entire song on this background thread ──
                    let layoutTag: AudioChannelLayoutTag = isSpatial
                        ? kAudioChannelLayoutTag_AudioUnit_5_1
                        : (ch == 2 ? kAudioChannelLayoutTag_Stereo : kAudioChannelLayoutTag_Mono)
                    guard let layout = AVAudioChannelLayout(layoutTag: layoutTag) else { return }
                    let format = AVAudioFormat(
                        commonFormat: .pcmFormatFloat32,
                        sampleRate: Double(sampleRate),
                        interleaved: false,
                        channelLayout: layout
                    )
                    
                    guard let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(totalFr)) else { return }
                    buffer.frameLength = AVAudioFrameCount(totalFr)
                    guard let channelData = buffer.floatChannelData else { return }
                    
                    // Zero all channels
                    for c in 0..<outCh {
                        memset(channelData[c], 0, totalFr * MemoryLayout<Float>.size)
                    }
                    
                    // Layer 1: Copy lossless PCM into channels 0 and 1
                    if let pcm = rawPCM {
                        for f in 0..<totalFr {
                            let idx = f * Int(ch)
                            channelData[0][f] = pcm[idx]
                            if ch > 1 { channelData[1][f] = pcm[idx + 1] }
                        }
                    }
                    
                    // Layer 2: Bake spatial atoms into all 6 channels
                    if isSpatial, let atoms = rawAtoms, atomCountOut > 0 {
                        let hopSize = 512
                        let windowLen = 1024
                        let halfWin = windowLen / 2
                        let twoPi = Float.pi * 2.0
                        
                        for i in 0..<atomCountOut {
                            let atom = atoms[i]
                            let centerFrame = Int(atom.frame_index) * hopSize + halfWin
                            
                            let winStart = centerFrame - halfWin
                            let winEnd = centerFrame + halfWin
                            
                            // Clip to buffer bounds
                            let fStart = max(0, winStart)
                            let fEnd = min(totalFr, winEnd)
                            if fStart >= fEnd { continue }
                            
                            let centerVol = Float((1.0 - abs(atom.x)) * max(0, 1.0 - atom.spread) * atom.z * 0.4)
                            let lSurroundVol = Float(max(0, -atom.x) * (1.0 - atom.z + atom.spread) * 0.7)
                            let rSurroundVol = Float(max(0, atom.x) * (1.0 - atom.z + atom.spread) * 0.7)
                            let subHarmonicVol: Float = atom.freq_hz < 100.0 ? 1.0 : 0.0
                            
                            for f in fStart..<fEnd {
                                let w = f - winStart
                                let hann = 0.5 * (1.0 - cos(twoPi * Float(w) / Float(windowLen - 1)))
                                let time = Float(f) / Float(sampleRate)
                                let wave = atom.magnitude * hann * sin(twoPi * atom.freq_hz * time + atom.phase)
                                
                                channelData[0][f] -= wave * (lSurroundVol + centerVol)
                                channelData[1][f] -= wave * (rSurroundVol + centerVol)
                                channelData[2][f] += wave * centerVol
                                channelData[3][f] += wave * subHarmonicVol
                                channelData[4][f] += wave * lSurroundVol
                                channelData[5][f] += wave * rSurroundVol
                            }
                        }
                    }
                    
                    DispatchQueue.main.async {
                        self.rawPcmPtr = rawPCM
                        self.rawAtomsPtr = rawAtoms
                        self.spatialAtomCount = atomCountOut
                        self.totalFrames = totalFr
                        self.rawPcmChannels = Int(ch)
                        self.sampleRate = Double(sampleRate)
                        self.fullBuffer = buffer
                        self.audioFormat = format
                        
                        self.setupEngine(format: format, isSpatial: isSpatial, ch: Int(ch),
                                         sampleRate: sampleRate, qtsqSize: qtsqSize, codecName: "Photon HQ")
                    }
                }
            } else if isLossyFloat {
                // MDCT2 perceptual coder (qtc AUDIO_FLOAT lens). Interleaved
                // float32 out, already at the engine's native format.
                var floatPCM: UnsafeMutablePointer<Float>? = nil
                var totalSamples: Int = 0
                var srOut: UInt32 = 0
                var chOut: UInt8 = 0
                res = qtsq_decompress_audio_float(&localCtx, &floatPCM, &totalSamples,
                                                  &srOut, &chOut)
                if res == QTSQ_OK && floatPCM != nil && totalSamples > 0 {
                    let outChannels = max(1, Int(chOut))
                    let totalFr = totalSamples / outChannels
                    let outRate = srOut > 0 ? srOut : sampleRate
                    let layoutTag: AudioChannelLayoutTag = outChannels == 2
                        ? kAudioChannelLayoutTag_Stereo : kAudioChannelLayoutTag_Mono
                    guard let layout = AVAudioChannelLayout(layoutTag: layoutTag) else { return }
                    let format = AVAudioFormat(
                        commonFormat: .pcmFormatFloat32,
                        sampleRate: Double(outRate),
                        interleaved: false,
                        channelLayout: layout
                    )
                    guard let buffer = AVAudioPCMBuffer(pcmFormat: format,
                                                        frameCapacity: AVAudioFrameCount(totalFr)) else { return }
                    buffer.frameLength = AVAudioFrameCount(totalFr)
                    guard let channelData = buffer.floatChannelData else { return }

                    for f in 0..<totalFr {
                        let idx = f * outChannels
                        channelData[0][f] = floatPCM![idx]
                        if outChannels > 1 { channelData[1][f] = floatPCM![idx + 1] }
                    }
                    free(floatPCM)

                    DispatchQueue.main.async {
                        self.totalFrames = totalFr
                        self.rawPcmChannels = outChannels
                        self.sampleRate = Double(outRate)
                        self.fullBuffer = buffer
                        self.audioFormat = format
                        self.setupEngine(format: format, isSpatial: false, ch: outChannels,
                                         sampleRate: outRate, qtsqSize: qtsqSize,
                                         codecName: "MDCT2 Lossy", lossless: false)
                    }
                }
            } else {
                res = qtsq_decompress_lensing(&localCtx, &rawData, &rawSize)
                
                if res == QTSQ_OK && rawData != nil {
                    let wavData = Data(bytes: rawData!, count: rawSize)
                    free(rawData)
                    DispatchQueue.main.async {
                        self.setupFallbackEngine(wavData: wavData, sampleRate: sampleRate, qtsqSize: qtsqSize)
                    }
                }
            }
            
            DispatchQueue.main.async {
                self.isDecoding = false
            }
        }
    }
    
    // MARK: - Engine Setup (lightweight — buffer is already pre-rendered)
    
    private func setupEngine(format: AVAudioFormat, isSpatial: Bool, ch: Int,
                             sampleRate: UInt32, qtsqSize: UInt64, codecName: String,
                             lossless: Bool = true) {
        self.duration = Double(totalFrames) / Double(sampleRate)
        
        buildWaveform()
        
        let engine = AVAudioEngine()
        engine.attach(playerNode)
        engine.connect(playerNode, to: engine.mainMixerNode, format: format)
        
        self.isLosslessPath = !isSpatial && lossless
        self.losslessDetail = isSpatial ? "5.1 Spatial Renderer"
                           : lossless ? "32-bit Float Lossless"
                           : "\(codecName) • Perceptual"
        
        do { try engine.start() } catch {
            self.formatInfo = "Audio engine error"
            return
        }
        
        self.audioEngine = engine
        self.isReady = true
        
        self.formatInfo = "\(sampleRate) Hz • \(isSpatial ? "5.1 Surround" : (ch == 1 ? "Mono" : "Stereo")) • \(formatTime(duration))"
        let ratio = qtsqSize > 0 ? Double(self.totalFrames * ch * 4) / Double(qtsqSize) : 0
        self.compressionInfo = String(format: "\(codecName) • Pre-rendered • %.1f:1", ratio)
        self.durationString = formatTime(duration)
    }
    
    // MARK: - Schedule playback from a specific frame
    
    private func schedulePlayback(fromFrame: AVAudioFramePosition) {
        guard let fullBuf = fullBuffer, let format = audioFormat else { return }
        let totalFr = AVAudioFramePosition(fullBuf.frameLength)
        let startFrame = max(0, min(fromFrame, totalFr))
        let framesToPlay = Int(totalFr - startFrame)
        
        if framesToPlay <= 0 {
            playbackFinished()
            return
        }
        
        playerNode.stop()
        
        // Create a sub-buffer starting from the seek position
        guard let subBuffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(framesToPlay)) else { return }
        subBuffer.frameLength = AVAudioFrameCount(framesToPlay)
        
        if let srcData = fullBuf.floatChannelData, let dstData = subBuffer.floatChannelData {
            let channelCount = Int(format.channelCount)
            for c in 0..<channelCount {
                memcpy(dstData[c], srcData[c].advanced(by: Int(startFrame)),
                       framesToPlay * MemoryLayout<Float>.size)
            }
        }
        
        playerNode.scheduleBuffer(subBuffer, at: nil, options: []) { [weak self] in
            DispatchQueue.main.async {
                self?.playbackFinished()
            }
        }
        
        seekOffsetFrames = startFrame
        playerNode.play()
        startTimer()
    }
    
    // MARK: - Transport Controls
    
    func togglePlayPause() {
        guard isReady else { return }
        
        if isPlaying {
            // Capture current playback position before pausing
            let currentFrame = currentPlaybackFrame()
            playerNode.pause()
            isPlaying = false
            timer?.invalidate()
            seekOffsetFrames = currentFrame
        } else {
            isPlaying = true
            
            // If we were paused mid-song, resume from the paused position
            let resumeFrame = seekOffsetFrames
            
            if resumeFrame >= AVAudioFramePosition(totalFrames) || resumeFrame <= 0 {
                // Start from beginning
                schedulePlayback(fromFrame: 0)
            } else {
                // Resume from where we left off
                schedulePlayback(fromFrame: resumeFrame)
            }
        }
    }
    
    func stop() {
        playerNode.stop()
        isPlaying = false
        progress = 0
        seekOffsetFrames = 0
        timer?.invalidate()
        elapsedString = "0:00"
        durationString = formatTime(duration)
    }
    
    func seek(to value: Double) {
        let frame = AVAudioFramePosition(value * Double(totalFrames))
        let clampedFrame = max(0, min(frame, AVAudioFramePosition(totalFrames)))
        
        seekOffsetFrames = clampedFrame
        
        if isPlaying {
            schedulePlayback(fromFrame: clampedFrame)
        } else {
            // Update display even when paused
            let elapsed = Double(clampedFrame) / sampleRate
            progress = value
            elapsedString = formatTime(elapsed)
            durationString = "-\(formatTime(max(0, duration - elapsed)))"
        }
    }
    
    /// Returns the current playback frame position by querying the player node
    private func currentPlaybackFrame() -> AVAudioFramePosition {
        guard let nodeTime = playerNode.lastRenderTime,
              let playerTime = playerNode.playerTime(forNodeTime: nodeTime) else {
            return seekOffsetFrames
        }
        return seekOffsetFrames + playerTime.sampleTime
    }
    
    private func playbackFinished() {
        isPlaying = false
        progress = 1.0
        seekOffsetFrames = 0
        timer?.invalidate()
        elapsedString = formatTime(duration)
        durationString = "-0:00"
    }
    
    // MARK: - Timer & Time Display
    
    private func startTimer() {
        timer?.invalidate()
        let newTimer = Timer.scheduledTimer(withTimeInterval: 1.0/30.0, repeats: true) { [weak self] _ in
            DispatchQueue.main.async { self?.updateTimeDisplay() }
        }
        RunLoop.main.add(newTimer, forMode: .common)
        self.timer = newTimer
    }
    
    private func updateTimeDisplay() {
        guard totalFrames > 0, isPlaying else { return }
        
        let frame = currentPlaybackFrame()
        let elapsed = min(Double(frame) / sampleRate, duration)
        let remaining = max(0, duration - elapsed)
        
        progress = elapsed / duration
        elapsedString = formatTime(elapsed)
        durationString = "-\(formatTime(remaining))"
    }
    
    // MARK: - Waveform
    
    private func buildWaveform() {
        let barCount = 200
        let step = max(1, totalFrames / barCount)
        var samples = [Float]()
        
        guard let pcm = rawPcmPtr else { return }
        let ch = max(1, rawPcmChannels)
        
        for i in 0..<barCount {
            let start = i * step
            let end = min(start + step, totalFrames)
            if start >= totalFrames { break }
            
            var sum: Float = 0
            for j in start..<end {
                var maxCh: Float = 0
                for c in 0..<ch {
                    maxCh = max(maxCh, abs(pcm[j * ch + c]))
                }
                sum += maxCh
            }
            let avg = sum / Float(max(1, end - start))
            samples.append(min(1.0, avg * 2.0 + 0.03))
        }
        self.waveformSamples = samples
    }
    
    // MARK: - Fallback (non-Photon)
    
    private func setupFallbackEngine(wavData: Data, sampleRate: UInt32, qtsqSize: UInt64) {}
    
    // MARK: - Helpers
    
    private func formatTime(_ t: TimeInterval) -> String {
        let mins = Int(t) / 60
        let secs = Int(t) % 60
        return String(format: "%d:%02d", mins, secs)
    }
    
    deinit {
        timer?.invalidate()
        audioEngine?.stop()
        if let ptr = rawPcmPtr { free(ptr) }
        if let aPtr = rawAtomsPtr { free(aPtr) }
    }
}

// ═══════════════════════════════════════════════════════════
//  SpectralWaveformView
// ═══════════════════════════════════════════════════════════

struct SpectralWaveformView: View {
    let samples: [Float]
    let progress: Double
    
    var body: some View {
        GeometryReader { geo in
            let h = geo.size.height
            let barWidth: CGFloat = 2.5
            let spacing: CGFloat = 1
            let totalBar = barWidth + spacing
            let barCount = min(samples.count, Int(geo.size.width / totalBar))
            
            HStack(spacing: spacing) {
                ForEach(0..<max(1, barCount), id: \.self) { i in
                    let amp = i < samples.count ? CGFloat(samples[i]) : 0.1
                    let barH = max(2, amp * h * 0.9)
                    let played = Double(i) / Double(max(barCount, 1)) <= progress
                    
                    RoundedRectangle(cornerRadius: 1.5)
                        .fill(
                            played
                            ? LinearGradient(colors: [.orange, .red.opacity(0.7)],
                                             startPoint: .top, endPoint: .bottom)
                            : LinearGradient(colors: [.orange.opacity(0.25), .orange.opacity(0.15)],
                                             startPoint: .top, endPoint: .bottom)
                        )
                        .frame(width: barWidth, height: barH)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }
}
