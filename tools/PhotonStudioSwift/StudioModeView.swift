import SwiftUI
import AVFoundation
import UniformTypeIdentifiers

/// Audio codec used for studio recordings.
enum StudioCodec: Int, CaseIterable, Identifiable {
    case photonHQ = 0
    case gravitational = 1
    var id: Int { rawValue }
    var label: String {
        switch self {
        case .photonHQ: return "Photon HQ (spatial, lossless)"
        case .gravitational: return "Classic Gravitational (compact)"
        }
    }
    var shortLabel: String {
        switch self {
        case .photonHQ: return "PH04"
        case .gravitational: return "GFC"
        }
    }
}

/// One saved recording in this Studio session.
struct SessionTake: Identifiable, Codable {
    let id: UUID
    var url: URL             // rename updates JSON + the file on disk
    var name: String         // rename updates JSON + the file on disk
    let codec: String
    let duration: TimeInterval
    let size: UInt64
    let date: Date
}

struct StudioModeView: View {

    // Photon Engine (Swift ↔ C Bridge)
    @StateObject private var photonEngine = PhotonEngineService()
    @State private var isLiveSensing = false

    // Real recording + playback
    @StateObject private var recorder = AudioRecorderService()
    @StateObject private var playback = TakePlaybackController()
    @StateObject private var inputDevices = InputDeviceService()  // device names for header + settings
    @State private var lastTake: RecordedTake? = nil
    @State private var sessionTakes: [SessionTake] = []
    @State private var isProcessing = false          // encoding / saving
    @State private var statusMessage: String? = nil  // transient toast

    // Recording settings
    @AppStorage("studio.codec") private var codecRaw = StudioCodec.photonHQ.rawValue
    @AppStorage("studio.input.deviceUID") private var deviceUID = ""
    @AppStorage("studio.monitoring") private var monitoring = false
    /* Metronome state intentionally shares the SAME @AppStorage keys the
     * MetronomePopover writes — the old @State pair never saw the popover's
     * changes, so the click track could never actually engage. */
    @AppStorage("studio.metronome.on") private var metronomeOn = false
    @AppStorage("studio.metronome.bpm") private var metronomeBPM: Double = 120
    private var codec: StudioCodec { StudioCodec(rawValue: codecRaw) ?? .photonHQ }
    @State private var wasSensingBeforeRecord = false

    // Animation states
    @State private var pulseAnim: Bool = false

    // Popover / Sheet states
    @State private var showSettings = false
    @State private var showMetronomeMenu = false
    @State private var showTimelinePanel = false
    @State private var showAppSettings = false      // full settings page (sheet)
    @State private var selectedSourceID: UInt32? = nil
    
