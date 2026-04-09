# sdlos_tests.cmake — Catch2 unit tests (optional).
#
# Enable with:  cmake -B build -DSDLOS_ENABLE_TESTS=ON
#
# Fetches Catch2 via FetchContent and defines test executables.

option(SDLOS_ENABLE_TESTS "Build unit tests (requires internet on first configure)" OFF)

if(NOT SDLOS_ENABLE_TESTS)
    message(STATUS "[sdlos] Tests: disabled (pass -DSDLOS_ENABLE_TESTS=ON to enable)")
    return()
endif()

include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
)
FetchContent_MakeAvailable(Catch2)

# test_jade

add_executable(test_jade tests/test_jade.cc)
target_link_libraries(test_jade PRIVATE sdlos_engine Catch2::Catch2WithMain)
sdlos_target_compile_options(test_jade)
sdlos_link_sdl(test_jade)

# test_frame_graph

add_executable(test_frame_graph tests/test_frame_graph.cc)
target_link_libraries(test_frame_graph PRIVATE sdlos_engine Catch2::Catch2WithMain)
sdlos_target_compile_options(test_frame_graph)
sdlos_link_sdl(test_frame_graph)

# test_render_tree

add_executable(test_render_tree tests/test_render_tree.cc)
target_link_libraries(test_render_tree PRIVATE sdlos_engine Catch2::Catch2WithMain)
sdlos_target_compile_options(test_render_tree)
sdlos_link_sdl(test_render_tree)

# CTest discovery

include(CTest)
include(Catch)
catch_discover_tests(test_jade)
catch_discover_tests(test_frame_graph)
catch_discover_tests(test_render_tree)

message(STATUS "[sdlos] Tests: test_jade + test_frame_graph + test_render_tree enabled")

