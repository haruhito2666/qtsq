package com.haruhito.qtsq.ui.theme

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

// ─────────────────────────────────────────────
// QTSQ minimal design system.
// Reference style: white background, soft gray cards (20dp),
// generous 20-24dp padding, small gray labels, bold black headlines,
// matte icon tiles, floating pill bottom nav.
// UI-only — no logic changes.
// ─────────────────────────────────────────────

/** Standard screen padding used everywhere: 20dp sides. */
val ScreenHPadding = 20.dp

/** Soft gray card modifier — flat, no shadow, no gradient. */
fun Modifier.softCard(radius: androidx.compose.ui.unit.Dp = CardRadius): Modifier =
    this.clip(RoundedCornerShape(radius)).background(CardGray)

// ── Top bar: title only (no avatar / bell icons) ──
@Composable
fun QtsqTopBar(
    title: String,
    onBellClick: (() -> Unit)? = null,
    onAvatarClick: (() -> Unit)? = null
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = ScreenHPadding)
            .padding(top = 12.dp, bottom = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.Center
    ) {
        Text(
            text = title,
            style = Typography.titleSmall.copy(fontWeight = FontWeight.Bold, fontSize = 17.sp),
            color = TextPrimary,
            textAlign = TextAlign.Center
        )
    }
}

// ── Stat header: small gray label + large bold value + optional delta ──
@Composable
fun QtsqStatHeader(
    label: String,
    valueMain: String,
    valueSuffix: String = "",
    deltaText: String? = null,
    modifier: Modifier = Modifier
) {
    Column(modifier = modifier.padding(horizontal = ScreenHPadding)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                text = label,
                style = Typography.bodySmall.copy(fontSize = 15.sp, fontWeight = FontWeight.Medium),
                color = TextSecondary
            )
            Spacer(Modifier.width(6.dp))
            Icon(
                Icons.Rounded.Visibility,
                contentDescription = null,
                tint = TextTertiary,
                modifier = Modifier.size(16.dp)
            )
        }
        Spacer(Modifier.height(6.dp))
        Row(verticalAlignment = Alignment.Bottom) {
            Text(
                text = valueMain,
                style = Typography.displayMedium.copy(fontSize = 42.sp, fontWeight = FontWeight.ExtraBold),
                color = TextPrimary
            )
            if (valueSuffix.isNotEmpty()) {
                Text(
                    text = valueSuffix,
                    style = Typography.displayMedium.copy(fontSize = 42.sp, fontWeight = FontWeight.Bold),
                    color = TextTertiary,
                    modifier = Modifier.padding(bottom = 1.dp)
                )
            }
            if (deltaText != null) {
                Spacer(Modifier.width(10.dp))
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier.padding(bottom = 10.dp)
                ) {
                    Icon(
                        Icons.Rounded.NorthEast,
                        contentDescription = null,
                        tint = Success,
                        modifier = Modifier.size(16.dp)
                    )
                    Text(
                        text = deltaText,
                        style = Typography.labelSmall.copy(fontSize = 14.sp, fontWeight = FontWeight.Bold),
                        color = Success
                    )
                }
            }
        }
    }
}

