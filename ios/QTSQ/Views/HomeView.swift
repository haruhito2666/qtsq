import SwiftUI

struct HomeView: View {
    @EnvironmentObject var engine: QtsqEngine
    var onCameraClick: () -> Void
    var onAudioClick: () -> Void
    var onConvertClick: () -> Void
    var onDecryptClick: () -> Void
    var onFileClick: (String) -> Void

    private var qtsqFiles: [URL] {
        let dir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
        let files = (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: [.contentModificationDateKey]))?.filter { $0.pathExtension == "qtsq" } ?? []
        return files.sorted { (try? $0.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? Date.distantPast > (try? $1.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? Date.distantPast }
    }

    private var totalFiles: Int { qtsqFiles.count }

    private var totalSizeKB: Int {
        qtsqFiles.compactMap { try? FileManager.default.attributesOfItem(atPath: $0.path)[.size] as? Int }.reduce(0, +) / 1024
    }

    var body: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 0) {
                // ── Top Bar ──
                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("QTSQ")
                            .font(QFont.heading())
                            .foregroundColor(QColor.accentPrimary)
                            .tracking(2)
                        Text("Quantum Tensor Sequence")
                            .font(QFont.bodySmall())
                            .foregroundColor(QColor.textTertiary)
                    }
                    Spacer()
                    Button(action: onConvertClick) {
                        Image(systemName: "plus")
                            .font(.system(size: QIconSize.md, weight: .medium))
                            .foregroundColor(QColor.textPrimary)
                            .frame(width: 40, height: 40)
                            .background(QColor.surfaceCard)
                            .clipShape(RoundedRectangle(cornerRadius: QRadius.md))
                    }
                }
                .padding(.horizontal, QSpacing.xl)
                .padding(.vertical, QSpacing.lg)

                // ── Stats Card ──
                VStack(alignment: .leading, spacing: QSpacing.lg) {
                    Text("\(totalFiles)")
                        .font(QFont.display())
                        .foregroundColor(QColor.textPrimary)
                    Text(totalFiles == 1 ? "File stored" : "Files stored")
                        .font(QFont.bodyMedium())
                        .foregroundColor(QColor.textSecondary)

                    // Metric bar
                    GeometryReader { geo in
                        ZStack(alignment: .leading) {
                            RoundedRectangle(cornerRadius: 3)
                                .fill(QColor.divider)
                                .frame(height: 6)
                            RoundedRectangle(cornerRadius: 3)
                                .fill(
                                    LinearGradient(
                                        colors: [QColor.accentPrimary, QColor.categoryTeal],
                                        startPoint: .leading,
                                        endPoint: .trailing
                                    )
                                )
                                .frame(width: geo.size.width * CGFloat(min(max(Double(totalSizeKB) / 10240.0, 0.05), 1.0)), height: 6)
                        }
                    }
                    .frame(height: 6)

                    Text(totalSizeKB >= 1024 ? String(format: "%.1f MB used", Double(totalSizeKB) / 1024.0) : "\(totalSizeKB) KB used")
                        .font(QFont.bodySmall())
                        .foregroundColor(QColor.textTertiary)
                }
                .padding(QSpacing.xxl)
                .background(QColor.surfaceCard)
                .clipShape(RoundedRectangle(cornerRadius: QRadius.xl))
                .padding(.horizontal, QSpacing.xl)

                Spacer().frame(height: QSpacing.xxl)

                // ── Quick Actions ──
                HStack(spacing: QSpacing.md) {
                    QActionCard(
                        title: "Camera",
                        subtitle: "Direct to .qtsq",
                        iconName: "camera.fill",
                        iconColor: QColor.categoryPurple,
                        iconBg: QColor.categoryPurpleBg,
                        action: onCameraClick
                    )
                    QActionCard(
                        title: "Audio",
                        subtitle: "Direct to .qtsq",
                        iconName: "mic.fill",
                        iconColor: QColor.categoryRed,
                        iconBg: QColor.categoryRedBg,
                        action: onAudioClick
                    )
                }
                .padding(.horizontal, QSpacing.xl)

                Spacer().frame(height: QSpacing.md)

                HStack(spacing: QSpacing.md) {
                    QActionCard(
                        title: "Convert",
                        subtitle: "Files to .qtsq",
                        iconName: "arrow.left.arrow.right",
                        iconColor: QColor.categoryTeal,
                        iconBg: QColor.categoryTealBg,
                        action: onConvertClick
                    )
                    QActionCard(
                        title: "Decrypt",
                        subtitle: "Open .qtsq",
                        iconName: "lock.open.fill",
                        iconColor: QColor.categoryOrange,
                        iconBg: QColor.categoryOrangeBg,
                        action: onDecryptClick
                    )
                }
                .padding(.horizontal, QSpacing.xl)

                Spacer().frame(height: QSpacing.xxl + 4)

                // ── Recent Files ──
                QSectionHeader(title: "Recent captures")

                if qtsqFiles.isEmpty {
                    QEmptyState(
                        iconName: "tray",
                        title: "No files yet",
                        subtitle: "Capture or convert to create .qtsq files"
                    )
                } else {
                    ForEach(qtsqFiles.prefix(5), id: \.path) { file in
                        let name = file.lastPathComponent
                        let sizeKB = (try? FileManager.default.attributesOfItem(atPath: file.path)[.size] as? Int).map { $0 / 1024 } ?? 0
                        let isAudio = name.hasPrefix("audio_")
                        let isEncrypted = name.contains("encrypted")

                        QFileRow(
                            fileName: name,
                            subtitle: "\(sizeKB) KB",
                            iconName: isAudio ? "waveform" : (isEncrypted ? "lock.fill" : "photo.fill"),
                            iconColor: isAudio ? QColor.categoryRed : (isEncrypted ? QColor.categoryOrange : QColor.categoryPurple),
                            iconBg: isAudio ? QColor.categoryRedBg : (isEncrypted ? QColor.categoryOrangeBg : QColor.categoryPurpleBg),
                            action: { onFileClick(file.path) }
                        )
                    }
                }

                Spacer().frame(height: QSpacing.xxl)
            }
        }
        .background(QColor.backgroundWhite)
    }
}
