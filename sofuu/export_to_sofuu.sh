#!/usr/bin/env bash
# export_to_sofuu.sh — Export Sofuu-Optimized QTSQ Table Engine to Sofuu Runtime
# Author: Haruhito

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOFUU_REPO="${1:-/Users/priyanshuboruah/ai-native-js-runtime}"

echo "=== Exporting Sofuu QTSQ Subsystem ==="
echo "Source: $SCRIPT_DIR"
echo "Destination: $SOFUU_REPO"

# 1. Run local test suite to ensure clean build
echo "1. Building and running test suite..."
make -C "$SCRIPT_DIR" clean
make -C "$SCRIPT_DIR" test

# 2. Target directories in Sofuu repo
EXPORT_INC="$SOFUU_REPO/crates/sofuu-ffi/include"
EXPORT_LIB="$SOFUU_REPO/crates/sofuu-ffi/lib"
mkdir -p "$EXPORT_INC" "$EXPORT_LIB"

# 3. Copy headers and static archive
echo "2. Copying artifacts..."
cp "$SCRIPT_DIR/sofuu_qtsq_table.h" "$EXPORT_INC/sofuu_qtsq_table.h"
cp "$SCRIPT_DIR/sofuu_qtsq_table.c" "$EXPORT_INC/sofuu_qtsq_table.c"
cp "$SCRIPT_DIR/libsofuu_qtsq.a" "$EXPORT_LIB/libsofuu_qtsq.a"

# 4. Also copy to dist if applicable
if [ -d "$SOFUU_REPO/dist" ]; then
    mkdir -p "$SOFUU_REPO/dist/sofuu_qtsq"
    cp "$SCRIPT_DIR/sofuu_qtsq_table.h" "$SOFUU_REPO/dist/sofuu_qtsq/"
    cp "$SCRIPT_DIR/libsofuu_qtsq.a" "$SOFUU_REPO/dist/sofuu_qtsq/"
fi

echo "=== Export Complete ==="
echo "Exported:"
echo "  - $EXPORT_INC/sofuu_qtsq_table.h"
echo "  - $EXPORT_INC/sofuu_qtsq_table.c"
echo "  - $EXPORT_LIB/libsofuu_qtsq.a"
