import SwiftUI

// Phase 7: the container codec is a user-facing Settings preference. qtc is
// the default; zlib is an explicit legacy opt-out. Stored in UserDefaults via
// @AppStorage so every compress surface (CompressView) reads the same value.
enum VaultCodec: String, CaseIterable, Identifiable {
    case qtc = "qtc"
    case zlib = "zlib"
    var id: String { rawValue }
    var detail: String {
        switch self {
        case .qtc:  return "Singularity engine · smaller captures · default"
        case .zlib: return "Legacy engine · maximum compatibility"
        }
    }
}

struct SettingsView: View {
    @EnvironmentObject var appState: AppState
    @AppStorage("vault.codec") private var codec: VaultCodec = .qtc
    
    var body: some View {
        VStack(alignment: .leading, spacing: 24) {
            // Header
            HStack(spacing: 8) {
                Image(systemName: "gearshape")
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundColor(VaultTheme.emberOrange)
                
                Text("Settings & Specification")
                    .font(.system(size: 18, weight: .semibold))
                    .foregroundColor(VaultTheme.textPrimary)
            }
            .padding(.bottom, 4)
            
            // About Vault Section
            HStack(spacing: 20) {
                BrandRingMark(size: 64, lineWidth: 4)
                
                VStack(alignment: .leading, spacing: 4) {
                    Text("QTSQ Media Vault")
                        .font(.system(size: 18, weight: .bold))
                        .foregroundColor(VaultTheme.textPrimary)
                    
                    Text("Specification Version 1.0 · Quantum Tensor Sequence Format")
                        .font(.system(size: 12, design: .monospaced))
                        .foregroundColor(VaultTheme.textSecondary)
                    
                    Text("Architected by Priyanshu Boruah")
                        .font(.system(size: 12, weight: .medium))
                        .foregroundColor(VaultTheme.emberOrange)
                    
                    Text("Private-by-default media container for confidential workflows.")
                        .font(.system(size: 12))
                        .foregroundColor(VaultTheme.textSecondary)
                }
            }
            .padding(20)
            .frame(maxWidth: .infinity, alignment: .leading)
            .vaultCard(cornerRadius: 10)
            
            // Compression Codec Section
            VStack(alignment: .leading, spacing: 14) {
                Text("COMPRESSION CODEC")
                    .font(.system(size: 10, weight: .heavy))
                    .tracking(1.2)
                    .foregroundColor(VaultTheme.textSecondary)
                    .padding(.bottom, 4)
                
                Picker("Compression codec", selection: $codec) {
                    ForEach(VaultCodec.allCases) { c in
                        Text(c.rawValue).tag(c)
                    }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                
                Text(codec.detail)
                    .font(.system(size: 12, weight: .medium))
                    .foregroundColor(VaultTheme.textPrimary)
                
                Text("Applies to new compressions. Existing .qtsq files always decode by format detection, regardless of this setting.")
                    .font(.system(size: 11))
                    .foregroundColor(VaultTheme.textSecondary)
            }
            .padding(20)
            .frame(maxWidth: .infinity, alignment: .leading)
            .vaultCard(cornerRadius: 10)
            
            // Core Security & Compression Capabilities
            VStack(alignment: .leading, spacing: 14) {
                Text("CORE CAPABILITIES & PROTOCOLS")
                    .font(.system(size: 10, weight: .heavy))
                    .tracking(1.2)
                    .foregroundColor(VaultTheme.textSecondary)
                    .padding(.bottom, 4)
                
                CapabilityRow(icon: "archivebox", title: "Compression Engines", text: "Singularity, Horizon, Accretion, and Lensing tensor pipelines")
                CapabilityRow(icon: "photo.on.rectangle.angled", title: "Native Multi-Modal", text: "Lossless images, MP3-class audio, text corpus, and 4DGS splats")
                CapabilityRow(icon: "lock.shield", title: "Cryptographic Vault", text: "AES-256-GCM authenticated encryption with Argon2/PBKDF2 key derivation")
                CapabilityRow(icon: "cross.case", title: "Integrity & Parity", text: "SHA-256 validation with optional Reed-Solomon error correction")
                CapabilityRow(icon: "arrow.triangle.merge", title: "Wave Differential", text: "Bit-exact differential patches via gravitational waves (.qtsw)")
                CapabilityRow(icon: "network.slash", title: "Zero Metadata Guarantee", text: "All GPS, camera serials, and EXIF tags stripped on ingestion")
            }
            .padding(20)
            .frame(maxWidth: .infinity, alignment: .leading)
            .vaultCard(cornerRadius: 10)
            
            Spacer()
        }
        .padding(32)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(VaultTheme.paperWhite)
    }
}

struct CapabilityRow: View {
    let icon: String
    let title: String
    let text: String
    
    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: icon)
                .font(.system(size: 13))
                .foregroundColor(VaultTheme.emberOrange)
                .frame(width: 18)
                .padding(.top, 1)
            
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundColor(VaultTheme.textPrimary)
                
                Text(text)
                    .font(.system(size: 12))
                    .foregroundColor(VaultTheme.textSecondary)
            }
            
            Spacer()
        }
    }
}

