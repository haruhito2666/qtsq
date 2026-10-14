package com.haruhito.qtsq

import android.os.Bundle
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.compose.BackHandler
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.view.WindowCompat
import com.haruhito.qtsq.screens.*
import com.haruhito.qtsq.ui.theme.*
import com.clerk.api.Clerk

private const val TAG = "QtsqMain"

class MainActivity : ComponentActivity() {
    private val qtsqEngine = QtsqEngine()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        
        // Edge-to-edge: draw behind system bars, transparent nav bar
        enableEdgeToEdge()
        WindowCompat.setDecorFitsSystemWindows(window, false)
        window.statusBarColor = android.graphics.Color.TRANSPARENT
        window.navigationBarColor = android.graphics.Color.TRANSPARENT

        setContent {
            QTSQTheme {
                val context = LocalContext.current
                var currentScreen by remember { mutableStateOf("loading") }
                var viewerFilePath by remember { mutableStateOf("") }
                var selectedNavTab by remember { mutableIntStateOf(0) }
                // Track if user is in manual auth flow to prevent LaunchedEffect override
                var inAuthFlow by remember { mutableStateOf(false) }
    // Non-null asks ConvertScreen to open its picker with this MIME filter.
    var convertPickerFilter by remember { mutableStateOf<String?>(null) }

                val clerkSession by Clerk.sessionFlow.collectAsState()
                val isClerkReady by Clerk.isInitialized.collectAsState()

                // React to auth state changes
                LaunchedEffect(isClerkReady, clerkSession) {
                    Log.d(TAG, "Auth state: isClerkReady=$isClerkReady, session=${clerkSession?.id}, inAuthFlow=$inAuthFlow, currentScreen=$currentScreen")
                    Log.d(TAG, "Auth state: user=${Clerk.user?.firstName}, email=${Clerk.user?.primaryEmailAddress?.emailAddress}")
                    
                    if (!isClerkReady) {
                        Log.d(TAG, "Clerk not ready yet, staying on loading")
                        return@LaunchedEffect
                    }
                    
                    if (clerkSession != null) {
                        // Session exists — always go to nav regardless of auth flow
                        Log.d(TAG, "Session active! Going to nav")
                        inAuthFlow = false
                        currentScreen = "nav"
                    } else if (!inAuthFlow) {
                        // No session and not in auth flow — show onboarding
                        Log.d(TAG, "No session, showing onboarding")
                        currentScreen = "onboarding"
                    }
                    // If inAuthFlow==true and session==null, stay on current auth screen
                }

                if (currentScreen == "loading") {
                    Box(modifier = Modifier.fillMaxSize().background(BackgroundWhite), contentAlignment = Alignment.Center) {
                        CircularProgressIndicator(color = PrimaryCyan)
                    }
                    return@QTSQTheme
                }

                // ── System Back Navigation Handling ──
                BackHandler(enabled = currentScreen != "nav" || selectedNavTab != 0) {
                    when {
                        currentScreen == "camera" -> {
                            selectedNavTab = 0
                            currentScreen = "nav"
                        }
                        currentScreen == "viewer" || currentScreen == "decrypt" || currentScreen == "audio" -> {
                            currentScreen = "nav"
                        }
                        currentScreen == "capture" -> {
                            selectedNavTab = 0
                            currentScreen = "nav"
                        }
                        currentScreen == "signin" || currentScreen == "signup" -> {
                            inAuthFlow = false
                            currentScreen = "onboarding"
                        }
                        currentScreen == "onboarding" -> {
                            (context as? android.app.Activity)?.finish()
                        }
                        currentScreen == "nav" && selectedNavTab != 0 -> {
                            selectedNavTab = 0
                        }
                    }
                }

                // Full-screen overlays (no bottom nav)
                when (currentScreen) {
                    "onboarding" -> {
                        OnboardingScreen(
                            onNavigateToSignIn = {
                                inAuthFlow = true
                                currentScreen = "signin"
                            },
                            onNavigateToSignUp = {
                                inAuthFlow = true
                                currentScreen = "signup"
                            }
                        )
                        return@QTSQTheme
                    }
                    "signin" -> {
                        SignInScreen(
                            onNavigateBack = {
                                inAuthFlow = false
                                currentScreen = "onboarding"
                            },
                            onSignInSuccess = {
                                Log.d(TAG, "SignIn success callback fired!")
                                inAuthFlow = false
                                currentScreen = "nav"
                            }
                        )
                        return@QTSQTheme
                    }
                    "signup" -> {
                        SignUpScreen(
                            onNavigateBack = {
                                inAuthFlow = false
                                currentScreen = "onboarding"
                            },
                            onSignUpSuccess = {
                                Log.d(TAG, "SignUp success callback fired!")
                                inAuthFlow = false
                                currentScreen = "nav"
                            }
                        )
                        return@QTSQTheme
                    }
                    "camera" -> {
                        CameraScreen(
                            qtsqEngine = qtsqEngine,
                            onClose = { 
                                selectedNavTab = 0
                                currentScreen = "nav" 
                            },
                            onCaptured = { file ->
                                viewerFilePath = file.absolutePath
                                currentScreen = "viewer"
                            }
                        )
                        return@QTSQTheme
                    }
                    "audio" -> {
                        AudioScreen(
                            qtsqEngine = qtsqEngine,
                            onClose = { currentScreen = "nav" },
                            onRecorded = { file ->
                                viewerFilePath = file.absolutePath
                                currentScreen = "viewer"
                            }
                        )
                        return@QTSQTheme
                    }
                    "capture" -> {
                        SceneCaptureScreen(
                            qtsqEngine = qtsqEngine,
                            onClose = {
                                selectedNavTab = 0
                                currentScreen = "nav"
                            }
                        )
                        return@QTSQTheme
                    }
                    "viewer" -> {
                        ViewerScreen(
                            qtsqEngine = qtsqEngine,
                            filePath = viewerFilePath,
                            onClose = { currentScreen = "nav" }
                        )
                        return@QTSQTheme
                    }
                    "decrypt" -> {
                        DecryptScreen(
                            qtsqEngine = qtsqEngine,
                            onClose = { currentScreen = "nav" }
                        )
                        return@QTSQTheme
                    }
                }

                // ── Main content with floating pill nav overlaid (no blocking bar bg) ──
                Scaffold(
                    containerColor = androidx.compose.ui.graphics.Color.White,
                    // Don't reserve space for system bars here — screens handle
                    // status padding themselves and the pill sits on the raw
                    // gesture inset, so it can drop flush to the bottom.
                    contentWindowInsets = WindowInsets(0, 0, 0, 0)
                ) { innerPadding ->
                    Box(
                        modifier = Modifier
                            .fillMaxSize()
                            .padding(top = innerPadding.calculateTopPadding())
                    ) {
                        Box(modifier = Modifier.fillMaxSize()) {
                            when (selectedNavTab) {
                            0 -> DashboardScreen(
                                qtsqEngine = qtsqEngine,
                                onCameraClick = { currentScreen = "camera" },
                                onAudioClick = { currentScreen = "audio" },
            // The Convert screen is driven by the nav tab, not by
                            // currentScreen, so switch tabs and ask it to open the
                            // picker pre-filtered to video.
                            onVideoClick = {
                                convertPickerFilter = "video/*"
                                selectedNavTab = 3
                            },
                                onSceneCaptureClick = { currentScreen = "capture" },
                                onConvertClick = { selectedNavTab = 3 },
                                onDecryptClick = { currentScreen = "decrypt" },
                                onFileClick = { filePath ->
                                    viewerFilePath = filePath
                                    currentScreen = "viewer"
                                }
                            )
                            1 -> LibraryScreen(
                                onFileClick = { filePath ->
                                    viewerFilePath = filePath
                                    currentScreen = "viewer"
                                }
                            )
                            // 2 = Camera (handled above as overlay)
                            3 -> ConvertScreen(
                                qtsqEngine = qtsqEngine,
                                initialPickerFilter = convertPickerFilter,
                                onPickerFilterConsumed = { convertPickerFilter = null },
                                onClose = { selectedNavTab = 0 }
                            )
                            4 -> ProfileScreen(qtsqEngine = qtsqEngine)
                        }
                        }
                        // Floating pill — overlaid, centered at bottom, transparent outside
                        Box(
                            modifier = Modifier.align(Alignment.BottomCenter)
                        ) {
                            QtsqBottomNav(
                                selectedTab = selectedNavTab,
                                onTabSelected = { index ->
                                    selectedNavTab = index
                                    when (index) {
                                        2 -> currentScreen = "camera"
                                        else -> currentScreen = "nav"
                                    }
                                }
                            )
                        }
                    }
                }
            }
        }
    }
}