    var body: some View {
        HStack(spacing: 0) {
            
            // MAIN VIEWPORT (Radar & Controls)
            VStack(spacing: 0) {
                
            // Top Header HUD
            HStack(alignment: .center) {
                VStack(alignment: .leading, spacing: 6) {
                    Text("PHOTON AUDIO ENGINE")
                        .font(.system(size: 14, weight: .black, design: .default))
                        .tracking(1.5)
                        .foregroundColor(.black.opacity(0.8))
                        .padding(.leading, 56) // macOS traffic-light clearance

                    HStack(spacing: 6) {
                        HUDTag(title: "ENGINE", value: engineStatusLabel, color: engineStatusColor)
                        HUDTag(title: "MICS", value: photonEngine.stats.micCount > 0 ? "\(photonEngine.stats.micCount)" : "—", color: .blue)
                        HUDTag(title: "SONAR", value: photonEngine.state == .running ? "\(photonEngine.stats.sonarSources)" : "—", color: .indigo)
                        HUDTag(title: "TDOA", value: photonEngine.state == .running && photonEngine.stats.tdoaSources > 0 ? "\(photonEngine.stats.tdoaSources)" : "—", color: .mint)
                        HUDTag(title: "TRACKS", value: photonEngine.state == .running ? "\(photonEngine.stats.fusedTracks)" : "—", color: .teal)
                        HUDTag(title: "FPS", value: photonEngine.stats.sensingFPS > 0 ? String(format: "%.0f Hz", photonEngine.stats.sensingFPS) : "—", color: .black.opacity(0.55))
                    }
                    .padding(.leading, 56) // aligns tags under the title
                }

                Spacer()

                // Recording Timer + capture-format readout
                VStack(alignment: .trailing, spacing: 3) {
                    HStack(spacing: 10) {
                        Circle()
                            .fill(recorder.isRecording ? (recorder.isPaused ? Color.orange : Color.red) : Color.gray.opacity(0.4))
                            .frame(width: 10, height: 10)
                            .opacity(recorder.isRecording && !recorder.isPaused && pulseAnim ? 0.3 : 1.0)
                        Text(timeString(from: recorder.duration))
                            .font(.system(size: 28, weight: .semibold, design: .monospaced))
                            .foregroundColor(recorder.isRecording ? (recorder.isPaused ? .orange : .red) : .black.opacity(0.6))
                            .shadow(color: recorder.isRecording ? Color.red.opacity(0.3) : .clear, radius: 10)
                        if isProcessing {
                            ProgressView()
                                .controlSize(.small)
                        }
                    }
                    Text(captureFormatReadout)
                        .font(.system(size: 9, weight: .heavy))
                        .tracking(0.6)
                        .foregroundColor(.black.opacity(0.35))
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
            }
            .padding(.horizontal, 20)
            .padding(.top, 14)
            .padding(.bottom, 16)

                // The 3D-ish Radar map (Light mode tuned)
                ZStack {
                    SpatialMapViewDetailed(
                        sources: photonEngine.trackedSources,
                        isSensing: photonEngine.state == .running,
                        isRecording: recorder.isRecording,
                        pulseAnim: $pulseAnim,
                        selectedSourceID: $selectedSourceID
                    )

                    // Spatial atom mini-visualization while a Photon take plays
                    if playback.isPlaying && !playback.atoms.isEmpty {
                        AtomPlaybackOverlay(
                            atoms: playback.atoms,
                            progress: playback.progress,
                            timeRange: playback.atomTimeRange
                        )
                    }

                    // Transient status toast
                    if let msg = statusMessage {
                        VStack {
                            Text(msg)
                                .font(.system(size: 12, weight: .semibold))
                                .foregroundColor(.white)
                                .padding(.horizontal, 16)
                                .padding(.vertical, 9)
                                .background(Capsule().fill(Color.black.opacity(0.75)))
                                .shadow(color: Color.black.opacity(0.2), radius: 8, y: 3)
                            Spacer()
                        }
                        .padding(.top, 14)
                        .transition(.move(edge: .top).combined(with: .opacity))
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .clipShape(RoundedRectangle(cornerRadius: 16, style: .continuous))
                .background(
                    RoundedRectangle(cornerRadius: 16, style: .continuous)
                        .fill(Color.white)
                )
                .shadow(color: Color.black.opacity(0.05), radius: 10, y: 3)
                .overlay(
                    RoundedRectangle(cornerRadius: 16, style: .continuous)
                        .stroke(Color.black.opacity(0.06), lineWidth: 1)
                )
                .padding(.horizontal, 20)

                // Floating Transport Controls
                VStack(spacing: 10) {
                    TransportDock(
                        isRecording: recorder.isRecording,
                        isPaused: recorder.isPaused,
                        inputLevel: recorder.inputLevel,
                        canPlay: lastTake != nil && !recorder.isRecording,
                        isPlaying: playback.isPlaying,
                        pulseAnim: $pulseAnim,
                        showSettings: $showSettings,
                        showMetronomeMenu: $showMetronomeMenu,
                        showTimelinePanel: $showTimelinePanel,
                        codecLabel: codec.shortLabel,
                        metronomeEnabled: metronomeOn,
                        takeCount: sessionTakes.count,
                        photonEngine: photonEngine,
                        recorder: recorder,
                        inputDevices: inputDevices,
                        recordAction: { toggleRecord() },
                        pauseAction: { togglePause() },
                        playAction: { playback.toggle(take: lastTake) }
                    )

                    // Seek + loop mini-transport (last take / loaded file)
                    if !recorder.isRecording && (lastTake != nil || playback.loadedPath != nil) {
                        HStack(spacing: 12) {
                            Button(action: { playback.setLoop(!playback.loopEnabled) }) {
                                Image(systemName: "repeat")
                                    .font(.system(size: 12, weight: .semibold))
                                    .foregroundColor(playback.loopEnabled ? .blue : .black.opacity(0.4))
                            }
                            .buttonStyle(PlainButtonStyle())
                            .help("Loop playback")

                            Text(loadedTakeName)
                                .font(.system(size: 11, weight: .semibold))
                                .foregroundColor(.black.opacity(0.65))
                                .lineLimit(1)
                                .truncationMode(.middle)
                                .frame(width: 130, alignment: .leading)
                                .help(loadedTakeName)

                            Slider(value: Binding(
                                get: { playback.progress },
                                set: { playback.seek(to: $0) }
                            ), in: 0...1)
                                .frame(width: 220)

                            Text(String(format: "%.0f%%", playback.progress * 100))
                                .font(.system(size: 10, weight: .medium, design: .monospaced))
                                .foregroundColor(.black.opacity(0.45))
                                .frame(width: 40, alignment: .trailing)
                        }
                        .padding(.horizontal, 18)
                        .padding(.vertical, 8)
                        .background(
                            Capsule().fill(Color.white.opacity(0.97))
                                .shadow(color: .black.opacity(0.06), radius: 10, y: 4)
                        )
                        .overlay(Capsule().stroke(Color.black.opacity(0.05), lineWidth: 1))
                        .transition(.opacity.combined(with: .move(edge: .bottom)))
                    }
                }
                .padding(.vertical, 24)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Color(white: 0.96).edgesIgnoringSafeArea(.all))
            
            // DIVIDER
            Rectangle()
                .fill(Color.black.opacity(0.1))
                .frame(width: 1)
            
            // RIGHT SIDEBAR (Inspector)
            InspectorSidebarView(
                selectedSourceID: $selectedSourceID,
                isLiveSensing: $isLiveSensing,
                photonEngine: photonEngine,
                sessionTakes: sessionTakes,
                onOpenSettings: { showAppSettings = true }
            )
            .frame(width: 360)
        }
        // Force light mode
        .preferredColorScheme(.light)
        .onAppear {
            loadSessionTakes()
            withAnimation(.easeInOut(duration: 1.0).repeatForever()) {
                pulseAnim = true
            }
            // Saved input device vanished → recorder fell back to default.
            NotificationCenter.default.addObserver(
                forName: .studioRecorderFellBackToDefault,
                object: nil, queue: .main) { _ in
                    deviceUID = ""
                    showToast("Selected mic unavailable — recording from system default")
                }
        }
        .onDisappear {
            playback.stop()
        }

        // Session takes list
        .sheet(isPresented: $showTimelinePanel) {
            TakesSheetView(
                takes: sessionTakes,
                revealInFinder: { take in
                    NSWorkspace.shared.activateFileViewerSelecting([take.url])
                },
                playTake: { take in
                    playback.playFile(url: take.url) { showToast($0) }
                },
                exportTake: { take in
                    exportTakeAsWav(take)
                },
                renameTake: { take, newName in
                    renameTake(take, to: newName)
                },
                deleteTake: { take in
                    deleteTake(take)
                },
                playback: playback
            )
        }

        // Full settings page (gear button in the sidebar header, or Cmd-,)
        .sheet(isPresented: $showAppSettings) {
            StudioSettingsView(recorder: recorder, inputDevices: inputDevices, takeCount: sessionTakes.count)
        }
        .onReceive(NotificationCenter.default.publisher(for: .studioOpenSettings)) { _ in
            showAppSettings = true
        }
    }
    
    // MARK: Actions

    private func showToast(_ msg: String) {
        withAnimation { statusMessage = msg }
        DispatchQueue.main.asyncAfter(deadline: .now() + 4) {
            withAnimation { if statusMessage == msg { statusMessage = nil } }
        }
    }

    private func toggleRecord() {
        if recorder.isRecording {
            finishRecordingAndSave()
        } else {
            startRecording()
        }
    }

    /// Header readout: selected input · capture format · codec. The recorder
    /// only knows rate/channels once an engine has run, so those parts
    /// appear while (or after) recording.
    private var captureFormatReadout: String {
        var parts: [String] = []
        if let sel = inputDevices.device(uid: deviceUID) {
            parts.append(sel.displayName)
        } else if let def = inputDevices.defaultUID.flatMap({ inputDevices.device(uid: $0) }) {
            parts.append(def.displayName)
        } else {
            parts.append("System Default")
        }
        if recorder.isRecording {
            parts.append(String(format: "%.1f kHz", recorder.sampleRate / 1000))
            parts.append(recorder.channels == 1 ? "1ch" : "\(recorder.channels)ch")
        }
        parts.append(codec.shortLabel)
        return parts.joined(separator: " · ")
    }

    /// Name shown next to the seek slider — the loaded file's name from
    /// disk playback, or the last take's save name when held in memory.
    private var loadedTakeName: String {
        if let path = playback.loadedPath {
            return (path as NSString).lastPathComponent
        }
        if lastTake != nil { return "Last take" }
        return "Take"
    }

    private func startRecording() {
        guard !isProcessing else { return }
        playback.stop()

        /* macOS allows one input-tap owner: pause Photon sensing while the
         * recorder owns the microphone, restore it after the take. */
        wasSensingBeforeRecord = isLiveSensing && photonEngine.state == .running
        if photonEngine.state == .running {
            photonEngine.stop()
            isLiveSensing = false
        }

        recorder.metronomeEnabled = metronomeOn
        recorder.metronomeBPM = metronomeBPM
        recorder.selectDevice(uid: deviceUID.isEmpty ? nil : deviceUID)
        recorder.setMonitoring(monitoring)

        if !recorder.start() {
            restoreSensingIfNeeded()
            showToast("Microphone unavailable — check permissions")
        }
    }

    /// Pause/resume the in-flight take without releasing the mic.
    private func togglePause() {
        guard recorder.isRecording else { return }
        if recorder.isPaused {
            recorder.resume()
            showToast("Recording resumed")
        } else {
            recorder.pause()
            showToast("Paused — press again to resume, stop to finish")
        }
    }

    private func finishRecordingAndSave() {
        guard var take = recorder.stop() else {
            restoreSensingIfNeeded()
            showToast("Empty take — nothing recorded")
            return
        }
        restoreSensingIfNeeded()
        isProcessing = true

        let chosenCodec = codec
        DispatchQueue.global(qos: .userInitiated).async {
            var ctx = qtsq_context_t()
            qtsq_init(&ctx)

            let result = encodeTake(take: take, codec: chosenCodec, ctx: &ctx)

            // Codec round-trip: decode what was just encoded so the Play
            // button previews the actual .qtsq output, not the raw capture.
            if result == QTSQ_OK {
                var pcm16: UnsafeMutablePointer<Int16>? = nil
                var outSamples: Int = 0
                var outRate: UInt32 = 0
                var outCh: UInt8 = 0
                if qtsq_decompress_audio(&ctx, &pcm16, &outSamples, &outRate, &outCh) == QTSQ_OK,
                   let buf = pcm16, outSamples > 0 {
                    take.encodedPCM = Data(bytes: buf, count: outSamples * MemoryLayout<Int16>.size)
                    take.encodedSampleRate = Double(outRate)
                    take.encodedChannels = Int(outCh)
                }
                if pcm16 != nil { free(pcm16) }
            }

            DispatchQueue.main.async {
                isProcessing = false
                guard result == QTSQ_OK else {
                    qtsq_free(&ctx)
                    showToast("Encoding failed: \(String(cString: qtsq_error_string(result)))")
                    return
                }
                lastTake = take
                presentSavePanel(ctx: &ctx, take: take)
                qtsq_free(&ctx)
            }
        }
    }

    /// Encodes a finished take into `ctx` with the selected codec.
    private func encodeTake(take: RecordedTake, codec: StudioCodec, ctx: inout qtsq_context_t) -> Int32 {
        let totalSamples = take.pcm.count / MemoryLayout<Float>.size
        return take.pcm.withUnsafeBytes { (raw: UnsafeRawBufferPointer) -> Int32 in
            guard let base = raw.bindMemory(to: Float.self).baseAddress else { return QTSQ_ERR_NULL }
            switch codec {
            case .photonHQ:
                /* Photon HQ expects at least stereo — duplicate mono input */
                if take.channels >= 2 {
                    return qtsq_compress_audio_hq(&ctx, base, totalSamples, UInt32(take.sampleRate), UInt8(take.channels))
                }
                var stereo = [Float](repeating: 0, count: totalSamples * 2)
                for i in 0..<totalSamples {
                    stereo[i * 2] = base[i]
                    stereo[i * 2 + 1] = base[i]
                }
                return stereo.withUnsafeMutableBufferPointer { ptr in
                    qtsq_compress_audio_hq(&ctx, ptr.baseAddress!, ptr.count, UInt32(take.sampleRate), 2)
                }
            case .gravitational:
                /* Classic codec takes interleaved int16 */
                var s16 = [Int16](repeating: 0, count: totalSamples)
                for i in 0..<totalSamples {
                    let v = max(-1.0, min(1.0, base[i]))
                    s16[i] = Int16(v * 32767.0)
                }
                return s16.withUnsafeMutableBufferPointer { ptr in
                    qtsq_compress_gravitational_audio(&ctx, ptr.baseAddress!, UInt32(totalSamples), UInt8(take.channels), UInt32(take.sampleRate))
                }
            }
        }
    }

    private func presentSavePanel(ctx: inout qtsq_context_t, take: RecordedTake) {
        let save = NSSavePanel()
        save.allowedContentTypes = [UTType(filenameExtension: "qtsq")!]
        let fmt = DateFormatter()
        fmt.dateFormat = "HH-mm-ss"
        save.nameFieldStringValue = "Studio Take \(fmt.string(from: Date())).qtsq"

        guard save.runModal() == .OK, let url = save.url else {
            showToast("Take kept in memory — press Play to preview")
            return
        }

        let writeResult = url.path.withCString { cPath -> Int32 in
            if !qtsqPromptAndEncrypt(&ctx) { return -1 }
            return qtsq_write(&ctx, cPath)
        }

        if writeResult == 0 {
            let size = ((try? FileManager.default.attributesOfItem(atPath: url.path))?[.size] as? UInt64) ?? 0
            let name = url.deletingPathExtension().lastPathComponent
            sessionTakes.insert(SessionTake(
                id: UUID(),
                url: url, name: name,
                codec: codec.shortLabel,
                duration: take.duration,
                size: size, date: Date()
            ), at: 0)
            saveSessionTakes()
            showToast("Saved \(url.lastPathComponent)")
        } else if writeResult == -1 {
            showToast("Save cancelled")
        } else {
            showToast("Failed to write .qtsq file")
        }
    }

    // MARK: Session-take persistence

    /// Takes survive relaunches (and Studio ↔ Vault switches) as a JSON list
    /// in Application Support; entries whose file vanished are dropped on load.
    private static var takesFileURL: URL {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("QTSQ", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("studio-takes.json")
    }

    private func loadSessionTakes() {
        guard let data = try? Data(contentsOf: Self.takesFileURL),
              let takes = try? JSONDecoder().decode([SessionTake].self, from: data) else { return }
        sessionTakes = takes.filter { FileManager.default.fileExists(atPath: $0.url.path) }
    }

    private func saveSessionTakes() {
        if let data = try? JSONEncoder().encode(sessionTakes) {
            try? data.write(to: Self.takesFileURL, options: .atomic)
        }
    }

    // MARK: Take management (rename / delete / export)

    /// Renames a take: file on disk + the JSON list stay in sync.
    private func renameTake(_ take: SessionTake, to newName: String) {
        let oldURL = take.url
        let newURL = oldURL.deletingLastPathComponent()
            .appendingPathComponent(newName)
            .appendingPathExtension(oldURL.pathExtension)

        guard FileManager.default.fileExists(atPath: oldURL.path) else {
            showToast("File missing — removed from the list")
            sessionTakes.removeAll { $0.id == take.id }
            saveSessionTakes()
            return
        }
        guard newURL.path != oldURL.path else { return }

        // Don't clobber an existing file.
        if FileManager.default.fileExists(atPath: newURL.path) {
            showToast("A file with that name already exists")
            return
        }

        do {
            try FileManager.default.moveItem(at: oldURL, to: newURL)
            if let idx = sessionTakes.firstIndex(where: { $0.id == take.id }) {
                sessionTakes[idx].name = newURL.deletingPathExtension().lastPathComponent
                sessionTakes[idx].url = newURL
            }
            saveSessionTakes()
            showToast("Renamed to \(newURL.lastPathComponent)")
        } catch {
            showToast("Rename failed: \(error.localizedDescription)")
        }
    }

    /// Moves a take's file to the Trash (recoverable) and drops it from
    /// the list. If playback was on this take, stop it first.
    private func deleteTake(_ take: SessionTake) {
        if playback.loadedPath == take.url.path {
            playback.stop()
        }
        try? FileManager.default.removeItem(at: take.url)

        sessionTakes.removeAll { $0.id == take.id }
        saveSessionTakes()
        showToast("Moved \(take.name).qtsq to Trash")
    }

    /// Exports a saved take to WAV: decodes via the C library (device-
    /// encrypted takes unlock with this Mac's key) and wraps with the
    /// shared WAV writer. Gravitational takes already decode to a
    /// complete WAV (RIFF) — written as-is.
    private func exportTakeAsWav(_ take: SessionTake) {
        let save = NSSavePanel()
        save.allowedContentTypes = [UTType(filenameExtension: "wav")!]
        save.nameFieldStringValue = take.name + ".wav"
        guard save.runModal() == .OK, let url = save.url else { return }

        isProcessing = true
        let srcPath = take.url.path
        let dstPath = url.path

        DispatchQueue.global(qos: .userInitiated).async {
            var ctx = qtsq_context_t()
            guard qtsq_read(&ctx, srcPath) == QTSQ_OK else {
                DispatchQueue.main.async {
                    isProcessing = false
                    showToast("Could not read the take")
                }
                return
            }

            // Device-encrypted takes unlock with this Mac's key.
            if qtsq_is_encrypted(&ctx) != 0,
               ctx.security.encryption_type == 2,
               DeviceKeyStore.shared.isAvailable {
                _ = DeviceKeyStore.shared.decryptWithDeviceKey(&ctx)
            }

            // Gravitational: decoder returns a complete WAV already.
            if ctx.header.strategy == 0xFF {
                var wavOut: UnsafeMutablePointer<UInt8>? = nil
                var wavLen: Int = 0
                let r = qtsq_decompress_gravitational_audio(&ctx, &wavOut, &wavLen)
                if r == QTSQ_OK, let buf = wavOut, wavLen > 0 {
                    let data = Data(bytes: buf, count: wavLen)
                    free(buf)
                    qtsq_free(&ctx)
                    DispatchQueue.main.async {
                        isProcessing = false
                        do {
                            try data.write(to: URL(fileURLWithPath: dstPath), options: .atomic)
                            showToast("Exported \(url.lastPathComponent)")
                        } catch {
                            showToast("Write failed: \(error.localizedDescription)")
                        }
                    }
                    return
                }
                if wavOut != nil { free(wavOut) }
                DispatchQueue.main.async {
                    qtsq_free(&ctx)
                    isProcessing = false
                    showToast("Decode failed: \(String(cString: qtsq_error_string(r)))")
                }
                return
            }

            // Photon / lensing / procedural audio: int16 + shared makeWav.
            var pcm16: UnsafeMutablePointer<Int16>? = nil
            var samples: Int = 0
            var rate: UInt32 = 0
            var ch: UInt8 = 0
            let r = qtsq_decompress_audio(&ctx, &pcm16, &samples, &rate, &ch)
            guard r == QTSQ_OK, let buf = pcm16, samples > 0, ch > 0 else {
                if pcm16 != nil { free(pcm16) }
                DispatchQueue.main.async {
                    qtsq_free(&ctx)
                    isProcessing = false
                    showToast("Decode failed: \(String(cString: qtsq_error_string(r)))")
                }
                return
            }
            let data = Data(bytes: buf, count: samples * MemoryLayout<Int16>.size)
            free(buf)
            qtsq_free(&ctx)

            let wav = makeWav(pcm: data,
                                                    sampleRate: Int(rate > 0 ? rate : 44100),
                                                    channels: Int(ch),
                                                    bitsPerSample: 16)
            DispatchQueue.main.async {
                isProcessing = false
                do {
                    try wav.write(to: URL(fileURLWithPath: dstPath), options: .atomic)
                    showToast("Exported \(url.lastPathComponent)")
                } catch {
                    showToast("Write failed: \(error.localizedDescription)")
                }
            }
        }
    }

    private func restoreSensingIfNeeded() {
        if wasSensingBeforeRecord {
            wasSensingBeforeRecord = false
            isLiveSensing = true
            if photonEngine.state == .idle || photonEngine.state == .stopped {
                photonEngine.requestPermissions()
            }
        }
    }

    private func timeString(from interval: TimeInterval) -> String {
        let minutes = Int(interval) / 60
        let seconds = Int(interval) % 60
        let ms = Int((interval.truncatingRemainder(dividingBy: 1)) * 100)
        return String(format: "%02d:%02d:%02d", minutes, seconds, ms)
    }
    
    // Photon Engine HUD helpers
    private var engineStatusLabel: String {
        switch photonEngine.state {
        case .idle: return "IDLE"
        case .requestingPermissions: return "PERMS"
        case .starting: return "INIT"
        case .running: return "LIVE"
        case .error: return "ERROR"
        case .stopped: return "STOPPED"
        }
    }
    
    private var engineStatusColor: Color {
        switch photonEngine.state {
        case .running: return .green
        case .error: return .red
        case .starting, .requestingPermissions: return .orange
        default: return .gray
        }
    }
}

// MARK: - Subviews

struct HUDTag: View {
    let title: String
    let value: String
    let color: Color

    var body: some View {
        HStack(spacing: 6) {
            Text(title)
                .font(.system(size: 9, weight: .heavy))
                .foregroundColor(.black.opacity(0.4))
            Text(value)
                .font(.system(size: 10, weight: .bold))
                .foregroundColor(color)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(Color.white)
        .cornerRadius(6)
        .shadow(color: Color.black.opacity(0.05), radius: 3, y: 1)
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(Color.black.opacity(0.05), lineWidth: 1))
    }
}

struct TransportDock: View {
    let isRecording: Bool
    let isPaused: Bool
    let inputLevel: Float
    let canPlay: Bool
    let isPlaying: Bool
    @Binding var pulseAnim: Bool

    @Binding var showSettings: Bool
    @Binding var showMetronomeMenu: Bool
    @Binding var showTimelinePanel: Bool

    let codecLabel: String
    let metronomeEnabled: Bool
    let takeCount: Int
    @ObservedObject var photonEngine: PhotonEngineService
    @ObservedObject var recorder: AudioRecorderService
    @ObservedObject var inputDevices: InputDeviceService

    var recordAction: () -> Void
    var pauseAction: () -> Void
    var playAction: () -> Void

    var body: some View {
        HStack(spacing: 32) {
            // Recording Settings
            Button(action: { showSettings.toggle() }) {
                Image(systemName: "slider.horizontal.3")
                    .font(.system(size: 15, weight: .medium))
                    .foregroundColor(showSettings ? .blue : .black.opacity(0.6))
            }
            .buttonStyle(PlainButtonStyle())
            .popover(isPresented: $showSettings, arrowEdge: .bottom) {
                RecordingSettingsPopover(codecLabel: codecLabel, photonEngine: photonEngine, recorder: recorder, inputDevices: inputDevices)
            }

            // Metronome
            Button(action: { showMetronomeMenu.toggle() }) {
                Image(systemName: "metronome")
                    .font(.system(size: 15, weight: .medium))
                    .foregroundColor(showMetronomeMenu ? .blue : (metronomeEnabled ? .orange : .black.opacity(0.6)))
            }
            .buttonStyle(PlainButtonStyle())
            .popover(isPresented: $showMetronomeMenu, arrowEdge: .bottom) {
                MetronomePopover()
            }

            // Live input level meter (visible while recording)
            LevelMeter(level: inputLevel)
                .frame(width: 34, height: 22)
                .opacity(isRecording ? 1.0 : 0.25)

            // Record / Stop button — circle when idle, square when rolling
            Button(action: recordAction) {
                ZStack {
                    Circle()
                        .fill(Color.white)
                        .frame(width: 38, height: 38)
                        .shadow(color: .black.opacity(0.15), radius: 6, y: 3)
                        .overlay(Circle().stroke(Color.black.opacity(0.05), lineWidth: 1))

                    if isRecording {
                        RoundedRectangle(cornerRadius: 3)
                            .fill(Color.red)
                            .frame(width: 12, height: 12)
                            .shadow(color: .red.opacity(0.4), radius: 6)
                    } else {
                        Circle()
                            .fill(Color.red)
                            .frame(width: 16, height: 16)
                    }
                }
            }
            .buttonStyle(PlainButtonStyle())
            .scaleEffect(isRecording && pulseAnim && !isPaused ? 1.05 : 1.0)
            .help(isRecording ? "Stop and save the take" : "Record")

            // Pause / Resume — only visible mid-take
            if isRecording {
                Button(action: pauseAction) {
                    Image(systemName: isPaused ? "play.fill" : "pause.fill")
                        .font(.system(size: 15, weight: .black))
                        .foregroundColor(isPaused ? .green : .orange)
                }
                .buttonStyle(PlainButtonStyle())
                .help(isPaused ? "Resume recording" : "Pause recording")
                .transition(.scale.combined(with: .opacity))
            }

            // Play last take
            Button(action: playAction) {
                Image(systemName: isPlaying ? "pause.fill" : "play.fill")
                    .font(.system(size: 16, weight: .black))
                    .foregroundColor(!canPlay ? .black.opacity(0.2) : (isPlaying ? .blue : .black.opacity(0.8)))
            }
            .buttonStyle(PlainButtonStyle())
            .disabled(!canPlay)
            .help(canPlay ? "Play last take" : "No take yet")

            // Session takes
            Button(action: { showTimelinePanel.toggle() }) {
                Image(systemName: "list.bullet.rectangle")
                    .font(.system(size: 15, weight: .medium))
                    .foregroundColor(showTimelinePanel ? .blue : .black.opacity(0.6))
                    .overlay(alignment: .topTrailing) {
                        if takeCount > 0 {
                            Text("\(takeCount)")
                                .font(.system(size: 8, weight: .black, design: .monospaced))
                                .foregroundColor(.white)
                                .padding(.horizontal, 3.5)
                                .padding(.vertical, 1)
                                .background(Capsule().fill(Color.blue))
                                .offset(x: 9, y: -4)
                        }
                    }
            }
            .buttonStyle(PlainButtonStyle())
            .help("Session takes")
        }
        .padding(.horizontal, 32)
        .padding(.vertical, 6)
        .background(
            Capsule()
                .fill(Color.white.opacity(0.97))
                .shadow(color: .black.opacity(0.08), radius: 14, y: 5)
        )
        .overlay(
            Capsule()
                .stroke(Color.black.opacity(0.05), lineWidth: 1)
        )
    }

}

// MARK: Detailed Radar Map (Light Theme)

struct SpatialMapViewDetailed: View {
    let sources: [PhotonTrackedSource]
    let isSensing: Bool
    let isRecording: Bool
    @Binding var pulseAnim: Bool

    @Binding var selectedSourceID: UInt32?

    var body: some View {
        GeometryReader { geo in
            let center = CGPoint(x: geo.size.width / 2, y: geo.size.height / 2)
            let scaleX: CGFloat = geo.size.width / 10.0 // 10 units wide
            let scaleY: CGFloat = geo.size.height / 10.0 // 10 units tall

            ZStack {
                // Background grid lines (Light Gray)
                Path { path in
                    for i in -10...10 {
                        // Verticals
                        let x = center.x + CGFloat(i) * scaleX
                        path.move(to: CGPoint(x: x, y: 0))
                        path.addLine(to: CGPoint(x: x, y: geo.size.height))
                        // Horizontals
                        let y = center.y + CGFloat(i) * scaleY
                        path.move(to: CGPoint(x: 0, y: y))
                        path.addLine(to: CGPoint(x: geo.size.width, y: y))
                    }
                }
                .stroke(Color.blue.opacity(0.04), lineWidth: 1)

                // Distance Rings (Rectangular)
                ForEach(1...5, id: \.self) { ring in
                    let fraction = CGFloat(ring) / 5.0
                    RoundedRectangle(cornerRadius: 16)
                        .stroke(Color.blue.opacity(0.08), lineWidth: ring % 2 == 0 ? 1 : 0.5)
                        .frame(width: geo.size.width * fraction, height: geo.size.height * fraction)
                        .position(center)

                    // Ring labels
                    Text("\(ring)m")
                        .font(.system(size: 9, weight: .bold))
                        .foregroundColor(.blue.opacity(0.45))
                        .padding(.horizontal, 4)
                        .background(Color.white)
                        .position(x: center.x, y: center.y - (geo.size.height * fraction / 2.0))
                }

                // Origin Axis
                Path { path in
                    path.move(to: CGPoint(x: center.x, y: 0))
                    path.addLine(to: CGPoint(x: center.x, y: geo.size.height))
                    path.move(to: CGPoint(x: 0, y: center.y))
                    path.addLine(to: CGPoint(x: geo.size.width, y: center.y))
                }
                .stroke(Color.black.opacity(0.1), style: StrokeStyle(lineWidth: 1, dash: [4, 4]))

                // Host node at the origin — this Mac
                HostRadarNode(isRecording: isRecording)
                    .position(center)

                // Draw live tracked sources
                ForEach(sources, id: \.id) { src in
                    /* Clamp into the visible room so Kalman jitter or
                     * out-of-range estimates never push nodes off-card. */
                    let ux = max(-4.6, min(4.6, src.x))
                    let uy = max(-4.6, min(4.6, src.y))
                    let screenPos = CGPoint(x: center.x + ux * scaleX, y: center.y - uy * scaleY)
                    let isSelected = src.id == selectedSourceID

                    BouncingArrowBody(
                        start: center,
                        end: screenPos,
                        color: src.color.opacity(0.9)
                    )

                    SourceRadarNode(source: src, isSelected: isSelected)
                        .position(screenPos)
                        .onTapGesture {
                            withAnimation {
                                if selectedSourceID == src.id {
                                    selectedSourceID = nil
                                } else {
                                    selectedSourceID = src.id
                                }
                            }
                        }
                }

                // Empty state — sits clear of the host node at center
                if sources.isEmpty {
                    VStack(spacing: 8) {
                        Image(systemName: isSensing ? "waveform.path" : "antenna.radiowaves.left.and.right.slash")
                            .font(.system(size: 26, weight: .light))
                            .foregroundColor(.black.opacity(0.25))
                        Text(emptyStateText)
                            .font(.system(size: 12, weight: .medium))
                            .foregroundColor(.black.opacity(0.4))
                    }
                    .position(x: center.x, y: min(center.y + 72, geo.size.height - 40))
                }
            }
            .clipped()
        }
    }

    private var emptyStateText: String {
        if isRecording { return "Live sensing paused while recording" }
        if isSensing { return "Listening for sound sources…" }
        return "Turn on Live Sensing to detect sound sources"
    }
}

// Streaming Arrows Component
struct BouncingArrowBody: View {
    let start: CGPoint
    let end: CGPoint
    let color: Color
    
    var body: some View {
        let dx = end.x - start.x
        let dy = end.y - start.y
        let length = max(sqrt(dx*dx + dy*dy), 1.0)
        let angle = atan2(dy, dx)
        
        // One chevron perfectly spaced roughly every 20-30 pts depending on line length
        let count = max(Int(length / 28), 2)
        
        // Perpendicular offset vectors for placing the two streams side by side
        let nx = -dy / length
        let ny = dx / length
        let offsetAmount: CGFloat = 6.0 // Spatial separation between outgoing and incoming
        
        let outStartX = start.x - nx * offsetAmount
        let outStartY = start.y - ny * offsetAmount
        
        let inStartX = start.x + nx * offsetAmount
        let inStartY = start.y + ny * offsetAmount
        
        TimelineView(.animation) { timeline in
            let now = timeline.date.timeIntervalSinceReferenceDate
            let speed = 0.15 // Elegant, slower flow velocity
            
            ZStack {
                // Outward Stream (Host -> Node)
                ForEach(0..<count, id: \.self) { i in
                    let offset = Double(i) / Double(count)
                    let p = (now * speed + offset).truncatingRemainder(dividingBy: 1.0)
                    let currentX = outStartX + dx * p
                    let currentY = outStartY + dy * p
                    
                    Image(systemName: "chevron.right")
                        .font(.system(size: 9, weight: .heavy))
                        .foregroundColor(color)
                        .rotationEffect(.radians(Double(angle)))
                        .position(x: currentX, y: currentY)
                        .opacity(p < 0.15 ? (p / 0.15) : (p > 0.85 ? (1.0 - p) / 0.15 : 1.0))
                }
                
                // Inward Stream (Node -> Host)
                ForEach(0..<count, id: \.self) { i in
                    let offset = Double(i) / Double(count)
                    let p = (now * speed + offset).truncatingRemainder(dividingBy: 1.0)
                    // Inward moves backward along the vector (or forward from end to start)
                    let currentX = (inStartX + dx) - dx * p
                    let currentY = (inStartY + dy) - dy * p
                    
                    Image(systemName: "chevron.right")
                        .font(.system(size: 9, weight: .heavy))
                        .foregroundColor(color.opacity(0.6))
                        .rotationEffect(.radians(Double(angle + .pi)))
                        .position(x: currentX, y: currentY)
                        .opacity(p < 0.15 ? (p / 0.15) : (p > 0.85 ? (1.0 - p) / 0.15 : 1.0))
                }
            }
        }
    }
}

// Node UI for Map
struct HostRadarNode: View {
    let isRecording: Bool

    var body: some View {
        VStack(spacing: 6) {
            ZStack {
                Circle()
                    .fill(Color.white)
                    .frame(width: 34, height: 34)
                    .overlay(Circle().stroke(isRecording ? Color.red : Color.blue.opacity(0.7), lineWidth: 2))
                    .shadow(color: (isRecording ? Color.red : Color.blue).opacity(0.3), radius: 6, y: 2)

                Image(systemName: "laptopcomputer")
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundColor(isRecording ? .red : .blue)
            }

            Text("THIS MAC")
                .font(.system(size: 8, weight: .heavy))
                .tracking(0.8)
                .foregroundColor(.black.opacity(0.5))
                .padding(.horizontal, 6)
                .padding(.vertical, 3)
                .background(Color.white.opacity(0.9))
                .cornerRadius(4)
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(Color.black.opacity(0.05), lineWidth: 0.5))
        }
    }
}

// MARK: - Spatial Atom Playback Overlay (Photon takes)

/// Live visualization of a playing Photon take's decoded spatial atoms:
/// frequency maps to vertical position, pan to horizontal, magnitude to
/// glow size. Atoms near the current playback time brighten; the rest
/// sit dim so the composition reads as a moving spectrum.
struct AtomPlaybackOverlay: View {
    let atoms: [PhotonAtomInfo]
    let progress: Double                 // 0...1 playback position
    let timeRange: (start: Double, end: Double)

    /// Frequency range for the vertical axis (log scale).
    private static let minFreq: Double = 80
    private static let maxFreq: Double = 12000

    private var nowSeconds: Double {
        let span = max(timeRange.end - timeRange.start, 0.001)
        return timeRange.start + progress * span
    }

    private var maxMagnitude: Double {
        max(atoms.map { $0.magnitude }.max() ?? 1.0, 0.0001)
    }

    var body: some View {
        GeometryReader { geo in
            let logMin = log2(Self.minFreq)

            ZStack {
                ForEach(atoms) { atom in
                    // Vertical: log-frequency (80 Hz bottom → 12 kHz top)
                    let freqNorm = (log2(max(atom.freq, Self.minFreq)) - logMin)
                        / (log2(Self.maxFreq) - logMin)
                    let y = geo.size.height * (1.0 - CGFloat(max(0, min(1, freqNorm))))

                    // Horizontal: pan −1...1
                    let x = geo.size.width * (0.5 + CGFloat(max(-1, min(1, atom.pan))) * 0.42)

                    // Activity: how close the playhead is to this atom
                    let age = abs(atom.time - nowSeconds)
                    let activity = max(0, 1.0 - age / 0.75)   // ~750 ms window
                    let intensity = 0.12 + 0.88 * activity

                    let size = 3 + CGFloat(pow(atom.magnitude / maxMagnitude, 0.5)) * 9
                    let color = Color.purple.opacity(Double(intensity))

                    Circle()
                        .fill(activity > 0.35 ? color : Color.purple.opacity(0.10))
                        .frame(width: size, height: size)
                        .shadow(color: Color.purple.opacity(activity * 0.7), radius: activity * 6)
                        .position(x: x, y: y)
                }

                // Playhead label
                VStack {
                    Spacer()
                    HStack {
                        Label(String(format: "%.1f kHz · %d atoms",
                                     nowSeconds > 0 ? peakFreqNow / 1000.0 : 0, atoms.count),
                              systemImage: "waveform")
                            .font(.system(size: 9, weight: .bold, design: .monospaced))
                            .foregroundColor(.purple.opacity(0.8))
                            .padding(.horizontal, 8)
                            .padding(.vertical, 4)
                            .background(Capsule().fill(Color.white.opacity(0.85)))
                            .overlay(Capsule().stroke(Color.purple.opacity(0.2), lineWidth: 0.5))
                            .padding(8)
                        Spacer()
                    }
                }
            }
        }
        .allowsHitTesting(false)
        .animation(.easeOut(duration: 0.12), value: progress)
    }

    /// Loudest active atom's frequency, for the label.
    private var peakFreqNow: Double {
        let active = atoms.filter { abs($0.time - nowSeconds) < 0.35 }
        let pool = active.isEmpty ? atoms : active
        return pool.map { $0 }.max { $0.magnitude < $1.magnitude }?.freq ?? 0
    }
}

struct SourceRadarNode: View {
    let source: PhotonTrackedSource
    let isSelected: Bool

    var body: some View {
        VStack(spacing: 6) {
            ZStack {
                if isSelected {
                    Circle()
                        .fill(source.color.opacity(0.15))
                        .frame(width: 90, height: 90)
                }

                Circle()
                    .fill(RadialGradient(gradient: Gradient(colors: [source.color.opacity(0.4), source.color.opacity(0.0)]), center: .center, startRadius: 0, endRadius: 30))
                    .frame(width: 60, height: 60)

                Circle()
                    .fill(source.color)
                    .frame(width: 24, height: 24)
                    .overlay(Circle().stroke(Color.white, lineWidth: isSelected ? 3 : 1))
                    .shadow(color: source.color.opacity(0.5), radius: 6, y: 2)

                Image(systemName: source.icon)
                    .font(.system(size: 11))
                    .foregroundColor(.white)
            }

            HStack(spacing: 4) {
                Text("Source \(source.id)")
                    .font(.system(size: 10, weight: .bold))
                    .foregroundColor(.black.opacity(0.8))
                Text(source.freqLabel)
                    .font(.system(size: 9, design: .monospaced))
                    .foregroundColor(.gray)
            }
            .padding(.horizontal, 6)
            .padding(.vertical, 4)
            .background(Color.white.opacity(0.9))
            .cornerRadius(4)
            .overlay(RoundedRectangle(cornerRadius: 4).stroke(Color.black.opacity(0.05), lineWidth: 0.5))
            .shadow(color: .black.opacity(0.08), radius: 3, y: 2)
        }
    }
}

// MARK: - Right Sidebar Inspector (Light Theme)

struct InspectorSidebarView: View {

    @Binding var selectedSourceID: UInt32?
    @Binding var isLiveSensing: Bool
    @ObservedObject var photonEngine: PhotonEngineService
    let sessionTakes: [SessionTake]
    let onOpenSettings: () -> Void
    
    var body: some View {
        VStack(spacing: 0) {
            // Sidebar Header
            HStack(spacing: 10) {
                Text("INSPECTOR")
                    .font(.system(size: 12, weight: .bold, design: .default))
                    .tracking(1.5)
                    .foregroundColor(.black.opacity(0.8))

                Spacer()

                HStack(spacing: 10) {
                    Button(action: onOpenSettings) {
                        Image(systemName: "gearshape")
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundColor(.black.opacity(0.45))
                    }
                    .buttonStyle(PlainButtonStyle())
                    .help("Settings")

                    Text(isLiveSensing ? "LIVE" : "OFF")
                        .font(.system(size: 8, weight: .black))
                        .tracking(0.5)
                        .foregroundColor(isLiveSensing ? .red : .gray)
                        .padding(.vertical, 3)
                        .padding(.horizontal, 6)
                        .background(isLiveSensing ? Color.red.opacity(0.1) : Color.gray.opacity(0.12))
                        .cornerRadius(4)

                    Toggle("", isOn: $isLiveSensing)
                        .labelsHidden()
                        .toggleStyle(SwitchToggleStyle(tint: .red))
                        .scaleEffect(0.7, anchor: .trailing)
                        .onChange(of: isLiveSensing) { newValue in
                            if newValue {
                                // Request permissions first. The UI will stay in `.requestingPermissions` state
                                // until granted, at which point the engine moves to `.idle`.
                                // We check if it is explicitly stopped/idle to start it.
                                if photonEngine.state == .idle || photonEngine.state == .stopped {
                                    photonEngine.requestPermissions()
                                }
                            } else {
                                photonEngine.stop()
                            }
                        }
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 14)
            .background(Color.white)

            // Handle permissions state explicitly
            if photonEngine.state == .idle && isLiveSensing && photonEngine.permissions.microphone {
                // If it went to idle but has permissions, auto-start
                Color.clear.onAppear { photonEngine.start() }
            }
            
            Divider().background(Color.black.opacity(0.05))
            
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {

                    // RADIO ENVIRONMENT — real host info + live BLE sightings
                    VStack(alignment: .leading, spacing: 12) {
                        HStack {
                            Label("RADIO ENVIRONMENT", systemImage: "dot.radiowaves.left.and.right")
                                .font(.system(size: 11, weight: .bold))
                                .foregroundColor(.black.opacity(0.6))
                            Spacer()
                        }

                        HostInspectorCard(photonEngine: photonEngine)

                        // Mono-input guidance: TDOA needs ≥2 channels.
                        if photonEngine.state == .running && photonEngine.stats.micCount < 2 {
                            HStack(alignment: .top, spacing: 8) {
                                Image(systemName: "exclamationmark.triangle.fill")
                                    .font(.system(size: 11))
                                    .foregroundColor(.orange)
                                Text("Mono input — angle tracking is off. Create a Multi-Input Device in Audio MIDI Setup (or plug in a second mic) to enable it.")
                                    .font(.system(size: 10))
                                    .foregroundColor(.black.opacity(0.55))
                            }
                            .padding(10)
                            .background(Color.orange.opacity(0.08))
                            .cornerRadius(8)
                        }

                        if photonEngine.bleBeacons.isEmpty {
                            HStack(spacing: 8) {
                                Image(systemName: "antenna.radiowaves.left.and.right.slash")
                                    .font(.system(size: 12))
                                    .foregroundColor(.black.opacity(0.3))
                                Text(isLiveSensing ? "Scanning for nearby devices…" : "Enable Live Sensing to scan nearby devices")
                                    .font(.system(size: 11))
                                    .foregroundColor(.black.opacity(0.4))
                            }
                            .padding(.horizontal, 4)
                            .padding(.vertical, 6)
                        } else {
                            ForEach(photonEngine.bleBeacons) { beacon in
                                BeaconRow(beacon: beacon)
                            }
                        }
                    }

                    // PROXIMITY ZONES — RSSI-motion activity per distance band
                    if photonEngine.state == .running && !photonEngine.zoneActivity.isEmpty {
                        VStack(alignment: .leading, spacing: 10) {
                            HStack {
                                Label("PROXIMITY ZONES", systemImage: "square.stack.3d.up.fill")
                                    .font(.system(size: 11, weight: .bold))
                                    .foregroundColor(.black.opacity(0.6))
                                Spacer()
                            }
                            ZoneActivityBar(zones: photonEngine.zoneActivity)
                        }
                    }

                    Divider().background(Color.black.opacity(0.05))

                    // SOURCES PANEL — real fused tracks only
                    VStack(alignment: .leading, spacing: 12) {
                        HStack {
                            Label("TRACKED SOURCES", systemImage: "waveform.badge.magnifyingglass")
                                .font(.system(size: 11, weight: .bold))
                                .foregroundColor(.black.opacity(0.6))
                            Spacer()
                            Text("\(photonEngine.trackedSources.count)")
                                .font(.system(size: 10, weight: .heavy, design: .monospaced))
                                .foregroundColor(.black.opacity(0.45))
                        }

                        if photonEngine.trackedSources.isEmpty {
                            HStack(spacing: 8) {
                                Image(systemName: "waveform.slash")
                                    .font(.system(size: 12))
                                    .foregroundColor(.black.opacity(0.3))
                                Text(isLiveSensing ? "No sources detected yet" : "Enable Live Sensing to detect sound sources")
                                    .font(.system(size: 11))
                                    .foregroundColor(.black.opacity(0.4))
                            }
                            .padding(.horizontal, 4)
                            .padding(.vertical, 6)
                        } else {
                            ForEach(photonEngine.trackedSources, id: \.id) { src in
                                SourceInspectorCard(
                                    source: src,
                                    isSelected: selectedSourceID == src.id,
                                    onSelect: {
                                        withAnimation {
                                            selectedSourceID = (selectedSourceID == src.id) ? nil : src.id
                                        }
                                    }
                                )
                            }
                        }
                    }

                    // ── SESSION ──
                    VStack(alignment: .leading, spacing: 10) {
                        HStack(spacing: 5) {
                            Image(systemName: "record.circle")
                                .font(.system(size: 10, weight: .semibold))
                                .foregroundColor(.black.opacity(0.4))
                            Text("SESSION")
                                .font(.system(size: 11, weight: .bold))
                                .tracking(1.0)
                                .foregroundColor(.black.opacity(0.6))
                        }

                        VStack(spacing: 6) {
                            sessionStatRow("TAKES", "\(sessionTakes.count)")
                            sessionStatRow("TOTAL TIME", sessionDurationString)
                            sessionStatRow("ON DISK", ByteCountFormatter.string(fromByteCount: sessionTotalBytes, countStyle: .file))
                        }
                    }
                }
                .padding(16)
            }
        }
        // Frosted glass sidebar for Light Mode
        .background(VisualEffectView(material: .sidebar, blendingMode: .behindWindow))
        // Keep the LIVE toggle honest: if the engine dies, stop claiming LIVE.
        .onChange(of: photonEngine.state) { newState in
            if case .error = newState {
                isLiveSensing = false
            }
        }
    }

    // MARK: Session stats

    private var sessionTotalDuration: TimeInterval {
        sessionTakes.reduce(0) { $0 + $1.duration }
    }
    private var sessionTotalBytes: Int64 {
        sessionTakes.reduce(0) { $0 + Int64($1.size) }
    }
    private var sessionDurationString: String {
        let total = Int(sessionTotalDuration.rounded())
        return String(format: "%d:%02d", total / 60, total % 60)
    }

    private func sessionStatRow(_ title: String, _ value: String) -> some View {
        HStack {
            Text(title)
                .font(.system(size: 9, weight: .heavy))
                .tracking(0.8)
                .foregroundColor(.black.opacity(0.4))
            Spacer()
            Text(value)
                .font(.system(size: 11, weight: .bold, design: .monospaced))
                .foregroundColor(.black.opacity(0.8))
        }
    }
}

/// RSSI-motion activity per proximity band (matches the C zone model:
/// 0–4 → <1m, <2.5m, <4m, <6m, farther).
struct ZoneActivityBar: View {
    let zones: [Float]
    private let labels = ["<1m", "<2.5m", "<4m", "<6m", "far"]

    var body: some View {
        HStack(alignment: .bottom, spacing: 10) {
            ForEach(0..<min(zones.count, labels.count), id: \.self) { i in
                let v = CGFloat(max(0, min(1, zones[i])))
                VStack(spacing: 4) {
                    RoundedRectangle(cornerRadius: 2)
                        .fill(
                            LinearGradient(colors: [Color.teal.opacity(0.35 + 0.65 * v),
                                                    Color.teal.opacity(0.2 + 0.8 * v)],
                                           startPoint: .bottom, endPoint: .top)
                        )
                        .frame(width: 28, height: 6 + 40 * v)
                        .animation(.easeOut(duration: 0.3), value: zones[i])
                    Text(labels[i])
                        .font(.system(size: 8, weight: .bold, design: .monospaced))
                        .foregroundColor(.black.opacity(0.4))
                }
                .frame(maxWidth: .infinity)
            }
        }
        .padding(10)
        .background(Color.white.opacity(0.6))
        .cornerRadius(8)
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Color.black.opacity(0.05), lineWidth: 1))
    }
}

// Sideboard Cards

/// Real host-machine status: mic count, hardware sample rate, engine state.
struct HostInspectorCard: View {
    @ObservedObject var photonEngine: PhotonEngineService

