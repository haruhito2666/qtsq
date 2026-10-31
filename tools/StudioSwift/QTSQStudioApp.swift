import SwiftUI
import UniformTypeIdentifiers

@main
struct QTSQStudioApp: App {
    @StateObject private var appState = AppState()
    
    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(appState)
                .frame(minWidth: 950, minHeight: 650)
                .background(VaultTheme.paperWhite)
        }
        .windowStyle(HiddenTitleBarWindowStyle())
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("Open File...") {
                    openFilePanel()
                }
                .keyboardShortcut("O", modifiers: .command)
            }
        }
    }
    
    private func openFilePanel() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.allowedContentTypes = [UTType(filenameExtension: "qtsq")!]
        if panel.runModal() == .OK, let url = panel.url {
            appState.openFile(path: url.path)
        }
    }
}

struct ContentView: View {
    @EnvironmentObject var appState: AppState

    var body: some View {
        HStack(spacing: 0) {
            SidebarView()

            Rectangle()
                .fill(VaultTheme.border)
                .frame(width: 1)

            // Main Content Area (Paper White)
            MainContentView()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(VaultTheme.paperWhite)
        }
    }
}

// macOS Visual Effect View for Glassmorphism
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

struct MainContentView: View {
    @EnvironmentObject var appState: AppState
    
    var body: some View {
        switch appState.currentPanel {
        case .welcome:
            WelcomeView()
        case .viewer(let id):
            if let file = appState.files.first(where: { $0.id == id }) {
                ViewerView(file: file)
            } else {
                Text("File not found")
            }
        case .info(let id):
            if let file = appState.files.first(where: { $0.id == id }) {
                InfoView(file: file)
            }
        case .encrypt(let id):
            if let file = appState.files.first(where: { $0.id == id }) {
                EncryptView(file: file)
            }
        case .health(let id):
            if let file = appState.files.first(where: { $0.id == id }) {
                HealthView(file: file)
            }
        case .compress:
            CompressView()
        case .decompress:
            DecompressView()
        case .diff:
            DiffView()
        case .settings:
            SettingsView()
        }
    }
}
