import SwiftUI
import UniformTypeIdentifiers

struct ConvertView: View {
    @EnvironmentObject var engine: QtsqEngine
    @State private var selectedURL: URL?
    @State private var password = ""
    @State private var isConverting = false
    @State private var resultMessage: String?
    @State private var showFilePicker = false

    var body: some View {
        VStack(spacing: 0) {
            // ── Top Bar ──
            HStack {
                Text("Compress File")
                    .font(QFont.heading())
                    .foregroundColor(QColor.textPrimary)
                Spacer()
            }
            .padding(.horizontal, QSpacing.xl)
            .padding(.vertical, QSpacing.lg)

            Spacer().frame(height: QSpacing.xxl)

            // ── File Selection Card ──
            Button(action: { showFilePicker = true }) {
                VStack(spacing: QSpacing.lg) {
                    QIconBox(
                        systemName: "doc.badge.plus",
                        iconColor: QColor.accentPrimary,
                        bgColor: QColor.accentPrimaryLight,
                        size: 56,
                        iconSize: 28
                    )
                    Text(selectedURL == nil ? "Select a file to compress" : "File selected ✓")
                        .font(QFont.body())
                        .foregroundColor(selectedURL == nil ? QColor.textSecondary : QColor.success)
                    if selectedURL == nil {
                        Text("Supports images, audio, text, and more")
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

            // ── Compress Button ──
            QAccentButton(
                title: isConverting ? "Compressing..." : "Compress to .qtsq",
                iconName: "archivebox.fill",
                isLoading: isConverting,
                action: compressFile
            )
            .disabled(selectedURL == nil || isConverting)
            .opacity(selectedURL == nil ? 0.5 : 1.0)

            // ── Result ──
            if let msg = resultMessage {
                Spacer().frame(height: QSpacing.xxl)
                QResultBanner(
                    message: msg,
                    isSuccess: msg.hasPrefix("Saved")
                )
            }

            Spacer()
        }
        .background(QColor.backgroundWhite)
        .fileImporter(
            isPresented: $showFilePicker,
            allowedContentTypes: [.data, .image, .audio, .plainText],
            allowsMultipleSelection: false
        ) { result in
            if case .success(let urls) = result, let url = urls.first {
                selectedURL = url
                resultMessage = nil
            }
        }
    }

    private func compressFile() {
        guard let url = selectedURL else { return }
        isConverting = true
        resultMessage = nil

        DispatchQueue.global(qos: .userInitiated).async {
            let msg = performCompression(url: url)
            DispatchQueue.main.async {
                resultMessage = msg
                isConverting = false
            }
        }
    }

    private func performCompression(url: URL) -> String {
        guard url.startAccessingSecurityScopedResource() else {
            return "Cannot access file."
        }
        defer { url.stopAccessingSecurityScopedResource() }

        guard let data = try? Data(contentsOf: url) else {
            return "Failed to read file."
        }

        let uti = UTType(filenameExtension: url.pathExtension)
        let pw = password.isEmpty ? nil : password
        var compressed: Data?

        if uti?.conforms(to: .image) == true {
            if let uiImage = UIImage(data: data) {
                let w = Int(uiImage.size.width * uiImage.scale)
                let h = Int(uiImage.size.height * uiImage.scale)
                if let cgImage = uiImage.cgImage {
                    let bytesPerRow = w * 3
                    var rgbData = Data(count: w * h * 3)
                    rgbData.withUnsafeMutableBytes { buf in
                        guard let ptr = buf.baseAddress else { return }
                        let ctx = CGContext(
                            data: ptr,
                            width: w, height: h,
                            bitsPerComponent: 8,
                            bytesPerRow: bytesPerRow,
                            space: CGColorSpaceCreateDeviceRGB(),
                            bitmapInfo: CGImageAlphaInfo.none.rawValue
                        )
                        ctx?.draw(cgImage, in: CGRect(x: 0, y: 0, width: w, height: h))
                    }

                    let isLossy = ["jpg", "jpeg", "webp"].contains(url.pathExtension.lowercased())
                    if isLossy {
                        compressed = engine.compressImageUpscale(pixels: rgbData, width: w, height: h, channels: 3, password: pw, sourceSize: data.count)
                    } else {
                        compressed = engine.compressImage(pixels: rgbData, width: w, height: h, channels: 3, password: pw)
                    }
                }
            }
        } else if uti?.conforms(to: .audio) == true {
            compressed = engine.compressBytes(data, type: "audio", password: pw)
        } else {
            compressed = engine.compressBytes(data, type: "raw", password: pw)
        }

        guard let output = compressed else { return "Compression failed." }

        let dir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
        let outURL = dir.appendingPathComponent("converted_\(Int(Date().timeIntervalSince1970)).qtsq")
        do {
            try output.write(to: outURL)
            return "Saved \(outURL.lastPathComponent) (\(output.count / 1024) KB)"
        } catch {
            return "Save failed: \(error.localizedDescription)"
        }
    }
}
