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
set(CMAKE_CXX_EXTENSIONS        OFF)

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
# vcpkg toolchain (optional)
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
# -----------------------------------------------------------------------------

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

if(EXISTS "${CMAKE_SOURCE_DIR}/deps/SDL_ttf/CMakeLists.txt")
    set(SDLTTF_VENDORED          ON CACHE BOOL "" FORCE)
    set(SDLTTF_FREETYPE_VENDORED ON CACHE BOOL "" FORCE)
    set(SDLTTF_HARFBUZZ_VENDORED ON CACHE BOOL "" FORCE)
endif()

sdlos_optional_addon(TTF SDL_ttf)

# =============================================================================
# Third-party text libraries
# =============================================================================

include(FetchContent)

# simdutf — SIMD-accelerated UTF-8/UTF-16/UTF-32 validation & transcoding
set(SIMDUTF_TESTS      OFF CACHE BOOL "" FORCE)
set(SIMDUTF_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SIMDUTF_TOOLS      OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    simdutf
    URL https://github.com/simdutf/simdutf/archive/refs/tags/v5.6.4.tar.gz
)
FetchContent_MakeAvailable(simdutf)

# utf8cpp — header-only UTF-8/UTF-16/UTF-32 for C++
# Use a tarball URL (no git required, faster, avoids ALIAS target registration
# failures that can occur with GIT_REPOSITORY + GIT_TAG on some generators).
FetchContent_Declare(
    utfcpp
    URL      https://github.com/nemtrif/utfcpp/archive/refs/tags/v4.0.4.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(utfcpp)

# utfcpp's CMakeLists creates `utf8cpp` (INTERFACE) and an ALIAS
# `utf8cpp::utf8cpp`.  Guard against the rare case where FetchContent
# populates the sources but the ALIAS wasn't registered (e.g. an older
# generator that skips ALIAS targets for INTERFACE libraries).
if(NOT TARGET utf8cpp::utf8cpp)
    if(TARGET utf8cpp)
        add_library(utf8cpp::utf8cpp ALIAS utf8cpp)
    else()
        # Last resort: synthesise a header-only target pointing at the
        # populated source tree so the build doesn't fail hard.
        FetchContent_GetProperties(utfcpp SOURCE_DIR utfcpp_SOURCE_DIR)
        add_library(_utf8cpp_iface INTERFACE)
        target_include_directories(_utf8cpp_iface INTERFACE
            "${utfcpp_SOURCE_DIR}/source")
        add_library(utf8cpp::utf8cpp ALIAS _utf8cpp_iface)
        message(WARNING
            "[sdlos] utf8cpp::utf8cpp ALIAS was missing — synthesised a "
            "fallback INTERFACE target from ${utfcpp_SOURCE_DIR}/source")
    endif()
endif()

# fastgltf — fast, modern C++17 glTF 2.0 parser used by gltf_scene.cxx.
# Loaded as a tarball (no git required, reproducible, fast on CI).
FetchContent_Declare(
    fastgltf
    URL      https://github.com/spnda/fastgltf/archive/refs/tags/v0.8.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# Disable fastgltf's own test targets — we only need the library.
set(FASTGLTF_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(FASTGLTF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(fastgltf)

# -----------------------------------------------------------------------------
# Compiler warning flags  (shared across all targets)
# -----------------------------------------------------------------------------

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
    -Wno-unused-parameter
)

# Helper: apply the standard warning + optimisation flags to a target.
function(sdlos_target_compile_options TARGET)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
        target_compile_options(${TARGET} PRIVATE
            ${_SDLOS_WARN_FLAGS}
            -Wno-c99-extensions)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${TARGET} PRIVATE
            ${_SDLOS_WARN_FLAGS}
            -Wmisleading-indentation
            -Wduplicated-cond
            -Wlogical-op)
    elseif(MSVC)
        target_compile_options(${TARGET} PRIVATE
            /W4 /permissive- /w14640 /w14826 /w15038)
    endif()

    if(NOT MSVC)
        target_compile_options(${TARGET} PRIVATE
            $<$<CONFIG:Debug>:          -O0 -g3>
            $<$<CONFIG:RelWithDebInfo>: -O2 -g>
            $<$<CONFIG:Release>:        -O3>
            $<$<CONFIG:MinSizeRel>:     -Os>)
    else()
        target_compile_options(${TARGET} PRIVATE
            $<$<CONFIG:Debug>:          /Od /Zi>
            $<$<CONFIG:RelWithDebInfo>: /O2 /Zi>
            $<$<CONFIG:Release>:        /O2>
            $<$<CONFIG:MinSizeRel>:     /O1>)
    endif()

    target_compile_definitions(${TARGET} PRIVATE
        $<$<CONFIG:Debug>:BUILD_TYPE_DEBUG>
        $<$<CXX_COMPILER_ID:MSVC>:NOMINMAX WIN32_LEAN_AND_MEAN>)
endfunction()

# Helper: link SDL3 + optional add-ons into a target.
function(sdlos_link_sdl TARGET)
    target_link_libraries(${TARGET} PRIVATE SDL3::SDL3)

    if(SDL_IMAGE_AVAILABLE)
        if(TARGET SDL3_image::SDL3_image)
            target_link_libraries(${TARGET} PRIVATE SDL3_image::SDL3_image)
        elseif(TARGET SDL_image::SDL_image)
            target_link_libraries(${TARGET} PRIVATE SDL_image::SDL_image)
        endif()
        target_compile_definitions(${TARGET} PRIVATE SDL_IMAGE_AVAILABLE=1)
    endif()

    if(SDL_MIXER_AVAILABLE)
        if(TARGET SDL3_mixer::SDL3_mixer)
            target_link_libraries(${TARGET} PRIVATE SDL3_mixer::SDL3_mixer)
        elseif(TARGET SDL_mixer::SDL_mixer)
            target_link_libraries(${TARGET} PRIVATE SDL_mixer::SDL_mixer)
        endif()
    endif()

    if(SDL_TTF_AVAILABLE)
        if(TARGET SDL3_ttf::SDL3_ttf)
            target_link_libraries(${TARGET} PRIVATE SDL3_ttf::SDL3_ttf)
        elseif(TARGET SDL_ttf::SDL_ttf)
            target_link_libraries(${TARGET} PRIVATE SDL_ttf::SDL_ttf)
        endif()
        target_compile_definitions(${TARGET} PRIVATE SDL_TTF_AVAILABLE=1)
    endif()
endfunction()

# =============================================================================
# sdlos_engine  —  static library: the runtime every app links against.
#
# Godot analogy: this is the engine. sdlos (the desktop shell) and every
# standalone jade app are all just different "games" running on this engine.
# Nothing in here knows about the desktop shell, AppCatalog, or OS lifecycle.
# =============================================================================

set(SDLOS_ENGINE_SOURCES
    # Scene graph + GPU renderer
    src/render_tree.cxx
    src/sdl_renderer.cxx
    src/text_renderer.cxx
    src/image_cache.cxx

    # Event system
    src/event_bus.cxx

    # Layout + style pipeline
    src/style_applier.cxx
    src/style_draw.cxx
    src/css_loader.cxx

    # Input / interaction
    src/hit_test.cxx
    src/node_events.cxx

    # Jade parser (the sdlos-native app format)
    src/jade/jade_lexer.cxx
    src/jade/jade_parser.cxx

    # Video / camera frame source
    src/video_texture.cxx

    # Widgets built on top of RenderTree
    src/widgets/input_text_box.cxx
    src/widgets/select_box.cxx
    src/widgets/number_dragger.cxx
    src/widgets/range_slider.cxx

    # Debug overlays
    src/debug/layout_debug.cxx

    # 3D glTF render layer (fastgltf + SDL3 GPU pipeline)
    src/gltf/gltf_scene.cxx
)

add_library(sdlos_engine STATIC ${SDLOS_ENGINE_SOURCES})

target_include_directories(sdlos_engine PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/core"
    "${CMAKE_SOURCE_DIR}/src/widgets"
    "${CMAKE_SOURCE_DIR}/src/jade"
    "${CMAKE_SOURCE_DIR}/src/debug"
)

sdlos_target_compile_options(sdlos_engine)
sdlos_link_sdl(sdlos_engine)

target_link_libraries(sdlos_engine PUBLIC
    simdutf
    utf8cpp::utf8cpp
    fastgltf
)



# =============================================================================
# sdlos  —  the desktop shell executable.
#
# One app running on sdlos_engine. Hosts the desktop scene, AppCatalog,
# virtual filesystem, notifications, and the full OS lifecycle.
# =============================================================================

set(SDLOS_SHELL_SOURCES
    src/main.cxx
    src/os.cxx
    src/desktop.cxx
    src/app.cxx
    src/app_catalog.cxx
    src/virtual_file_system.cxx
    src/notification_center.cxx
)

add_executable(sdlos ${SDLOS_SHELL_SOURCES})

target_link_libraries(sdlos PRIVATE sdlos_engine)

sdlos_target_compile_options(sdlos)
# SDL is already transitively available via sdlos_engine, but link explicitly
# so the shell can call SDL_Init / SDL_Quit directly.
sdlos_link_sdl(sdlos)

# =============================================================================
# sdlos_jade_app(<name> <jade_file>)
#
# Build a self-contained standalone binary from a single .jade file.
# The jade file is embedded as a compile-time string path definition so the
# binary knows where to find its scene at startup.
#
# Usage:
#   sdlos_jade_app(calculator examples/apps/calculator.jade)
#
# Produces an executable called <name> that boots sdlos_engine, parses
# <jade_file>, and runs the main loop — no desktop shell required.
# This is the "sdlos-native app" format: jade is the scene, sdlos_engine
# is the runtime, the binary is the standalone product.
# =============================================================================

function(sdlos_jade_app NAME JADE_FILE)
    # Optional keyword arguments:
    #   BEHAVIOR <path>   — C++ fragment #include-d into jade_host.cxx.
    #                       Defines jade_app_init() so the app can subscribe
    #                       to EventBus topics and mutate the RenderTree.
    #                       Passed as SDLOS_APP_BEHAVIOR="<abs-path>" — the
    #                       same pattern used for SDLOS_JADE_ENTRY.
    # Optional keyword arguments:
    #   WIN_W <pixels>    — debug window width  (default: 375)
    #   WIN_H <pixels>    — debug window height (default: 667)
    #                       Passed as SDLOS_WIN_W / SDLOS_WIN_H so that
    #                       jade_host.cxx can use the right window size in
    #                       BUILD_TYPE_DEBUG builds without modifying the host.
    cmake_parse_arguments(PARSE_ARGV 2 _APP "" "BEHAVIOR;WIN_W;WIN_H;DATA_DIR" "")

    # Resolve jade file to an absolute path so the definition survives
    # out-of-source builds.
    if(IS_ABSOLUTE "${JADE_FILE}")
        set(_jade_abs "${JADE_FILE}")
    else()
        set(_jade_abs "${CMAKE_SOURCE_DIR}/${JADE_FILE}")
    endif()

    if(NOT EXISTS "${_jade_abs}")
        message(WARNING "[sdlos_jade_app] jade file not found: ${_jade_abs}")
    endif()

    # jade_host.cxx is the only source file.  When a BEHAVIOR is given its
    # absolute path is passed as SDLOS_APP_BEHAVIOR so that jade_host.cxx can
    # #include it directly — one TU, no linker extensions required.
    if(_APP_BEHAVIOR)
        if(IS_ABSOLUTE "${_APP_BEHAVIOR}")
            set(_behavior_abs "${_APP_BEHAVIOR}")
        else()
            set(_behavior_abs "${CMAKE_SOURCE_DIR}/${_APP_BEHAVIOR}")
        endif()

        if(NOT EXISTS "${_behavior_abs}")
            message(WARNING "[sdlos_jade_app] behavior file not found: ${_behavior_abs}")
        endif()

        message(STATUS "[sdlos] jade app '${NAME}' behavior → ${_behavior_abs}")
    endif()

    add_executable(${NAME} src/jade_host.cxx)

    set(_behavior_def "")
    if(_APP_BEHAVIOR)
        set(_behavior_def "SDLOS_APP_BEHAVIOR=\"${_behavior_abs}\"")
    endif()

    set(_win_w_def "")
    set(_win_h_def "")
    if(_APP_WIN_W)
        set(_win_w_def "SDLOS_WIN_W=${_APP_WIN_W}")
    endif()
    if(_APP_WIN_H)
        set(_win_h_def "SDLOS_WIN_H=${_APP_WIN_H}")
    endif()

    target_compile_definitions(${NAME} PRIVATE
        SDLOS_JADE_ENTRY="${_jade_abs}"
        SDLOS_APP_NAME="${NAME}"
        ${_behavior_def}
        ${_win_w_def}
        ${_win_h_def}
    )

    target_link_libraries(${NAME} PRIVATE sdlos_engine)
    sdlos_target_compile_options(${NAME})
    sdlos_link_sdl(${NAME})

    # Optional: copy app data/ dir next to the binary so relative paths work.
    if(_APP_DATA_DIR)
        if(IS_ABSOLUTE "${_APP_DATA_DIR}")
            set(_data_abs "${_APP_DATA_DIR}")
        else()
            set(_data_abs "${CMAKE_SOURCE_DIR}/${_APP_DATA_DIR}")
        endif()
        if(EXISTS "${_data_abs}")
            add_custom_command(TARGET ${NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${_data_abs}"
                    "$<TARGET_FILE_DIR:${NAME}>/data"
                COMMENT "[sdlos] copying data for '${NAME}'"
                VERBATIM
            )
            message(STATUS "[sdlos] jade app '${NAME}' data  → ${_data_abs}")
        else()
            message(WARNING "[sdlos_jade_app] DATA_DIR not found: ${_data_abs}")
        endif()
    endif()

    message(STATUS "[sdlos] jade app '${NAME}' → ${_jade_abs}")
endfunction()

# -----------------------------------------------------------------------------
# Standalone jade apps — auto-discovered
#
# Each app owns its own <name>.cmake file inside its source directory.
# To add an app:    sdlos create <name>   (writes examples/apps/<name>/<name>.cmake)
# To remove an app: delete the directory  (no CMakeLists.txt edit needed)
#
# The glob runs at configure time.  Re-run cmake after adding or removing an app.
# -----------------------------------------------------------------------------

# -----------------------------------------------------------------------------
# Helper: copy a single file from the source tree into the binary directory
# so apps can load it at runtime via SDL_GetBasePath() + filename.
# Used for CSS layout files that sit next to the .jade / .cxx sources but
# must be deployed alongside the executable (separate from DATA_DIR / data/).
# -----------------------------------------------------------------------------

function(sdlos_copy_resource TARGET SRC_RELATIVE)
    get_filename_component(_fname "${SRC_RELATIVE}" NAME)
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/${SRC_RELATIVE}"
            "$<TARGET_FILE_DIR:${TARGET}>/${_fname}"
        COMMENT "[sdlos] copying ${_fname} for '${TARGET}'"
        VERBATIM
    )
endfunction()

# sdlos_copy_resource_to(TARGET SRC_RELATIVE DEST_RELATIVE)
# Like sdlos_copy_resource but lets you specify a destination sub-path
# (e.g. "data/shaders/msl/foo.metal") relative to the binary directory.
# Intermediate directories are created automatically.
function(sdlos_copy_resource_to TARGET SRC_RELATIVE DEST_RELATIVE)
    get_filename_component(_dname "${DEST_RELATIVE}" DIRECTORY)
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${TARGET}>/${_dname}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/${SRC_RELATIVE}"
            "$<TARGET_FILE_DIR:${TARGET}>/${DEST_RELATIVE}"
        COMMENT "[sdlos] ${SRC_RELATIVE} → ${DEST_RELATIVE} for '${TARGET}'"
        VERBATIM
    )
endfunction()

# PBR mesh shaders — engine-level resources required by GltfScene::init().
# Placed after the helper function definitions (CMake is top-to-bottom).
# All app binaries share the same output directory, so copying once on
# sdlos_engine rebuild makes them available to all app binaries (shared output dir).
sdlos_copy_resource_to(sdlos_engine
    "assets/shaders/msl/pbr_mesh.vert.metal"
    "data/shaders/msl/pbr_mesh.vert.metal")
sdlos_copy_resource_to(sdlos_engine
    "assets/shaders/msl/pbr_mesh.frag.metal"
    "data/shaders/msl/pbr_mesh.frag.metal")

# Include every app that has registered itself with a .cmake file.
file(GLOB _app_cmake_files "${CMAKE_CURRENT_SOURCE_DIR}/examples/apps/*/*.cmake")
foreach(_f ${_app_cmake_files})
    include(${_f})
endforeach()
unset(_app_cmake_files)



# =============================================================================
# Tests  (optional — enable with -DSDLOS_ENABLE_TESTS=ON)
# =============================================================================

option(SDLOS_ENABLE_TESTS "Build unit tests (requires internet on first configure)" OFF)

if(SDLOS_ENABLE_TESTS)
    # FetchContent already included unconditionally above.

    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.4
    )
    FetchContent_MakeAvailable(Catch2)

    # test_jade links sdlos_engine directly — no shell, no window, no GPU init.
    add_executable(test_jade tests/test_jade.cxx)

    target_link_libraries(test_jade PRIVATE
        sdlos_engine
        Catch2::Catch2WithMain
    )

    sdlos_target_compile_options(test_jade)

    include(CTest)
    include(Catch)
    catch_discover_tests(test_jade)

    message(STATUS "[sdlos] Tests: test_jade enabled")