    private var stateLabel: String {
        switch photonEngine.state {
        case .idle: return "Idle"
        case .requestingPermissions: return "Permissions…"
        case .starting: return "Starting…"
        case .running: return "Live"
        case .error: return "Error"
        case .stopped: return "Stopped"
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                ZStack {
                    RoundedRectangle(cornerRadius: 8)
                        .fill(photonEngine.state == .running ? Color.blue : Color(white: 0.9))
                        .frame(width: 36, height: 36)
                    Image(systemName: "laptopcomputer")
                        .foregroundColor(photonEngine.state == .running ? .white : .blue)
                        .font(.system(size: 16, weight: .semibold))
                }

                VStack(alignment: .leading, spacing: 2) {
                    Text("This Mac")
                        .font(.system(size: 13, weight: .heavy))
                        .foregroundColor(.black.opacity(0.85))
                    Text(stateLabel)
                        .font(.system(size: 11, weight: .medium))
                        .foregroundColor(photonEngine.state == .running ? .green : .black.opacity(0.5))
                }
                Spacer()
            }
            .padding(14)

            Divider().background(Color.black.opacity(0.05))

            HStack {
                InspectorStatBox(title: "MICS", value: photonEngine.stats.micCount > 0 ? "\(photonEngine.stats.micCount)" : "—", isLight: true)
                Divider().background(Color.black.opacity(0.05)).frame(height: 24)
                InspectorStatBox(title: "SAMPLE RATE", value: photonEngine.stats.sampleRate > 0 ? String(format: "%.1f kHz", photonEngine.stats.sampleRate / 1000.0) : "—", isLight: true)
                Divider().background(Color.black.opacity(0.05)).frame(height: 24)
                InspectorStatBox(title: "SENSE FPS", value: photonEngine.stats.sensingFPS > 0 ? String(format: "%.0f", photonEngine.stats.sensingFPS) : "—", isLight: true)
            }
            .padding(.vertical, 10)
            .background(Color.white.opacity(0.5))
        }
        .background(Color.white)
        .cornerRadius(10)
        .shadow(color: Color.black.opacity(0.04), radius: 3, y: 1)
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color.black.opacity(0.05), lineWidth: 1))
    }
}

