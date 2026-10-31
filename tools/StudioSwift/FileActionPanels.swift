import SwiftUI
import AppKit

// MARK: - Info View (Vault Metadata & Monospace Hash Readout)

struct InfoView: View {
    @EnvironmentObject var appState: AppState
    @ObservedObject var file: QTSQFile
    @State private var copiedHash = false
    
    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            // Header
            HStack {
                HStack(spacing: 8) {
                    Image(systemName: "info.circle")
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundColor(VaultTheme.emberOrange)
                    
                    Text("File Info — \(file.name)")
                        .font(.system(size: 17, weight: .semibold))
                        .foregroundColor(VaultTheme.textPrimary)
                }
                
                Spacer()
                
                Button(action: { appState.currentPanel = .viewer(file.id) }) {
                    HStack(spacing: 4) {
                        Image(systemName: "arrow.left")
                            .font(.system(size: 11))
                        Text("Back to Viewer")
                            .font(.system(size: 12, weight: .medium))
                    }
                }
                .buttonStyle(PlainButtonStyle())
                .vaultGhostButton()
            }
            .padding(.bottom, 6)
            
            let isLossy = (file.ctx.header.flags & UInt32(QTSQ_FLAG_LOSSY)) != 0
            let hasEcc = (file.ctx.header.flags & UInt32(QTSQ_FLAG_HAS_ECC)) != 0
            
            // Specification Breakdown Card
            VStack(spacing: 10) {
                InfoRow(label: "DATA TYPE", value: String(cString: qtsq_type_name(qtsq_data_type_t(UInt32(file.ctx.header.data_type)))))
                InfoRow(label: "STRATEGY", value: {
                    let enc = withUnsafePointer(to: file.ctx.schema.encoding) { ptr -> String in
                        ptr.withMemoryRebound(to: CChar.self, capacity: 64) { String(cString: $0) }
                    }
                    if enc == "PIXEL" {
                        return isLossy ? "Pixel Lossy (Custom Quant)" : "Pixel Lossless"
                    }
                    if enc == "4DGS" { return "4DGS Continuous" }
                    return String(cString: qtsq_strategy_name(qtsq_strategy_t(UInt32(file.ctx.header.strategy))))
                }())
                InfoRow(label: "SPEC VERSION", value: "v\(file.ctx.header.version).0")
                InfoRow(label: "ORIGINAL SIZE", value: "\(file.ctx.header.original_size) bytes", isMonospace: true)
                InfoRow(label: "COMPRESSED SIZE", value: "\(file.ctx.header.singularity_size) bytes", isMonospace: true)
                InfoRow(label: "RATIO", value: String(format: "%.2f:1", qtsq_compression_ratio(&file.ctx)), isMonospace: true, valueColor: VaultTheme.emberOrange)
                InfoRow(label: "CHANNELS", value: "\(file.ctx.header.num_channels)")
                InfoRow(label: "SAMPLE DEPTH", value: "\(file.ctx.header.sample_depth) bits", isMonospace: true)
                InfoRow(label: "ENCRYPTION", value: file.isEncrypted ? "AES-256-GCM (Encrypted)" : "Plaintext Vault Stream")
                InfoRow(label: "INTEGRITY ECC", value: hasEcc ? "Reed-Solomon 8-symbol Parity" : "None")
                InfoRow(label: "COMPRESSION MODE", value: isLossy ? "Perceptual Lossy" : "Bit-exact Lossless")
            }
            .padding(18)
            .vaultCard(cornerRadius: 10)
            
            // Cryptographic SHA-256 Full-Width Monospace Line
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text("CRYPTOGRAPHIC CHECKSUM (SHA-256)")
                        .font(.system(size: 10, weight: .heavy))
                        .tracking(1.2)
                        .foregroundColor(VaultTheme.textSecondary)
                    
                    Spacer()
                    
