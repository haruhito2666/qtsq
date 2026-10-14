package com.haruhito.qtsq.screens

import android.util.Log
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ArrowBack
import androidx.compose.material.icons.rounded.Email
import androidx.compose.material.icons.rounded.Lock
import androidx.compose.material.icons.rounded.Person
import androidx.compose.material.icons.rounded.Visibility
import androidx.compose.material.icons.rounded.VisibilityOff
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.clerk.api.Clerk
import com.clerk.api.network.serialization.errorMessage
import com.clerk.api.network.serialization.onFailure
import com.clerk.api.network.serialization.onSuccess
import com.clerk.api.signin.SignIn
import com.clerk.api.signin.attemptSecondFactor
import com.clerk.api.signin.prepareSecondFactor
import com.clerk.api.signup.SignUp
import com.clerk.api.signup.attemptVerification
import com.clerk.api.signup.prepareVerification
import com.clerk.api.signup.update
import kotlinx.coroutines.launch
import com.haruhito.qtsq.ui.theme.*

private const val TAG = "QtsqAuth"

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SignInScreen(
    onNavigateBack: () -> Unit,
    onSignInSuccess: () -> Unit
) {
    var email by remember { mutableStateOf("") }
    var password by remember { mutableStateOf("") }
    var showPassword by remember { mutableStateOf(false) }
    var isLoading by remember { mutableStateOf(false) }
    var errorMsg by remember { mutableStateOf<String?>(null) }
    var needsSecondFactor by remember { mutableStateOf(false) }
    var verificationCode by remember { mutableStateOf("") }
    
    val scope = rememberCoroutineScope()

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(BackgroundWhite)
            .statusBarsPadding(),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        TopAppBar(
            title = { Text("Sign In", color = TextPrimary, style = Typography.titleLarge) },
            navigationIcon = {
                IconButton(onClick = onNavigateBack) {
                    Icon(Icons.AutoMirrored.Rounded.ArrowBack, contentDescription = "Back")
                }
            },
            colors = TopAppBarDefaults.topAppBarColors(containerColor = BackgroundWhite)
        )

        Spacer(modifier = Modifier.height(24.dp))

        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 32.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            OutlinedTextField(
                value = email,
                onValueChange = { email = it },
                label = { Text("Email", color = TextSecondary) },
                leadingIcon = { Icon(Icons.Rounded.Email, contentDescription = null, tint = PrimaryCyan) },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Email),
                modifier = Modifier.fillMaxWidth(),
                enabled = !needsSecondFactor,
                colors = OutlinedTextFieldDefaults.colors(
                    focusedBorderColor = PrimaryCyan,
                    unfocusedBorderColor = LightCyan,
                    focusedContainerColor = CardGray,
                    unfocusedContainerColor = LightCyan.copy(alpha=0.5f)
                ),
                shape = RoundedCornerShape(12.dp)
            )

            if (needsSecondFactor) {
                OutlinedTextField(
                    value = verificationCode,
                    onValueChange = { verificationCode = it },
                    label = { Text("Verification Code", color = TextSecondary) },
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    modifier = Modifier.fillMaxWidth(),
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedBorderColor = PrimaryCyan,
                        unfocusedBorderColor = LightCyan,
                        focusedContainerColor = CardGray,
                        unfocusedContainerColor = LightCyan.copy(alpha=0.5f)
                    ),
                    shape = RoundedCornerShape(12.dp)
                )
            } else {
                OutlinedTextField(
                    value = password,
                    onValueChange = { password = it },
                    label = { Text("Password", color = TextSecondary) },
                    leadingIcon = { Icon(Icons.Rounded.Lock, contentDescription = null, tint = PrimaryCyan) },
                    trailingIcon = {
                        IconButton(onClick = { showPassword = !showPassword }) {
                            Icon(
                                if (showPassword) Icons.Rounded.VisibilityOff else Icons.Rounded.Visibility,
                                contentDescription = "Toggle password visibility",
                                tint = TextTertiary
                            )
                        }
                    },
                    visualTransformation = if (showPassword) VisualTransformation.None else PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                    modifier = Modifier.fillMaxWidth(),
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedBorderColor = PrimaryCyan,
                        unfocusedBorderColor = LightCyan,
                        focusedContainerColor = CardGray,
                        unfocusedContainerColor = LightCyan.copy(alpha=0.5f)
                    ),
                    shape = RoundedCornerShape(12.dp)
                )
            }

            if (errorMsg != null) {
                Text(
                    text = errorMsg!!,
                    color = Error,
                    style = Typography.bodySmall,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.fillMaxWidth()
                )
            }

            Spacer(modifier = Modifier.height(24.dp))

            Button(
                onClick = {
                    if (!needsSecondFactor && (email.isBlank() || password.isBlank())) {
                        errorMsg = "Please fill in all fields."
                        return@Button
                    }
                    if (needsSecondFactor && verificationCode.isBlank()) {
                        errorMsg = "Please enter the verification code."
                        return@Button
                    }
                    scope.launch {
                        isLoading = true
                        errorMsg = null
                        try {
                            if (needsSecondFactor) {
                                Log.d(TAG, "SignIn: verifying second factor code=$verificationCode")
                                val signinUser = Clerk.signIn
                                if (signinUser == null) {
                                    errorMsg = "Sign-in session lost. Please try again."
                                    needsSecondFactor = false
                                    return@launch
                                }
                                val result = signinUser.attemptSecondFactor(
                                    SignIn.AttemptSecondFactorParams.EmailCode(verificationCode.trim())
                                )
                                result.onSuccess { signIn ->
                                    if (signIn.status == SignIn.Status.COMPLETE) {
                                        onSignInSuccess()
                                    } else {
                                        errorMsg = "Additional steps needed: ${signIn.status}"
                                    }
                                }
                                result.onFailure { error ->
                                    Log.e(TAG, "SignIn: second factor failed=${error.errorMessage}")
                                    errorMsg = error.errorMessage ?: "Verification failed."
                                }
                            } else {
                                Log.d(TAG, "SignIn: attempting with email=$email")
                                val result = SignIn.create(
                                    SignIn.CreateParams.Strategy.Password(
                                        identifier = email.trim(),
                                        password = password
                                    )
                                )
                                result.onSuccess { signIn ->
                                    Log.d(TAG, "SignIn: success, status=${signIn.status}")
                                    Log.d(TAG, "SignIn: session=${Clerk.session?.id}, user=${Clerk.user?.firstName}")
                                    if (signIn.status == SignIn.Status.COMPLETE) {
                                        onSignInSuccess()
                                    } else if (signIn.status == SignIn.Status.NEEDS_SECOND_FACTOR) {
                                        Log.d(TAG, "SignIn: needs second factor. preparing...")
                                        needsSecondFactor = true
                                        val prepResult = signIn.prepareSecondFactor()
                                        prepResult.onFailure { prepErr ->
                                            Log.e(TAG, "SignIn: prepareSecondFactor failed=${prepErr.errorMessage}")
                                            errorMsg = prepErr.errorMessage ?: "Failed to send 2FA code."
                                        }
                                    } else {
                                        errorMsg = "Sign in requires additional steps: ${signIn.status}"
                                        Log.w(TAG, "SignIn: non-complete status=${signIn.status}")
                                    }
                                }
                                result.onFailure { error ->
                                    Log.e(TAG, "SignIn: failed=$error, msg=${error.errorMessage}")
                                    errorMsg = error.errorMessage ?: "Sign in failed. Check your credentials."
                                }
                            }
                        } catch (e: Exception) {
                            Log.e(TAG, "SignIn: exception", e)
                            errorMsg = e.localizedMessage ?: "Failed to sign in."
                        } finally {
                            isLoading = false
                        }
                    }
                },
                modifier = Modifier
                    .fillMaxWidth()
                    .height(56.dp),
                elevation = ButtonDefaults.buttonElevation(defaultElevation = 0.dp),
                colors = ButtonDefaults.buttonColors(containerColor = InkBlack, contentColor = Color.White),
                shape = RoundedCornerShape(32.dp),
                enabled = !isLoading
            ) {
                if (isLoading) {
                    CircularProgressIndicator(color = Color.White, modifier = Modifier.size(24.dp))
                } else {
                    Text(if (needsSecondFactor) "Verify Code" else "Sign In", style = Typography.labelLarge.copy(fontSize = 16.sp, fontWeight = FontWeight.Bold))
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SignUpScreen(
    onNavigateBack: () -> Unit,
    onSignUpSuccess: () -> Unit
) {
    var email by remember { mutableStateOf("") }
    var password by remember { mutableStateOf("") }
    var showPassword by remember { mutableStateOf(false) }
    var isLoading by remember { mutableStateOf(false) }
    var errorMsg by remember { mutableStateOf<String?>(null) }
    var needsVerification by remember { mutableStateOf(false) }
    var verificationCode by remember { mutableStateOf("") }
    
    val scope = rememberCoroutineScope()

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(BackgroundWhite)
            .statusBarsPadding(),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        TopAppBar(
            title = { Text("Create Account", color = TextPrimary, style = Typography.titleLarge) },
            navigationIcon = {
                IconButton(onClick = onNavigateBack) {
                    Icon(Icons.AutoMirrored.Rounded.ArrowBack, contentDescription = "Back")
                }
            },
            colors = TopAppBarDefaults.topAppBarColors(containerColor = BackgroundWhite)
        )

        Spacer(modifier = Modifier.height(24.dp))

        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 32.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            var name by remember { mutableStateOf("") }

            OutlinedTextField(
                value = name,
                onValueChange = { name = it },
                label = { Text("Display Name", color = TextSecondary) },
                leadingIcon = { Icon(Icons.Rounded.Person, contentDescription = null, tint = PrimaryCyan) },
                modifier = Modifier.fillMaxWidth(),
                colors = OutlinedTextFieldDefaults.colors(
                    focusedBorderColor = PrimaryCyan,
                    unfocusedBorderColor = LightCyan,
                    focusedContainerColor = CardGray,
                    unfocusedContainerColor = LightCyan.copy(alpha=0.5f)
                ),
                shape = RoundedCornerShape(12.dp)
            )

            OutlinedTextField(
                value = email,
                onValueChange = { email = it },
                label = { Text("Email Address", color = TextSecondary) },
                leadingIcon = { Icon(Icons.Rounded.Email, contentDescription = null, tint = PrimaryCyan) },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Email),
                modifier = Modifier.fillMaxWidth(),
                colors = OutlinedTextFieldDefaults.colors(
                    focusedBorderColor = PrimaryCyan,
                    unfocusedBorderColor = LightCyan,
                    focusedContainerColor = CardGray,
                    unfocusedContainerColor = LightCyan.copy(alpha=0.5f)
                ),
                shape = RoundedCornerShape(12.dp)
            )

            if (needsVerification) {
                OutlinedTextField(
                    value = verificationCode,
                    onValueChange = { verificationCode = it },
                    label = { Text("Email Verification Code", color = TextSecondary) },
                    modifier = Modifier.fillMaxWidth(),
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedBorderColor = PrimaryCyan,
                        unfocusedBorderColor = LightCyan,
                        focusedContainerColor = CardGray,
                        unfocusedContainerColor = LightCyan.copy(alpha=0.5f)
                    ),
                    shape = RoundedCornerShape(12.dp)
                )
            } else {
                OutlinedTextField(
                    value = password,
                    onValueChange = { password = it },
                    label = { Text("Secure Password", color = TextSecondary) },
                    leadingIcon = { Icon(Icons.Rounded.Lock, contentDescription = null, tint = PrimaryCyan) },
                    trailingIcon = {
                        IconButton(onClick = { showPassword = !showPassword }) {
                            Icon(
                                if (showPassword) Icons.Rounded.VisibilityOff else Icons.Rounded.Visibility,
                                contentDescription = "Toggle password visibility",
                                tint = TextTertiary
                            )
                        }
                    },
                    visualTransformation = if (showPassword) VisualTransformation.None else PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                    modifier = Modifier.fillMaxWidth(),
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedBorderColor = PrimaryCyan,
                        unfocusedBorderColor = LightCyan,
                        focusedContainerColor = CardGray,
                        unfocusedContainerColor = LightCyan.copy(alpha=0.5f)
                    ),
                    shape = RoundedCornerShape(12.dp)
                )
            }
            
            Text(
                text = "By signing up, your captured volumes get synced smoothly behind mathematically pristine cryptography gates.",
                style = Typography.bodySmall,
                color = TextTertiary,
                textAlign = TextAlign.Center,
                modifier = Modifier.padding(vertical = 8.dp)
            )

            if (errorMsg != null) {
                Text(
                    text = errorMsg!!,
                    color = Error,
                    style = Typography.bodySmall,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.fillMaxWidth()
                )
            }

            Spacer(modifier = Modifier.height(16.dp))

            Button(
                onClick = {
                    if (!needsVerification && (email.isBlank() || password.length < 6 || name.isBlank())) {
                        errorMsg = "Please fill in all fields (password >= 6 chars)."
                        return@Button
                    }
                    if (needsVerification && verificationCode.isBlank()) {
                        errorMsg = "Please enter the verification code."
                        return@Button
                    }
                    scope.launch {
                        isLoading = true
                        errorMsg = null
                        try {
                            if (needsVerification) {
                                Log.d(TAG, "SignUp: verifying code=$verificationCode")
                                val signupUser = Clerk.signUp
                                if (signupUser == null) {
                                    errorMsg = "Sign-up session lost. Please try again."
                                    needsVerification = false
                                    return@launch
                                }
                                val result = signupUser.attemptVerification(
                                    SignUp.AttemptVerificationParams.EmailCode(verificationCode.trim())
                                )
                                result.onSuccess { signUp ->
                                    Log.d(TAG, "SignUp: verification success, status=${signUp.status}, missingFields=${signUp.missingFields}")
                                    Log.d(TAG, "SignUp: session=${Clerk.session?.id}, user=${Clerk.user?.firstName}")
                                    when (signUp.status) {
                                        SignUp.Status.COMPLETE -> {
                                            onSignUpSuccess()
                                        }
                                        SignUp.Status.MISSING_REQUIREMENTS -> {
                                            // Fill missing fields (likely first_name/last_name/username) and complete
                                            Log.d(TAG, "SignUp: filling missing requirements with name=$name")
                                            val cleanUsername = name.trim().lowercase().replace(Regex("[^a-z0-9]"), "")
                                            val fallbackUsername = email.substringBefore("@").lowercase().replace(Regex("[^a-z0-9]"), "")
                                            val finalUsername = cleanUsername.ifEmpty { fallbackUsername }

                                            val updateResult = signUp.update(
                                                SignUp.SignUpUpdateParams.Standard(
                                                    firstName = name.trim().ifEmpty { null },
                                                    lastName = "",
                                                    username = finalUsername
                                                )
                                            )
                                            updateResult.onSuccess { updated ->
                                                Log.d(TAG, "SignUp: after update status=${updated.status}, missing=${updated.missingFields}")
                                                if (updated.status == SignUp.Status.COMPLETE) {
                                                    onSignUpSuccess()
                                                } else {
                                                    errorMsg = "Still missing fields: ${updated.missingFields}"
                                                }
                                            }
                                            updateResult.onFailure { error ->
                                                Log.e(TAG, "SignUp: update failed=${error.errorMessage}")
                                                errorMsg = error.errorMessage ?: "Failed to complete sign up."
                                            }
                                        }
                                        else -> {
                                            errorMsg = "Additional steps needed: ${signUp.status}"
                                        }
                                    }
                                }
                                result.onFailure { error ->
                                    Log.e(TAG, "SignUp: verification failed=${error.errorMessage}")
                                    errorMsg = error.errorMessage ?: "Verification failed."
                                }
                            } else {
                                Log.d(TAG, "SignUp: creating with name=$name, email=$email")
                                val cleanUsername = name.trim().lowercase().replace(Regex("[^a-z0-9]"), "")
                                val fallbackUsername = email.substringBefore("@").lowercase().replace(Regex("[^a-z0-9]"), "")
                                val finalUsername = cleanUsername.ifEmpty { fallbackUsername }
                                
                                val result = SignUp.create(SignUp.CreateParams.Standard(
                                    emailAddress = email.trim(),
                                    password = password,
                                    firstName = name.trim(),
                                    lastName = "",
                                    username = finalUsername
                                ))
                                result.onSuccess { signUp ->
                                    Log.d(TAG, "SignUp: create success, status=${signUp.status}")
                                    if (signUp.status == SignUp.Status.COMPLETE) {
                                        Log.d(TAG, "SignUp: complete, session=${Clerk.session?.id}")
                                        onSignUpSuccess()
                                    } else {
                                        Log.d(TAG, "SignUp: needs verification, preparing email code")
                                        needsVerification = true
                                        val prepResult = signUp.prepareVerification(
                                            SignUp.PrepareVerificationParams.Strategy.EmailCode()
                                        )
                                        prepResult.onFailure { error ->
                                            Log.e(TAG, "SignUp: prepareVerification failed=${error.errorMessage}")
                                            errorMsg = error.errorMessage ?: "Failed to send verification email."
                                        }
                                    }
                                }
                                result.onFailure { error ->
                                    Log.e(TAG, "SignUp: create failed=${error.errorMessage}")
                                    errorMsg = error.errorMessage ?: "Failed to sign up."
                                }
                            }
                        } catch (e: Exception) {
                            Log.e(TAG, "SignUp: exception", e)
                            errorMsg = e.localizedMessage ?: "Failed to sign up."
                        } finally {
                            isLoading = false
                        }
                    }
                },
                modifier = Modifier
                    .fillMaxWidth()
                    .height(56.dp),
                elevation = ButtonDefaults.buttonElevation(defaultElevation = 0.dp),
                colors = ButtonDefaults.buttonColors(containerColor = InkBlack, contentColor = Color.White),
                shape = RoundedCornerShape(32.dp),
                enabled = !isLoading
            ) {
                if (isLoading) {
                    CircularProgressIndicator(color = Color.White, modifier = Modifier.size(24.dp))
                } else {
                    Text(if (needsVerification) "Verify Code" else "Complete Account", style = Typography.labelLarge.copy(fontSize = 16.sp, fontWeight = FontWeight.Bold))
                }
            }
        }
    }
}
