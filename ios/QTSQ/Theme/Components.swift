import SwiftUI

// MARK: - Icon Box (Category-style colored icon in rounded square)

struct QIconBox: View {
    let systemName: String
    let iconColor: Color
    let bgColor: Color
    var size: CGFloat = 44
    var iconSize: CGFloat = 22

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: QRadius.md)
                .fill(bgColor)
                .frame(width: size, height: size)
            Image(systemName: systemName)
                .font(.system(size: iconSize, weight: .medium))
                .foregroundColor(iconColor)
        }
    }
}

// MARK: - Card Container

struct QCard<Content: View>: View {
    var padding: CGFloat = QSpacing.lg
    @ViewBuilder let content: () -> Content

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            content()
        }
        .padding(padding)
        .background(QColor.surfaceCard)
        .clipShape(RoundedRectangle(cornerRadius: QRadius.xl))
    }
}

// MARK: - Section Header

struct QSectionHeader: View {
    let title: String

    var body: some View {
        Text(title)
            .font(QFont.titleSmall())
            .foregroundColor(QColor.textPrimary)
            .padding(.horizontal, QSpacing.xl)
            .padding(.vertical, QSpacing.xs)
    }
}

// MARK: - File Row

struct QFileRow: View {
    let fileName: String
    let subtitle: String
    let iconName: String
    let iconColor: Color
    let iconBg: Color
    var action: () -> Void = {}

    var body: some View {
        Button(action: action) {
            HStack(spacing: 14) {
                QIconBox(
                    systemName: iconName,
                    iconColor: iconColor,
                    bgColor: iconBg
                )

                VStack(alignment: .leading, spacing: 2) {
                    Text(fileName)
                        .font(QFont.label())
                        .foregroundColor(QColor.textPrimary)
                        .lineLimit(1)
                    Text(subtitle)
                        .font(QFont.bodySmall())
                        .foregroundColor(QColor.textTertiary)
                }

                Spacer()

                Image(systemName: "chevron.right")
                    .font(.system(size: 14, weight: .medium))
                    .foregroundColor(QColor.textTertiary)
            }
            .padding(.horizontal, QSpacing.xl)
            .padding(.vertical, 10)
        }
        .buttonStyle(.plain)
    }
}

// MARK: - Info Chip

struct QInfoChip: View {
    let label: String
    let value: String

    var body: some View {
        VStack(spacing: 2) {
            Text(label)
                .font(QFont.labelSmall())
                .foregroundColor(QColor.textSecondary)
            Text(value)
                .font(QFont.label(13))
                .foregroundColor(QColor.textPrimary)
        }
    }
}

// MARK: - Quick Action Card

struct QActionCard: View {
    let title: String
    let subtitle: String
    let iconName: String
    let iconColor: Color
    let iconBg: Color
    var action: () -> Void = {}

    var body: some View {
        Button(action: action) {
            VStack(alignment: .leading, spacing: QSpacing.lg) {
                QIconBox(
                    systemName: iconName,
                    iconColor: iconColor,
                    bgColor: iconBg,
                    size: 48,
                    iconSize: 24
                )

                VStack(alignment: .leading, spacing: 2) {
                    Text(title)
                        .font(QFont.titleSmall())
                        .foregroundColor(QColor.textPrimary)
                    Text(subtitle)
                        .font(QFont.bodySmall())
                        .foregroundColor(QColor.textTertiary)
                }
            }
            .padding(QSpacing.xl)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(QColor.surfaceCard)
            .clipShape(RoundedRectangle(cornerRadius: QRadius.xl))
        }
        .buttonStyle(.plain)
    }
}

// MARK: - Settings Row

struct QSettingsRow<Trailing: View>: View {
    let iconName: String
    let iconColor: Color
    let title: String
    let subtitle: String
    @ViewBuilder let trailing: () -> Trailing

    var body: some View {
        HStack(spacing: 14) {
            ZStack {
                RoundedRectangle(cornerRadius: 10)
                    .fill(iconColor.opacity(0.12))
                    .frame(width: 40, height: 40)
                Image(systemName: iconName)
                    .font(.system(size: QIconSize.md, weight: .medium))
                    .foregroundColor(iconColor)
            }

            VStack(alignment: .leading, spacing: 1) {
                Text(title)
                    .font(QFont.label())
                    .foregroundColor(QColor.textPrimary)
                Text(subtitle)
                    .font(QFont.bodySmall())
                    .foregroundColor(QColor.textTertiary)
            }

            Spacer()

            trailing()
        }
        .padding(.horizontal, QSpacing.lg)
        .padding(.vertical, 14)
    }
}

// MARK: - Accent Button

struct QAccentButton: View {
    let title: String
    let iconName: String
    var isLoading: Bool = false
    var action: () -> Void = {}

    var body: some View {
        Button(action: action) {
            HStack(spacing: 8) {
                if isLoading {
                    ProgressView()
                        .tint(.white)
                } else {
                    Image(systemName: iconName)
                        .font(.system(size: QIconSize.md))
                }
                Text(title)
                    .font(QFont.body())
                    .fontWeight(.semibold)
            }
            .foregroundColor(.white)
            .frame(maxWidth: .infinity)
            .frame(height: 56)
            .background(QColor.accentPrimary)
            .clipShape(RoundedRectangle(cornerRadius: QRadius.lg))
        }
        .padding(.horizontal, QSpacing.xl)
    }
}

// MARK: - Empty State

struct QEmptyState: View {
    let iconName: String
    let title: String
    let subtitle: String

    var body: some View {
        VStack(spacing: QSpacing.md) {
            Image(systemName: iconName)
                .font(.system(size: QIconSize.xxl))
                .foregroundColor(QColor.textTertiary)
            Text(title)
                .font(QFont.bodyMedium())
                .foregroundColor(QColor.textTertiary)
            Text(subtitle)
                .font(QFont.bodySmall())
                .foregroundColor(QColor.textTertiary)
        }
        .padding(QSpacing.xxxl)
        .frame(maxWidth: .infinity)
        .background(QColor.surfaceCard)
        .clipShape(RoundedRectangle(cornerRadius: QRadius.lg))
        .padding(.horizontal, QSpacing.xl)
    }
}

// MARK: - Result Banner

struct QResultBanner: View {
    let message: String
    let isSuccess: Bool

    var body: some View {
        HStack(spacing: QSpacing.md) {
            Image(systemName: isSuccess ? "checkmark.circle.fill" : "xmark.circle.fill")
                .foregroundColor(isSuccess ? QColor.success : QColor.error)
            Text(message)
                .font(QFont.bodyMedium())
                .foregroundColor(QColor.textPrimary)
        }
        .padding(QSpacing.lg)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(isSuccess ? QColor.categoryGreenBg : QColor.categoryRedBg)
        .clipShape(RoundedRectangle(cornerRadius: QRadius.lg))
        .padding(.horizontal, QSpacing.xl)
    }
}