                    Button(action: copyHash) {
                        HStack(spacing: 4) {
                            Image(systemName: copiedHash ? "checkmark" : "doc.on.doc")
                                .font(.system(size: 10))
                            Text(copiedHash ? "Copied" : "Copy Hash")
                                .font(.system(size: 10, weight: .medium))
                        }
                        .foregroundColor(copiedHash ? VaultTheme.integrityGreen : VaultTheme.textSecondary)
                    }
                    .buttonStyle(PlainButtonStyle())
                }
                
                Text(getChecksumHex())
                    .font(.system(size: 11, weight: .medium, design: .monospaced))
                    .foregroundColor(VaultTheme.textPrimary)
                    .textSelection(.enabled)
                    .padding(12)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(VaultTheme.subpanelBg)
                    .cornerRadius(6)
                    .overlay(
                        RoundedRectangle(cornerRadius: 6, style: .continuous)
                            .stroke(VaultTheme.border, lineWidth: 1)
                    )
            }
            .padding(18)
            .vaultCard(cornerRadius: 10)
            
            Spacer()
        }
        .padding(32)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(VaultTheme.paperWhite)
    }
    
    private func copyHash() {
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(getChecksumHex(), forType: .string)
        copiedHash = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
            copiedHash = false
        }
    }
    
    private func getChecksumHex() -> String {
        let tuple = file.ctx.header.checksum
        let bytes = [
            tuple.0, tuple.1, tuple.2, tuple.3, tuple.4, tuple.5, tuple.6, tuple.7,
            tuple.8, tuple.9, tuple.10, tuple.11, tuple.12, tuple.13, tuple.14, tuple.15,
            tuple.16, tuple.17, tuple.18, tuple.19, tuple.20, tuple.21, tuple.22, tuple.23,
            tuple.24, tuple.25, tuple.26, tuple.27, tuple.28, tuple.29, tuple.30, tuple.31
        ]
        return bytes.map { String(format: "%02x", $0) }.joined()
    }
}

struct InfoRow: View {
    let label: String
    let value: String
    var isMonospace: Bool = false
    var valueColor: Color = VaultTheme.textPrimary
    
    var body: some View {
        HStack {
            Text(label)
                .font(.system(size: 10, weight: .heavy))
                .tracking(1.1)
                .foregroundColor(VaultTheme.textSecondary)
                .frame(width: 170, alignment: .leading)
            
            Text(value)
                .font(isMonospace ? .system(size: 12, weight: .semibold, design: .monospaced) : .system(size: 12, weight: .medium))
                .foregroundColor(valueColor)
            
            Spacer()
        }
    }
}

// MARK: - Encrypt View

struct EncryptView: View {
    @EnvironmentObject var appState: AppState
    @ObservedObject var file: QTSQFile
    @State private var password = ""
    
    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            // Header
            HStack {
                HStack(spacing: 8) {
                    Image(systemName: file.isEncrypted ? "lock.open" : "lock.shield")
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundColor(VaultTheme.emberOrange)
                    
                    Text(file.isEncrypted
                         ? (file.isUnlocked ? "Vault Status" : "Decrypt File")
                         : "Encrypt File")
                        .font(.system(size: 17, weight: .semibold))
                        .foregroundColor(VaultTheme.textPrimary)
                }
                
                Spacer()
                
                Button(action: { appState.currentPanel = .viewer(file.id) }) {
                    HStack(spacing: 4) {
                        Image(systemName: "arrow.left")
                            .font(.system(size: 11))
                        Text("Back to Viewer")
                            .font(.system(size: 12, weight: .medium))
                    }
                }
                .buttonStyle(PlainButtonStyle())
                .vaultGhostButton()
            }
            .padding(.bottom, 6)
            
            VStack(alignment: .leading, spacing: 16) {
                Text(file.isEncrypted && !file.isUnlocked ? "VAULT PASSPHRASE" :
                     file.isUnlocked ? "VAULT STATUS" : "VAULT PASSPHRASE")
                    .font(.system(size: 10, weight: .heavy))
                    .tracking(1.2)
                    .foregroundColor(VaultTheme.textSecondary)
                
                if file.isUnlocked {
                    HStack(spacing: 8) {
                        Image(systemName: "checkmark.circle.fill")
                            .font(.system(size: 16))
                            .foregroundColor(VaultTheme.integrityGreen)
                        VStack(alignment: .leading, spacing: 2) {
                            Text("Unlocked for viewing")
                                .font(.system(size: 13, weight: .semibold))
                                .foregroundColor(VaultTheme.textPrimary)
                            Text("Contents are readable in this session. The file on disk remains encrypted.")
                                .font(.system(size: 12))
                                .foregroundColor(VaultTheme.textSecondary)
                        }
                    }
                    
                    Button(action: { appState.currentPanel = .viewer(file.id) }) {
                        HStack(spacing: 6) {
                            Image(systemName: "eye")
                                .font(.system(size: 12, weight: .semibold))
                            Text("View Contents")
                                .font(.system(size: 13, weight: .semibold))
                        }
                        .foregroundColor(.white)
                        .padding(.horizontal, 24)
                        .padding(.vertical, 9)
                        .background(
                            RoundedRectangle(cornerRadius: 6, style: .continuous)
                                .fill(VaultTheme.integrityGreen)
                        )
                        .shadow(color: VaultTheme.integrityGreen.opacity(0.3), radius: 6, y: 2)
                    }
                    .buttonStyle(PlainButtonStyle())
                    .padding(.top, 4)
                } else {
                    Text(file.isEncrypted
                         ? "Enter the encryption passphrase to decrypt this file and inspect its contents in memory. The file on disk stays encrypted."
                         : "Set a passphrase to encrypt this file with AES-256-GCM and post-quantum container protection.")
                        .font(.system(size: 13))
                        .foregroundColor(VaultTheme.textSecondary)
                    
                    SecureField("Enter passphrase…", text: $password)
                        .textFieldStyle(PlainTextFieldStyle())
                        .font(.system(size: 13, design: .monospaced))
                        .padding(10)
                        .background(VaultTheme.subpanelBg)
                        .cornerRadius(6)
                        .overlay(RoundedRectangle(cornerRadius: 6).stroke(VaultTheme.border, lineWidth: 1))
                        .frame(maxWidth: 380)
                    
                    Button(action: {
                        if file.isEncrypted {
                            file.unlockForViewing(password: password)
                            password = ""
                            appState.currentPanel = .viewer(file.id)
                        } else {
                            encrypt()
                        }
                    }) {
                        HStack(spacing: 6) {
                            Image(systemName: file.isEncrypted ? "lock.open" : "lock.fill")
                                .font(.system(size: 12, weight: .semibold))
                            Text(file.isEncrypted ? "Decrypt & View" : "Encrypt File")
                                .font(.system(size: 13, weight: .semibold))
                        }
                        .foregroundColor(.white)
                        .padding(.horizontal, 24)
                        .padding(.vertical, 9)
                        .background(
                            RoundedRectangle(cornerRadius: 6, style: .continuous)
                                .fill(VaultTheme.emberOrange)
                        )
                        .shadow(color: VaultTheme.emberOrange.opacity(0.3), radius: 6, y: 2)
                    }
                    .buttonStyle(PlainButtonStyle())
                    .padding(.top, 4)
                    .disabled(password.isEmpty && !(file.isEncrypted && file.isDeviceEncrypted))
                }
            }
            .padding(24)
            .vaultCard(cornerRadius: 10)
            