/// Nearby Bluetooth device sighting (name + live RSSI).
struct BeaconRow: View {
    let beacon: BLEBeacon

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: "dot.radiowaves.right")
                .font(.system(size: 12))
                .foregroundColor(.blue.opacity(0.8))
                .frame(width: 20)

            Text(beacon.id)
                .font(.system(size: 12, weight: .semibold))
                .foregroundColor(.black.opacity(0.8))
                .lineLimit(1)

            Spacer()

            SignalMeterBlock(strength: beacon.strength)
            Text("\(beacon.rssi) dBm")
                .font(.system(size: 9, design: .monospaced))
                .foregroundColor(.gray)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(Color.white)
        .cornerRadius(8)
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Color.black.opacity(0.05), lineWidth: 1))
    }
}

/// A real fused source tracked by the Photon engine.
struct SourceInspectorCard: View {
    let source: PhotonTrackedSource
    let isSelected: Bool
    let onSelect: () -> Void

    /// Seconds since the fusion engine last updated this track (0 = unknown).
    private var ageSeconds: TimeInterval? {
        guard source.lastSeenUs > 0 else { return nil }
        let age = Date().timeIntervalSince1970 - Double(source.lastSeenUs) / 1e6
        return max(0, age)
    }

    var body: some View {
        Button(action: onSelect) {
            VStack(spacing: 0) {
                HStack(spacing: 12) {
                    ZStack {
                        Circle()
                            .fill(isSelected ? source.color : Color(white: 0.9))
                            .frame(width: 36, height: 36)
                            .shadow(color: isSelected ? source.color.opacity(0.4) : .clear, radius: 4)

                        Image(systemName: source.icon)
                            .foregroundColor(isSelected ? .white : source.color)
                            .font(.system(size: 14, weight: .semibold))
                    }

                    VStack(alignment: .leading, spacing: 2) {
                        Text("Source \(source.id)")
                            .font(.system(size: 13, weight: .heavy))
                            .foregroundColor(.black.opacity(0.85))
                        Text(source.freqLabel)
                            .font(.system(size: 11, weight: .medium))
                            .foregroundColor(.black.opacity(0.5))
                    }
                    Spacer()

                    // Confidence
                    VStack(alignment: .trailing, spacing: 4) {
                        Text("\(Int(source.confidence * 100))%")
                            .font(.system(size: 11, weight: .bold, design: .monospaced))
                            .foregroundColor(.black.opacity(0.7))
                        GeometryReader { geo in
                            ZStack(alignment: .leading) {
                                Capsule().fill(Color.black.opacity(0.08))
                                Capsule().fill(source.color)
                                    .frame(width: geo.size.width * CGFloat(max(0, min(1, source.confidence))))
                            }
                        }
                        .frame(width: 44, height: 4)
                    }
                }
                .padding(14)

                Divider().background(Color.black.opacity(0.05))

                HStack(spacing: 0) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("POSITION (XYZ)")
                            .font(.system(size: 9, weight: .heavy))
                            .foregroundColor(.black.opacity(0.4))
                        Text("\(String(format: "%.2f", source.x)), \(String(format: "%.2f", source.y)), \(String(format: "%.2f", source.z))")
                            .font(.system(size: 12, weight: .medium, design: .monospaced))
                            .foregroundColor(.black.opacity(0.8))
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(12)

                    if let age = ageSeconds {
                        VStack(alignment: .trailing, spacing: 4) {
                            Text("LAST SEEN")
                                .font(.system(size: 9, weight: .heavy))
                                .foregroundColor(.black.opacity(0.4))
                            Text(String(format: "%.1fs ago", age))
                                .font(.system(size: 12, weight: .medium, design: .monospaced))
                                .foregroundColor(age < 1.0 ? .green : .orange)
                        }
                        .padding(12)
                    }
                }
                .background(Color.white.opacity(0.5))
            }
            .background(Color.white)
            .cornerRadius(10)
            .shadow(color: Color.black.opacity(isSelected ? 0.12 : 0.04), radius: isSelected ? 8 : 3, y: isSelected ? 4 : 1)
            .overlay(RoundedRectangle(cornerRadius: 10).stroke(isSelected ? source.color : Color.black.opacity(0.05), lineWidth: isSelected ? 2 : 1))
        }
        .buttonStyle(PlainButtonStyle())
    }
}

