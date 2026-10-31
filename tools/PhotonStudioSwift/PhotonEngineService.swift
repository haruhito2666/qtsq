/*
 * Quantum Tensor Sequence — Photon Engine Service (Swift ↔ C Bridge)
 *
 * Manages the lifecycle of the C-native sensing pipeline:
 *   1. Permission Requests (Mic, BLE, Location)
 *   2. Core Audio engine start/stop
 *   3. Real-time sensing loop (sonar → TDOA → fusion)
 *   4. Publishing fused spatial data for SwiftUI observation
 *
 * Architecture:
 *   SwiftUI (ObservableObject) ←— PhotonEngineService ←— C Engine
 *                                       ↓
 *                              GCD Timer (10 Hz sensing loop)
 *                                       ↓
 *                          photon_coreaudio_get_frame()
 *                              → sonar_detect_echoes()
 *                              → tdoa_locate_sources()
 *                              → wifi/ble scan()
 *                              → fusion_update()
 *                              → fusion_get_sources()
 *                                       ↓
 *                              @Published trackedSources
 *
 * Author: Priyanshu Boruah / QTSQ Project
 */

import Foundation
import SwiftUI
import AVFoundation
import CoreBluetooth

// MARK: - Tracked Source (SwiftUI-observable model)

/// A spatial source tracked by the Photon engine,
/// bridged from the C `photon_source_t` struct.
struct PhotonTrackedSource: Identifiable, Equatable {
    let id: UInt32           // Persistent tracking ID from Kalman filter
    var x: CGFloat           // Room-space X position (meters)
    var y: CGFloat           // Room-space Y position (meters)
    var z: CGFloat           // Room-space Z position (meters)
    var confidence: Double   // 0.0 – 1.0
    var freqLo: Float        // Dominant frequency band low (Hz)
    var freqHi: Float        // Dominant frequency band high (Hz)
    var lastSeenUs: UInt64   // Microsecond timestamp

    /// Human-readable frequency label
    var freqLabel: String {
        if freqHi < 300 { return "Bass" }
        if freqHi < 2000 { return "\(Int(freqLo))–\(Int(freqHi)) Hz" }
        if freqHi < 8000 { return String(format: "%.1f kHz", freqHi / 1000.0) }
        return "Ultrasonic"
    }

    /// Icon for the source based on frequency
    var icon: String {
        if freqHi < 300 { return "hifispeaker.fill" }
        if freqHi < 2000 { return "guitars.fill" }
        if freqHi < 6000 { return "mic.fill" }
        return "waveform"
    }

    /// Color based on source ID for visual distinction
    var color: Color {
        let palette: [Color] = [
            .pink, .orange, .green, .cyan, .purple,
            .mint, .indigo, .yellow, .teal, .red
        ]
        return palette[Int(id) % palette.count]
    }
}

// MARK: - Engine State

enum PhotonEngineState: Equatable {
    case idle
    case requestingPermissions
    case starting
    case running
    case error(String)
    case stopped
}

// MARK: - Permission State

struct PhotonPermissions {
    var microphone: Bool = false
    var bluetooth: Bool = false
    var location: Bool = false // Not required; WiFi module disables gracefully
}

// MARK: - Engine Statistics

struct PhotonStats {
    var micCount: UInt32 = 0
    var sampleRate: Double = 0
    var framesProcessed: UInt64 = 0
    var sonarSources: UInt32 = 0
    var tdoaSources: UInt32 = 0
    var fusedTracks: UInt32 = 0
    var lastFrameTimeUs: UInt64 = 0
    var sensingFPS: Double = 0
}

// MARK: - BLE Sighting

/// A nearby Bluetooth device seen by the engine's radio scanner.
struct BLEBeacon: Identifiable, Equatable {
    let id: String          // Device name (or "unknown")
    let rssi: Int           // dBm (negative; closer to 0 = stronger)

    /// 0...1 strength for UI meters (−100 dBm → 0, −40 dBm → 1)
    var strength: Double {
        Double(max(-100, min(-40, rssi)) + 100) / 60.0
    }
}

