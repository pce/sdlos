cmake_minimum_required(VERSION 3.25)

project(sdlos
    VERSION 0.1.0
    DESCRIPTION "SDL3 GPU desktop environment"
    LANGUAGES C CXX
)

# ┌────────────────────────────────────────────────────────────────────────────┐
# │  Build configuration                                                       │
# │                                                                            │
# │  cmake/sdlos_tooling.cmake    — sanitizers, clang-format, clang-tidy, IWYU │
# │  cmake/sdlos_platform.cmake   — C++23, build type, vcpkg, iOS/tvOS guards  │
# └────────────────────────────────────────────────────────────────────────────┘

include(cmake/sdlos_tooling.cmake)
include(cmake/sdlos_platform.cmake)

# ┌────────────────────────────────────────────────────────────────────────────┐
# │  Dependencies — one file per library under cmake/deps/                     │
# │                                                                            │
# │  Resolution strategy (per dep):                                            │
# │    1. find_package       — global install (brew, apt) or vcpkg             │
# │    2. Vendored deps/<lib> (+ ../userspace/deps/<lib> monorepo fallback)    │
# │    3. FetchContent       — upstream tarball / git (requires network)       │
# │                                                                            │
# │  Pass -DSDLOS_NO_FETCH=ON to disable step 3 (air-gapped / CI builds).      │
# │                                                                            │
# │  Each file is self-contained: sets cache/normal variables, creates targets.│
# │  include() does NOT create a scope, so all variables are visible here.     │
# └────────────────────────────────────────────────────────────────────────────┘

include(cmake/deps/DepSDL3.cmake)
include(cmake/deps/DepSDLImage.cmake)
include(cmake/deps/DepSDLMixer.cmake)
include(cmake/deps/DepSDLTtf.cmake)
include(cmake/deps/DepSimdutf.cmake)
include(cmake/deps/DepUtfcpp.cmake)
include(cmake/deps/DepFastgltf.cmake)

# ┌────────────────────────────────────────────────────────────────────────────┐
# │  Shared build helpers                                                      │
# │                                                                            │
# │  sdlos_target_compile_options(TARGET)  — warnings + opt + sanitizers       │
# │  sdlos_link_sdl(TARGET)               — link SDL3 + add-ons + frameworks   │
# │  sdlos_jade_app(NAME JADE ...)        — standalone jade app binary         │
# │  sdlos_copy_resource[_to](...)        — post-build file copy               │
# └────────────────────────────────────────────────────────────────────────────┘

include(cmake/sdlos_functions.cmake)

# ┌────────────────────────────────────────────────────────────────────────────┐
# │#                                                                          /
# │# sdlos_engine  —  static library: the runtime every app links against.
# │#
# │# Godot analogy: this is the engine.  sdlos (the desktop shell) and every
# │# standalone jade app are all just different "games" running on this engine.
# │# Nothing in here knows about the desktop shell or OS lifecycle.           │
# └───────────────────────────────────────────────────────────────────────────┘

set(SDLOS_ENGINE_SOURCES
    # Scene graph + GPU renderer
    src/render_tree.cc
    src/sdl_renderer.cc
    src/text_renderer.cc
    src/image_cache.cc

    # Event system
    src/event_bus.cc

    # Layout + style pipeline
    src/style_applier.cc
    src/style_draw.cc
    src/css_loader.cc

    # Input / interaction
    src/hit_test.cc
    src/node_events.cc

    # Jade parser
    src/jade/jade_lexer.cc
    src/jade/jade_parser.cc

    # VFS implementations
    src/vfs/vfs.cc
    src/vfs/local_mount.cc
    src/vfs/union_mount.cc
    src/vfs/mem_mount.cc

    # Audio
    src/audio/sfx_player.cc
    src/audio/audio_player.cc

    # Video / camera
    src/video_texture.cc

    # Widgets
    src/widgets/input_text_box.cc
    src/widgets/select_box.cc
    src/widgets/number_dragger.cc
    src/widgets/range_slider.cc

    # Debug overlays
    src/debug/layout_debug.cc

    # 3D glTF pipeline
    src/gltf/gltf_scene.cc

    # Frame graph (pipeline.pug → CompiledGraph)
    src/frame_graph/pug_parser.cc
    src/frame_graph/compiled_graph.cc
    src/frame_graph/frame_graph.cc
)

