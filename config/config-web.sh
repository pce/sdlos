#!/usr/bin/env bash
# Configure sdlos for WebAssembly using Emscripten.
#
# Prerequisites:
#   Install emsdk:  https://emscripten.org/docs/getting_started/downloads.html
#   Activate:       source <emsdk_dir>/emsdk_env.sh
#
# Usage:
#   source <emsdk_dir>/emsdk_env.sh
#   bash config/config-web.sh [Debug|Release]
#
# Build:
#   cmake --build build/web -j
#
# Serve locally:
#   cd build/web && python3 -m http.server 8080

set -euo pipefail
cd "${0%/*}/.."

BUILD_TYPE="${1:-Release}"

# Locate the Emscripten CMake toolchain file.
if [ -n "${EMSDK:-}" ]; then
    EMSDK_TOOLCHAIN="${EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake"
elif command -v emcc &>/dev/null; then
    EMCC_DIR="$(dirname "$(command -v emcc)")"
    EMSDK_TOOLCHAIN="${EMCC_DIR}/cmake/Modules/Platform/Emscripten.cmake"
else
    echo "error: Emscripten not found in PATH and EMSDK is not set."
    echo ""
    echo "Install emsdk:"
    echo "  git clone https://github.com/emscripten-core/emsdk.git"
    echo "  cd emsdk && ./emsdk install latest && ./emsdk activate latest"
    echo "  source ./emsdk_env.sh"
    exit 1
fi

if [ ! -f "${EMSDK_TOOLCHAIN}" ]; then
    echo "error: Emscripten toolchain not found at: ${EMSDK_TOOLCHAIN}"
    exit 1
fi

echo "[sdlos] Emscripten toolchain: ${EMSDK_TOOLCHAIN}"

cmake \
    -DCMAKE_TOOLCHAIN_FILE="${EMSDK_TOOLCHAIN}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DSDLTTF_VENDORED=ON \
    -DSDLTTF_FREETYPE_VENDORED=ON \
    -DSDLTTF_HARFBUZZ_VENDORED=ON \
    -DSDLOS_ENABLE_TESTS=OFF \
    -B "build/web" \
    -S .

echo ""
echo "Web/WASM build configured in build/web/"
echo "Build:  cmake --build build/web -j"
echo "Serve:  cd build/web && python3 -m http.server 8080"
echo "Open:   http://localhost:8080/styleguide.html"

