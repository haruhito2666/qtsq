import SwiftUI
import UniformTypeIdentifiers

struct SidebarView: View {
    @EnvironmentObject var appState: AppState
    @State private var hoveredItem: String? = nil
    
    var body: some View {
        VStack(spacing: 0) {
            // Window Header Brand Area
            HStack(spacing: 8) {
                Circle()
                    .stroke(VaultTheme.emberOrange, lineWidth: 2)
                    .frame(width: 14, height: 14)
                    .overlay(
                        Circle()
                            .fill(VaultTheme.emberOrange)
                            .frame(width: 6, height: 6)
                    )
                
                Text("QTSQ")
                    .font(.system(size: 13, weight: .black, design: .default))
                    .tracking(1.8)
                    .foregroundColor(VaultTheme.textPrimary)
                
                Spacer()
            }
            .padding(.horizontal, 18)
            .padding(.top, 18)
            .padding(.bottom, 14)
            
            Divider()
                .background(VaultTheme.border)
            
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    
                    // VAULT SECTION
                    VStack(alignment: .leading, spacing: 3) {
                        HStack {
                            Text("VAULT")
                                .font(.system(size: 10, weight: .heavy))
                                .tracking(1.4)
                                .foregroundColor(VaultTheme.textSecondary)
                            
                            Spacer()
                            
                            Button(action: openFileAction) {
                                Image(systemName: "plus")
                                    .font(.system(size: 11, weight: .bold))
                                    .foregroundColor(VaultTheme.textSecondary)
                            }
                            .buttonStyle(PlainButtonStyle())
                        }
                        .padding(.horizontal, 16)
                        .padding(.bottom, 4)
                        
                        if appState.files.isEmpty {
                            HStack(spacing: 8) {
                                Image(systemName: "tray")
                                    .font(.system(size: 11))
                                    .foregroundColor(VaultTheme.textTertiary)
                                Text("No open files")
                                    .font(.system(size: 11))
                                    .foregroundColor(VaultTheme.textTertiary)
                            }
                            .padding(.horizontal, 16)
                            .padding(.vertical, 8)
                        } else {
                            ForEach(appState.files) { file in
                                VaultFileRow(
                                    file: file,
                                    isSelected: isViewerSelected(id: file.id),
                                    action: { appState.currentPanel = .viewer(file.id) }
                                )
                            }
                        }
                    }
                    
                    // TOOLS SECTION
                    VStack(alignment: .leading, spacing: 3) {
                        Text("TOOLS")
                            .font(.system(size: 10, weight: .heavy))
                            .tracking(1.4)
                            .foregroundColor(VaultTheme.textSecondary)
                            .padding(.horizontal, 16)
                            .padding(.bottom, 4)
                        
                        VaultToolRow(
                            title: "Compress",
                            icon: "archivebox",
                            isSelected: appState.currentPanel == .compress,
                            action: { appState.currentPanel = .compress }
                        )
                        
                        VaultToolRow(
                            title: "Decompress",
                            icon: "arrow.up.doc",
                            isSelected: appState.currentPanel == .decompress,
                            action: { appState.currentPanel = .decompress }
                        )
                        
                        VaultToolRow(
                            title: "Diff & Patch",
                            icon: "arrow.triangle.merge",
                            isSelected: appState.currentPanel == .diff,
                            action: { appState.currentPanel = .diff }
                        )
                        
                        VaultToolRow(
                            title: "Settings",
                            icon: "gearshape",
                            isSelected: appState.currentPanel == .settings,
                            action: { appState.currentPanel = .settings }
                        )
                    }
                }
                .padding(.vertical, 14)
            }
            
            Divider()
                .background(VaultTheme.border)
            
            // SIDEBAR FOOTER PRIVACY STATUS STRIP
            VStack(alignment: .leading, spacing: 6) {
                HStack(spacing: 6) {
                    Circle()
                        .fill(VaultTheme.integrityGreen)
                        .frame(width: 5, height: 5)
                    Text("Local-only")
                        .font(.system(size: 10, weight: .medium))
                        .foregroundColor(VaultTheme.textSecondary)
                    
                    Text("·")
                        .foregroundColor(VaultTheme.textTertiary)
                    
                    Circle()
                        .fill(VaultTheme.integrityGreen)
                        .frame(width: 5, height: 5)
                    Text("Encrypted")
                        .font(.system(size: 10, weight: .medium))
                        .foregroundColor(VaultTheme.textSecondary)
                }
                
                HStack(spacing: 6) {
                    Circle()
                        .fill(VaultTheme.integrityGreen)
                        .frame(width: 5, height: 5)
                    Text("Zero metadata")
                        .font(.system(size: 10, weight: .medium))
                        .foregroundColor(VaultTheme.textSecondary)
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(VaultTheme.cardWhite.opacity(0.6))
        }
        .frame(minWidth: 220, idealWidth: 240, maxWidth: 260)
        .background(VaultTheme.sidebarBg)
    }
    
