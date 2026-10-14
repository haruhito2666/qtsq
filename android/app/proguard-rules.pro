# ProGuard Rules for QTSQ Release Build

# ── JNI: native method names must match C symbols (Java_com_haruhito_qtsq_...) ──
# Without this, R8 renames native methods in minified release builds and every
# QtsqEngine call throws UnsatisfiedLinkError / NoSuchMethodError at runtime.
-keepclasseswithmembernames class com.haruhito.qtsq.QtsqEngine {
    native <methods>;
}

# QtsqResult is constructed reflectively from C via FindClass + GetMethodID
# with the exact signature ([BIJIIILjava/lang/String;III)V — keep name+shape.
-keep class com.haruhito.qtsq.QtsqEngine$QtsqResult { *; }

# Keep Supabase & Ktor / SLF4J logging classes that are dynamically loaded
-dontwarn org.slf4j.**
-keep class org.slf4j.** { *; }

# Keep Kotlinx Serialization classes
-keep class kotlinx.serialization.** { *; }
-dontwarn kotlinx.serialization.**

# Keep coroutines
-keep class kotlinx.coroutines.** { *; }
-dontwarn kotlinx.coroutines.**

# Keep Compose UI classes
-keep class androidx.compose.** { *; }
-dontwarn androidx.compose.**

# Ktor exceptions and references
-dontwarn io.ktor.**
-keep class io.ktor.** { *; }

# Supabase gotrue/postgrest
-dontwarn io.github.jan.supabase.**
-keep class io.github.jan.supabase.** { *; }
