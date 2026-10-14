package com.haruhito.qtsq.screens

import android.net.Uri
import android.util.Log
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.*
import androidx.compose.material.icons.automirrored.rounded.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.haruhito.qtsq.QtsqEngine
import com.haruhito.qtsq.ui.theme.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.InputStream

@Composable
fun DecryptScreen(qtsqEngine: QtsqEngine, onClose: () -> Unit) {
    val context = LocalContext.current
    val coroutineScope = rememberCoroutineScope()
    var isDecrypting by remember { mutableStateOf(false) }
    var selectedUri by remember { mutableStateOf<Uri?>(null) }
    var password by remember { mutableStateOf("") }
    var decryptionResult by remember { mutableStateOf<String?>(null) }
    val isSuccess = decryptionResult?.startsWith("Decrypted") == true

    val filePickerLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument(),
        onResult = { uri: Uri? ->
            selectedUri = uri
            decryptionResult = null
        }
    )

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.White)
            .statusBarsPadding()
            .navigationBarsPadding()
    ) {
        // ── Header: back + centered title like reference ──
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            IconButton(onClick = onClose) {
                Icon(Icons.AutoMirrored.Rounded.ArrowBack, null, tint = TextPrimary)
            }
            Text(
                "Decrypt", style = Typography.titleSmall.copy(fontSize = 15.sp, fontWeight = FontWeight.SemiBold),
                color = TextPrimary, modifier = Modifier.weight(1f), textAlign = TextAlign.Center
            )
            Spacer(Modifier.size(48.dp))
        }

        Spacer(Modifier.height(12.dp))
        QtsqStatHeader(label = "Encrypted file", valueMain = if (selectedUri == null) "No file" else "Ready", valueSuffix = "")

        Spacer(Modifier.height(16.dp))

        // ── File card — soft gray, matte tile ──
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = ScreenHPadding)
                .clip(RoundedCornerShape(CardRadius))
                .background(CardGray)
                .clickable { filePickerLauncher.launch(arrayOf("*/*")) }
                .padding(vertical = 32.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Box(
                modifier = Modifier.size(48.dp).clip(RoundedCornerShape(TileRadius)).background(TileDark),
                contentAlignment = Alignment.Center
            ) {
                Icon(Icons.Rounded.LockOpen, null, tint = Color.White, modifier = Modifier.size(24.dp))
            }
            Spacer(Modifier.height(14.dp))
            Text(
                if (selectedUri == null) "Select a .qtsq file" else "File selected",
                style = Typography.bodyMedium.copy(fontWeight = FontWeight.Medium, fontSize = 14.sp),
                color = if (selectedUri == null) TextSecondary else Success
            )
            Text(
                "Open and decrypt encrypted archives",
                style = Typography.bodySmall.copy(fontSize = 12.sp),
                color = TextTertiary, modifier = Modifier.padding(top = 4.dp)
            )
        }

        Spacer(Modifier.height(14.dp))

        OutlinedTextField(
            value = password,
            onValueChange = { password = it },
            label = { Text("Password (optional)", style = Typography.bodyMedium.copy(fontSize = 14.sp), color = TextTertiary) },
            visualTransformation = PasswordVisualTransformation(),
            singleLine = true,
            modifier = Modifier.fillMaxWidth().padding(horizontal = ScreenHPadding),
            shape = RoundedCornerShape(16.dp),
            colors = OutlinedTextFieldDefaults.colors(
                focusedBorderColor = InkBlack,
                unfocusedBorderColor = Color.Transparent,
                focusedContainerColor = CardGray,
                unfocusedContainerColor = CardGray,
                focusedLabelColor = TextSecondary
            )
        )

        Spacer(Modifier.weight(1f))

        if (decryptionResult != null) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = ScreenHPadding)
                    .clip(RoundedCornerShape(16.dp))
                    .background(if (isSuccess) SuccessBg else ErrorBg)
                    .padding(14.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Icon(
                    if (isSuccess) Icons.Rounded.CheckCircle else Icons.Rounded.Error,
                    null,
                    tint = if (isSuccess) Success else Error,
                    modifier = Modifier.size(20.dp)
                )
                Spacer(Modifier.width(10.dp))
                Text(decryptionResult!!, style = Typography.bodyMedium.copy(fontSize = 13.sp), color = TextPrimary)
            }
            Spacer(Modifier.height(12.dp))
        }

        // ── Black pill button like reference Continue ──
        Box(Modifier.padding(horizontal = ScreenHPadding)) {
            QtsqPrimaryButton(
                text = if (isDecrypting) "Decrypting…" else "Decrypt",
                loading = isDecrypting,
                enabled = selectedUri != null,
                onClick = {
                    val uri = selectedUri ?: return@QtsqPrimaryButton
                    isDecrypting = true
                    coroutineScope.launch {
                        val resultMsg = withContext(Dispatchers.IO) {
                            try {
                                val inputStream: InputStream? = context.contentResolver.openInputStream(uri)
                                val bytes = inputStream?.readBytes()
                                inputStream?.close()
                                if (bytes != null) {
                                    val result = qtsqEngine.decompressBytes(bytes, password.ifEmpty { null })
                                    if (result != null) {
                                        val outFile = File(context.filesDir, "decrypted_${System.currentTimeMillis()}.bin")
                                        outFile.writeBytes(result.data)
                                        "Decrypted to ${outFile.name} (${result.data.size} bytes)"
                                    } else "Decryption failed internally."
                                } else "Failed to read file."
                            } catch (e: Exception) {
                                Log.e("DecryptScreen", "Error", e)
                                "Error: ${e.message}"
                            }
                        }
                        decryptionResult = resultMsg
                        isDecrypting = false
                    }
                }
            )
        }
        Spacer(Modifier.height(20.dp))
    }
}