// MARK: Utilities

struct SignalMeterBlock: View {
    let strength: Double
    var body: some View {
        HStack(spacing: 3) {
            ForEach(0..<4) { i in
                RoundedRectangle(cornerRadius: 1.5)
                    .fill(i < Int(strength * 4) ? Color.green : Color.black.opacity(0.1))
                    .frame(width: 4, height: CGFloat(i * 3 + 6))
            }
        }
    }
}

struct InspectorStatBox: View {
    let title: String
    let value: String
    let isLight: Bool
    
    var body: some View {
        VStack(spacing: 4) {
            Text(title).font(.system(size: 9, weight: .heavy)).foregroundColor(.black.opacity(0.4))
            Text(value).font(.system(size: 11, weight: .bold, design: .monospaced)).foregroundColor(.black.opacity(0.8))
        }
        .frame(maxWidth: .infinity)
    }
}

/// Live input level bar (driven by the recorder's RMS).
struct LevelMeter: View {
    let level: Float   // 0...1

    var body: some View {
        HStack(spacing: 3) {
            ForEach(0..<5, id: \.self) { i in
                let threshold = Float(i + 1) / 5.0
                let on = level >= threshold * 0.55
                let c = i > 3 ? Color.red : (i > 2 ? Color.orange : Color.green)
                RoundedRectangle(cornerRadius: 1.5)
                    .fill(on ? c : Color.black.opacity(0.08))
                    .frame(maxHeight: .infinity)
            }
        }
        .padding(.horizontal, 4)
        .padding(.vertical, 4)
        .background(Color.black.opacity(0.05))
        .cornerRadius(6)
    }
}

