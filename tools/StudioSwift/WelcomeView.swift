import SwiftUI
import UniformTypeIdentifiers

struct WelcomeView: View {
    @EnvironmentObject var appState: AppState
    
    var body: some View {
        VStack(spacing: 0) {
            Spacer()
            
            // Centered Signature Brand Ring Mark
            BrandRingMark(size: 96, lineWidth: 5.5)
                .padding(.bottom, 28)
            
            // Promise Headline
            VStack(spacing: 8) {
                Text("Private media, provably yours")
                    .font(.system(size: 26, weight: .semibold, design: .default))
                    .foregroundColor(VaultTheme.textPrimary)
                
                Text("A private-by-default media vault for encrypted images, video, audio, text, and tensors.")
                    .font(.system(size: 13, weight: .regular))
                    .foregroundColor(VaultTheme.textSecondary)
                    .multilineTextAlignment(.center)
            }
            .padding(.bottom, 32)
            
            // Primary Ember Orange Action Button
            Button(action: openFileAction) {
                HStack(spacing: 8) {
                    Image(systemName: "folder.badge.plus")
                        .font(.system(size: 13, weight: .semibold))
                    Text("Open .qtsq file")
                        .font(.system(size: 13, weight: .semibold))
                }
                .foregroundColor(.white)
                .padding(.horizontal, 36)
                .padding(.vertical, 12)
                .background(
                    RoundedRectangle(cornerRadius: 22, style: .continuous)
                        .fill(VaultTheme.emberOrange)
                )
                .shadow(color: VaultTheme.emberOrange.opacity(0.35), radius: 8, x: 0, y: 3)
            }
            .buttonStyle(PlainButtonStyle())
            .padding(.bottom, 64)
            
            Spacer()
            
            // 4 Guarantee / Trust Reassurance Cards
            HStack(spacing: 14) {
                GuaranteeCard(
                    icon: "checkmark.shield",
                    title: "Zero metadata",
                    subtitle: "EXIF & GPS stripped on ingest"
                )
                
                GuaranteeCard(
                    icon: "lock.shield",
                    title: "Encrypted",
                    subtitle: "AES-256-GCM / Post-quantum"
                )
                
                GuaranteeCard(
                    icon: "network.slash",
                    title: "Local-only",
                    subtitle: "100% offline, zero telemetry"
                )
                
                GuaranteeCard(
                    icon: "waveform.badge.magnifyingglass",
                    title: "Anti-steganography",
                    subtitle: "Frequency domain checked"
                )
            }
            .padding(.horizontal, 32)
            .padding(.bottom, 32)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(VaultTheme.paperWhite)
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

// MARK: - Guarantee Card

struct GuaranteeCard: View {
    let icon: String
    let title: String
    let subtitle: String
    
    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: icon)
                .font(.system(size: 14, weight: .regular))
                .foregroundColor(VaultTheme.emberOrange)
                .frame(width: 18)
                .padding(.top, 2)
            
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundColor(VaultTheme.textPrimary)
                
                Text(subtitle)
                    .font(.system(size: 10, weight: .regular))
                    .foregroundColor(VaultTheme.textSecondary)
                    .lineLimit(2)
                    .fixedSize(horizontal: false, vertical: true)
            }
            
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .vaultCard(cornerRadius: 10)
    }
}



