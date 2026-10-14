import SwiftUI
import AVFoundation

struct ViewerView: View {
    @EnvironmentObject var engine: QtsqEngine
    let filePath: String
    var onClose: () -> Void

    @State private var isLoading = true
    @State private var errorMessage: String?
    @State private var result: QtsqResult?
    @State private var showPasswordDialog = false
    @State private var password = ""
    @State private var isSharing = false

    // Audio
    @State private var audioPlayer: AVAudioPlayer?
    @State private var isPlaying = false

    // Image zoom
    @State private var imageScale: CGFloat = 1.0
    @State private var imageOffset: CGSize = .zero

    var body: some View {
        VStack(spacing: 0) {
            // ── Top Bar ──
            HStack {
                Button(action: {
                    audioPlayer?.stop()
                    onClose()
                }) {
                    Image(systemName: "arrow.left")
                        .font(.system(size: QIconSize.md, weight: .medium))
                        .foregroundColor(QColor.textPrimary)
                }

                Text(URL(fileURLWithPath: filePath).lastPathComponent)
                    .font(QFont.title())
                    .foregroundColor(QColor.textPrimary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .padding(.horizontal, QSpacing.sm)

                Spacer()

                // Share button
                if URL(string: filePath) != nil {
                    Button(action: { isSharing = true }) {
                        Image(systemName: "square.and.arrow.up")
                            .font(.system(size: QIconSize.md))
                            .foregroundColor(QColor.accentPrimary)
                    }
                }

                // Password retry
                if errorMessage != nil {
                    Button(action: { showPasswordDialog = true }) {
                        Image(systemName: "key.fill")
                            .font(.system(size: QIconSize.md))
                            .foregroundColor(QColor.accentPrimary)
                    }
                }
            }
            .padding(.horizontal, QSpacing.xl)
            .padding(.vertical, QSpacing.md)

            // ── Content ──
            if isLoading {
                Spacer()
                VStack(spacing: QSpacing.lg) {
                    ProgressView()
                        .scaleEffect(1.5)
                        .tint(QColor.accentPrimary)
                    Text("Decompressing...")
                        .font(QFont.bodyMedium())
                        .foregroundColor(QColor.textSecondary)
                }
                Spacer()
            } else if let error = errorMessage {
                Spacer()
                VStack(spacing: QSpacing.lg) {
                    Image(systemName: "exclamationmark.circle.fill")
                        .font(.system(size: 64))
                        .foregroundColor(QColor.error)
                    Text(error)
                        .font(QFont.body())
                        .foregroundColor(QColor.textPrimary)
                        .multilineTextAlignment(.center)
                    Button(action: { showPasswordDialog = true }) {
                        HStack(spacing: 8) {
                            Image(systemName: "key.fill")
                            Text("Try with Password")
                        }
                        .foregroundColor(.white)
                        .padding(.horizontal, QSpacing.xxl)
                        .padding(.vertical, QSpacing.md)
                        .background(QColor.accentPrimary)
                        .clipShape(RoundedRectangle(cornerRadius: QRadius.lg))
                    }
                }
                .padding(QSpacing.xxxl)
                Spacer()
            } else if let r = result {
                viewContent(r)
            }

            // ── File Info Footer ──
            if let r = result {
                fileInfoFooter(r)
            }
        }
        .background(QColor.backgroundWhite)
        .onAppear { loadFile(password: nil) }
        .onDisappear { audioPlayer?.stop() }
        .alert("Enter Password", isPresented: $showPasswordDialog) {
            SecureField("Password", text: $password)
            Button("Decrypt") { loadFile(password: password) }
            Button("Cancel", role: .cancel) {}
        }
        .sheet(isPresented: $isSharing) {
            if let url = URL(string: filePath) {
                ShareSheet(items: [url])
            }
        }
    }

    // MARK: - Load File

    private func loadFile(password: String?) {
        isLoading = true
        errorMessage = nil

        DispatchQueue.global(qos: .userInitiated).async {
            guard FileManager.default.fileExists(atPath: filePath),
                  let data = try? Data(contentsOf: URL(fileURLWithPath: filePath)) else {
                DispatchQueue.main.async { errorMessage = "File not found."; isLoading = false }
                return
            }

            if let res = engine.decompress(data, password: password) {
                DispatchQueue.main.async { result = res; isLoading = false }
            } else {
                DispatchQueue.main.async {
                    errorMessage = "Decompression failed. Wrong password?"
                    isLoading = false
                }
            }
        }
    }

    // MARK: - Content Router

    @ViewBuilder
    private func viewContent(_ r: QtsqResult) -> some View {
        if r.type.isImage {
            imageViewer(r)
        } else if r.type.isAudio {
            audioViewer(r)
        } else if r.type.isText {
            textViewer(r)
        } else {
            rawViewer(r)
        }
    }

    // MARK: - Image Viewer

    @ViewBuilder
    private func imageViewer(_ r: QtsqResult) -> some View {
        VStack(spacing: QSpacing.md) {
            if let uiImage = createImage(from: r) {
                ZStack(alignment: .bottomTrailing) {
                    Image(uiImage: uiImage)
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                        .scaleEffect(imageScale)
                        .offset(imageOffset)
                        .gesture(
                            MagnificationGesture()
                                .onChanged { value in imageScale = max(1, value) }
                                .onEnded { _ in if imageScale < 1.2 { imageScale = 1; imageOffset = .zero } }
                        )
                        .gesture(
                            DragGesture()
                                .onChanged { value in if imageScale > 1 { imageOffset = value.translation } }
                        )
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                        .background(Color.black)
                        .clipShape(RoundedRectangle(cornerRadius: QRadius.lg))

                    // Zoom controls
                    HStack(spacing: 4) {
                        Button { imageScale = max(1, imageScale / 1.5); if imageScale <= 1 { imageOffset = .zero } } label: {
                            Image(systemName: "minus").foregroundColor(.white)
                        }
                        Text(String(format: "%.1fx", imageScale))
                            .font(QFont.mono)
                            .foregroundColor(.white)
                        Button { imageScale = min(50, imageScale * 1.5) } label: {
                            Image(systemName: "plus").foregroundColor(.white)
                        }
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)
                    .background(Color.black.opacity(0.6))
                    .clipShape(RoundedRectangle(cornerRadius: QRadius.sm))
                    .padding(QSpacing.lg)
                }
                .padding(.horizontal, QSpacing.lg)

                Text("\(r.width) × \(r.height) px")
                    .font(QFont.bodyMedium())
                    .foregroundColor(QColor.textSecondary)
            } else {
                Spacer()
                VStack(spacing: QSpacing.lg) {
                    Image(systemName: "photo.badge.exclamationmark")
                        .font(.system(size: 64))
                        .foregroundColor(QColor.textSecondary)
                    Text("Could not render image")
                        .font(QFont.body())
                        .foregroundColor(QColor.textSecondary)
                }
                Spacer()
            }
        }
    }

    private func createImage(from r: QtsqResult) -> UIImage? {
        let w = r.width
        let h = r.height
        guard w > 0, h > 0 else { return nil }

        let bitsPerComponent = 8
        let bytesPerRow = w * 4
        guard let colorSpace = CGColorSpace(name: CGColorSpace.sRGB),
              let context = CGContext(
                data: UnsafeMutableRawPointer(mutating: (r.data as NSData).bytes),
                width: w, height: h,
                bitsPerComponent: bitsPerComponent,
                bytesPerRow: bytesPerRow,
                space: colorSpace,
                bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
              ),
              let cgImage = context.makeImage()
        else { return nil }

        return UIImage(cgImage: cgImage)
    }

    // MARK: - Audio Viewer

    @ViewBuilder
    private func audioViewer(_ r: QtsqResult) -> some View {
        let channels = r.type == .audioStereo ? 2 : 1
        let sampleRate = r.audioSampleRate > 0 ? r.audioSampleRate : 44100
        let durationSec = r.data.count > 0 ? r.data.count / (2 * channels * sampleRate) : 0

        VStack(spacing: QSpacing.xxxl) {
            Spacer()

            // Play circle
            ZStack {
                Circle()
                    .fill(isPlaying ? QColor.accentSecondary.opacity(0.15) : QColor.surfaceCard)
                    .frame(width: 180, height: 180)
                Circle()
                    .fill(isPlaying ? QColor.accentSecondary.opacity(0.3) : QColor.accentPrimary.opacity(0.1))
                    .frame(width: 120, height: 120)
                Image(systemName: isPlaying ? "pause.fill" : "play.fill")
                    .font(.system(size: 56))
                    .foregroundColor(isPlaying ? QColor.accentSecondary : QColor.accentPrimary)
            }

            Text(String(format: "%02d:%02d", durationSec / 60, durationSec % 60))
                .font(QFont.display(48))
                .foregroundColor(isPlaying ? QColor.accentSecondary : QColor.textPrimary)

            Text("\(sampleRate / 1000)kHz • \(channels)ch • \(r.data.count / 1024) KB")
                .font(QFont.bodyMedium())
                .foregroundColor(QColor.textSecondary)

            // Controls
            HStack(spacing: QSpacing.xxl) {
                Button {
                    audioPlayer?.stop()
                    isPlaying = false
                } label: {
                    Image(systemName: "stop.fill")
                        .font(.system(size: QIconSize.xl))
                        .foregroundColor(QColor.textSecondary)
                        .frame(width: 56, height: 56)
                        .background(QColor.surfaceCard)
                        .clipShape(Circle())
                }

                Button {
                    isPlaying.toggle()
                    // Audio playback would be connected here
                } label: {
                    Image(systemName: isPlaying ? "pause.fill" : "play.fill")
                        .font(.system(size: QIconSize.xxl))
                        .foregroundColor(.white)
                        .frame(width: 72, height: 72)
                        .background(isPlaying ? QColor.accentSecondary : QColor.accentPrimary)
                        .clipShape(Circle())
                }
            }

            Spacer()
        }
    }

    // MARK: - Text Viewer

    @ViewBuilder
    private func textViewer(_ r: QtsqResult) -> some View {
        VStack(alignment: .leading, spacing: QSpacing.md) {
            HStack(spacing: 8) {
                Image(systemName: "doc.text.fill")
                    .foregroundColor(QColor.accentPrimary)
                Text(r.type.label)
                    .font(QFont.bodyMedium())
                    .fontWeight(.semibold)
                    .foregroundColor(QColor.accentPrimary)
            }
            .padding(.horizontal, QSpacing.lg)

            ScrollView {
                Text(String(data: r.data, encoding: .utf8) ?? "Could not decode text.")
                    .font(QFont.mono)
                    .foregroundColor(QColor.textPrimary)
                    .padding(QSpacing.lg)
            }
            .background(QColor.surfaceWhite)
            .clipShape(RoundedRectangle(cornerRadius: QRadius.lg))
            .shadow(color: .black.opacity(0.05), radius: 4, y: 2)
            .padding(.horizontal, QSpacing.lg)
        }
        .padding(.top, QSpacing.lg)
    }

    // MARK: - Raw Data Viewer

    @ViewBuilder
    private func rawViewer(_ r: QtsqResult) -> some View {
        let hex = r.data.prefix(512).map { String(format: "%02X", $0) }.joined(separator: " ")

        VStack(alignment: .leading, spacing: QSpacing.md) {
            HStack(spacing: 8) {
                Image(systemName: "doc.badge.gearshape")
                    .foregroundColor(QColor.accentPrimary)
                Text("Raw Binary (Type: 0x\(String(format: "%02X", r.type.rawValue)))")
                    .font(QFont.bodyMedium())
                    .fontWeight(.semibold)
                    .foregroundColor(QColor.accentPrimary)
            }
            .padding(.horizontal, QSpacing.lg)

            ScrollView {
                Text(hex)
                    .font(QFont.mono)
                    .foregroundColor(Color(hex: 0x00FF88))
                    .padding(QSpacing.lg)
            }
            .background(Color(hex: 0x1E1E24))
            .clipShape(RoundedRectangle(cornerRadius: QRadius.lg))
            .padding(.horizontal, QSpacing.lg)

            Text("Showing first \(min(512, r.data.count)) of \(r.data.count) bytes")
                .font(QFont.bodyMedium())
                .foregroundColor(QColor.textSecondary)
                .padding(.horizontal, QSpacing.lg)
        }
        .padding(.top, QSpacing.lg)
    }

    // MARK: - File Info Footer

    @ViewBuilder
    private func fileInfoFooter(_ r: QtsqResult) -> some View {
        let fileSize = (try? FileManager.default.attributesOfItem(atPath: filePath)[.size] as? Int) ?? 0
        let ratio = r.originalSize > 0 ? Float(fileSize) / Float(r.originalSize) * 100 : 0

        VStack(spacing: 0) {
            Divider()
            HStack {
                Spacer()
                QInfoChip(label: "Type", value: r.type.label)
                Spacer()
                QInfoChip(label: "Size", value: "\(fileSize / 1024) KB")
                Spacer()
                QInfoChip(label: "Ratio", value: String(format: "%.1f%%", ratio))
                Spacer()
            }
            .padding(.vertical, QSpacing.lg)

            if r.vertexCount > 0 {
                HStack {
                    Spacer()
                    QInfoChip(label: "Engine", value: r.encoding.isEmpty ? "QTSQ HD" : r.encoding)
                    Spacer()
                    QInfoChip(label: "Mesh", value: "\(r.vertexCount)v / \(r.faceCount)f")
                    Spacer()
                    QInfoChip(label: "Splats", value: "\(r.splatCount)")
                    Spacer()
                }
                .padding(.bottom, QSpacing.lg)
            }
        }
        .background(QColor.surfaceCard)
    }
}

// MARK: - iOS 15 Share Sheet
struct ShareSheet: UIViewControllerRepresentable {
    var items: [Any]
    
    func makeUIViewController(context: Context) -> UIActivityViewController {
        let controller = UIActivityViewController(activityItems: items, applicationActivities: nil)
        return controller
    }
    
    func updateUIViewController(_ uiViewController: UIActivityViewController, context: Context) {}
}
