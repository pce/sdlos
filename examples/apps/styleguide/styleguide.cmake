# styleguide — 4-slide Keynote-style presentation
#
# Registered automatically by the root CMakeLists.txt glob:
#   file(GLOB _app_cmake_files "examples/apps/*/*.cmake")
#
# Resources
#   scene.css        : PBR material overrides, loaded at runtime via SDL_GetBasePath()
#   data/pages/      : per-slide jade files loaded at runtime
#   data/models/     : Crystal_Small_03.glb — included via DATA_DIR copy below

sdlos_jade_app(styleguide
    examples/apps/styleguide/styleguide.jade
    BEHAVIOR examples/apps/styleguide/styleguide.cc
    DATA_DIR examples/apps/styleguide/data
    WIN_W 1200
    WIN_H 800
)

sdlos_copy_resource(styleguide examples/apps/styleguide/scene.css)

