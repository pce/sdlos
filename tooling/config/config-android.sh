#!/usr/bin/env bash
# Prepare and optionally build the sdlos Android APK.
#
# What this does:
#   1. Verifies deps/SDL3/android-project/ exists (run ./pre_cmake.sh first).
#   2. Copies android/app/build.gradle into SDL3's android-project/app/.
#   3. Copies android/app/src/ (manifests, Java, res) if not already present.
#   4. Builds the debug or release APK via Gradle.
#
# Prerequisites:
#   - Android SDK / NDK  (ANDROID_HOME or installed via Android Studio)
#   - Java 17+           (brew install --cask temurin@17 OR sdk install java 17-tem)
#   - ./pre_cmake.sh     (clones deps/SDL3 with the android-project template)
#
# Usage:
#   ./pre_cmake.sh                             # clone SDL3 first
#   bash config/config-android.sh [debug|release]
#
# Output APK:
#   deps/SDL3/android-project/app/build/outputs/apk/<variant>/app-<variant>.apk

set -euo pipefail
cd "${0%/*}/.."

VARIANT="${1:-debug}"
SDL_ANDROID="${PWD}/deps/SDL3/android-project"
OUR_ANDROID="${PWD}/android"

# ── Preflight checks ────────────────────────────────────────────────────────

if [ ! -d "${SDL_ANDROID}" ]; then
    echo "error: ${SDL_ANDROID} not found."
    echo "       Run ./pre_cmake.sh first to clone SDL3."
    exit 1
fi

if ! command -v java &>/dev/null; then
    echo "error: Java not found. Install Java 17+:"
    echo "  macOS: brew install --cask temurin@17"
    echo "  Linux: sdk install java 17-tem"
    exit 1
fi

JAVA_VER=$(java -version 2>&1 | head -1 | sed 's/.*version "\([0-9]*\).*/\1/')
if [ "${JAVA_VER:-0}" -lt 17 ]; then
    echo "error: Java 17+ required (found version ${JAVA_VER:-?})."
    exit 1
fi

# ── Inject our files into SDL3's android-project ────────────────────────────

echo "[sdlos] Copying build.gradle into SDL3 android-project..."
cp "${OUR_ANDROID}/app/build.gradle" "${SDL_ANDROID}/app/build.gradle"

# Copy our AndroidManifest override if present.
if [ -f "${OUR_ANDROID}/app/src/main/AndroidManifest.xml" ]; then
    mkdir -p "${SDL_ANDROID}/app/src/main"
    cp "${OUR_ANDROID}/app/src/main/AndroidManifest.xml" \
       "${SDL_ANDROID}/app/src/main/AndroidManifest.xml"
    echo "[sdlos] AndroidManifest.xml copied."
fi

# Copy our Java activity if present.
JAVA_SRC="${OUR_ANDROID}/app/src/main/java/dev/sdlos/app"
JAVA_DST="${SDL_ANDROID}/app/src/main/java/dev/sdlos/app"
if [ -d "${JAVA_SRC}" ]; then
    mkdir -p "${JAVA_DST}"
    cp -r "${JAVA_SRC}/." "${JAVA_DST}/"
    echo "[sdlos] Java source files copied."
fi

# ── Build ────────────────────────────────────────────────────────────────────

echo "[sdlos] Building ${VARIANT} APK..."
cd "${SDL_ANDROID}"

GRADLE_TASK="assemble${VARIANT^}"
./gradlew "${GRADLE_TASK}" --info

APK="${SDL_ANDROID}/app/build/outputs/apk/${VARIANT}/app-${VARIANT}.apk"

echo ""
if [ -f "${APK}" ]; then
    echo "APK ready: ${APK}"
    echo ""
    echo "Install on connected device:"
    echo "  adb install -r ${APK}"
else
    echo "warning: APK not found at expected location: ${APK}"
fi

