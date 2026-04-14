#!/usr/bin/env bash
# Configure sdlos for iOS using the Xcode generator.
#
# Usage:
#   bash config/config-ios-xcode.sh [Debug|Release]
#
# Output:
#   build/ios/sdlos.xcodeproj
#
# Then open with:
#   open build/ios/sdlos.xcodeproj
#
# To set a signing team at configure time:
#   SDLOS_IOS_TEAM=XXXXXXXXXX bash config/config-ios-xcode.sh
#
# Environment variables:
#   SDLOS_IOS_TEAM           Apple Developer Team ID (e.g. XXXXXXXXXX)
#   SDLOS_IOS_BUNDLE_PREFIX  Reverse-DNS bundle prefix (default: dev.sdlos)
#   SDLOS_IOS_DEPLOYMENT     Minimum iOS version      (default: 16.0)

set -euo pipefail
cd "${0%/*}/.."

BUILD_TYPE="${1:-Debug}"
TEAM="${SDLOS_IOS_TEAM:-}"
BUNDLE_PREFIX="${SDLOS_IOS_BUNDLE_PREFIX:-dev.sdlos}"
DEPLOYMENT="${SDLOS_IOS_DEPLOYMENT:-16.0}"

CMAKE_ARGS=(
    -G "Xcode"
    -DCMAKE_SYSTEM_NAME="iOS"
    -DCMAKE_OSX_ARCHITECTURES="arm64"
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${DEPLOYMENT}"
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

    # iOS does not allow user-space shared libraries — build everything static.
    -DBUILD_SHARED_LIBS=OFF

    # SDL3 static
    -DSDL_SHARED=OFF
    -DSDL_STATIC=ON
    -DSDL_TEST_LIBRARY=OFF

    # SDL3_ttf static + vendored deps (no system FreeType/HarfBuzz required)
    -DSDLTTF_SHARED=OFF
    -DSDLTTF_STATIC=ON
    -DSDLTTF_VENDORED=ON
    -DSDLTTF_FREETYPE_VENDORED=ON
    -DSDLTTF_HARFBUZZ_VENDORED=ON
    # plutosvg/plutovg = SVG-based color emoji — unused; sdlos uses COLR/Twemoji
    -DSDLTTF_PLUTOSVG=OFF

    # SDL3_image static
    -DSDLIMAGE_SHARED=OFF
    -DSDLIMAGE_STATIC=ON

    # SDL3_mixer static
    -DSDLMIXER_SHARED=OFF
    -DSDLMIXER_STATIC=ON

    # disable desktop-only features on iOS
    -DSDLOS_ENABLE_TESTS=OFF

    # pass bundle identity info into CMake for Info.plist generation
    -DSDLOS_IOS_BUNDLE_PREFIX="${BUNDLE_PREFIX}"
)

# Inject the Team ID only if provided — avoids Xcode prompt during configure.
if [ -n "${TEAM}" ]; then
    CMAKE_ARGS+=(-DSDLOS_IOS_TEAM="${TEAM}")
fi

# --fresh deletes CMakeCache.txt before configuring (requires CMake 3.24+).
# This guarantees BUILD_SHARED_LIBS=OFF and all other cache variables take
# effect even when re-configuring an existing build directory.
cmake --fresh "${CMAKE_ARGS[@]}" -B "build/ios" -S .

echo ""
echo "iOS Xcode project generated in build/ios/"
echo "Open with:  open build/ios/sdlos.xcodeproj"
echo ""
if [ -z "${TEAM}" ]; then
    echo "NOTE: No signing team set. Open the project, select a target,"
    echo "      and set your Team under Signing & Capabilities."
    echo "      Or re-run: SDLOS_IOS_TEAM=<your-team-id> $0"
fi