    private func isViewerSelected(id: UUID) -> Bool {
        if case .viewer(let currentId) = appState.currentPanel {
            return currentId == id
        }
        return false
    }
    
    private func openFileAction() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.allowedContentTypes = [UTType(filenameExtension: "qtsq")!]
        if panel.runModal() == .OK, let url = panel.url {
            appState.openFile(path: url.path)
        }
    }
}

// MARK: - Vault File Row

struct VaultFileRow: View {
    @ObservedObject var file: QTSQFile
    let isSelected: Bool
    let action: () -> Void
    
    @State private var isHovered = false
    
    var body: some View {
        Button(action: action) {
            HStack(spacing: 10) {
                // Type Glyph (SF Symbols, 1.5px stroke style)
                Image(systemName: glyphForFile())
                    .font(.system(size: 13, weight: .regular))
                    .foregroundColor(isSelected ? VaultTheme.emberOrange : VaultTheme.textSecondary)
                    .frame(width: 18)
                
                Text(file.name)
                    .font(.system(size: 12, weight: isSelected ? .semibold : .regular))
                    .foregroundColor(isSelected ? VaultTheme.textPrimary : VaultTheme.textPrimary.opacity(0.85))
                    .lineLimit(1)
                    .truncationMode(.middle)
                
                Spacer()
                
                // Mandatory Security Lock Glyph (Privacy by default)
                Image(systemName: "lock.fill")
                    .font(.system(size: 9))
                    .foregroundColor(file.isEncrypted ? VaultTheme.emberOrange : VaultTheme.textTertiary)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 7)
            .background(
                RoundedRectangle(cornerRadius: 7, style: .continuous)
                    .fill(isSelected ? VaultTheme.cardWhite : (isHovered ? VaultTheme.cardWhite.opacity(0.6) : Color.clear))
            )
            .overlay(
                RoundedRectangle(cornerRadius: 7, style: .continuous)
                    .stroke(isSelected ? VaultTheme.border.opacity(2.0) : Color.clear, lineWidth: 1)
            )
            .shadow(color: isSelected ? VaultTheme.cardShadowColor : .clear, radius: 3, y: 1)
        }
        .buttonStyle(PlainButtonStyle())
        .padding(.horizontal, 8)
        .onHover { isHovered = $0 }
    }
    
    private func glyphForFile() -> String {
        let type = qtsq_data_type_t(UInt32(file.ctx.header.data_type))
        if type == QTSQ_TYPE_IMAGE_GRAY || type == QTSQ_TYPE_IMAGE_RGB || type == QTSQ_TYPE_IMAGE_RGBA {
            return "photo"
        } else if type == QTSQ_TYPE_AUDIO_MONO || type == QTSQ_TYPE_AUDIO_STEREO || type == QTSQ_TYPE_AUDIO_MULTI {
            return "waveform"
        } else if type == QTSQ_TYPE_TEXT_ASCII || type == QTSQ_TYPE_TEXT_UTF8 || type == QTSQ_TYPE_JSON || type == QTSQ_TYPE_CSV {
            return "doc.text"
        } else if type == QTSQ_TYPE_TENSOR {
            return "cpu"
        } else if type == QTSQ_TYPE_VIDEO_FRAMES {
            return "film"
        } else if type == QTSQ_TYPE_CONTAINER {
            return "archivebox"
        }
        return "doc"
    }
}

// MARK: - Vault Tool Row

struct VaultToolRow: View {
    let title: String
    let icon: String
    let isSelected: Bool
    let action: () -> Void
    
    @State private var isHovered = false
    
    var body: some View {
        Button(action: action) {
            HStack(spacing: 10) {
                Image(systemName: icon)
                    .font(.system(size: 13, weight: .regular))
                    .foregroundColor(isSelected ? VaultTheme.emberOrange : VaultTheme.textSecondary)
                    .frame(width: 18)
                
                Text(title)
                    .font(.system(size: 12, weight: isSelected ? .semibold : .regular))
                    .foregroundColor(isSelected ? VaultTheme.textPrimary : VaultTheme.textPrimary.opacity(0.85))
                
                Spacer()
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 7)
            .background(
                RoundedRectangle(cornerRadius: 7, style: .continuous)
                    .fill(isSelected ? VaultTheme.cardWhite : (isHovered ? VaultTheme.cardWhite.opacity(0.6) : Color.clear))
            )
            .overlay(
                RoundedRectangle(cornerRadius: 7, style: .continuous)
                    .stroke(isSelected ? VaultTheme.border.opacity(2.0) : Color.clear, lineWidth: 1)
            )
            .shadow(color: isSelected ? VaultTheme.cardShadowColor : .clear, radius: 3, y: 1)
        }
        .buttonStyle(PlainButtonStyle())
        .padding(.horizontal, 8)
        .onHover { isHovered = $0 }
    }
}

