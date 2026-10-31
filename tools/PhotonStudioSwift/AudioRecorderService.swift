import AVFoundation
import CoreAudio
import AudioToolbox

// MARK: - Recorded Take

/// One finished recording (in-memory interleaved Float32 PCM).
struct RecordedTake {
    let pcm: Data
    let sampleRate: Double
    let channels: Int
    let duration: TimeInterval

    /// Codec round-trip preview: PCM decoded back from the encoded
    /// qtsq context (int16 interleaved). Populated by Studio Mode after
    /// encoding so Play previews the compressed output, not the raw take.
    var encodedPCM: Data?
    var encodedSampleRate: Double = 0
    var encodedChannels: Int = 0
}

// MARK: - Audio Recorder Service

/// Live microphone capture with pause/resume, an optional metronome
/// click track, input-device selection, and optional monitoring.
///
/// Uses its own AVAudioEngine. macOS allows a single owner of the input
/// tap at a time, so Studio Mode pauses Photon sensing while recording
/// and restores it afterwards.
final class AudioRecorderService: NSObject, ObservableObject {

    @Published var isRecording = false
    @Published var isPaused = false
    @Published var duration: TimeInterval = 0        // accumulated, pauses excluded
    @Published var inputLevel: Float = 0             // 0...1 RMS, for UI meters

    private var engine: AVAudioEngine?
    private var recordFormat: AVAudioFormat?

    private var pcm = [Float]()                      // interleaved float32
    private let pcmLock = NSLock()
    private var framesRecorded = 0
    private(set) var channels = 1
    private(set) var sampleRate: Double = 48000

    /// Accumulated duration frozen while paused (so the timer stays
    /// honest across pause/resume cycles).
    private var durationAtPause: TimeInterval = 0

    // Input device selection (nil = system default)
    private var selectedDeviceUID: String? = nil

    // Software monitoring (input → output). Off by default: speakers
    // will feed back instantly; meant for headphones.
    private var monitoringEnabled = false

    // Metronome configuration (read at start; change between takes)
    var metronomeEnabled = false
    var metronomeBPM: Double = 120

    private var clickPlayer: AVAudioPlayerNode?
    private var clickFormat: AVAudioFormat?
    private var clickTimer: DispatchSourceTimer?
    private var nextClickSample: AVAudioFramePosition = 0
    private var clickBeatCounter = 0

    // MARK: Lifecycle

    /// Configures which input to record from. Call before start().
    /// nil = system default input.
    func selectDevice(uid: String?) {
        selectedDeviceUID = uid
    }

    /// Enables/disables software monitoring for the NEXT start. Off by
    /// default and meant for headphones — speakers feed back.
    func setMonitoring(_ enabled: Bool) {
        monitoringEnabled = enabled
        if !enabled, let engine = engine, engine.isRunning {
            engine.disconnectNodeOutput(engine.inputNode)
        }
    }

    /// Starts capturing the selected input. Returns false when the
    /// engine cannot start (no mic / permission denied / device unplugged).
    func start() -> Bool {
        guard !isRecording else { return true }

        let engine = AVAudioEngine()
        let input = engine.inputNode

        // Select the chosen device on the input unit BEFORE asking for
        // its format — the hardware format follows the selected device.
        if let uid = selectedDeviceUID,
           !Self.selectDevice(uid, on: input.audioUnit) {
            // Device vanished between pick and start — fall back to the
            // system default rather than failing the take.
            selectedDeviceUID = nil
            DispatchQueue.main.async {
                NotificationCenter.default.post(
                    name: .studioRecorderFellBackToDefault, object: nil)
            }
        }

        let format = input.inputFormat(forBus: 0)
        guard format.sampleRate > 0, format.channelCount > 0 else { return false }

        recordFormat = format
        channels = Int(format.channelCount)
        sampleRate = format.sampleRate

        pcmLock.lock()
        pcm.removeAll(keepingCapacity: false)
        pcmLock.unlock()
        framesRecorded = 0
        durationAtPause = 0
        duration = 0
        isPaused = false
        inputLevel = 0

        input.installTap(onBus: 0, bufferSize: 4096, format: format) { [weak self] buffer, _ in
            self?.handleBuffer(buffer)
        }

        do {
            try engine.start()
        } catch {
            input.removeTap(onBus: 0)
            return false
        }
        self.engine = engine

        if monitoringEnabled {
            startMonitoring(engine: engine, format: format)
        }
        if metronomeEnabled {
            startMetronome(engine: engine)
        }

        isRecording = true
        return true
    }

    /// Pauses capture: the mic tap stays attached (so the engine and
    /// monitoring keep running) but buffers are discarded until resume.
    func pause() {
        guard isRecording, !isPaused else { return }
        isPaused = true
        durationAtPause = duration
        stopClick()   // silence the click during pause
        inputLevel = 0
    }

    /// Resumes capture. The click track restarts from the next beat —
    // an honest simplification (phase is not preserved across pauses).
    func resume() {
        guard isRecording, isPaused else { return }
        isPaused = false
        if metronomeEnabled, let engine = engine {
            startMetronome(engine: engine)
        }
    }

    /// Stops capture and returns the take. Returns nil for empty takes.
    func stop() -> RecordedTake? {
        guard isRecording, let engine = engine else { return nil }
        isRecording = false
        isPaused = false

        stopMetronome()
        stopMonitoring()
        engine.inputNode.removeTap(onBus: 0)
        engine.stop()

        pcmLock.lock()
        let data = pcm.withUnsafeBytes { Data($0) }
        pcm.removeAll(keepingCapacity: false)
        let frames = framesRecorded
        pcmLock.unlock()

        let sr = sampleRate
        let ch = channels
        duration = 0
        durationAtPause = 0
        inputLevel = 0

        engine.reset()
        self.engine = nil

        guard frames > 0, !data.isEmpty else { return nil }
        return RecordedTake(pcm: data, sampleRate: sr, channels: ch,
                            duration: Double(frames) / sr)
    }