add_library(sdlos_engine STATIC ${SDLOS_ENGINE_SOURCES})

target_include_directories(sdlos_engine PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/core"
    "${CMAKE_SOURCE_DIR}/src/widgets"
    "${CMAKE_SOURCE_DIR}/src/jade"
    "${CMAKE_SOURCE_DIR}/src/debug"
    "${CMAKE_SOURCE_DIR}/src/vfs"
    "${CMAKE_SOURCE_DIR}/src/audio"
)

sdlos_target_compile_options(sdlos_engine)
sdlos_link_sdl(sdlos_engine)

target_link_libraries(sdlos_engine PUBLIC
    simdutf
    utf8cpp::utf8cpp
    fastgltf
)

# PBR mesh shaders — engine-level resources required by GltfScene::init().
sdlos_copy_resource_to(sdlos_engine
    "assets/shaders/msl/pbr_mesh.vert.metal"
    "data/shaders/msl/pbr_mesh.vert.metal")
sdlos_copy_resource_to(sdlos_engine
    "assets/shaders/msl/pbr_mesh.frag.metal"
    "data/shaders/msl/pbr_mesh.frag.metal")

# sdlos  —  the desktop shell executable (macOS / Linux / Windows only).

if(NOT ANDROID AND NOT IOS AND NOT EMSCRIPTEN)
    add_executable(sdlos
        src/main.cc
        src/io.cc
        src/desktop.cc
        src/app.cc
        src/notification_center.cc
    )
    target_link_libraries(sdlos PRIVATE sdlos_engine)
    sdlos_target_compile_options(sdlos)
    sdlos_link_sdl(sdlos)
endif()

# Standalone jade apps — auto-discovered from examples/apps/*/*.cmake
#
# To add an app:    sdlos create <name>
# To remove an app: delete the directory
# Re-run cmake after adding or removing an app (glob runs at configure time).

file(GLOB _app_cmake_files "${CMAKE_CURRENT_SOURCE_DIR}/examples/apps/*/*.cmake")
foreach(_f ${_app_cmake_files})
    include(${_f})
endforeach()
unset(_app_cmake_files)


# ──────────────────────────────────────────────────────────────────────────┐
# ┌────────────────────────────────────────────────────────────────────────────┐
# │ Emscripten / Web target                                                    │


if(EMSCRIPTEN)
    set(SDLOS_WEB_APP "styleguide" CACHE STRING
        "Jade app to compile for the web (must match examples/apps/<name>/<name>.jade)")

    set(_web_jade     "${CMAKE_SOURCE_DIR}/examples/apps/${SDLOS_WEB_APP}/${SDLOS_WEB_APP}.jade")
    set(_web_behavior "${CMAKE_SOURCE_DIR}/examples/apps/${SDLOS_WEB_APP}/${SDLOS_WEB_APP}.cc")

    add_executable(${SDLOS_WEB_APP} src/jade_host.cc)

    target_compile_definitions(${SDLOS_WEB_APP} PRIVATE
        SDLOS_JADE_ENTRY="${_web_jade}"
        SDLOS_APP_NAME="${SDLOS_WEB_APP}"
        SDLOS_APP_BEHAVIOR="${_web_behavior}"
    )

    target_link_libraries(${SDLOS_WEB_APP} PRIVATE sdlos_engine)
    sdlos_target_compile_options(${SDLOS_WEB_APP})
    sdlos_link_sdl(${SDLOS_WEB_APP})

    target_link_options(${SDLOS_WEB_APP} PRIVATE
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=67108864
        -sUSE_PTHREADS=0
        "SHELL:-sEXPORTED_RUNTIME_METHODS=[ccall]"
        --shell-file "${CMAKE_SOURCE_DIR}/assets/web/shell.html"
    )

    set_target_properties(${SDLOS_WEB_APP} PROPERTIES SUFFIX ".html")
    message(STATUS "[sdlos] Emscripten web target: ${SDLOS_WEB_APP}.html")
endif()

#  │
#  │#  Tests + status summary
#  └───────────────────────────────────────────────────────────────────────────┘

include(cmake/sdlos_tests.cmake)
include(cmake/sdlos_status.cmake)
