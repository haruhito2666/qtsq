import SwiftUI

struct ContentView: View {
    @EnvironmentObject var engine: QtsqEngine
    @State private var selectedTab = 0
    @State private var showCamera = false
    @State private var showAudio = false
    @State private var showDecrypt = false
    @State private var viewerFilePath: String?

    var body: some View {
        ZStack {
            TabView(selection: $selectedTab) {
                // ── Home ──
                HomeView(
                    onCameraClick: { showCamera = true },
                    onAudioClick: { showAudio = true },
                    onConvertClick: { selectedTab = 3 },
                    onDecryptClick: { showDecrypt = true },
                    onFileClick: { path in viewerFilePath = path }
                )
                .tabItem {
                    Label("Home", systemImage: "house.fill")
                }
                .tag(0)

                // ── Library ──
                LibraryView(
                    onFileClick: { path in viewerFilePath = path }
                )
                .tabItem {
                    Label("Library", systemImage: "square.grid.2x2.fill")
                }
                .tag(1)

                // ── Camera (center) ──
                Color.clear
                    .tabItem {
                        Label("Camera", systemImage: "camera.fill")
                    }
                    .tag(2)

                // ── Convert ──
                ConvertView()
                    .tabItem {
                        Label("Convert", systemImage: "arrow.left.arrow.right")
                    }
                    .tag(3)

                // ── Profile ──
                ProfileView()
                    .tabItem {
                        Label("Profile", systemImage: "person.fill")
                    }
                    .tag(4)
            }
            .tint(QColor.accentPrimary)
            .onChange(of: selectedTab) { newValue in
                if newValue == 2 {
                    showCamera = true
                    // Reset to previous tab so the camera tab doesn't stay selected
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                        selectedTab = 0
                    }
                }
            }
        }
        .fullScreenCover(isPresented: $showCamera) {
            CameraView(
                onCaptured: { url in
                    showCamera = false
                    viewerFilePath = url.path
                },
                onClose: { showCamera = false }
            )
        }
        .fullScreenCover(isPresented: $showAudio) {
            AudioView(
                onRecorded: { url in
                    showAudio = false
                    viewerFilePath = url.path
                },
                onClose: { showAudio = false }
            )
        }
        .fullScreenCover(isPresented: $showDecrypt) {
            DecryptView(onClose: { showDecrypt = false })
        }
        .fullScreenCover(item: $viewerFilePath) { path in
            ViewerView(
                filePath: path,
                onClose: { viewerFilePath = nil }
            )
        }
    }
}

// Make String identifiable for .fullScreenCover(item:)
extension String: @retroactive Identifiable {
    public var id: String { self }
}
