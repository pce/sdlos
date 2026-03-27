# Optional developer tooling wired into the build.
# All options are OFF by default so a normal build is unaffected.
#
# Sanitizers
#
#   -DSDLOS_ENABLE_ASAN=ON    AddressSanitizer  (heap/stack/global overflows,
#                              use-after-free, use-after-return)
#   -DSDLOS_ENABLE_UBSAN=ON   UndefinedBehaviorSanitizer  (signed overflow,
#                              null deref, misaligned access, …)
#   -DSDLOS_ENABLE_TSAN=ON    ThreadSanitizer  (data races, lock-order inversion)
#
#   ASan and TSan instrument the same memory accesses in incompatible ways —
#   enabling both is a hard error.  UBSan is independent and may be combined
#   with either.
#
# clang-format
#
#   Searches for clang-format on PATH and creates two targets:
#     make format        — format all sources in-place
#     make format-check  — exit non-zero if any file is not formatted (CI)
#
#   Pass -DSDLOS_CLANG_FORMAT_EXE=/path/to/clang-format to override discovery.
#
# clang-tidy
#
#   -DSDLOS_ENABLE_CLANG_TIDY=ON
#   Sets CMAKE_CXX_CLANG_TIDY so every C++ TU is checked during the build.
#   Reads .clang-tidy from the source root when present.
#
#   Pass -DSDLOS_CLANG_TIDY_EXE=/path/to/clang-tidy to override discovery.
#
# include-what-you-use
#
#   -DSDLOS_ENABLE_IWYU=ON
#   Sets CMAKE_CXX_INCLUDE_WHAT_YOU_USE so IWYU runs on every C++ TU.
#
# Usage in CMakeLists.txt:
#   include(cmake/sdlos_tooling.cmake)
#   # …define targets…
#   sdlos_apply_sanitizers(my_target)   # <-- called for each target


# Sanitizer options

