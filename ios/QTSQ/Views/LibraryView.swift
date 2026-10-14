import SwiftUI

struct LibraryView: View {
    var onFileClick: (String) -> Void

    @State private var activeFilter: String?

    private var allFiles: [URL] {
        let dir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
        let files = (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: [.contentModificationDateKey]))?.filter { $0.pathExtension == "qtsq" } ?? []
        return files.sorted { (try? $0.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? Date.distantPast > (try? $1.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? Date.distantPast }
    }

    private var imageFiles: [URL] { allFiles.filter { $0.lastPathComponent.hasPrefix("QTSQ_") || $0.lastPathComponent.hasPrefix("converted_") } }
    private var audioFiles: [URL] { allFiles.filter { $0.lastPathComponent.hasPrefix("audio_") } }
    private var convertedFiles: [URL] { allFiles.filter { $0.lastPathComponent.hasPrefix("converted_") } }
    private var otherFiles: [URL] { allFiles.filter { !$0.lastPathComponent.hasPrefix("QTSQ_") && !$0.lastPathComponent.hasPrefix("audio_") && !$0.lastPathComponent.hasPrefix("converted_") } }

    private var filteredFiles: [URL] {
        switch activeFilter {
        case "image": return imageFiles
        case "audio": return audioFiles
        case "converted": return convertedFiles
        case "other": return otherFiles
        default: return allFiles
        }
    }

    private var categories: [(String, String, Color, Color, Int, String)] {
        [
            ("Images", "photo.fill", QColor.categoryPurple, QColor.categoryPurpleBg, imageFiles.count, "image"),
            ("Audio", "waveform", QColor.categoryRed, QColor.categoryRedBg, audioFiles.count, "audio"),
            ("Converted", "arrow.left.arrow.right", QColor.categoryTeal, QColor.categoryTealBg, convertedFiles.count, "converted"),
            ("Other", "doc.fill", QColor.accentSecondary, QColor.categoryBlueBg, otherFiles.count, "other")
        ]
    }

    var body: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 0) {
                // ── Top Bar ──
                HStack {
                    Text("Library")
                        .font(QFont.heading())
                        .foregroundColor(QColor.textPrimary)
                    Spacer()
                    Text("\(allFiles.count) items")
                        .font(QFont.bodyMedium())
                        .foregroundColor(QColor.textTertiary)
                }
                .padding(.horizontal, QSpacing.xl)
                .padding(.vertical, QSpacing.lg)

                // ── Categories Grid ──
                VStack(spacing: QSpacing.md) {
                    HStack(spacing: QSpacing.md) {
                        ForEach(0..<2) { i in
                            let cat = categories[i]
                            CategoryCard(
                                title: cat.0,
                                iconName: cat.1,
                                iconColor: cat.2,
                                iconBg: cat.3,
                                count: cat.4,
                                isActive: activeFilter == cat.5,
                                action: { activeFilter = activeFilter == cat.5 ? nil : cat.5 }
                            )
                        }
                    }
                    HStack(spacing: QSpacing.md) {
                        ForEach(2..<4) { i in
                            let cat = categories[i]
                            CategoryCard(
                                title: cat.0,
                                iconName: cat.1,
                                iconColor: cat.2,
                                iconBg: cat.3,
                                count: cat.4,
                                isActive: activeFilter == cat.5,
                                action: { activeFilter = activeFilter == cat.5 ? nil : cat.5 }
                            )
                        }
                    }
                }
                .padding(QSpacing.lg)
                .background(QColor.surfaceCard)
                .clipShape(RoundedRectangle(cornerRadius: QRadius.xl))
                .padding(.horizontal, QSpacing.xl)

                // ── Tags ──
                if !allFiles.isEmpty {
                    Spacer().frame(height: QSpacing.xxl)
                    QSectionHeader(title: "Tags")
                    Spacer().frame(height: QSpacing.md)

                    ScrollView(.horizontal, showsIndicators: false) {
                        HStack(spacing: 10) {
                            if !imageFiles.isEmpty {
                                TagChip(iconName: "camera.fill", label: "Camera", color: QColor.categoryPurple)
                            }
                            if !audioFiles.isEmpty {
                                TagChip(iconName: "mic.fill", label: "Recordings", color: QColor.categoryRed)
                            }
                            if allFiles.contains(where: { $0.lastPathComponent.contains("encrypted") }) {
                                TagChip(iconName: "lock.fill", label: "Encrypted", color: QColor.categoryOrange)
                            }
                            TagChip(iconName: "folder.fill", label: "All files", color: QColor.categoryTeal)
                        }
                        .padding(.horizontal, QSpacing.xl)
                    }
                }

                // ── File List ──
                Spacer().frame(height: QSpacing.xxl)
                HStack {
                    Text(activeFilter?.capitalized ?? "All files")
                        .font(QFont.titleSmall())
                        .foregroundColor(QColor.textPrimary)
                    Spacer()
                    if activeFilter != nil {
                        Button("Clear") { activeFilter = nil }
                            .font(QFont.label())
                            .foregroundColor(QColor.accentPrimary)
                    }
                }
                .padding(.horizontal, QSpacing.xl)
                .padding(.bottom, QSpacing.xs)

                if filteredFiles.isEmpty {
                    QEmptyState(
                        iconName: "folder",
                        title: "No files found",
                        subtitle: ""
                    )
                } else {
                    ForEach(filteredFiles, id: \.path) { file in
                        let name = file.lastPathComponent
                        let sizeKB = (try? FileManager.default.attributesOfItem(atPath: file.path)[.size] as? Int).map { $0 / 1024 } ?? 0
                        let isAudio = name.hasPrefix("audio_")
                        let isConverted = name.hasPrefix("converted_")
                        let dateStr = formatDate(file)

                        QFileRow(
                            fileName: name,
                            subtitle: "\(sizeKB) KB • \(dateStr)",
                            iconName: isAudio ? "waveform" : (isConverted ? "arrow.left.arrow.right" : "photo.fill"),
                            iconColor: isAudio ? QColor.categoryRed : (isConverted ? QColor.categoryTeal : QColor.categoryPurple),
                            iconBg: isAudio ? QColor.categoryRedBg : (isConverted ? QColor.categoryTealBg : QColor.categoryPurpleBg),
                            action: { onFileClick(file.path) }
                        )
                    }
                }

                Spacer().frame(height: QSpacing.xxl)
            }
        }
        .background(QColor.backgroundWhite)
    }

    private func formatDate(_ url: URL) -> String {
        let date = (try? FileManager.default.attributesOfItem(atPath: url.path)[.modificationDate] as? Date) ?? Date()
        let fmt = DateFormatter()
        fmt.dateFormat = "MMM dd, HH:mm"
        return fmt.string(from: date)
    }
}