// MARK: - Recording Settings Popover (functional)

struct RecordingSettingsPopover: View {
    @AppStorage("studio.codec") private var codecRaw = StudioCodec.photonHQ.rawValue
    @AppStorage("studio.input.deviceUID") private var deviceUID = ""
    @AppStorage("studio.monitoring") private var monitoring = false
    let codecLabel: String
    @ObservedObject var photonEngine: PhotonEngineService
    @ObservedObject var recorder: AudioRecorderService
    @ObservedObject var inputDevices: InputDeviceService

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Recording Settings")
                .font(.system(size: 13, weight: .bold))

            // ── INPUT DEVICE ──
            VStack(alignment: .leading, spacing: 8) {
                Text("INPUT DEVICE")
                    .font(.system(size: 9, weight: .heavy))
                    .tracking(1.0)
                    .foregroundColor(.black.opacity(0.4))

                Picker("", selection: $deviceUID) {
                    Text("System Default")
                        .font(.system(size: 12))
                        .tag("")
                    ForEach(inputDevices.devices) { dev in
                        Text("\(dev.displayName) (\(dev.channelLabel))")
                            .font(.system(size: 12))
                            .tag(dev.uid)
                    }
                }
                .labelsHidden()
                .pickerStyle(.menu)
                .frame(maxWidth: .infinity)

                if let sel = inputDevices.device(uid: deviceUID) {
                    Text("\(sel.channelLabel) input · sensing radar stays on the system default")
                        .font(.system(size: 9))
                        .foregroundColor(.black.opacity(0.35))
                }

                Toggle(isOn: $monitoring) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Monitor input while recording")
                            .font(.system(size: 12))
                            .foregroundColor(.black.opacity(0.8))
                        Text("Hear yourself through the speakers — use headphones to avoid feedback")
                            .font(.system(size: 10))
                            .foregroundColor(.black.opacity(0.4))
                    }
                }
                .toggleStyle(SwitchToggleStyle(tint: .blue))
                .onChange(of: monitoring) { rec in recorder.setMonitoring(rec) }
            }

            Divider()

            VStack(alignment: .leading, spacing: 8) {
                Text("COMPRESSION")
                    .font(.system(size: 9, weight: .heavy))
                    .tracking(1.0)
                    .foregroundColor(.black.opacity(0.4))

                ForEach(StudioCodec.allCases) { codec in
                    Button(action: { codecRaw = codec.rawValue }) {
                        HStack(spacing: 8) {
                            Image(systemName: codecRaw == codec.rawValue ? "largecircle.fill.circle" : "circle")
                                .font(.system(size: 12))
                                .foregroundColor(codecRaw == codec.rawValue ? .blue : .black.opacity(0.3))
                            Text(codec.label)
                                .font(.system(size: 12))
                                .foregroundColor(.black.opacity(0.8))
                            Spacer()
                        }
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(PlainButtonStyle())
                }
            }

            Divider()

            Toggle(isOn: Binding(
                get: { photonEngine.isChirpEnabled },
                set: { photonEngine.setChirpEnabled($0) }
            )) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Sonar chirp emission")
                        .font(.system(size: 12))
                        .foregroundColor(.black.opacity(0.8))
                    Text("Ultrasonic 18–22 kHz pings for distance sensing")
                        .font(.system(size: 10))
                        .foregroundColor(.black.opacity(0.4))
                }
            }
            .toggleStyle(SwitchToggleStyle(tint: .blue))
            .disabled(photonEngine.state != .running)

            Divider()

            HStack {
                Text("FORMAT")
                    .font(.system(size: 9, weight: .heavy))
                    .tracking(1.0)
                    .foregroundColor(.black.opacity(0.4))
                Spacer()
                Text("QTSQ • \(codecLabel)")
                    .font(.system(size: 11, weight: .semibold, design: .monospaced))
                    .foregroundColor(.black.opacity(0.7))
            }
        }
        .padding(16)
        .frame(width: 300)
    }
}