// ── Info banner: soft gray rounded row with icon + text + chevron ──
@Composable
fun QtsqInfoBanner(
    text: String,
    onClick: (() -> Unit)? = null,
    modifier: Modifier = Modifier
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .padding(horizontal = ScreenHPadding)
            .clip(RoundedCornerShape(CardRadius))
            .background(CardGray)
            .clickable(enabled = onClick != null) { onClick?.invoke() }
            .padding(horizontal = 14.dp, vertical = 13.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(
            modifier = Modifier
                .size(28.dp)
                .clip(CircleShape)
                .background(Color.White),
            contentAlignment = Alignment.Center
        ) {
            Icon(Icons.Rounded.SouthWest, null, tint = Success, modifier = Modifier.size(15.dp))
        }
        Spacer(Modifier.width(10.dp))
        Text(
            text = text,
            style = Typography.bodyMedium.copy(fontSize = 13.sp),
            color = TextPrimary,
            modifier = Modifier.weight(1f),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
        Icon(Icons.Rounded.ChevronRight, null, tint = TextTertiary, modifier = Modifier.size(18.dp))
    }
}

// ── Quick action card: 20dp gray card, matte tile top, bold title + gray subtitle ──
@Composable
fun QtsqActionCard(
    title: String,
    subtitle: String,
    icon: ImageVector,
    tileColor: Color,
    modifier: Modifier = Modifier,
    onClick: () -> Unit
) {
    Column(
        modifier = modifier
            .clip(RoundedCornerShape(CardRadius))
            .background(CardGray)
            .clickable { onClick() }
            .padding(16.dp)
    ) {
        Box(
            modifier = Modifier
                .size(40.dp)
                .clip(RoundedCornerShape(TileRadius))
                .background(tileColor),
            contentAlignment = Alignment.Center
        ) {
            Icon(icon, contentDescription = title, tint = Color.White, modifier = Modifier.size(20.dp))
        }
        Spacer(Modifier.height(28.dp))
        Text(
            text = title,
            style = Typography.titleSmall.copy(fontSize = 17.sp, fontWeight = FontWeight.Bold),
            color = TextPrimary
        )
        Spacer(Modifier.height(2.dp))
        Text(
            text = subtitle,
            style = Typography.bodySmall.copy(fontSize = 13.sp, fontWeight = FontWeight.Medium),
            color = TextTertiary,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
    }
}

// ── Section title: 14sp semibold black ──
@Composable
fun QtsqSectionTitle(text: String, modifier: Modifier = Modifier) {
    Text(
        text = text,
        style = Typography.titleSmall.copy(fontSize = 17.sp, fontWeight = FontWeight.Bold),
        color = TextPrimary,
        modifier = modifier.padding(horizontal = ScreenHPadding)
    )
}

// ── Transaction / file row: icon tile in tinted circle + title + amount + chevron ──
@Composable
fun QtsqListRow(
    title: String,
    subtitle: String,
    amount: String? = null,
    icon: ImageVector,
    tileColor: Color = TileBlue,
    onClick: () -> Unit
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable { onClick() }
            .padding(horizontal = ScreenHPadding, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(
            modifier = Modifier
                .size(40.dp)
                .clip(CircleShape)
                .background(tileColor.copy(alpha = 0.12f)),
            contentAlignment = Alignment.Center
        ) {
            Icon(icon, null, tint = tileColor, modifier = Modifier.size(18.dp))
        }
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Text(
                text = title,
                style = Typography.bodyMedium.copy(fontWeight = FontWeight.Medium, fontSize = 14.sp),
                color = TextPrimary,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
            Text(
                text = subtitle,
                style = Typography.bodySmall.copy(fontSize = 12.sp),
                color = TextTertiary,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }
        if (amount != null) {
            Text(
                text = amount,
                style = Typography.bodyMedium.copy(fontWeight = FontWeight.SemiBold, fontSize = 14.sp),
                color = TextPrimary
            )
        }
    }
}

// ── Segmented control: gray pill with white selected tab ──
@Composable
fun QtsqSegmented(
    options: List<String>,
    selected: Int,
    onSelect: (Int) -> Unit,
    modifier: Modifier = Modifier
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .padding(horizontal = ScreenHPadding)
            .clip(RoundedCornerShape(PillRadius))
            .background(CardGray)
            .padding(4.dp),
        horizontalArrangement = Arrangement.spacedBy(4.dp)
    ) {
        options.forEachIndexed { i, label ->
            val isSel = i == selected
            Box(
                modifier = Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(PillRadius))
                    .background(if (isSel) Color.White else Color.Transparent)
                    .clickable { onSelect(i) }
                    .padding(vertical = 9.dp),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    text = label,
                    style = Typography.labelLarge.copy(
                        fontSize = 13.sp,
                        fontWeight = if (isSel) FontWeight.SemiBold else FontWeight.Medium
                    ),
                    color = if (isSel) TextPrimary else TextTertiary
                )
            }
        }
    }
}

