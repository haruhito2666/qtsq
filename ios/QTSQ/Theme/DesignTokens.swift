import SwiftUI

// MARK: - Colors

enum QColor {
    // ── Core Surfaces ──
    static let backgroundWhite = Color.white
    static let surfaceCard = Color(hex: 0xF5F6F8)
    static let surfaceWhite = Color.white
    static let divider = Color(hex: 0xE5E7EB)

    // ── Text ──
    static let textPrimary = Color(hex: 0x1A1D26)
    static let textSecondary = Color(hex: 0x6B7280)
    static let textTertiary = Color(hex: 0x9CA3AF)

    // ── Accents ──
    static let accentPrimary = Color(hex: 0x5A4FCF)
    static let accentPrimaryLight = Color(hex: 0xEDE9FE)
    static let accentSecondary = Color(hex: 0x007BFF)

    // ── Category Colors ──
    static let categoryPurple = Color(hex: 0x7C3AED)
    static let categoryPurpleBg = Color(hex: 0xF3EEFF)
    static let categoryRed = Color(hex: 0xEF4444)
    static let categoryRedBg = Color(hex: 0xFEF0F0)
    static let categoryTeal = Color(hex: 0x2CCFBC)
    static let categoryTealBg = Color(hex: 0xECFDF5)
    static let categoryGreen = Color(hex: 0x10B981)
    static let categoryGreenBg = Color(hex: 0xECFDF5)
    static let categoryOrange = Color(hex: 0xF59E0B)
    static let categoryOrangeBg = Color(hex: 0xFFF8E1)
    static let categoryBlueBg = Color(hex: 0xEFF6FF)

    // ── Navigation ──
    static let navActive = accentPrimary
    static let navInactive = Color(hex: 0x9CA3AF)

    // ── Semantic ──
    static let success = Color(hex: 0x10B981)
    static let error = Color(hex: 0xEF4444)
    static let warning = Color(hex: 0xF59E0B)
}

// MARK: - Spacing

enum QSpacing {
    static let xs: CGFloat = 4
    static let sm: CGFloat = 8
    static let md: CGFloat = 12
    static let lg: CGFloat = 16
    static let xl: CGFloat = 20
    static let xxl: CGFloat = 24
    static let xxxl: CGFloat = 32
}

// MARK: - Corner Radius

enum QRadius {
    static let sm: CGFloat = 8
    static let md: CGFloat = 12
    static let lg: CGFloat = 16
    static let xl: CGFloat = 20
    static let full: CGFloat = 999
}

// MARK: - Icon Size

enum QIconSize {
    static let sm: CGFloat = 16
    static let md: CGFloat = 20
    static let lg: CGFloat = 24
    static let xl: CGFloat = 28
    static let xxl: CGFloat = 36
}

// MARK: - Typography

enum QFont {
    static func heading(_ size: CGFloat = 22) -> Font {
        .system(size: size, weight: .bold, design: .rounded)
    }
    static func title(_ size: CGFloat = 18) -> Font {
        .system(size: size, weight: .semibold, design: .rounded)
    }
    static func titleSmall(_ size: CGFloat = 15) -> Font {
        .system(size: size, weight: .semibold, design: .rounded)
    }
    static func body(_ size: CGFloat = 16) -> Font {
        .system(size: size, weight: .regular)
    }
    static func bodyMedium(_ size: CGFloat = 14) -> Font {
        .system(size: size, weight: .regular)
    }
    static func bodySmall(_ size: CGFloat = 12) -> Font {
        .system(size: size, weight: .regular)
    }
    static func label(_ size: CGFloat = 14) -> Font {
        .system(size: size, weight: .medium)
    }
    static func labelSmall(_ size: CGFloat = 11) -> Font {
        .system(size: size, weight: .medium)
    }
    static func display(_ size: CGFloat = 42) -> Font {
        .system(size: size, weight: .semibold, design: .rounded)
    }
    static var mono: Font {
        .system(size: 13, weight: .regular, design: .monospaced)
    }
}

// MARK: - Color Extension

extension Color {
    init(hex: UInt, alpha: Double = 1.0) {
        self.init(
            .sRGB,
            red: Double((hex >> 16) & 0xFF) / 255.0,
            green: Double((hex >> 8) & 0xFF) / 255.0,
            blue: Double(hex & 0xFF) / 255.0,
            opacity: alpha
        )
    }
}
