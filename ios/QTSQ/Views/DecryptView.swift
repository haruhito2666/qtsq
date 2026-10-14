import SwiftUI
import UniformTypeIdentifiers

struct DecryptView: View {
    @EnvironmentObject var engine: QtsqEngine
    var onClose: () -> Void

    @State private var selectedURL: URL?
    @State private var password = ""
    @State private var isDecrypting = false
    @State private var resultMessage: String?
    @State private var showFilePicker = false

    var body: some View {
        VStack(spacing: 0) {
            // ── Top Bar ──
            HStack {
                Button(action: onClose) {
                    Image(systemName: "arrow.left")
                        .font(.system(size: QIconSize.md, weight: .medium))
                        .foregroundColor(QColor.textPrimary)
                }
                Text("Decrypt File")
                    .font(QFont.heading())
                    .foregroundColor(QColor.textPrimary)
                    .padding(.leading, QSpacing.sm)
                Spacer()
            }
            .padding(.horizontal, QSpacing.xl)
            .padding(.vertical, QSpacing.lg)

            Spacer().frame(height: QSpacing.xxl)

            // ── File Selection Card ──
            Button(action: { showFilePicker = true }) {
                VStack(spacing: QSpacing.lg) {
                    QIconBox(
                        systemName: "lock.open.fill",
                        iconColor: QColor.categoryOrange,
                        bgColor: QColor.categoryOrangeBg,
                        size: 56,
                        iconSize: 28
                    )
                    Text(selectedURL == nil ? "Select a .qtsq file" : "File selected ✓")
                        .font(QFont.body())
                        .foregroundColor(selectedURL == nil ? QColor.textSecondary : QColor.success)
                    if selectedURL == nil {
                        Text("Open and decrypt encrypted archives")
                            .font(QFont.bodySmall())
                            .foregroundColor(QColor.textTertiary)
                    }
                }
                .padding(40)
                .frame(maxWidth: .infinity)
                .background(QColor.surfaceCard)
                .clipShape(RoundedRectangle(cornerRadius: QRadius.xl))
            }
            .buttonStyle(.plain)
            .padding(.horizontal, QSpacing.xl)

            Spacer().frame(height: QSpacing.xl)

            // ── Password Input ──
            HStack {
                Image(systemName: "lock.fill")
                    .foregroundColor(QColor.textTertiary)
                SecureField("Password (Optional)", text: $password)
            }
            .padding()
            .background(QColor.surfaceCard)
            .clipShape(RoundedRectangle(cornerRadius: QRadius.lg))
            .overlay(
                RoundedRectangle(cornerRadius: QRadius.lg)
                    .stroke(QColor.divider, lineWidth: 1)
            )
            .padding(.horizontal, QSpacing.xl)

            Spacer().frame(height: QSpacing.xxl + 4)

            // ── Decrypt Button ──
            QAccentButton(
                title: isDecrypting ? "Decrypting..." : "Decrypt",
                iconName: "lock.open.fill",
                isLoading: isDecrypting,
                action: decryptFile
            )
            .disabled(selectedURL == nil || isDecrypting)
            .opacity(selectedURL == nil ? 0.5 : 1.0)

            // ── Result ──
            if let msg = resultMessage {
                Spacer().frame(height: QSpacing.xxl)
                QResultBanner(
                    message: msg,
                    isSuccess: msg.hasPrefix("Decrypted")
                )
            }

            Spacer()
        }
        .background(QColor.backgroundWhite)
        .fileImporter(
            isPresented: $showFilePicker,
            allowedContentTypes: [.data],
            allowsMultipleSelection: false
        ) { result in
            if case .success(let urls) = result, let url = urls.first {
                selectedURL = url
                resultMessage = nil
            }
        }
    }

    private func decryptFile() {
        guard let url = selectedURL else { return }
        isDecrypting = true
        resultMessage = nil

        DispatchQueue.global(qos: .userInitiated).async {
            let msg: String
            guard url.startAccessingSecurityScopedResource() else {
                DispatchQueue.main.async { resultMessage = "Cannot access file."; isDecrypting = false }
                return
            }
            defer { url.stopAccessingSecurityScopedResource() }

            guard let data = try? Data(contentsOf: url) else {
                DispatchQueue.main.async { resultMessage = "Failed to read file."; isDecrypting = false }
                return
            }

            let pw = password.isEmpty ? nil : password
            if let result = engine.decompress(data, password: pw) {
                let dir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
                let outURL = dir.appendingPathComponent("decrypted_\(Int(Date().timeIntervalSince1970)).bin")
                do {
                    try result.data.write(to: outURL)
                    msg = "Decrypted to \(outURL.lastPathComponent) (\(result.data.count) bytes)"
                } catch {
                    msg = "Save failed: \(error.localizedDescription)"
                }
            } else {
                msg = "Decryption failed. Wrong password?"
            }

            DispatchQueue.main.async {
                resultMessage = msg
                isDecrypting = false
            }
        }
    }
}