// MARK: - Metronome Popover (functional)

struct MetronomePopover: View {
    @AppStorage("studio.metronome.on") private var metronomeOn = false
    @AppStorage("studio.metronome.bpm") private var metronomeBPM: Double = 120

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Click Track")
                .font(.system(size: 13, weight: .bold))

            Toggle(isOn: $metronomeOn) {
                Text("Enable click while recording")
                    .font(.system(size: 12))
            }
            .toggleStyle(SwitchToggleStyle(tint: .orange))

            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text("TEMPO")
                        .font(.system(size: 9, weight: .heavy))
                        .tracking(1.0)
                        .foregroundColor(.black.opacity(0.4))
                    Spacer()
                    Text("\(Int(metronomeBPM)) BPM")
                        .font(.system(size: 12, weight: .bold, design: .monospaced))
                        .foregroundColor(metronomeOn ? .orange : .black.opacity(0.5))
                }
                Slider(value: $metronomeBPM, in: 60...240, step: 1)
                    .accentColor(.orange)
                    .disabled(!metronomeOn)
            }

            Text("Applies to the next recording.")
                .font(.system(size: 10))
                .foregroundColor(.black.opacity(0.4))
        }
        .padding(16)
        .frame(width: 240)
    }
}

// MARK: - Session Takes Sheet (functional)

struct TakesSheetView: View {
    @Environment(\.presentationMode) private var presentationMode
    let takes: [SessionTake]
    let revealInFinder: (SessionTake) -> Void
    let playTake: (SessionTake) -> Void
    let exportTake: (SessionTake) -> Void
    let renameTake: (SessionTake, String) -> Void
    let deleteTake: (SessionTake) -> Void
    @ObservedObject var playback: TakePlaybackController

    private var summaryDuration: String {
        let total = Int(takes.reduce(0) { $0 + $1.duration }.rounded())
        return String(format: "%d:%02d", total / 60, total % 60)
    }
    private var summaryBytes: Int64 {
        takes.reduce(0) { $0 + Int64($1.size) }
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Label("Session Takes", systemImage: "list.bullet.rectangle")
                    .font(.system(size: 15, weight: .bold))
                Spacer()
                Button("Done") { presentationMode.wrappedValue.dismiss() }
                    .buttonStyle(.borderedProminent)
            }
            .padding(16)

            if !takes.isEmpty {
                HStack(spacing: 4) {
                    Text("\(takes.count) take\(takes.count == 1 ? "" : "s") · \(summaryDuration) total · \(ByteCountFormatter.string(fromByteCount: summaryBytes, countStyle: .file))")
                        .font(.system(size: 10, weight: .medium, design: .monospaced))
                        .foregroundColor(.black.opacity(0.45))
                    Spacer()
                }
                .padding(.horizontal, 16)
                .padding(.bottom, 8)
            }

            Divider()

            if takes.isEmpty {
                VStack(spacing: 10) {
                    Image(systemName: "waveform")
                        .font(.system(size: 30, weight: .light))
                        .foregroundColor(.gray)
                    Text("No recordings yet this session")
                        .font(.system(size: 13))
                        .foregroundColor(.gray)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ScrollView {
                    VStack(spacing: 8) {
                        ForEach(takes) { take in
                            TakeRow(take: take,
                                    isPlayingThis: playback.isPlaying && playback.loadedPath == take.url.path,
                                    onPlay: { playTake(take) },
                                    revealInFinder: { revealInFinder(take) },
                                    onExport: { exportTake(take) },
                                    onRename: { newName in renameTake(take, newName) },
                                    onDelete: { deleteTake(take) })
                        }
                    }
                    .padding(16)
                }
            }
        }
        .frame(width: 560, height: 440)
    }
}

struct TakeRow: View {
    let take: SessionTake
    let isPlayingThis: Bool
    let onPlay: () -> Void
    let revealInFinder: () -> Void
    let onExport: () -> Void
    let onRename: (String) -> Void
    let onDelete: () -> Void

    @State private var isRenaming = false
    @State private var renameText = ""