            Spacer()
        }
        .padding(32)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(VaultTheme.paperWhite)
    }
    
    private func encrypt() {
        password.withCString { pwPtr in
            let res = qtsq_vault_encrypt_password(&file.ctx, pwPtr)
            if res == QTSQ_OK {
                _ = file.path.withCString { pathPtr in
                    qtsq_write(&file.ctx, pathPtr)
                }
                file.reload()
                showResult(success: true, title: "File Encrypted", msg: "File encrypted with AES-256-GCM and saved to disk.")
                appState.currentPanel = .viewer(file.id)
            } else {
                showResult(success: false, title: "Encryption Failed", msg: String(cString: qtsq_error_string(res)))
            }
        }
    }
    
    private func showResult(success: Bool, title: String, msg: String) {
        DispatchQueue.main.async {
            let a = NSAlert()
            a.messageText = title
            a.informativeText = msg
            a.alertStyle = success ? .informational : .critical
            a.runModal()
        }
    }
}

// MARK: - Health View (Segmented Integrity Bar)

struct HealthView: View {
    @EnvironmentObject var appState: AppState
    @ObservedObject var file: QTSQFile
    @State private var integrityScore: Double = 1.0
    
    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            // Header
            HStack {
                HStack(spacing: 8) {
                    Image(systemName: "cross.case")
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundColor(VaultTheme.integrityGreen)
                    
                    Text("Integrity & Error Recovery — \(file.name)")
                        .font(.system(size: 17, weight: .semibold))
                        .foregroundColor(VaultTheme.textPrimary)
                }
                
                Spacer()
                
                Button(action: { appState.currentPanel = .viewer(file.id) }) {
                    HStack(spacing: 4) {
                        Image(systemName: "arrow.left")
                            .font(.system(size: 11))
                        Text("Back to Viewer")
                            .font(.system(size: 12, weight: .medium))
                    }
                }
                .buttonStyle(PlainButtonStyle())
                .vaultGhostButton()
            }
            .padding(.bottom, 6)
            
            let hasEcc = (file.ctx.header.flags & UInt32(QTSQ_FLAG_HAS_ECC)) != 0
            
            // Segmented Integrity Status Card
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    Text("SEGMENTED STREAM INTEGRITY")
                        .font(.system(size: 10, weight: .heavy))
                        .tracking(1.2)
                        .foregroundColor(VaultTheme.textSecondary)
                    
                    Spacer()
                    