else()
    message(STATUS "[sdlos] Tests: disabled (pass -DSDLOS_ENABLE_TESTS=ON to enable)")
endif()

# =============================================================================
# Status summary
# =============================================================================

message(STATUS "")
message(STATUS "┌─ sdlos ─────────────────────────────────── ──- ─ ─")
message(STATUS "│  Version     : ${PROJECT_VERSION}")
message(STATUS "│  Build type  : ${CMAKE_BUILD_TYPE}")
message(STATUS "│  C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "│  Compiler    : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "│  Compiler bin: ${CMAKE_CXX_COMPILER}")
message(STATUS "│  SDL_image   : ${SDL_IMAGE_AVAILABLE}")
message(STATUS "│  SDL_mixer   : ${SDL_MIXER_AVAILABLE}")
message(STATUS "│  SDL_ttf     : ${SDL_TTF_AVAILABLE}")
message(STATUS "│  simdutf     : ON (FetchContent v5.6.4)")
message(STATUS "│  utf8cpp     : ON (FetchContent v4.0.4)")
message(STATUS "│  fastgltf    : ON (FetchContent v0.8.0)")
message(STATUS "│  SDL_camera  : (via SDL3 core)")
message(STATUS "│  Tests       : ${SDLOS_ENABLE_TESTS}")
message(STATUS "└───────────────────────────────────────── ─ ── ─")
message(STATUS "")