// MARK: - PhotonEngineService

/// Main service class managing the C sensing engine lifecycle.
/// Published properties drive the SwiftUI radar display.
final class PhotonEngineService: ObservableObject {

    // MARK: Published State (drives UI)
    @Published var state: PhotonEngineState = .idle
    @Published var trackedSources: [PhotonTrackedSource] = []
    @Published var permissions: PhotonPermissions = PhotonPermissions()
    @Published var stats: PhotonStats = PhotonStats()
    @Published var isChirpEnabled: Bool = true
    /// Nearby Bluetooth devices seen during the latest sensing frame.
    @Published var bleBeacons: [BLEBeacon] = []
    /// Proximity-zone motion activity (0–1 per band) from the WiFi/BLE RSSI engine.
    @Published var zoneActivity: [Float] = []

    // MARK: C Engine State (private)
    private var config = photon_config_t()
    private var sonarState = photon_sonar_state_t()
    private var tdoaState = photon_tdoa_state_t()
    private var wifiState = photon_wifi_state_t()
    private var fusionState = photon_fusion_state_t()

    private var sensingTimer: DispatchSourceTimer?
    private let sensingQueue = DispatchQueue(label: "com.qtsq.photon.sensing", qos: .userInteractive)

    // Chirp scheduling
    private var chirpTimer: DispatchSourceTimer?
    private var chirpWaveform: UnsafePointer<Float>?
    private var chirpLength: UInt32 = 0

    /// Guards against double-freeing the C engine state
    /// (e.g. user toggles sensing off, then leaves Studio → deinit).
    private var cEnginesInitialized = false

    // FPS tracking
    private var lastFPSTimestamp: UInt64 = 0
    private var frameCountSinceLastFPS: UInt64 = 0

    // MARK: - Initialization

    init() {
        photon_config_default(&config)
    }

    // MARK: - Permission Requests

    /// Request microphone access. Must be granted before starting.
    func requestPermissions() {
        state = .requestingPermissions

        // Microphone
        AVCaptureDevice.requestAccess(for: .audio) { [weak self] granted in
            DispatchQueue.main.async {
                self?.permissions.microphone = granted
                if granted {
                    // BLE permissions are implicitly requested when CBCentralManager
                    // initializes in the Core Audio shim. We mark it as granted
                    // here since the C layer handles the delegate.
                    self?.permissions.bluetooth = true
                    self?.state = .idle
                } else {
                    self?.state = .error("Microphone access denied. Photon Studio requires mic access for spatial sensing.")
                }
            }
        }
    }

    // MARK: - Engine Lifecycle

    /// Start the full sensing pipeline.
    func start() {
        guard permissions.microphone else {
            state = .error("Microphone permission required.")
            return
        }

        sensingQueue.async { [self] in
            DispatchQueue.main.async { self.state = .starting }

            // Initialize C engines
            photon_config_default(&config)

            let sonarOk = photon_sonar_init(&sonarState, &config)
            let tdoaOk = photon_tdoa_init(&tdoaState, &config)
            let wifiOk = photon_wifi_init(&wifiState)
            let fusionOk = photon_fusion_init(&fusionState, &config)

            guard sonarOk == 0, tdoaOk == 0, wifiOk == 0, fusionOk == 0 else {
                DispatchQueue.main.async {
                    self.state = .error("Failed to initialize sensing engines.")
                }
                return
            }
            cEnginesInitialized = true

            // Get chirp reference for emission
            var chirpLen: UInt32 = 0
            chirpWaveform = photon_sonar_get_chirp(&sonarState, &chirpLen)
            chirpLength = chirpLen

            // Start Core Audio
            let audioResult = photon_coreaudio_start(&config)
            guard audioResult == 0 else {
                DispatchQueue.main.async {
                    self.state = .error("Core Audio engine failed to start.")
                }
                return
            }

            // Read hardware info
            let micCount = photon_coreaudio_get_mic_count()
            let sampleRate = photon_coreaudio_get_sample_rate()

            DispatchQueue.main.async {
                self.stats.micCount = micCount
                self.stats.sampleRate = sampleRate
                self.state = .running
            }

            // Start sensing loop (10 Hz → every 100ms)
            startSensingLoop()

            // Start chirp emission loop
            if isChirpEnabled {
                startChirpLoop()
            }
        }
    }

