#!/bin/bash
# ─────────────────────────────────────────────────
# Cross-compile libqtsq for iOS Device and Simulator
# Generates an XCFramework to support both slices.
#
# Usage: ./build_ios_lib.sh
# Output: ios/lib/qtsq.xcframework
# ─────────────────────────────────────────────────

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$ROOT_DIR/src"
INC_DIR="$ROOT_DIR/include"
OUT_DIR="$ROOT_DIR/ios/lib"
APP_DIR="$ROOT_DIR/ios/QTSQ"
MIN_IOS="14.0"

# Find SDKs
SDK_IOS=$(xcrun --sdk iphoneos --show-sdk-path)
SDK_SIM=$(xcrun --sdk iphonesimulator --show-sdk-path)

# Use clang to support objective-c++ (.mm) transparently based on extension
CC_IOS=$(xcrun --sdk iphoneos -f clang)
AR_IOS=$(xcrun --sdk iphoneos -f ar)
METAL_IOS=$(xcrun --sdk iphoneos -f metal)
METALLIB_IOS=$(xcrun --sdk iphoneos -f metallib)

CC_SIM=$(xcrun --sdk iphonesimulator -f clang)
AR_SIM=$(xcrun --sdk iphonesimulator -f ar)

mkdir -p "$OUT_DIR"
# Clean up old builds
rm -f "$OUT_DIR/libqtsq_ios.a"
rm -rf "$OUT_DIR/qtsq.xcframework"

TEMP_DIR=$(mktemp -d)
trap "rm -rf $TEMP_DIR" EXIT

# Grab all .c and .mm files! (Phase 6a: + qtc compressor sources)
SRC_FILES=$(find "$SRC_DIR" -name "*.c" -o -name "*.mm" -maxdepth 1)
SRC_FILES="$SRC_FILES $(find "$ROOT_DIR/compressor/src" -name "*.c" -maxdepth 1)"

CFLAGS="-O3 -I$INC_DIR -I$ROOT_DIR/compressor/include -DQTSQ_IOS=1 -fobjc-arc -Wno-everything"

# 1. Build Device (arm64)
echo "─── Building for iOS Device (arm64) ───"
mkdir -p "$TEMP_DIR/device"
OBJ_DEVICE=""
for src in $SRC_FILES; do
    base=$(basename "$src")
    obj="$TEMP_DIR/device/$base.o"
    $CC_IOS -arch arm64 -isysroot $SDK_IOS -miphoneos-version-min=$MIN_IOS $CFLAGS -c "$src" -o "$obj"
    OBJ_DEVICE="$OBJ_DEVICE $obj"
done
$AR_IOS rcs "$TEMP_DIR/libqtsq_device.a" $OBJ_DEVICE

# 2. Build Simulator (arm64)
echo "─── Building for iOS Simulator (arm64) ───"
mkdir -p "$TEMP_DIR/sim_arm64"
OBJ_SIM=""
for src in $SRC_FILES; do
    base=$(basename "$src")
    obj="$TEMP_DIR/sim_arm64/$base.o"
    $CC_SIM -arch arm64 -isysroot $SDK_SIM -mios-simulator-version-min=$MIN_IOS $CFLAGS -c "$src" -o "$obj"
    OBJ_SIM="$OBJ_SIM $obj"
done
$AR_SIM rcs "$TEMP_DIR/libqtsq_sim.a" $OBJ_SIM

# 3. Create XCFramework
echo "─── Creating qtsq.xcframework ───"
xcodebuild -create-xcframework \
    -library "$TEMP_DIR/libqtsq_device.a" \
    -library "$TEMP_DIR/libqtsq_sim.a" \
    -output "$OUT_DIR/qtsq.xcframework"

# 4. Compile Metal Shaders for iOS
echo "─── Compiling Metal Shaders ───"
if [ -f "$SRC_DIR/qtsq_shaders.metal" ]; then
    $METAL_IOS -c -arch arm64 -isysroot $SDK_IOS -miphoneos-version-min=$MIN_IOS \
               "$SRC_DIR/qtsq_shaders.metal" -o "$TEMP_DIR/qtsq_shaders.air"
    $METALLIB_IOS "$TEMP_DIR/qtsq_shaders.air" -o "$APP_DIR/default.metallib"
    echo "✅ Compiled to $APP_DIR/default.metallib"
fi

echo ""
echo "✅ Successfully built: $OUT_DIR/qtsq.xcframework"