    var body: some View {
        HStack(spacing: 12) {
            // Play / Pause this take
            Button(action: onPlay) {
                Image(systemName: isPlayingThis ? "pause.circle.fill" : "play.circle.fill")
                    .font(.system(size: 22))
                    .foregroundColor(isPlayingThis ? .blue : .black.opacity(0.45))
            }
            .buttonStyle(PlainButtonStyle())
            .help(isPlayingThis ? "Pause" : "Play take")

            VStack(alignment: .leading, spacing: 2) {
                if isRenaming {
                    TextField("Take name", text: $renameText, onCommit: {
                        let trimmed = renameText.trimmingCharacters(in: .whitespaces)
                        if !trimmed.isEmpty { onRename(trimmed) }
                        isRenaming = false
                    })
                    .textFieldStyle(.roundedBorder)
                    .font(.system(size: 13, weight: .semibold))
                } else {
                    Text(take.name)
                        .font(.system(size: 13, weight: .semibold))
                        .lineLimit(1)
                }
                Text("\(take.codec) • \(String(format: "%.1f s", take.duration)) • \(ByteCountFormatter.string(fromByteCount: Int64(take.size), countStyle: .file))")
                    .font(.system(size: 11))
                    .foregroundColor(.secondary)
            }

            Spacer()

            if isRenaming {
                Button("Save") {
                    let trimmed = renameText.trimmingCharacters(in: .whitespaces)
                    if !trimmed.isEmpty { onRename(trimmed) }
                    isRenaming = false
                }
                .controlSize(.small)
                Button("Cancel") { isRenaming = false }
                    .controlSize(.small)
            } else {
                Button(action: onExport) {
                    Image(systemName: "square.and.arrow.up")
                        .font(.system(size: 12))
                }
                .controlSize(.small)
                .help("Export as WAV")

                Menu {
                    Button("Rename…") {
                        renameText = take.name
                        isRenaming = true
                    }
                    Button("Reveal in Finder", action: revealInFinder)
                    Divider()
                    Button("Delete", role: .destructive, action: onDelete)
                } label: {
                    Image(systemName: "ellipsis.circle")
                        .font(.system(size: 13))
                }
                .menuStyle(.borderlessButton)
                .frame(width: 26)
                .help("Take actions")
            }
        }
        .padding(10)
        .background(isPlayingThis ? Color.blue.opacity(0.06) : Color.white)
        .cornerRadius(8)
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(isPlayingThis ? Color.blue.opacity(0.4) : Color.black.opacity(0.06), lineWidth: 1))
    }
}

// MARK: - Last-Take Playback Controller

/// Plays takes from memory (int16 WAV preview) or from disk (.qtsq file
/// decoded via the C library). Supports seek, loop, and progress
/// reporting. One shared instance so a row play and dock play never
/// double-play.
final class TakePlaybackController: NSObject, ObservableObject, AVAudioPlayerDelegate {
    @Published var isPlaying = false
    @Published var progress: Double = 0          // 0...1
    @Published var loopEnabled = false
    /// Decoded atoms of the currently-loaded Photon take (for the
    /// spatial mini-visualization during playback).
    @Published var atoms: [PhotonAtomInfo] = []
    @Published var atomTimeRange: (start: Double, end: Double) = (0, 0)

    private var player: AVAudioPlayer?
    private var progressTimer: Timer?

    /// Identifier of the loaded item ("memory", or a file path) so a
    /// toggle on a different item restarts cleanly, and so the takes
    /// list can highlight the playing row.
    private(set) var loadedPath: String? = nil

    func toggle(take: RecordedTake?) {
        if isPlaying {
            player?.pause()
            isPlaying = false
            return
        }
        guard let take = take else { return }

        // Rebuild the player each time so Play always restarts the take.
        let wav = Self.wavFromTake(take)
        playWavData(wav, sampleRate: take.encodedChannels > 0 ? Int(take.encodedSampleRate) : Int(take.sampleRate), key: "memory")
    }

    /// Plays a saved .qtsq take straight from disk. Decodes on a
    /// background thread (device-encrypted takes unlock silently with
    /// this Mac's key, same policy as the vault viewer), then plays.
    func playFile(url: URL, onFail: @escaping (String) -> Void) {
        if isPlaying, loadedPath == url.path {
            player?.pause()
            isPlaying = false
            return
        }
        stop()

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            var ctx = qtsq_context_t()
            guard qtsq_read(&ctx, url.path) == QTSQ_OK else {
                DispatchQueue.main.async { onFail("Could not read \(url.lastPathComponent)") }
                return
            }

            // Device-encrypted takes unlock with this Mac's key (the
            // password policy of the vault viewer).
            if qtsq_is_encrypted(&ctx) != 0,
               ctx.security.encryption_type == 2,
               DeviceKeyStore.shared.isAvailable {
                _ = DeviceKeyStore.shared.decryptWithDeviceKey(&ctx)
            }

            // Gravitational takes decode to a complete WAV (RIFF) —
            // play that directly; qtsq_decompress_audio doesn't know
            // this strategy.
            if ctx.header.strategy == 0xFF {
                var wavOut: UnsafeMutablePointer<UInt8>? = nil
                var wavLen: Int = 0
                let gRes = qtsq_decompress_gravitational_audio(&ctx, &wavOut, &wavLen)
                var wavData: Data? = nil
                if gRes == QTSQ_OK, let buf = wavOut, wavLen > 0 {
                    wavData = Data(bytes: buf, count: wavLen)
                    free(buf)
                } else if wavOut != nil {
                    free(wavOut)
                }
                DispatchQueue.main.async { [weak self] in
                    qtsq_free(&ctx)
                    guard let wav = wavData else {
                        onFail("Could not decode \(url.lastPathComponent)")
                        return
                    }
                    self?.atoms = []
                    self?.atomTimeRange = (0, 0)
                    self?.playWavData(wav, sampleRate: 0, key: url.path)
                }
                return
            }

            var pcm16: UnsafeMutablePointer<Int16>? = nil
            var samples: Int = 0
            var rate: UInt32 = 0
            var ch: UInt8 = 0
            let decRes = qtsq_decompress_audio(&ctx, &pcm16, &samples, &rate, &ch)

            // Spatial atoms for the Photon mini-visualization.
            var atomInfos: [PhotonAtomInfo] = []
            var atomRange: (Double, Double) = (0, 0)
            if decRes == QTSQ_OK, ctx.header.strategy == 0x08 {
                var floatPCM: UnsafeMutablePointer<Float>? = nil
                var totalSamples: Int = 0
                var rawAtoms: UnsafeMutablePointer<photon_atom_t>? = nil
                var atomCount: Int = 0
                if qtsq_decompress_audio_hq(&ctx, &floatPCM, &totalSamples, &rawAtoms, &atomCount) == QTSQ_OK,
                   let atomsPtr = rawAtoms, let fl = floatPCM, totalSamples > 0, ch > 0 {
                    let sampleRate = rate > 0 ? Double(rate) : 44100.0
                    let duration = Double(totalSamples) / sampleRate / Double(ch)
                    atomRange = (0, max(duration, 0.1))
                    atomsPtr.withMemoryRebound(to: photon_atom_t.self, capacity: atomCount) { bound in
                        atomInfos = (0..<atomCount).map { i in
                            let a = bound[i]
                            return PhotonAtomInfo(
                                freq: Double(a.freq_hz),
                                magnitude: Double(a.magnitude),
                                pan: Double(a.x),
                                time: Double(a.frame_index) * 512.0 / sampleRate)
                        }
                    }
                    free(fl)
                    // rawAtoms freed below after copy
                    free(rawAtoms)
                }
            }

            DispatchQueue.main.async { [weak self] in
                qtsq_free(&ctx)
                guard decRes == QTSQ_OK, let buf = pcm16, samples > 0, ch > 0 else {
                    if pcm16 != nil { free(pcm16) }
                    onFail("Could not decode \(url.lastPathComponent)")
                    return
                }
                let data = Data(bytes: buf, count: samples * MemoryLayout<Int16>.size)
                free(buf)

                let wav = makeWav(
                    pcm: data, sampleRate: Int(rate > 0 ? rate : 44100),
                    channels: Int(ch), bitsPerSample: 16)
                self?.atoms = atomInfos
                self?.atomTimeRange = atomRange
                self?.playWavData(wav, sampleRate: Int(rate > 0 ? rate : 44100), key: url.path)
            }
        }
    }

    private func playWavData(_ wav: Data, sampleRate: Int, key: String) {
        guard let p = try? AVAudioPlayer(data: wav) else { return }
        p.delegate = self
        p.numberOfLoops = loopEnabled ? -1 : 0
        p.prepareToPlay()
        p.play()
        player = p
        loadedPath = key
        isPlaying = true
        startProgressTimer()
    }

    func seek(to fraction: Double) {
        guard let p = player else { return }
        let clamped = max(0, min(1, fraction))
        p.currentTime = clamped * p.duration
        progress = clamped
    }

    func setLoop(_ on: Bool) {
        loopEnabled = on
        player?.numberOfLoops = on ? -1 : 0
    }

    func stop() {
        progressTimer?.invalidate()
        progressTimer = nil
        player?.stop()
        player = nil
        isPlaying = false
        progress = 0
        atoms = []
        atomTimeRange = (0, 0)
        loadedPath = nil
    }

    private func startProgressTimer() {
        progressTimer?.invalidate()
        progressTimer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { [weak self] _ in
            guard let self = self, let p = self.player else { return }
            if p.duration > 0 {
                self.progress = p.currentTime / p.duration
            }
        }
    }

    func audioPlayerDidFinishPlaying(_ player: AVAudioPlayer, successfully flag: Bool) {
        DispatchQueue.main.async { self.isPlaying = false }
    }

    /// Wraps interleaved float32 PCM into an int16 WAV container.
    /// Prefers the codec round-trip preview (decoded from the encoded
    /// context) when present, so Play previews what was actually saved.
    static func wavFromTake(_ take: RecordedTake) -> Data {
        if let encoded = take.encodedPCM, take.encodedChannels > 0 {
            return makeWav(pcm: encoded,
                                                 sampleRate: Int(take.encodedSampleRate),
                                                 channels: take.encodedChannels,
                                                 bitsPerSample: 16)
        }
        let samples = take.pcm.withUnsafeBytes { raw -> [Float] in
            raw.bindMemory(to: Float.self).map { $0 }
        }
        var s16 = Data(capacity: samples.count * 2)
        for v in samples {
            let clamped = max(-1.0, min(1.0, v))
            var value = Int16(clamped * 32767.0).littleEndian
            withUnsafeBytes(of: &value) { s16.append(contentsOf: $0) }
        }
        return makeWav(pcm: s16,
                                             sampleRate: Int(take.sampleRate),
                                             channels: take.channels,
                                             bitsPerSample: 16)
    }
}

/// Lightweight mirror of photon_atom_t for SwiftUI visualization.
struct PhotonAtomInfo: Identifiable {
    let id = UUID()
    let freq: Double        // Hz
    let magnitude: Double   // raw amplitude
    let pan: Double         // -1...1
    let time: Double        // seconds into the take
}
