#!/bin/bash
set -euo pipefail

# pre_cmake.sh
# Fetch third-party libraries. Rules:
#  - deps/ is reserved for fetched core dependencies (SDL3, Dawn) and should be kept
#    untouched by local-only libraries.
#  - libs/ is the local workspace for temporary/local libraries (e.g. SDL3_dawn).
#    This is where SDL3_dawn will live by default if you provide it locally, or
#    where we will clone it if you explicitly set SDL3_DAWN_REPO.
#  - If you already have a local libs/SDL3_dawn, the script will prefer that and
#    will NOT clone into deps/.
#
# Environment variables:
#  - SDL3_REPO / SDL3_BRANCH (defaults provided)
#  - DAWN_REPO / DAWN_BRANCH (defaults provided)
#  - SDL3_DAWN_REPO / SDL3_DAWN_BRANCH (optional; if not set we will NOT clone SDL3_dawn)
#  - SDL3_LOCAL_PATH / DAWN_LOCAL_PATH / SDL3_DAWN_LOCAL_PATH to override local paths.
#  - SKIP_SDL3_DAWN=1 to skip any action related to SDL3_dawn entirely.
#
# Example:
#   ./pre_cmake.sh
#   SDL3_DAWN_REPO="https://github.com/yourorg/SDL3_dawn.git" ./pre_cmake.sh

# Ensure base directories exist
mkdir -p deps libs

# Core deps always go into deps/ (unless overridden by *_LOCAL_PATH)
DEPS_DIR="deps"
SDL_DIR="${SDL3_LOCAL_PATH:-${DEPS_DIR}/SDL3}"
SDL_IMAGE_DIR="${SDL_IMAGE_LOCAL_PATH:-${DEPS_DIR}/SDL_image}"
SDL_MIXER_DIR="${SDL_MIXER_LOCAL_PATH:-${DEPS_DIR}/SDL_mixer}"
SDL_TTF_DIR="${SDL_TTF_LOCAL_PATH:-${DEPS_DIR}/SDL_ttf}"

# SDL3_dawn adapter is no longer used (Dawn has been removed). Local libraries can still live in libs/
LIBS_DIR="libs"

# Repo & branch defaults (can be overridden via environment variables)
SDL3_REPO="${SDL3_REPO:-https://github.com/libsdl-org/SDL.git}"
SDL3_BRANCH="${SDL3_BRANCH:-release-3.4.x}"
SDL_IMAGE_REPO="${SDL_IMAGE_REPO:-https://github.com/libsdl-org/SDL_image.git}"
SDL_IMAGE_BRANCH="${SDL_IMAGE_BRANCH:-main}"
SDL_MIXER_REPO="${SDL_MIXER_REPO:-https://github.com/libsdl-org/SDL_mixer.git}"
SDL_MIXER_BRANCH="${SDL_MIXER_BRANCH:-main}"
SDL_TTF_REPO="${SDL_TTF_REPO:-https://github.com/libsdl-org/SDL_ttf.git}"
SDL_TTF_BRANCH="${SDL_TTF_BRANCH:-main}"

# Helper: clone if missing
clone_if_missing() {
    local dir="$1"; shift
    local repo="$1"; shift
    local branch="${1:-}"
    if [ -d "${dir}" ]; then
        echo "Already present: ${dir}"
        return 0
    fi
    echo "Cloning ${repo} ${branch:+(branch: ${branch})} into ${dir}"
    if [ -n "${branch}" ]; then
        git clone --branch "${branch}" "${repo}" "${dir}"
    else
        git clone "${repo}" "${dir}"
    fi
}

# 1) Ensure deps/ core checkouts (SDL3 and addons)
if [ -n "${SDL3_LOCAL_PATH:-}" ]; then
    echo "Using local SDL3 at ${SDL3_LOCAL_PATH}"
else
    clone_if_missing "${SDL_DIR}" "${SDL3_REPO}" "${SDL3_BRANCH}"
fi

# SDL_image
if [ -n "${SDL_IMAGE_LOCAL_PATH:-}" ]; then
    echo "Using local SDL_image at ${SDL_IMAGE_LOCAL_PATH}"
else
    clone_if_missing "${SDL_IMAGE_DIR}" "${SDL_IMAGE_REPO}" "${SDL_IMAGE_BRANCH}"
fi

# SDL_mixer
if [ -n "${SDL_MIXER_LOCAL_PATH:-}" ]; then
    echo "Using local SDL_mixer at ${SDL_MIXER_LOCAL_PATH}"
else
    clone_if_missing "${SDL_MIXER_DIR}" "${SDL_MIXER_REPO}" "${SDL_MIXER_BRANCH}"
fi

# SDL_ttf
if [ -n "${SDL_TTF_LOCAL_PATH:-}" ]; then
    echo "Using local SDL_ttf at ${SDL_TTF_LOCAL_PATH}"
else
    clone_if_missing "${SDL_TTF_DIR}" "${SDL_TTF_REPO}" "${SDL_TTF_BRANCH}"
fi

# 2) Dawn adapter handling removed: Dawn is no longer a dependency.
# If you have local adapters or special integrations, place them under libs/ and they'll be left untouched.

echo "Done."
echo " - SDL3 at:        ${SDL_DIR}"
echo " - SDL_image at:   ${SDL_IMAGE_DIR}"
echo " - SDL_mixer at:   ${SDL_MIXER_DIR}"
echo " - SDL_ttf at:     ${SDL_TTF_DIR}"