// ── Bottom Navigation Bar ──

private data class NavItem(
    val label: String,
    val icon: ImageVector,
    val activeIcon: ImageVector
)

private val navItems = listOf(
    NavItem("Home", Icons.Rounded.Home, Icons.Rounded.Home),
    NavItem("Library", Icons.Rounded.GridView, Icons.Rounded.GridView),
    NavItem("Camera", Icons.Rounded.CameraAlt, Icons.Rounded.CameraAlt), // center FAB
    NavItem("Convert", Icons.Rounded.SwapHoriz, Icons.Rounded.SwapHoriz),
    NavItem("Profile", Icons.Rounded.Person, Icons.Rounded.Person)
)

@Composable
private fun QtsqBottomNav(
    selectedTab: Int,
    onTabSelected: (Int) -> Unit
) {
    // Floating centered pill — transparent outer so content shows through,
    // only the rounded pill itself has a background. Sits just above the
    // system gesture bar.
    val visible = listOf(0, 1, 2, 4)
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .background(androidx.compose.ui.graphics.Color.Transparent)
            .navigationBarsPadding()
            .padding(bottom = 0.dp),
        contentAlignment = Alignment.Center
    ) {
        Row(
            modifier = Modifier
                .wrapContentWidth()
                .clip(androidx.compose.foundation.shape.RoundedCornerShape(32.dp))
                .background(NavPillBg)
                .padding(horizontal = 8.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(4.dp, Alignment.CenterHorizontally)
        ) {
            visible.forEach { logicalIndex ->
                val item = navItems[logicalIndex]
                val isSelected = selectedTab == logicalIndex
                Box(
                    modifier = Modifier
                        .size(width = 64.dp, height = 44.dp)
                        .clip(androidx.compose.foundation.shape.RoundedCornerShape(22.dp))
                        .background(if (isSelected) SurfaceWhite else androidx.compose.ui.graphics.Color.Transparent)
                        .clickable { onTabSelected(logicalIndex) },
                    contentAlignment = Alignment.Center
                ) {
                    Icon(
                        when (logicalIndex) {
                            0 -> Icons.Rounded.Home
                            1 -> Icons.Rounded.PieChart
                            2 -> Icons.Rounded.CameraAlt
                            else -> Icons.Rounded.Settings
                        },
                        contentDescription = item.label,
                        tint = if (isSelected) InkBlack else NavInactive,
                        modifier = Modifier.size(24.dp)
                    )
                }
            }
        }
    }
}