    /// Stop the sensing pipeline and free all C resources.
    /// Safe to call any time while the object is alive.
    func stop() {
        teardownEngine()

        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.trackedSources = []
            self.bleBeacons = []
            self.stats = PhotonStats()
            self.state = .stopped
        }
    }

    /// Pure resource teardown: cancels timers, stops Core Audio, frees C state.
    /// Must NOT capture self in escaping closures and must NOT touch
    /// @Published properties — it is also called from deinit, where both
    /// are illegal (retaining a deallocating object aborts the process).
    private func teardownEngine() {
        sensingTimer?.cancel()
        sensingTimer = nil
        chirpTimer?.cancel()
        chirpTimer = nil

        photon_coreaudio_stop()

        if cEnginesInitialized {
            photon_sonar_free(&sonarState)
            photon_tdoa_free(&tdoaState)
            photon_wifi_free(&wifiState)
            photon_fusion_free(&fusionState)
            cEnginesInitialized = false
        }
    }

    deinit {
        teardownEngine()
    }

    // MARK: - Sensing Loop

    /// 10 Hz sensing loop that reads mic frames and runs the full pipeline.
    private func startSensingLoop() {
        sensingTimer = DispatchSource.makeTimerSource(queue: sensingQueue)
        sensingTimer?.schedule(deadline: .now(), repeating: .milliseconds(100))
        sensingTimer?.setEventHandler { [weak self] in
            self?.processSensingFrame()
        }
        sensingTimer?.resume()
    }

    /// Single frame of the sensing pipeline.
    private func processSensingFrame() {
        // 1. Get audio frame from Core Audio ring buffer
        var frame = photon_audio_frame_t()
        let frameResult = photon_coreaudio_get_frame(&frame)

        // Sonar sources
        var sonarSources = [photon_source_t](repeating: photon_source_t(), count: Int(PHOTON_MAX_SOURCES))
        var numSonar: UInt32 = 0

        // TDOA sources
        var tdoaSources = [photon_source_t](repeating: photon_source_t(), count: Int(PHOTON_MAX_SOURCES))
        var numTdoa: UInt32 = 0

        if frameResult == 0 {
            // 2. Run Sonar echo detection on first mic channel
            if let micData = frame.channels.0, frame.num_samples > 0 {
                numSonar = photon_sonar_detect_echoes(
                    &sonarState, micData, frame.num_samples,
                    &sonarSources, UInt32(PHOTON_MAX_SOURCES)
                )

                // Subtract chirp from audio (clean the recording)
                photon_sonar_subtract_chirp(&sonarState, micData, frame.num_samples)
            }

            // 3. Run TDOA source localization (needs multi-mic frame)
            if frame.num_channels >= 2 {
                numTdoa = photon_tdoa_locate_sources(
                    &tdoaState, &frame,
                    &tdoaSources, UInt32(PHOTON_MAX_SOURCES)
                )
            }

            // Free the frame buffers
            photon_coreaudio_free_frame(&frame)
        }

        // 4. WiFi/BLE RF scanning
        var rfObs = [photon_rf_observation_t](repeating: photon_rf_observation_t(), count: Int(PHOTON_MAX_RF_OBS))
        var numRF: Int32 = 0

        // Scan BLE (always available without extra permissions)
        numRF = photon_coreaudio_scan_ble(&rfObs, UInt32(PHOTON_MAX_RF_OBS))

        // Convert sightings for the Radio Environment panel (dedupe by name,
        // keep strongest RSSI). The C layer clears its list after this read.
        var beacons: [BLEBeacon] = []
        var bestRSSI: [String: Int] = [:]
        for i in 0..<Int(numRF) {
            let obs = rfObs[i]
            let name = withUnsafeBytes(of: obs.identifier) { raw -> String in
                let base = raw.baseAddress!.assumingMemoryBound(to: CChar.self)
                return String(cString: base)
            }
            let rssi = Int(obs.rssi_dbm)
            if let existing = bestRSSI[name] {
                bestRSSI[name] = max(existing, rssi)
            } else {
                bestRSSI[name] = rssi
            }
        }
        for (name, rssi) in bestRSSI.sorted(by: { $0.value > $1.value }) {
            beacons.append(BLEBeacon(id: name, rssi: rssi))
        }

        // Feed WiFi/BLE into tracker
        if numRF > 0 {
            photon_wifi_update(&wifiState, &rfObs, UInt32(numRF))
        }

        // Get zone activity
        var zoneActivity = [Float](repeating: 0, count: Int(PHOTON_MAX_ZONES))
        var numZones: UInt32 = 0
        photon_wifi_get_zones(&wifiState, &zoneActivity, &numZones)

        // 5. Sensor Fusion
        photon_fusion_update(
            &fusionState,
            numSonar > 0 ? sonarSources : nil, numSonar,
            numTdoa > 0 ? tdoaSources : nil, numTdoa,
            numZones > 0 ? zoneActivity : nil, numZones,
            numRF > 0 ? rfObs : nil, UInt32(numRF)
        )

        // 6. Read fused output
        var fusedSources = [photon_source_t](repeating: photon_source_t(), count: Int(PHOTON_MAX_SOURCES))
        let numFused = photon_fusion_get_sources(&fusionState, &fusedSources, UInt32(PHOTON_MAX_SOURCES))

        // 7. Convert C structs to Swift models
        var swiftSources: [PhotonTrackedSource] = []
        for i in 0..<Int(numFused) {
            let src = fusedSources[i]
            swiftSources.append(PhotonTrackedSource(
                id: src.source_id,
                x: CGFloat(src.x),
                y: CGFloat(src.y),
                z: CGFloat(src.z),
                confidence: Double(src.confidence),
                freqLo: src.freq_lo,
                freqHi: src.freq_hi,
                lastSeenUs: src.last_seen_us
            ))
        }

        // 8. FPS calculation
        let now = UInt64(Date().timeIntervalSince1970 * 1e6)
        frameCountSinceLastFPS += 1
        if now - lastFPSTimestamp > 1_000_000 { // Every second
            let fps = Double(frameCountSinceLastFPS) / (Double(now - lastFPSTimestamp) / 1e6)
            lastFPSTimestamp = now
            frameCountSinceLastFPS = 0

            DispatchQueue.main.async {
                self.stats.sensingFPS = fps
            }
        }

        // 9. Publish to UI
        DispatchQueue.main.async {
            self.trackedSources = swiftSources
            self.bleBeacons = beacons
            self.zoneActivity = Array(zoneActivity.prefix(Int(numZones)))
            self.stats.framesProcessed += 1
            self.stats.sonarSources = numSonar
            self.stats.tdoaSources = numTdoa
            self.stats.fusedTracks = numFused
            self.stats.lastFrameTimeUs = now
        }
    }

    // MARK: - Chirp Emission

    /// Periodically emit ultrasonic chirps through the speaker for sonar ranging.
    private func startChirpLoop() {
        chirpTimer = DispatchSource.makeTimerSource(queue: sensingQueue)
        let intervalMs = Int(config.sonar_interval_ms)
        chirpTimer?.schedule(deadline: .now() + .milliseconds(intervalMs), repeating: .milliseconds(intervalMs))
        chirpTimer?.setEventHandler { [weak self] in
            guard let self = self,
                  let chirp = self.chirpWaveform,
                  self.chirpLength > 0,
                  self.isChirpEnabled else { return }
            photon_coreaudio_emit_chirp(chirp, self.chirpLength)
        }
        chirpTimer?.resume()
    }

    // MARK: - Configuration

    /// Update chirp emission on/off (user toggle in UI)
    func setChirpEnabled(_ enabled: Bool) {
        isChirpEnabled = enabled
        if enabled && state == .running {
            startChirpLoop()
        } else {
            chirpTimer?.cancel()
            chirpTimer = nil
        }
    }
}
