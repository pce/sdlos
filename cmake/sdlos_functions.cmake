# sdlos_functions.cmake — shared build helpers.
#
# Provides:
#   sdlos_target_compile_options(TARGET)  — warnings + optimisation + sanitizers
#   sdlos_link_sdl(TARGET)               — link SDL3 + optional add-ons + Apple frameworks
#   sdlos_jade_app(NAME JADE_FILE ...)   — define a standalone jade app executable
#   sdlos_copy_resource(TARGET SRC)      — post-build copy a file next to the binary
#   sdlos_copy_resource_to(TARGET SRC DEST) — post-build copy with destination sub-path
#
# Requires:
#   sdlos_platform.cmake   (C++ standard, build type)
#   sdlos_tooling.cmake    (sdlos_apply_sanitizers)
#   cmake/deps/*           (SDL_IMAGE_AVAILABLE, SDL_MIXER_AVAILABLE, SDL_TTF_AVAILABLE)

# Compiler warning flags

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

# sdlos_target_compile_options
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

    sdlos_apply_sanitizers(${TARGET})
endfunction()

# sdlos_link_sdl
function(sdlos_link_sdl TARGET)
    # SDL3 core — prefer static on iOS.
    if(IOS AND TARGET SDL3::SDL3-static)
        target_link_libraries(${TARGET} PRIVATE SDL3::SDL3-static)
    else()
        target_link_libraries(${TARGET} PRIVATE SDL3::SDL3)
    endif()

    if(SDL_IMAGE_AVAILABLE)
        if(IOS AND TARGET SDL3_image::SDL3_image-static)
            target_link_libraries(${TARGET} PRIVATE SDL3_image::SDL3_image-static)
        elseif(TARGET SDL3_image::SDL3_image)
            target_link_libraries(${TARGET} PRIVATE SDL3_image::SDL3_image)
        elseif(TARGET SDL_image::SDL_image)
            target_link_libraries(${TARGET} PRIVATE SDL_image::SDL_image)
        endif()
        target_compile_definitions(${TARGET} PRIVATE SDL_IMAGE_AVAILABLE=1)
    endif()

    if(SDL_MIXER_AVAILABLE)
        if(IOS AND TARGET SDL3_mixer::SDL3_mixer-static)
            target_link_libraries(${TARGET} PRIVATE SDL3_mixer::SDL3_mixer-static)
        elseif(TARGET SDL3_mixer::SDL3_mixer)
            target_link_libraries(${TARGET} PRIVATE SDL3_mixer::SDL3_mixer)
        elseif(TARGET SDL_mixer::SDL_mixer)
            target_link_libraries(${TARGET} PRIVATE SDL_mixer::SDL_mixer)
        endif()
        target_compile_definitions(${TARGET} PRIVATE SDL_MIXER_AVAILABLE=1)
    endif()

    if(SDL_TTF_AVAILABLE)
        if(IOS AND TARGET SDL3_ttf::SDL3_ttf-static)
            target_link_libraries(${TARGET} PRIVATE SDL3_ttf::SDL3_ttf-static)
        elseif(TARGET SDL3_ttf::SDL3_ttf)
            target_link_libraries(${TARGET} PRIVATE SDL3_ttf::SDL3_ttf)
        elseif(TARGET SDL_ttf::SDL_ttf)
            target_link_libraries(${TARGET} PRIVATE SDL_ttf::SDL_ttf)
        endif()
        target_compile_definitions(${TARGET} PRIVATE SDL_TTF_AVAILABLE=1)
    endif()

    # Apple system frameworks — PUBLIC so they propagate when sdlos_engine is static.
    if(APPLE)
        target_link_libraries(${TARGET} PUBLIC
            "-framework CoreFoundation"
            "-framework CoreText"
            "-framework CoreGraphics"
        )
    endif()
    if(IOS)
        target_link_libraries(${TARGET} PUBLIC
            "-framework UIKit"
            "-framework Metal"
            "-framework QuartzCore"
            "-framework AudioToolbox"
            "-framework CoreAudio"
            "-framework AVFoundation"
            "-framework CoreMotion"
            "-framework GameController"
            "-framework CoreVideo"
        )
    endif()
endfunction()

#  sdlos_copy_resource / sdlos_copy_resource_to
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

# sdlos_jade_app
#
# Build a self-contained standalone binary from a single .jade file.
#
# Usage:
#   sdlos_jade_app(calculator examples/apps/calc/calculator.jade
#       BEHAVIOR examples/apps/calc/calculator_behavior.cc
#       DATA_DIR examples/apps/calc/data
#       WIN_W 375  WIN_H 667)

function(sdlos_jade_app NAME JADE_FILE)
    # Android and Emscripten do not use per-app executables.
    if(ANDROID OR EMSCRIPTEN)
        return()
    endif()

    cmake_parse_arguments(PARSE_ARGV 2 _APP "" "BEHAVIOR;WIN_W;WIN_H;DATA_DIR" "")

    # Resolve jade file to absolute path.
    if(IS_ABSOLUTE "${JADE_FILE}")
        set(_jade_abs "${JADE_FILE}")
    else()
        set(_jade_abs "${CMAKE_SOURCE_DIR}/${JADE_FILE}")
    endif()

    if(NOT EXISTS "${_jade_abs}")
        message(WARNING "[sdlos_jade_app] jade file not found: ${_jade_abs}")
    endif()

    # Resolve optional behavior file.
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

    add_executable(${NAME} src/jade_host.cc)

    # iOS bundle metadata.
    if(IOS)
        set_target_properties(${NAME} PROPERTIES
            MACOSX_BUNDLE                          TRUE
            MACOSX_BUNDLE_BUNDLE_NAME              "${NAME}"
            MACOSX_BUNDLE_BUNDLE_IDENTIFIER        "${SDLOS_IOS_BUNDLE_PREFIX}.${NAME}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING     "${PROJECT_VERSION}"
            MACOSX_BUNDLE_BUNDLE_VERSION           "${PROJECT_VERSION}"
            MACOSX_BUNDLE_GUI_IDENTIFIER           "${SDLOS_IOS_BUNDLE_PREFIX}.${NAME}"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${SDLOS_IOS_BUNDLE_PREFIX}.${NAME}"
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
            XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET
                "${CMAKE_OSX_DEPLOYMENT_TARGET}"
        )
        if(SDLOS_IOS_TEAM)
            set_target_properties(${NAME} PROPERTIES
                XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${SDLOS_IOS_TEAM}"
                XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "iPhone Developer"
            )
        endif()
    endif()

    # Compile definitions.
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

    # Optional: copy app data/ dir next to the binary.
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

