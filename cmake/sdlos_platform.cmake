# sdlos_platform.cmake — C++ standard, build type, vcpkg, platform guards,
#                        and dependency resolution policy.
#
# Must be included BEFORE any dependency files so that iOS/tvOS static-library
# overrides and SDLOS_NO_FETCH are visible to all Dep*.cmake files.

# Dependency resolution policy
#
# Each Dep*.cmake file resolves its library in this order:
#
#   1. find_package  — global install (brew, apt) or vcpkg
#   2. Vendored      — deps/<lib> or ../userspace/deps/<lib>
#   3. FetchContent  — upstream tarball / git (requires network)
#
# Pass -DSDLOS_NO_FETCH=ON to disable step 3.  Useful in air-gapped
# environments, CI with vendored deps, or when you want to guarantee
# only system / vcpkg packages are used.

option(SDLOS_NO_FETCH
    "Disable FetchContent fallback — use only find_package or vendored deps" OFF)

# C++ standard

set(CMAKE_CXX_STANDARD          23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default build type

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "[sdlos] No build type specified — defaulting to RelWithDebInfo")
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "CMake build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY
        STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# vcpkg toolchain (optional)

if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain file")
    message(STATUS "[sdlos] vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
endif()

# iOS / tvOS: force static libraries
#
# App Store policy forbids user-space .dylib in app bundles.
# Set as BOTH a cache variable (FORCE) AND a normal variable so that
# cmake_dependent_option() / option() calls inside vendored subdirectories
# see BUILD_SHARED_LIBS as already DEFINED and skip their defaults.

if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "iOS/tvOS requires static libraries" FORCE)
    set(BUILD_SHARED_LIBS OFF)

    set(SDL_SHARED        OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC        ON  CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY  OFF CACHE BOOL "" FORCE)

    set(SDLTTF_SHARED     OFF CACHE BOOL "" FORCE)
    set(SDLTTF_STATIC     ON  CACHE BOOL "" FORCE)
    set(SDLIMAGE_SHARED   OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_STATIC   ON  CACHE BOOL "" FORCE)
    set(SDLMIXER_SHARED   OFF CACHE BOOL "" FORCE)
    set(SDLMIXER_STATIC   ON  CACHE BOOL "" FORCE)

    message(STATUS "[sdlos] iOS/tvOS: forcing BUILD_SHARED_LIBS=OFF for all vendored deps")
endif()