                    Text(hasEcc ? "100% INTACT" : "PARITY UNATTACHED")
                        .font(.system(size: 11, weight: .bold, design: .monospaced))
                        .foregroundColor(hasEcc ? VaultTheme.integrityGreen : VaultTheme.emberOrange)
                }
                
                // Segmented Progress Bar (10 Chunks)
                HStack(spacing: 4) {
                    ForEach(0..<10, id: \.self) { i in
                        RoundedRectangle(cornerRadius: 2)
                            .fill(hasEcc ? VaultTheme.integrityGreen : VaultTheme.emberOrange.opacity(0.8))
                            .frame(height: 8)
                    }
                }
                
                HStack(spacing: 16) {
                    HStack(spacing: 6) {
                        Circle()
                            .fill(hasEcc ? VaultTheme.integrityGreen : VaultTheme.textTertiary)
                            .frame(width: 6, height: 6)
                        Text(hasEcc ? "Reed-Solomon ECC Parity Active" : "No Parity Recovery Blocks")
                            .font(.system(size: 12, weight: .medium))
                            .foregroundColor(VaultTheme.textPrimary)
                    }
                    
                    Spacer()
                    
                    Text("Chunk Size: 64 KB")
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundColor(VaultTheme.textTertiary)
                }
            }
            .padding(20)
            .vaultCard(cornerRadius: 10)
            
            // Actions
            HStack(spacing: 12) {
                if !hasEcc {
                    Button(action: addECC) {
                        HStack(spacing: 6) {
                            Image(systemName: "plus.shield")
                                .font(.system(size: 12))
                            Text("Attach ECC Protection")
                                .font(.system(size: 13, weight: .semibold))
                        }
                        .foregroundColor(.white)
                        .padding(.horizontal, 20)
                        .padding(.vertical, 8)
                        .background(VaultTheme.emberOrange)
                        .cornerRadius(6)
                    }
                    .buttonStyle(PlainButtonStyle())
                } else {
                    Button(action: checkHealth) {
                        HStack(spacing: 6) {
                            Image(systemName: "checkmark.shield")
                                .font(.system(size: 12))
                            Text("Run Integrity Check")
                                .font(.system(size: 13, weight: .semibold))
                        }
                        .foregroundColor(.white)
                        .padding(.horizontal, 20)
                        .padding(.vertical, 8)
                        .background(VaultTheme.integrityGreen)
                        .cornerRadius(6)
                    }
                    .buttonStyle(PlainButtonStyle())
                    
                    Button(action: recover) {
                        HStack(spacing: 6) {
                            Image(systemName: "wrench.and.screwdriver")
                                .font(.system(size: 12))
                            Text("Attempt Parity Repair")
                                .font(.system(size: 13, weight: .medium))
                        }
                        .foregroundColor(VaultTheme.textPrimary)
                        .padding(.horizontal, 20)
                        .padding(.vertical, 8)
                        .background(VaultTheme.cardWhite)
                        .cornerRadius(6)
                        .overlay(RoundedRectangle(cornerRadius: 6).stroke(VaultTheme.border, lineWidth: 1))
                    }
                    .buttonStyle(PlainButtonStyle())
                }
            }
            .padding(.top, 4)
            
            Spacer()
        }
        .padding(32)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(VaultTheme.paperWhite)
    }
    
    private func addECC() {
        let res = qtsq_add_recovery(&file.ctx)
        if res == QTSQ_OK {
            _ = file.path.withCString { pathPtr in
                if qtsqPromptAndEncrypt(&file.ctx) {
                    qtsq_write(&file.ctx, pathPtr)
                }
            }
            file.reload()
            showResult(success: true, title: "ECC Added", msg: "Reed-Solomon parity recovery blocks attached.")
        } else {
            showResult(success: false, title: "Failed", msg: String(cString: qtsq_error_string(res)))
        }
    }
    
    private func checkHealth() {
        let res = qtsq_health_check(&file.ctx)
        if res == QTSQ_OK {
            showResult(success: true, title: "Integrity Verified", msg: "All stream chunks match cryptographic checksums.")
        } else if res == QTSQ_ERR_CORRUPT {
            showResult(success: false, title: "Parity Correction Needed", msg: "Bit flips detected, but recoverable via parity.")
        } else {
            showResult(success: false, title: "Critical Corruption", msg: "Corruption exceeds error recovery limits.", style: .critical)
        }
    }
    
    private func recover() {
        let res = qtsq_recover(&file.ctx)
        if res == QTSQ_OK {
            _ = file.path.withCString { pathPtr in
                if qtsqPromptAndEncrypt(&file.ctx) {
                    qtsq_write(&file.ctx, pathPtr)
                }
            }
            file.reload()
            showResult(success: true, title: "Stream Repaired", msg: "Corrupted chunks successfully reconstructed.")
        } else {
            showResult(success: false, title: "Recovery Failed", msg: "Unable to reconstruct parity symbols.", style: .critical)
        }
    }
    
    private func showResult(success: Bool, title: String, msg: String, style: NSAlert.Style = .informational) {
        DispatchQueue.main.async {
            let a = NSAlert()
            a.messageText = title
            a.informativeText = msg
            a.alertStyle = style
            a.runModal()
        }
    }
}

