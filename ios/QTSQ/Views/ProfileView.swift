import SwiftUI

struct ProfileView: View {
    @EnvironmentObject var engine: QtsqEngine

    private var fileCount: Int {
        let dir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
        return ((try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil))?.filter { $0.pathExtension == "qtsq" }.count) ?? 0
    }

    private var totalSizeKB: Int {
        let dir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
        let files = (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil))?.filter { $0.pathExtension == "qtsq" } ?? []
        return files.compactMap { try? FileManager.default.attributesOfItem(atPath: $0.path)[.size] as? Int }.reduce(0, +) / 1024
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                // ── Header ──
                Text("Profile")
                    .font(QFont.heading())
                    .foregroundColor(QColor.textPrimary)
                    .padding(.horizontal, QSpacing.xl)
                    .padding(.vertical, QSpacing.lg)

                // ── Avatar Card ──
                HStack(spacing: QSpacing.lg) {
                    ZStack {
                        Circle()
                            .fill(QColor.accentPrimaryLight)
                            .frame(width: 56, height: 56)
                        Image(systemName: "person.fill")
                            .font(.system(size: QIconSize.xl))
                            .foregroundColor(QColor.accentPrimary)
                    }
                    VStack(alignment: .leading, spacing: 2) {
                        Text("QTSQ User")
                            .font(QFont.title())
                            .foregroundColor(QColor.textPrimary)
                        Text("Quantum Tensor Sequence")
                            .font(QFont.bodySmall())
                            .foregroundColor(QColor.textTertiary)
                    }
                }
                .padding(QSpacing.xxl)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(QColor.surfaceCard)
                .clipShape(RoundedRectangle(cornerRadius: QRadius.xl))
                .padding(.horizontal, QSpacing.xl)

                Spacer().frame(height: QSpacing.xxl + 4)

                // ── Settings Section ──
                sectionHeader("SETTINGS")

                VStack(spacing: 0) {
                    QSettingsRow(
                        iconName: "cpu",
                        iconColor: QColor.categoryPurple,
                        title: "GPU Acceleration",
                        subtitle: engine.useGPU ? "Metal compute enabled" : "CPU fallback mode"
                    ) {
                        Toggle("", isOn: $engine.useGPU)
                            .tint(QColor.accentPrimary)
                            .labelsHidden()
                    }

                    Divider().padding(.horizontal, QSpacing.lg)

                    QSettingsRow(
                        iconName: "sparkles",
                        iconColor: QColor.categoryTeal,
                        title: "Default Quality",
                        subtitle: "HD (Triangle Mesh)"
                    ) {
                        Image(systemName: "chevron.right")
                            .font(.system(size: 14, weight: .medium))
                            .foregroundColor(QColor.textTertiary)
                    }

                    Divider().padding(.horizontal, QSpacing.lg)

                    QSettingsRow(
                        iconName: "lock.fill",
                        iconColor: QColor.categoryOrange,
                        title: "Default Encryption",
                        subtitle: "Off"
                    ) {
                        Image(systemName: "chevron.right")
                            .font(.system(size: 14, weight: .medium))
                            .foregroundColor(QColor.textTertiary)
                    }
                }
                .padding(.vertical, 4)
                .background(QColor.surfaceCard)
                .clipShape(RoundedRectangle(cornerRadius: QRadius.xl))
                .padding(.horizontal, QSpacing.xl)

                Spacer().frame(height: QSpacing.xxl + 4)

                // ── Storage Section ──
                sectionHeader("STORAGE")

                VStack(spacing: 0) {
                    QSettingsRow(
                        iconName: "externaldrive.fill",
                        iconColor: QColor.accentSecondary,
                        title: "Total Files",
                        subtitle: "\(fileCount) .qtsq files stored"
                    ) {
                        Text("\(fileCount)")
                            .font(QFont.titleSmall())
                            .fontWeight(.bold)
                            .foregroundColor(QColor.accentPrimary)
                    }

                    Divider().padding(.horizontal, QSpacing.lg)

                    QSettingsRow(
                        iconName: "internaldrive.fill",
                        iconColor: QColor.categoryGreen,
                        title: "Disk Usage",
                        subtitle: totalSizeKB >= 1024 ? String(format: "%.1f MB", Double(totalSizeKB) / 1024.0) : "\(totalSizeKB) KB"
                    ) {
                        Text(totalSizeKB >= 1024 ? String(format: "%.1f MB", Double(totalSizeKB) / 1024.0) : "\(totalSizeKB) KB")
                            .font(QFont.titleSmall())
                            .fontWeight(.bold)
                            .foregroundColor(QColor.categoryGreen)
                    }
                }
                .padding(.vertical, 4)
                .background(QColor.surfaceCard)
                .clipShape(RoundedRectangle(cornerRadius: QRadius.xl))
                .padding(.horizontal, QSpacing.xl)

                Spacer().frame(height: QSpacing.xxl + 4)

                // ── About Section ──
                sectionHeader("ABOUT")

                VStack(spacing: 0) {
                    QSettingsRow(
                        iconName: "info.circle.fill",
                        iconColor: QColor.textSecondary,
                        title: "App Version",
                        subtitle: "QTSQ v1.0"
                    ) {
                        Text("1.0")
                            .font(QFont.bodyMedium())
                            .foregroundColor(QColor.textTertiary)
                    }

                    Divider().padding(.horizontal, QSpacing.lg)

                    QSettingsRow(
                        iconName: "bolt.fill",
                        iconColor: QColor.textSecondary,
                        title: "Engine",
                        subtitle: "Gravitational + Metal Compute"
                    ) {
                        EmptyView()
                    }
                }
                .padding(.vertical, 4)
                .background(QColor.surfaceCard)
                .clipShape(RoundedRectangle(cornerRadius: QRadius.xl))
                .padding(.horizontal, QSpacing.xl)

                Spacer().frame(height: 40)
            }
        }
        .background(QColor.backgroundWhite)
    }

    private func sectionHeader(_ title: String) -> some View {
        Text(title)
            .font(QFont.labelSmall())
            .fontWeight(.semibold)
            .tracking(0.8)
            .foregroundColor(QColor.textTertiary)
            .padding(.horizontal, QSpacing.xl)
            .padding(.bottom, QSpacing.sm)
    }
}
