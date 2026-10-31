// Photon Studio — the standalone recording room.
// Studio Mode lifted out of QTSQStudio: this app is the studio, the whole
// studio, and nothing but the studio. Takes live in the same
// Application Support directory as the old embedded mode, so recordings
// made before the split keep working.

import SwiftUI
import AppKit

@main
struct PhotonStudioApp: App {
    var body: some Scene {
        WindowGroup {
            StudioModeView()
                .frame(minWidth: 950, minHeight: 650)
                .background(Color(red: 0.980, green: 0.980, blue: 0.972))  // VaultTheme.paperWhite
        }
        .windowStyle(HiddenTitleBarWindowStyle())
        .commands {
            CommandGroup(replacing: .appSettings) {
                Button("Photon Studio Settings…") {
                    NotificationCenter.default.post(name: .studioOpenSettings, object: nil)
                }
                .keyboardShortcut(",", modifiers: .command)
            }
        }
    }
}

/// Frosted-glass backdrop for the inspector sidebar (lifted from the
/// main app — the one chrome piece Studio Mode actually renders).
struct VisualEffectView: NSViewRepresentable {
    var material: NSVisualEffectView.Material
    var blendingMode: NSVisualEffectView.BlendingMode

    func makeNSView(context: Context) -> NSVisualEffectView {
        let view = NSVisualEffectView()
        view.material = material
        view.blendingMode = blendingMode
        view.state = .active
        return view
    }

    func updateNSView(_ nsView: NSVisualEffectView, context: Context) {
        nsView.material = material
        nsView.blendingMode = blendingMode
    }
}

/// Wraps raw PCM in a minimal WAV container — AVAudioPlayer cannot parse
/// headerless PCM. Same builder as the main app's AudioPlayerController.
/// makeWav, single-sourced here for the studio target.
func makeWav(pcm: Data, sampleRate: Int, channels: Int, bitsPerSample: Int) -> Data {
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
