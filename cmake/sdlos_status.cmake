# sdlos_status.cmake — configure-time status summary.
#
# Included last so all variables are set.

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
message(STATUS "│  simdutf     : ON (FetchContent v8.2.0)")
message(STATUS "│  utf8cpp     : ON (FetchContent v4.0.9)")
message(STATUS "│  fastgltf    : ON (FetchContent v0.9.0)")
message(STATUS "│  SDL_camera  : (via SDL3 core)")
message(STATUS "│  Tests       : ${SDLOS_ENABLE_TESTS}")
message(STATUS "│  No-fetch    : ${SDLOS_NO_FETCH}")
message(STATUS "│  Sanitizers  : ${SDLOS_STATUS_SANITIZERS}")
message(STATUS "│  clang-format: ${SDLOS_STATUS_CLANG_FORMAT}")
message(STATUS "│  clang-tidy  : ${SDLOS_STATUS_CLANG_TIDY}")
message(STATUS "│  IWYU        : ${SDLOS_STATUS_IWYU}")
message(STATUS "└───────────────────────────────────────── ─ ── ─")
message(STATUS "")

