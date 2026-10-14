package com.haruhito.qtsq.screens

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.slideInVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.haruhito.qtsq.ui.theme.*
import kotlinx.coroutines.delay

@Composable
fun OnboardingScreen(
    onNavigateToSignIn: () -> Unit,
    onNavigateToSignUp: () -> Unit
) {
    var showButtons by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        delay(900)
        showButtons = true
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.White)
            .statusBarsPadding()
            .navigationBarsPadding()
            .padding(horizontal = 24.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Spacer(Modifier.weight(1f))

        // Brand — tight, bold, letterspaced like reference headers
        Text(
            text = "QTSQ",
            style = Typography.displayMedium.copy(fontSize = 44.sp, fontWeight = FontWeight.ExtraBold),
            color = TextPrimary,
            textAlign = TextAlign.Center
        )
        Spacer(Modifier.height(10.dp))
        Text(
            text = "Compress images, video and audio\ninto tiny .qtsq files",
            style = Typography.bodyMedium.copy(fontSize = 14.sp),
            color = TextSecondary,
            textAlign = TextAlign.Center,
            lineHeight = 20.sp
        )

        Spacer(Modifier.weight(1f))

        AnimatedVisibility(
            visible = showButtons,
            enter = fadeIn(tween(500)) + slideInVertically(tween(500)) { it / 3 }
        ) {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                QtsqPrimaryButton(text = "Sign Up", onClick = onNavigateToSignUp)
                QtsqSecondaryButton(text = "Sign In", onClick = onNavigateToSignIn)
                Spacer(Modifier.height(8.dp))
            }
        }
    }
}