// ── Primary button: full-width black pill, white text ──
@Composable
fun QtsqPrimaryButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    loading: Boolean = false
) {
    Button(
        onClick = onClick,
        enabled = enabled && !loading,
        modifier = modifier
            .fillMaxWidth()
            .height(56.dp),
        shape = RoundedCornerShape(PillRadius),
        colors = ButtonDefaults.buttonColors(
            containerColor = InkBlack,
            contentColor = Color.White,
            disabledContainerColor = CardGray,
            disabledContentColor = TextTertiary
        ),
        elevation = ButtonDefaults.buttonElevation(0.dp, 0.dp, 0.dp, 0.dp, 0.dp)
    ) {
        if (loading) {
            CircularProgressIndicator(color = Color.White, modifier = Modifier.size(22.dp), strokeWidth = 2.dp)
        } else {
            Text(
                text = text,
                style = Typography.labelLarge.copy(fontSize = 15.sp, fontWeight = FontWeight.SemiBold),
                color = if (enabled) Color.White else TextTertiary
            )
        }
    }
}

// ── Secondary button: white with gray border ──
@Composable
fun QtsqSecondaryButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier
) {
    OutlinedButton(
        onClick = onClick,
        modifier = modifier.fillMaxWidth().height(54.dp),
        shape = RoundedCornerShape(PillRadius),
        colors = ButtonDefaults.outlinedButtonColors(contentColor = TextPrimary),
        border = ButtonDefaults.outlinedButtonBorder.copy(width = 1.dp)
    ) {
        Text(text, style = Typography.labelLarge.copy(fontSize = 15.sp, fontWeight = FontWeight.SemiBold))
    }
}

// ── Search bar: light gray rounded field ──
@Composable
fun QtsqSearchBar(
    value: String,
    onValueChange: (String) -> Unit,
    placeholder: String,
    modifier: Modifier = Modifier
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .padding(horizontal = ScreenHPadding)
            .clip(RoundedCornerShape(16.dp))
            .background(SearchBarBg)
            .padding(horizontal = 14.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Icon(Icons.Rounded.Search, null, tint = TextTertiary, modifier = Modifier.size(18.dp))
        Spacer(Modifier.width(8.dp))
        BasicTextField(
            value = value,
            onValueChange = onValueChange,
            modifier = Modifier.weight(1f),
            singleLine = true,
            textStyle = Typography.bodyMedium.copy(color = TextPrimary, fontSize = 14.sp),
            decorationBox = { inner ->
                if (value.isEmpty()) {
                    Text(placeholder, style = Typography.bodyMedium.copy(fontSize = 14.sp), color = TextTertiary)
                }
                inner()
            }
        )
    }
}

// ── Empty state card ──
@Composable
fun QtsqEmptyCard(
    icon: ImageVector,
    title: String,
    subtitle: String,
    modifier: Modifier = Modifier
) {
    Column(
        modifier = modifier
            .fillMaxWidth()
            .padding(horizontal = ScreenHPadding)
            .clip(RoundedCornerShape(CardRadius))
            .background(CardGray)
            .padding(vertical = 36.dp, horizontal = 24.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Icon(icon, null, tint = TextTertiary, modifier = Modifier.size(32.dp))
        Spacer(Modifier.height(12.dp))
        Text(title, style = Typography.bodyMedium.copy(fontWeight = FontWeight.Medium), color = TextSecondary)
        Spacer(Modifier.height(4.dp))
        Text(subtitle, style = Typography.bodySmall, color = TextTertiary, textAlign = TextAlign.Center)
    }
}

// ── Filter pill (small selectable chip) ──
@Composable
fun QtsqFilterPill(
    label: String,
    icon: ImageVector? = null,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier
) {
    Row(
        modifier = modifier
            .clip(RoundedCornerShape(PillRadius))
            .background(if (selected) InkBlack else CardGray)
            .clickable { onClick() }
            .padding(horizontal = 14.dp, vertical = 9.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        if (icon != null) {
            Icon(icon, null, tint = if (selected) Color.White else TextSecondary, modifier = Modifier.size(15.dp))
            Spacer(Modifier.width(6.dp))
        }
        Text(
            text = label,
            style = Typography.labelLarge.copy(fontSize = 13.sp, fontWeight = FontWeight.Medium),
            color = if (selected) Color.White else TextPrimary
        )
    }
}
