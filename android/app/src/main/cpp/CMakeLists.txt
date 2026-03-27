# android/app/src/main/cpp/CMakeLists.txt
# Android NDK CMake — builds libsdlos.so (the sdlos engine + jade host).
#
# This file is referenced from android/app/build.gradle via
# externalNativeBuild.cmake.path.  Gradle invokes it through the NDK CMake
# integration; the Android toolchain file is applied automatically.
#
# SDLOS_ROOT is passed in from build.gradle as a CMake argument.
# It resolves to the project root (the directory containing the top-level
# CMakeLists.txt).  The default falls back to the relative path from this
# file's location to the root:
#   android/app/src/main/cpp/ -> root = ../../../../../

cmake_minimum_required(VERSION 3.25)
project(sdlos_android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD          23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# Resolve the sdlos source root.
if(NOT DEFINED SDLOS_ROOT)
    set(SDLOS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../")
endif()
get_filename_component(SDLOS_ROOT "${SDLOS_ROOT}" ABSOLUTE)
message(STATUS "[sdlos-android] source root: ${SDLOS_ROOT}")

# ── SDL3 ─────────────────────────────────────────────────────────────────────
# SDL3 is vendored at deps/SDL3/ relative to the project root.
set(_SDL3_VENDOR "${SDLOS_ROOT}/deps/SDL3")
if(NOT EXISTS "${_SDL3_VENDOR}/CMakeLists.txt")
    message(FATAL_ERROR
        "[sdlos-android] deps/SDL3 not found at ${_SDL3_VENDOR}. "
        "Run ./pre_cmake.sh from the project root to clone SDL3 first."
    )
endif()
add_subdirectory("${_SDL3_VENDOR}" SDL3 EXCLUDE_FROM_ALL)

# ── Optional SDL add-ons ──────────────────────────────────────────────────────
# SDL_ttf (for text rendering)
set(_SDLTTF_VENDOR "${SDLOS_ROOT}/deps/SDL_ttf")
if(EXISTS "${_SDLTTF_VENDOR}/CMakeLists.txt")
    set(SDLTTF_VENDORED          ON CACHE BOOL "" FORCE)
    set(SDLTTF_FREETYPE_VENDORED ON CACHE BOOL "" FORCE)
    set(SDLTTF_HARFBUZZ_VENDORED ON CACHE BOOL "" FORCE)
    add_subdirectory("${_SDLTTF_VENDOR}" SDL_ttf EXCLUDE_FROM_ALL)
    set(SDL_TTF_AVAILABLE ON)
    message(STATUS "[sdlos-android] SDL_ttf: ON")
else()
    set(SDL_TTF_AVAILABLE OFF)
    message(STATUS "[sdlos-android] SDL_ttf: not found (optional)")
endif()

# SDL_image
set(_SDLIMAGE_VENDOR "${SDLOS_ROOT}/deps/SDL_image")
if(EXISTS "${_SDLIMAGE_VENDOR}/CMakeLists.txt")
    add_subdirectory("${_SDLIMAGE_VENDOR}" SDL_image EXCLUDE_FROM_ALL)
    set(SDL_IMAGE_AVAILABLE ON)
    message(STATUS "[sdlos-android] SDL_image: ON")
else()
    set(SDL_IMAGE_AVAILABLE OFF)
    message(STATUS "[sdlos-android] SDL_image: not found (optional)")
endif()

# SDL_mixer
set(_SDLMIXER_VENDOR "${SDLOS_ROOT}/deps/SDL_mixer")
if(EXISTS "${_SDLMIXER_VENDOR}/CMakeLists.txt")
    add_subdirectory("${_SDLMIXER_VENDOR}" SDL_mixer EXCLUDE_FROM_ALL)
    set(SDL_MIXER_AVAILABLE ON)
    message(STATUS "[sdlos-android] SDL_mixer: ON")
else()
    set(SDL_MIXER_AVAILABLE OFF)
    message(STATUS "[sdlos-android] SDL_mixer: not found (optional)")
endif()

# ── Third-party: simdutf, utfcpp, fastgltf ───────────────────────────────────
include(FetchContent)

set(SIMDUTF_TESTS      OFF CACHE BOOL "" FORCE)
set(SIMDUTF_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SIMDUTF_TOOLS      OFF CACHE BOOL "" FORCE)
FetchContent_Declare(simdutf
    URL https://github.com/simdutf/simdutf/archive/refs/tags/v8.2.0.tar.gz)
FetchContent_MakeAvailable(simdutf)

FetchContent_Declare(utfcpp
    URL      https://github.com/nemtrif/utfcpp/archive/refs/tags/v4.0.9.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(utfcpp)
if(NOT TARGET utf8cpp::utf8cpp)
    if(TARGET utf8cpp)
        add_library(utf8cpp::utf8cpp ALIAS utf8cpp)
    else()
        FetchContent_GetProperties(utfcpp SOURCE_DIR utfcpp_SOURCE_DIR)
        add_library(_utf8cpp_iface INTERFACE)
        target_include_directories(_utf8cpp_iface INTERFACE "${utfcpp_SOURCE_DIR}/source")
        add_library(utf8cpp::utf8cpp ALIAS _utf8cpp_iface)
    endif()
endif()

set(FASTGLTF_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(FASTGLTF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(fastgltf
    URL      https://github.com/spnda/fastgltf/archive/refs/tags/v0.9.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(fastgltf)

# ── sdlos engine sources ──────────────────────────────────────────────────────
set(S "${SDLOS_ROOT}/src")   # shorthand

set(SDLOS_ENGINE_SOURCES
    ${S}/render_tree.cxx        ${S}/sdl_renderer.cxx
    ${S}/text_renderer.cxx      ${S}/image_cache.cxx
    ${S}/virtual_file_system.cxx
    ${S}/event_bus.cxx
    ${S}/style_applier.cxx      ${S}/style_draw.cxx      ${S}/css_loader.cxx
    ${S}/hit_test.cxx           ${S}/node_events.cxx
    ${S}/jade/jade_lexer.cxx    ${S}/jade/jade_parser.cxx
    ${S}/vfs/vfs.cxx            ${S}/vfs/local_mount.cxx
    ${S}/vfs/union_mount.cxx    ${S}/vfs/mem_mount.cxx
    ${S}/video_texture.cxx
    ${S}/widgets/input_text_box.cxx  ${S}/widgets/select_box.cxx
    ${S}/widgets/number_dragger.cxx  ${S}/widgets/range_slider.cxx
    ${S}/debug/layout_debug.cxx
    ${S}/gltf/gltf_scene.cxx
    ${S}/frame_graph/pug_parser.cxx
    ${S}/frame_graph/compiled_graph.cxx
    ${S}/frame_graph/frame_graph.cxx
)

# ── libmain.so — SDL3 Android loads a shared library named "main" ─────────────
# The default jade entry is the styleguide.  Set SDLOS_ANDROID_APP to change it.
set(SDLOS_ANDROID_APP "styleguide" CACHE STRING
    "Name of the jade app to launch on Android (must match examples/apps/<name>/<name>.jade)")

set(_jade_entry "${SDLOS_ROOT}/examples/apps/${SDLOS_ANDROID_APP}/${SDLOS_ANDROID_APP}.jade")
set(_behavior   "${SDLOS_ROOT}/examples/apps/${SDLOS_ANDROID_APP}/${SDLOS_ANDROID_APP}.cxx")

if(NOT EXISTS "${_jade_entry}")
    message(WARNING "[sdlos-android] jade entry not found: ${_jade_entry}")
endif()

add_library(main SHARED
    ${SDLOS_ENGINE_SOURCES}
    ${S}/jade_host.cxx
)

target_include_directories(main PRIVATE
    "${S}"
    "${S}/core"
    "${S}/widgets"
    "${S}/jade"
    "${S}/debug"
    "${S}/vfs"
)

target_compile_definitions(main PRIVATE
    SDLOS_JADE_ENTRY="${_jade_entry}"
    SDLOS_APP_NAME="${SDLOS_ANDROID_APP}"
    SDLOS_APP_BEHAVIOR="${_behavior}"
    SDL_IMAGE_AVAILABLE=$<BOOL:${SDL_IMAGE_AVAILABLE}>
    SDL_MIXER_AVAILABLE=$<BOOL:${SDL_MIXER_AVAILABLE}>
    SDL_TTF_AVAILABLE=$<BOOL:${SDL_TTF_AVAILABLE}>
)

target_compile_options(main PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    $<$<CONFIG:Debug>:   -O0 -g3>
    $<$<CONFIG:Release>: -O3>
)

# Link against SDL3 and optional add-ons.
target_link_libraries(main PRIVATE SDL3::SDL3 simdutf utf8cpp::utf8cpp fastgltf)

if(SDL_TTF_AVAILABLE)
    if(TARGET SDL3_ttf::SDL3_ttf)
        target_link_libraries(main PRIVATE SDL3_ttf::SDL3_ttf)
    endif()
endif()
if(SDL_IMAGE_AVAILABLE)
    if(TARGET SDL3_image::SDL3_image)
        target_link_libraries(main PRIVATE SDL3_image::SDL3_image)
    endif()
endif()
if(SDL_MIXER_AVAILABLE)
    if(TARGET SDL3_mixer::SDL3_mixer)
        target_link_libraries(main PRIVATE SDL3_mixer::SDL3_mixer)
    endif()
endif()

# Android system libraries required by SDL3
target_link_libraries(main PRIVATE
    android
    log
    EGL
    GLESv3
)

message(STATUS "[sdlos-android] Android shared library 'main' configured")
message(STATUS "[sdlos-android] jade app: ${SDLOS_ANDROID_APP}")