// MARK: - Category Card

private struct CategoryCard: View {
    let title: String
    let iconName: String
    let iconColor: Color
    let iconBg: Color
    let count: Int
    let isActive: Bool
    var action: () -> Void

    var body: some View {
        Button(action: action) {
            VStack(alignment: .leading, spacing: 14) {
                QIconBox(systemName: iconName, iconColor: iconColor, bgColor: iconBg)
                VStack(alignment: .leading, spacing: 2) {
                    Text(title)
                        .font(QFont.titleSmall())
                        .foregroundColor(QColor.textPrimary)
                    Text("\(count) items")
                        .font(QFont.bodySmall())
                        .foregroundColor(QColor.textTertiary)
                }
            }
            .padding(QSpacing.lg)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(isActive ? iconBg : QColor.surfaceWhite)
            .clipShape(RoundedRectangle(cornerRadius: QRadius.lg))
        }
        .buttonStyle(.plain)
    }
}

// MARK: - Tag Chip

private struct TagChip: View {
    let iconName: String
    let label: String
    let color: Color

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: iconName)
                .font(.system(size: QIconSize.sm))
                .foregroundColor(color)
            Text(label)
                .font(QFont.label(13))
                .foregroundColor(QColor.textPrimary)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(QColor.surfaceCard)
        .clipShape(RoundedRectangle(cornerRadius: QRadius.md))
    }
}