    // MARK: Capture

    private func handleBuffer(_ buffer: AVAudioPCMBuffer) {
        // Paused: keep the engine warm but discard input.
        guard !isPaused else { return }
        guard let src = buffer.floatChannelData else { return }
        let frames = Int(buffer.frameLength)
        guard frames > 0 else { return }
        let ch = Int(buffer.format.channelCount)

        var level: Float = 0
        pcmLock.lock()
        pcm.reserveCapacity(pcm.count + frames * ch)
        for f in 0..<frames {
            for c in 0..<ch {
                let v = src[c][f]
                pcm.append(v)
                level += v * v
            }
        }
        framesRecorded += frames
        let totalFrames = framesRecorded
        pcmLock.unlock()

        let rms = sqrt(level / Float(frames * ch))
        DispatchQueue.main.async {
            self.inputLevel = min(1.0, rms * 4.0)
            self.duration = self.durationAtPause
                + Double(totalFrames) / self.sampleRate
        }
    }

    // MARK: Monitoring

    private func startMonitoring(engine: AVAudioEngine, format: AVAudioFormat) {
        // Software monitor: direct input → mainMixer → output connection.
        // The input tap (capture) is unaffected; this only adds an audible
        // path. Meant for headphones — speakers will feed back.
        engine.connect(engine.inputNode, to: engine.mainMixerNode, format: format)
    }

    private func stopMonitoring() {
        guard let engine = engine else { return }
        engine.disconnectNodeOutput(engine.inputNode)
    }

    // MARK: Device selection (CoreAudio)

    /// Sets the input device on an input unit. Returns false when the
    /// unit or device cannot be configured (unplugged UID, etc.).
    static func selectDevice(_ uid: String, on unit: AudioUnit?) -> Bool {
        guard let unit = unit else { return false }
        var deviceUID = uid as CFString
        let status = withUnsafeMutableBytes(of: &deviceUID) { raw in
            AudioUnitSetProperty(unit,
                                 kAudioOutputUnitProperty_CurrentDevice,
                                 kAudioUnitScope_Global,
                                 0,
                                 raw.baseAddress,
                                 UInt32(MemoryLayout<CFString>.size))
        }
        return status == noErr
    }

    // MARK: Metronome

    private func makeClick(accent: Bool) -> AVAudioPCMBuffer? {
        guard let format = clickFormat else { return nil }
        let sr = format.sampleRate
        let len = AVAudioFrameCount(sr * 0.05)   /* 50 ms */
        guard len > 0, let buf = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: len) else { return nil }
        buf.frameLength = len
        let freq: Float = accent ? 1200 : 800
        for f in 0..<Int(len) {
            let t = Float(f) / Float(sr)
            let env = exp(-Float(f) / Float(len) * 6.0)
            let v = sin(2 * .pi * freq * t) * env * (accent ? 0.5 : 0.35)
            for c in 0..<Int(format.channelCount) {
                buf.floatChannelData![c][f] = v
            }
        }
        return buf
    }

    private func startMetronome(engine: AVAudioEngine) {
        let outSR = engine.outputNode.outputFormat(forBus: 0).sampleRate
        guard outSR > 0 else { return }
        guard let stereo = AVAudioFormat(standardFormatWithSampleRate: outSR, channels: 2) else { return }

        let player = AVAudioPlayerNode()
        engine.attach(player)
        engine.connect(player, to: engine.mainMixerNode, format: stereo)
        player.play()

        clickPlayer = player
        clickFormat = stereo
        nextClickSample = 0
        clickBeatCounter = 0

        let beatSamples = AVAudioFramePosition(outSR * 60.0 / max(30, min(metronomeBPM, 300)))
        let lookaheadSamples = AVAudioFramePosition(outSR * 0.25)

        let timer = DispatchSource.makeTimerSource(queue: DispatchQueue.global(qos: .userInitiated))
        timer.schedule(deadline: .now(), repeating: .milliseconds(40))
        timer.setEventHandler { [weak self] in
            guard let self = self, let player = self.clickPlayer, !self.isPaused else { return }
            guard let renderTime = player.lastRenderTime,
                  let playerTime = player.playerTime(forNodeTime: renderTime) else { return }

            while self.nextClickSample < playerTime.sampleTime + AVAudioFramePosition(lookaheadSamples) {
                let accent = self.clickBeatCounter % 4 == 0
                if let buf = self.makeClick(accent: accent) {
                    let when = AVAudioTime(sampleTime: self.nextClickSample, atRate: outSR)
                    player.scheduleBuffer(buf, at: when)
                }
                self.nextClickSample += beatSamples
                self.clickBeatCounter += 1
            }
        }
        timer.resume()
        clickTimer = timer
    }

    private func stopClick() {
        clickPlayer?.stop()
    }

    private func stopMetronome() {
        clickTimer?.cancel()
        clickTimer = nil
        if let player = clickPlayer {
            player.stop()
            if let engine = engine {
                engine.detach(player)
            }
        }
        clickPlayer = nil
        clickFormat = nil
    }
}

extension Notification.Name {
    /// Posted when the selected input device vanished before/at start and
    /// the recorder fell back to the system default input.
    static let studioRecorderFellBackToDefault =
        Notification.Name("studioRecorderFellBackToDefault")
}
