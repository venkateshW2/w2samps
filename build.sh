#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
BUILD_TYPE="${1:-Debug}"   # pass Release as first arg for release builds

echo "==> Configuring ($BUILD_TYPE)..."
cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -G Ninja \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

echo "==> Building..."
cmake --build "$BUILD_DIR" --parallel

echo ""
echo "✓ Build complete."
echo ""
echo "Artefacts:"
find "$BUILD_DIR/W2Sampler_artefacts" -maxdepth 3 \( -name "*.app" -o -name "*.vst3" -o -name "*.clap" -o -name "*.component" \) 2>/dev/null | sort

echo ""
echo "Run standalone:"
echo "  open \"$BUILD_DIR/W2Sampler_artefacts/$BUILD_TYPE/Standalone/W2 Sampler.app\""
