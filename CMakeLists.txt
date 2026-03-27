cmake_minimum_required(VERSION 3.25)

project(sdlos
    VERSION 0.1.0
    DESCRIPTION "SDL3 GPU desktop environment"
    LANGUAGES C CXX
)

# -----------------------------------------------------------------------------
# C++ standard
# -----------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD          23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)   # -std=c++23, not -std=gnu++23

# Export compile_commands.json for clangd / LSP tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -----------------------------------------------------------------------------
# Default build type
# -----------------------------------------------------------------------------

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "[sdlos] No build type specified — defaulting to RelWithDebInfo")
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "CMake build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY
        STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# -----------------------------------------------------------------------------
# vcpkg toolchain (optional — set VCPKG_ROOT in the environment)
# -----------------------------------------------------------------------------

if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain file")
    message(STATUS "[sdlos] vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
endif()

# -----------------------------------------------------------------------------
# SDL3 — vendored copy in deps/ takes priority, otherwise find_package
# -----------------------------------------------------------------------------

set(_SDL3_VENDOR "${CMAKE_SOURCE_DIR}/deps/SDL3")

if(EXISTS "${_SDL3_VENDOR}/CMakeLists.txt")
    message(STATUS "[sdlos] SDL3: using vendored copy at ${_SDL3_VENDOR}")
    add_subdirectory("${_SDL3_VENDOR}" EXCLUDE_FROM_ALL)
else()
    message(STATUS "[sdlos] SDL3: searching via find_package")
    find_package(SDL3 REQUIRED CONFIG)
endif()

# -----------------------------------------------------------------------------
# Optional SDL add-ons (image / mixer / ttf)
#
# Each addon is only added when found under deps/. EXCLUDE_FROM_ALL keeps it
# out of the default build graph so a bare `cmake --build` does not compile
# the addon unless sdlos explicitly links it.
# -----------------------------------------------------------------------------

# Usage: sdlos_optional_addon(<VAR_SUFFIX> <subdir_name>)
#   Sets SDL_<VAR_SUFFIX>_AVAILABLE to ON/OFF.
macro(sdlos_optional_addon VAR_SUFFIX SUBDIR)
    set(SDL_${VAR_SUFFIX}_AVAILABLE OFF)
    if(EXISTS "${CMAKE_SOURCE_DIR}/deps/${SUBDIR}/CMakeLists.txt")
        message(STATUS "[sdlos] ${SUBDIR}: found in deps/")
        add_subdirectory("${CMAKE_SOURCE_DIR}/deps/${SUBDIR}" EXCLUDE_FROM_ALL)
        set(SDL_${VAR_SUFFIX}_AVAILABLE ON)
    else()
        message(STATUS "[sdlos] ${SUBDIR}: not found in deps/ (optional)")
    endif()
endmacro()

sdlos_optional_addon(IMAGE SDL_image)
sdlos_optional_addon(MIXER SDL_mixer)

# SDL_ttf requires FreeType (and optionally HarfBuzz).  Neither is guaranteed
# to be installed system-wide, so we tell SDL_ttf to use its own vendored
# copies.  These cache variables must be set BEFORE add_subdirectory so that
# SDL_ttf's CMakeLists.txt sees them when it runs find_package(Freetype).
#
# Override with -DSDLTTF_VENDORED=OFF on the command line if you explicitly
# want to use system libraries.
if(EXISTS "${CMAKE_SOURCE_DIR}/deps/SDL_ttf/CMakeLists.txt")
    set(SDLTTF_VENDORED          ON CACHE BOOL
        "Use SDL_ttf's bundled FreeType + HarfBuzz (sdlos default)" FORCE)
    set(SDLTTF_FREETYPE_VENDORED ON CACHE BOOL
        "Use SDL_ttf's bundled FreeType (sdlos default)" FORCE)
    set(SDLTTF_HARFBUZZ_VENDORED ON CACHE BOOL
        "Use SDL_ttf's bundled HarfBuzz (sdlos default)" FORCE)
endif()

sdlos_optional_addon(TTF   SDL_ttf)

# -----------------------------------------------------------------------------
# Sources
# -----------------------------------------------------------------------------

set(SDLOS_SOURCES
    src/main.cxx
    src/os.cxx
    src/desktop.cxx
    src/app.cxx
    src/app_catalog.cxx
    src/virtual_file_system.cxx
    src/notification_center.cxx
    src/event_bus.cxx
    src/sdl_renderer.cxx
    src/render_tree.cxx
    src/text_renderer.cxx
    src/widgets/input_box.cxx
    src/debug/layout_debug.cxx
)

add_executable(sdlos ${SDLOS_SOURCES})

# -----------------------------------------------------------------------------
# Include paths
# -----------------------------------------------------------------------------

target_include_directories(sdlos PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/core"
    "${CMAKE_SOURCE_DIR}/src/widgets"
)

# -----------------------------------------------------------------------------
# Compiler flags
# -----------------------------------------------------------------------------

# --- Warnings (Clang and GCC share the same set) ----------------------------

set(_SDLOS_WARN_FLAGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wcast-align
    -Wunused
    -Wconversion
    -Wsign-conversion
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -Wno-unused-parameter    # Too noisy in SDL-heavy callback code.
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
    target_compile_options(sdlos PRIVATE
        ${_SDLOS_WARN_FLAGS}
        -Wno-c99-extensions
        # -stdlib=libc++ is intentionally NOT set here: the llvm22-custom preset
        # injects it via CMAKE_CXX_FLAGS so it applies uniformly without being
        # duplicated on every target.
    )
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(sdlos PRIVATE
        ${_SDLOS_WARN_FLAGS}
        -Wmisleading-indentation
        -Wduplicated-cond
        -Wlogical-op
    )
elseif(MSVC)
    target_compile_options(sdlos PRIVATE
        /W4
        /permissive-
        /w14640    # thread-unsafe static member init
        /w14826    # implicit sign extension
        /w15038    # initialisation order
    )
endif()

# --- Per-configuration flags (generator-expression safe) --------------------
#
# These work correctly for both single-config generators (Makefiles, Ninja)
# and multi-config generators (Xcode, Visual Studio).

if(NOT MSVC)
    target_compile_options(sdlos PRIVATE
        $<$<CONFIG:Debug>:          -O0 -g3>
        $<$<CONFIG:RelWithDebInfo>: -O2 -g>
        $<$<CONFIG:Release>:        -O3>
        $<$<CONFIG:MinSizeRel>:     -Os>
    )
else()
    target_compile_options(sdlos PRIVATE
        $<$<CONFIG:Debug>:          /Od /Zi>
        $<$<CONFIG:RelWithDebInfo>: /O2 /Zi>
        $<$<CONFIG:Release>:        /O2>
        $<$<CONFIG:MinSizeRel>:     /O1>
    )
endif()

# --- Preprocessor defines ---------------------------------------------------

target_compile_definitions(sdlos PRIVATE
    # Expose BUILD_TYPE_DEBUG so OS.cxx can choose windowed vs fullscreen.
    $<$<CONFIG:Debug>:BUILD_TYPE_DEBUG>
    # Suppress MSVC's min/max macros which conflict with std::min/max.
    $<$<CXX_COMPILER_ID:MSVC>:NOMINMAX WIN32_LEAN_AND_MEAN>
)

# -----------------------------------------------------------------------------
# Link SDL3
# -----------------------------------------------------------------------------

target_link_libraries(sdlos PRIVATE SDL3::SDL3)

# -----------------------------------------------------------------------------
# Link optional SDL add-ons
#
# Usage: sdlos_link_addon(<display-name> <primary-target> <fallback-target>)
# Tries the primary CMake target first (SDL3-style), then the legacy alias.
# -----------------------------------------------------------------------------

macro(sdlos_link_addon DISPLAY PRIMARY FALLBACK)
    if(TARGET "${PRIMARY}")
        target_link_libraries(sdlos PRIVATE "${PRIMARY}")
        message(STATUS "[sdlos] linked: ${PRIMARY}")
    elseif(TARGET "${FALLBACK}")
        target_link_libraries(sdlos PRIVATE "${FALLBACK}")
        message(STATUS "[sdlos] linked: ${FALLBACK}")
    else()
        message(STATUS "[sdlos] ${DISPLAY}: no known CMake target found — skipping link")
    endif()
endmacro()

if(SDL_IMAGE_AVAILABLE)
    sdlos_link_addon(SDL_image "SDL3_image::SDL3_image" "SDL_image::SDL_image")
endif()

if(SDL_MIXER_AVAILABLE)
    sdlos_link_addon(SDL_mixer "SDL3_mixer::SDL3_mixer" "SDL_mixer::SDL_mixer")
endif()

if(SDL_TTF_AVAILABLE)
    sdlos_link_addon(SDL_ttf "SDL3_ttf::SDL3_ttf" "SDL_ttf::SDL_ttf")
    # Expose SDL_TTF_AVAILABLE as a preprocessor define so text_renderer.cxx
    # can conditionally compile the SDL_ttf code paths at the C++ level.
    target_compile_definitions(sdlos PRIVATE SDL_TTF_AVAILABLE=1)
endif()

# -----------------------------------------------------------------------------
# Status summary
# -----------------------------------------------------------------------------

message(STATUS "")
message(STATUS "┌─ sdlos ───────────────────────────── ──── ── ─ ─")
message(STATUS "│  Version     : ${PROJECT_VERSION}")
message(STATUS "│  Build type  : ${CMAKE_BUILD_TYPE}")
message(STATUS "│  C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "│  Compiler    : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "│  Compiler bin: ${CMAKE_CXX_COMPILER}")
message(STATUS "│  SDL_image   : ${SDL_IMAGE_AVAILABLE}")
message(STATUS "│  SDL_mixer   : ${SDL_MIXER_AVAILABLE}")
message(STATUS "│  SDL_ttf     : ${SDL_TTF_AVAILABLE}")
message(STATUS "└──────────────────────────────────────── ─")
message(STATUS "")
