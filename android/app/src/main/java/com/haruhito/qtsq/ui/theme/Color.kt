package com.haruhito.qtsq.ui.theme

import androidx.compose.ui.graphics.Color

// ─── Core Surfaces — clean light style (inspired by minimal finance UI) ───
// Pure white app background, soft gray cards, generous whitespace, no gradients.
val BackgroundWhite = Color(0xFFFFFFFF)
val SurfaceWhite = Color(0xFFFFFFFF)
val SurfaceCard = Color(0xFFF6F6F7) // soft card gray (#F6F6F7)
val CardGray = Color(0xFFF6F6F7)
val CardGrayPressed = Color(0xFFEDEDEF)
val Divider = Color(0xFFEEEEF0)
val SearchBarBg = Color(0xFFF6F6F7)

// ─── Ink / Brand ───
// Ink black is the primary action color (buttons, selected nav, headlines).
// Blue is reserved for links / informational accents only.
val InkBlack = Color(0xFF1A1A1A)
val PrimaryCyan = InkBlack
val LightCyan = CardGray

// ─── Text ───
val TextPrimary = Color(0xFF131313)
val TextSecondary = Color(0xFF6E6E73)
val TextTertiary = Color(0xFF9CA3AF)

// ─── Accents ───
val AccentPrimary = InkBlack
val AccentPrimaryLight = CardGray
val AccentSecondary = Color(0xFF4C8DFF) // link blue only
val LinkBlue = Color(0xFF2F7CF6)

// ─── Icon tiles — matte solid pastels, no gradients ───
val TileBlue = Color(0xFF5B9DFF)
val TileOrange = Color(0xFFFF9E5E)
val TilePurple = Color(0xFFA78BFA)
val TileDark = Color(0xFF222222)
val TileGreen = Color(0xFF34C77B)
val TilePink = Color(0xFFFF8FA3)
val TileIconTint = Color.White

// ─── Category Colors (legacy names kept — now matte) ───
val CategoryPurple = Color.White
val CategoryPurpleBg = TilePurple
val CategoryRed = Color.White
val CategoryRedBg = TilePink
val CategoryTeal = Color.White
val CategoryTealBg = TileBlue
val CategoryGreen = Color.White
val CategoryGreenBg = TileGreen
val CategoryOrange = Color.White
val CategoryOrangeBg = TileOrange
val CategoryBlueBg = TileBlue

// ─── Navigation ───
val NavActive = InkBlack
val NavInactive = Color(0xFFB8B8BE)
val NavPillBg = Color(0xFFDCE9FF) // light blue floating pill

// ─── Semantic ───
val Success = Color(0xFF16A34A)
val SuccessBg = Color(0xFFE8F7EE)
val Error = Color(0xFFEF4444)
val ErrorBg = Color(0xFFFDECEC)
val Warning = Color(0xFFF59E0B)

// ─── Legacy aliases ───
val BackgroundOffWhite = BackgroundWhite
val GlassBorder = Color(0x3BFFFFFF)
val GlassHighlight = Color(0x6BFFFFFF)
