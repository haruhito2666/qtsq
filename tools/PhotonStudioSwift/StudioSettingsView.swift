import SwiftUI

extension Notification.Name {
    /// App menu → "Photon Studio Settings…" (Cmd-,) → StudioModeView
    /// opens the settings sheet.
    static let studioOpenSettings = Notification.Name("studio.openSettings")
}

/// The app's settings page — the complete surface behind the dock popovers.
/// Every control binds the SAME @AppStorage keys the popovers write, so the
/// two surfaces can never drift. Live effects (monitoring) apply through the
/// recorder exactly like the recording popover does.
struct StudioSettingsView: View {
    @Environment(\.presentationMode) private var presentationMode

    @ObservedObject var recorder: AudioRecorderService
    @ObservedObject var inputDevices: InputDeviceService
    let takeCount: Int

    @AppStorage("studio.codec") private var codecRaw = StudioCodec.photonHQ.rawValue
    @AppStorage("studio.input.deviceUID") private var deviceUID = ""
    @AppStorage("studio.monitoring") private var monitoring = false
    @AppStorage("studio.metronome.on") private var metronomeOn = false
    @AppStorage("studio.metronome.bpm") private var metronomeBPM: Double = 120

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Label("Settings", systemImage: "gearshape")
                    .font(.system(size: 15, weight: .bold))
                Spacer()
                Button("Done") { presentationMode.wrappedValue.dismiss() }
                    .buttonStyle(.borderedProminent)
            }
            .padding(16)

            Divider()

            ScrollView {
                VStack(alignment: .leading, spacing: 0) {
                    inputSection
                    Divider().padding(.vertical, 14)
                    recordingDefaultsSection
                    Divider().padding(.vertical, 14)
                    storageSection
                    Divider().padding(.vertical, 14)
                    aboutSection
                }
                .padding(16)
            }
        }
        .frame(width: 460, height: 560)
    }

    // MARK: - INPUT

    private var inputSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            sectionLabel("INPUT")

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

            if deviceUID.isEmpty {
                if let def = inputDevices.defaultUID.flatMap({ inputDevices.device(uid: $0) }) {
                    Text("Recording from: \(def.displayName)")
                        .font(.system(size: 9))
                        .foregroundColor(.black.opacity(0.35))
                }
            } else if let sel = inputDevices.device(uid: deviceUID) {
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
    }

    // MARK: - RECORDING DEFAULTS

    private var recordingDefaultsSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            sectionLabel("RECORDING DEFAULTS")

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

            Divider().padding(.top, 4)

            Toggle(isOn: $metronomeOn) {
                Text("Click track while recording")
                    .font(.system(size: 12))
                    .foregroundColor(.black.opacity(0.8))
            }
            .toggleStyle(SwitchToggleStyle(tint: .orange))

            HStack {
                Text(String(format: "%d BPM", Int(metronomeBPM)))
                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                    .foregroundColor(metronomeOn ? .black.opacity(0.7) : .black.opacity(0.3))
                    .frame(width: 64, alignment: .leading)
                Slider(value: $metronomeBPM, in: 60...240, step: 1)
                    .disabled(!metronomeOn)
            }

            Text("Defaults applied to the next recording.")
                .font(.system(size: 9))
                .foregroundColor(.black.opacity(0.35))
        }
    }

    // MARK: - STORAGE

    private var storageSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            sectionLabel("STORAGE")

            Text(takesDirectory.path.replacingOccurrences(of: NSHomeDirectory(), with: "~"))
                .font(.system(size: 10, weight: .medium, design: .monospaced))
                .foregroundColor(.black.opacity(0.55))
                .lineLimit(1)
                .truncationMode(.middle)
                .help(takesDirectory.path)

            HStack {
                Text("\(takeCount) take\(takeCount == 1 ? "" : "s") this session · encrypted .qtsq")
                    .font(.system(size: 10))
                    .foregroundColor(.black.opacity(0.45))
                Spacer()
                Button {
                    NSWorkspace.shared.activateFileViewerSelecting([takesDirectory])
                } label: {
                    Label("Reveal Folder", systemImage: "folder")
                        .font(.system(size: 11, weight: .semibold))
                }
                .controlSize(.small)
            }
        }
    }

    // MARK: - ABOUT

    private var aboutSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            sectionLabel("ABOUT")

            HStack {
                Text("Photon Studio")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundColor(.black.opacity(0.8))
                Spacer()
                Text(versionLine)
                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                    .foregroundColor(.black.opacity(0.55))
            }

            Text("Photon sonar/TDOA spatial sensing · QTSQ take encryption (ML-KEM-768)")
                .font(.system(size: 9))
                .foregroundColor(.black.opacity(0.35))
        }
    }

    // MARK: - Helpers

    private var versionLine: String {
        let v = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "1.0"
        let b = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "1"
        return "\(v) (\(b))"
    }

    /// Same directory the takes JSON and device keys live in — shared with
    /// QTSQStudio by design.
    private var takesDirectory: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("QTSQ", isDirectory: true)
    }

    private func sectionLabel(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 9, weight: .heavy))
            .tracking(1.0)
            .foregroundColor(.black.opacity(0.4))
    }
}