option(SDLOS_ENABLE_ASAN  "Enable AddressSanitizer (ASan)"            OFF)
option(SDLOS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer (UBSan)" OFF)
option(SDLOS_ENABLE_TSAN  "Enable ThreadSanitizer (TSan)"             OFF)

if(SDLOS_ENABLE_ASAN AND SDLOS_ENABLE_TSAN)
    message(FATAL_ERROR
        "[sdlos] SDLOS_ENABLE_ASAN and SDLOS_ENABLE_TSAN are mutually exclusive. "
        "ASan and TSan both instrument memory accesses but use incompatible "
        "run-time support libraries.  Enable only one at a time."
    )
endif()

# Internal: accumulate the sanitizer flags once so the function is cheap.
set(_SDLOS_SAN_COMPILE_FLAGS "")
set(_SDLOS_SAN_LINK_FLAGS    "")

if(SDLOS_ENABLE_ASAN)
    list(APPEND _SDLOS_SAN_COMPILE_FLAGS
        -fsanitize=address
        -fno-omit-frame-pointer
        # Improve stack-use-after-return detection; small overhead.
        -fsanitize-address-use-after-scope
    )
    list(APPEND _SDLOS_SAN_LINK_FLAGS -fsanitize=address)
    message(STATUS "[sdlos] Sanitizer: ASan enabled")
endif()

if(SDLOS_ENABLE_UBSAN)
    list(APPEND _SDLOS_SAN_COMPILE_FLAGS
        -fsanitize=undefined
        # Extra checks beyond the default UBSan group:
        -fsanitize=float-divide-by-zero
        -fsanitize=implicit-conversion
        -fsanitize=local-bounds
        -fno-omit-frame-pointer
    )
    list(APPEND _SDLOS_SAN_LINK_FLAGS -fsanitize=undefined)
    message(STATUS "[sdlos] Sanitizer: UBSan enabled")
endif()

if(SDLOS_ENABLE_TSAN)
    list(APPEND _SDLOS_SAN_COMPILE_FLAGS
        -fsanitize=thread
        -fno-omit-frame-pointer
    )
    list(APPEND _SDLOS_SAN_LINK_FLAGS -fsanitize=thread)
    message(STATUS "[sdlos] Sanitizer: TSan enabled")
endif()

# sdlos_apply_sanitizers(<target>)
#
# Apply the active sanitizer flags to <target>.
# Called from sdlos_target_compile_options() so every engine and app target
# automatically picks up the current sanitizer configuration.
# Sanitizers are silently skipped on Android, iOS, and Emscripten because
# those platforms use different runtimes or do not support them at all.
function(sdlos_apply_sanitizers TARGET)
    # Mobile / WASM platforms either have their own sanitizer tooling
    # (Android's hwasan) or don't support the GNU sanitizer flags.
    if(ANDROID OR IOS OR EMSCRIPTEN)
        return()
    endif()

    if(NOT (CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU"))
        if(_SDLOS_SAN_COMPILE_FLAGS)
            message(WARNING
                "[sdlos] Sanitizers requested but compiler "
                "${CMAKE_CXX_COMPILER_ID} may not support them. "
                "Use Clang, AppleClang, or GCC."
            )
        endif()
        return()
    endif()

    if(_SDLOS_SAN_COMPILE_FLAGS)
        target_compile_options(${TARGET} PRIVATE ${_SDLOS_SAN_COMPILE_FLAGS})
        target_link_options(${TARGET}    PRIVATE ${_SDLOS_SAN_LINK_FLAGS})
    endif()
endfunction()

# clang-format

# Allow explicit override from the command line; otherwise discover from PATH.
# Search in a set of versioned names to find whichever is installed.
if(NOT SDLOS_CLANG_FORMAT_EXE)
    find_program(SDLOS_CLANG_FORMAT_EXE
        NAMES
            clang-format
            clang-format-19
            clang-format-18
            clang-format-17
            clang-format-16
        DOC "Path to clang-format executable"
    )
endif()

if(SDLOS_CLANG_FORMAT_EXE)
    # Gather every C++ source and header under src/, tests/, and examples/.
    # All common extensions are listed individually — GLOB_RECURSE does not
    # support regex, but multiple patterns in one call do the same job.
    # deps/, build/, and cmake-build-* are never under these roots so they
    # are implicitly excluded.
    foreach(_dir src tests examples/apps)
        foreach(_ext cc cxx cpp c hh hpp hxx h)
            file(GLOB_RECURSE _glob
                "${CMAKE_SOURCE_DIR}/${_dir}/*.${_ext}")
            list(APPEND _SDLOS_FORMAT_SOURCES ${_glob})
        endforeach()
    endforeach()

    # format — reformat all sources in-place.
    add_custom_target(format
        COMMAND ${SDLOS_CLANG_FORMAT_EXE}
            -i
            --style=file          # honour .clang-format at source root
            ${_SDLOS_FORMAT_SOURCES}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "[sdlos] clang-format: formatting sources in-place"
        VERBATIM
    )

    # format-check — verify formatting without modifying files (for CI).
    # Returns exit code 1 if any file is not correctly formatted.
    add_custom_target(format-check
        COMMAND ${SDLOS_CLANG_FORMAT_EXE}
            --dry-run
            --Werror
            --style=file
            ${_SDLOS_FORMAT_SOURCES}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "[sdlos] clang-format: checking sources (no modifications)"
        VERBATIM
    )

    message(STATUS "[sdlos] clang-format: ${SDLOS_CLANG_FORMAT_EXE}  (targets: format / format-check)")
else()
    message(STATUS "[sdlos] clang-format: not found — 'format' and 'format-check' targets unavailable")
    message(STATUS "         Install via: brew install clang-format  OR  apt install clang-format")
endif()

# clang-tidy

option(SDLOS_ENABLE_CLANG_TIDY "Run clang-tidy on every C++ TU during build" OFF)

if(SDLOS_ENABLE_CLANG_TIDY)
    if(NOT SDLOS_CLANG_TIDY_EXE)
        find_program(SDLOS_CLANG_TIDY_EXE
            NAMES
                clang-tidy
                clang-tidy-19
                clang-tidy-18
                clang-tidy-17
                clang-tidy-16
            DOC "Path to clang-tidy executable"
        )
    endif()

    if(SDLOS_CLANG_TIDY_EXE)
        # Emit per-file diagnostics without treating warnings as hard errors
        # so the build still produces binaries even when issues are found.
        # Change --warnings-as-errors=* to make tidy failures fatal.
        set(CMAKE_CXX_CLANG_TIDY
            "${SDLOS_CLANG_TIDY_EXE}"
            "--extra-arg=-std=c++23"
            "--extra-arg=-Wno-unknown-warning-option"
            CACHE STRING "clang-tidy invocation" FORCE
        )
        message(STATUS "[sdlos] clang-tidy: ${SDLOS_CLANG_TIDY_EXE}  (runs on every C++ TU)")
    else()
        message(WARNING
            "[sdlos] SDLOS_ENABLE_CLANG_TIDY=ON but clang-tidy not found. "
            "Install via: brew install llvm  OR  apt install clang-tidy"
        )
    endif()
endif()

# include-what-you-use (IWYU)

option(SDLOS_ENABLE_IWYU "Run include-what-you-use on every C++ TU during build" OFF)

if(SDLOS_ENABLE_IWYU)
    find_program(SDLOS_IWYU_EXE
        NAMES include-what-you-use iwyu
        DOC "Path to include-what-you-use executable"
    )

    if(SDLOS_IWYU_EXE)
        set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE
            "${SDLOS_IWYU_EXE}"
            "-Xiwyu" "--cxx17ns"
            "-Xiwyu" "--no_fwd_decls"
            CACHE STRING "IWYU invocation" FORCE
        )
        message(STATUS "[sdlos] IWYU: ${SDLOS_IWYU_EXE}  (runs on every C++ TU)")
    else()
        message(WARNING
            "[sdlos] SDLOS_ENABLE_IWYU=ON but include-what-you-use not found. "
            "Install via: brew install include-what-you-use  OR  apt install iwyu"
        )
    endif()
endif()

# Status variables -- set here, printed inline in the parent CMakeLists.txt

set(SDLOS_STATUS_SANITIZERS "off")
if(SDLOS_ENABLE_ASAN AND SDLOS_ENABLE_UBSAN)
    set(SDLOS_STATUS_SANITIZERS "ASan + UBSan")
elseif(SDLOS_ENABLE_ASAN)
    set(SDLOS_STATUS_SANITIZERS "ASan")
elseif(SDLOS_ENABLE_UBSAN)
    set(SDLOS_STATUS_SANITIZERS "UBSan")
elseif(SDLOS_ENABLE_TSAN)
    set(SDLOS_STATUS_SANITIZERS "TSan")
endif()

set(SDLOS_STATUS_CLANG_FORMAT "off (clang-format not found)")
if(SDLOS_CLANG_FORMAT_EXE)
    set(SDLOS_STATUS_CLANG_FORMAT "${SDLOS_CLANG_FORMAT_EXE}  (make format / format-check)")
endif()

set(SDLOS_STATUS_CLANG_TIDY "off")
if(SDLOS_ENABLE_CLANG_TIDY AND SDLOS_CLANG_TIDY_EXE)
    set(SDLOS_STATUS_CLANG_TIDY "${SDLOS_CLANG_TIDY_EXE}  (per-TU during build)")
elseif(SDLOS_ENABLE_CLANG_TIDY)
    set(SDLOS_STATUS_CLANG_TIDY "requested but not found")
endif()

set(SDLOS_STATUS_IWYU "off")
if(SDLOS_ENABLE_IWYU AND SDLOS_IWYU_EXE)
    set(SDLOS_STATUS_IWYU "${SDLOS_IWYU_EXE}  (per-TU during build)")
elseif(SDLOS_ENABLE_IWYU)
    set(SDLOS_STATUS_IWYU "requested but not found")
endif()


