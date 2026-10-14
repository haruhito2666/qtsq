import SwiftUI
import AVFoundation

struct AudioView: View {
    @EnvironmentObject var engine: QtsqEngine
    var onRecorded: (URL) -> Void
    var onClose: () -> Void

    @State private var isRecording = false
    @State private var recordingDuration: TimeInterval = 0
    @State private var timer: Timer?
    @State private var audioRecorder: AVAudioRecorder?

    var body: some View {
        VStack(spacing: 0) {
            // ── Top Bar ──
            HStack {
                Button(action: {
                    stopRecording()
                    onClose()
                }) {
                    Image(systemName: "xmark")
                        .font(.system(size: QIconSize.lg, weight: .medium))
                        .foregroundColor(QColor.textPrimary)
                        .frame(width: 44, height: 44)
                        .background(QColor.surfaceCard)
                        .clipShape(Circle())
                }
                Spacer()
                Text("Audio Recorder")
                    .font(QFont.title())
                    .foregroundColor(QColor.textPrimary)
                Spacer()
                Color.clear.frame(width: 44, height: 44)
            }
            .padding(.horizontal, QSpacing.xl)
            .padding(.top, QSpacing.lg)

            Spacer()

            // ── Recording Visualization ──
            ZStack {
                Circle()
                    .fill(isRecording ? QColor.categoryRed.opacity(0.1) : QColor.surfaceCard)
                    .frame(width: 200, height: 200)
                Circle()
                    .fill(isRecording ? QColor.categoryRed.opacity(0.2) : QColor.accentPrimary.opacity(0.1))
                    .frame(width: 140, height: 140)
                Image(systemName: isRecording ? "stop.fill" : "mic.fill")
                    .font(.system(size: 56))
                    .foregroundColor(isRecording ? QColor.categoryRed : QColor.accentPrimary)
            }

            Spacer().frame(height: QSpacing.xxxl)

            // Timer
            Text(formatTime(recordingDuration))
                .font(QFont.display(48))
                .foregroundColor(isRecording ? QColor.categoryRed : QColor.textPrimary)
                .monospacedDigit()

            Spacer().frame(height: QSpacing.sm)

            Text(isRecording ? "Recording..." : "Direct to .qtsq")
                .font(QFont.bodyMedium())
                .foregroundColor(QColor.textSecondary)

            Spacer()

            // ── Controls ──
            HStack(spacing: QSpacing.xxl) {
                // Cancel
                Button(action: {
                    stopRecording()
                    onClose()
                }) {
                    Image(systemName: "xmark")
                        .font(.system(size: QIconSize.xl))
                        .foregroundColor(QColor.textSecondary)
                        .frame(width: 56, height: 56)
                        .background(QColor.surfaceCard)
                        .clipShape(Circle())
                }

                // Record / Stop
                Button(action: {
                    if isRecording {
                        finishRecording()
                    } else {
                        startRecording()
                    }
                }) {
                    ZStack {
                        Circle()
                            .stroke(isRecording ? QColor.categoryRed : QColor.accentPrimary, lineWidth: 4)
                            .frame(width: 80, height: 80)
                        if isRecording {
                            RoundedRectangle(cornerRadius: 6)
                                .fill(QColor.categoryRed)
                                .frame(width: 32, height: 32)
                        } else {
                            Circle()
                                .fill(QColor.categoryRed)
                                .frame(width: 64, height: 64)
                        }
                    }
                }
            }
            .padding(.bottom, 60)
        }
        .background(QColor.backgroundWhite)
    }

    private func formatTime(_ seconds: TimeInterval) -> String {
        let mins = Int(seconds) / 60
        let secs = Int(seconds) % 60
        return String(format: "%02d:%02d", mins, secs)
    }

    private func startRecording() {
        let session = AVAudioSession.sharedInstance()
        do {
            try session.setCategory(.playAndRecord, mode: .default)
            try session.setActive(true)
        } catch {
            return
        }

        let tempURL = FileManager.default.temporaryDirectory.appendingPathComponent("recording_\(UUID().uuidString).wav")
        let settings: [String: Any] = [
            AVFormatIDKey: Int(kAudioFormatLinearPCM),
            AVSampleRateKey: 44100,
            AVNumberOfChannelsKey: 1,
            AVLinearPCMBitDepthKey: 16,
            AVLinearPCMIsFloatKey: false
        ]

        do {
            audioRecorder = try AVAudioRecorder(url: tempURL, settings: settings)
            audioRecorder?.record()
            isRecording = true
            recordingDuration = 0
            timer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { _ in
                recordingDuration += 1
            }
        } catch {}
    }

    private func stopRecording() {
        audioRecorder?.stop()
        audioRecorder = nil
        timer?.invalidate()
        timer = nil
        isRecording = false
    }

    private func finishRecording() {
        guard let recorder = audioRecorder else { return }
        let tempURL = recorder.url
        stopRecording()

        // Compress the recorded WAV to QTSQ
        DispatchQueue.global(qos: .userInitiated).async {
            guard let wavData = try? Data(contentsOf: tempURL) else { return }

            // Skip WAV header (44 bytes) to get raw PCM
            let pcmData = wavData.count > 44 ? wavData.dropFirst(44) : wavData
            let pcmSamples = pcmData.withUnsafeBytes { buf -> [Int16] in
                let count = buf.count / 2
                return buf.bindMemory(to: Int16.self).prefix(count).map { $0 }
            }

            if let compressed = engine.compressAudio(pcm: pcmSamples, sampleRate: 44100, channels: 1) {
                let dir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
                let outURL = dir.appendingPathComponent("audio_\(Int(Date().timeIntervalSince1970)).qtsq")
                try? compressed.write(to: outURL)
                try? FileManager.default.removeItem(at: tempURL)

                DispatchQueue.main.async {
                    onRecorded(outURL)
                }
            }
        }
    }
}
