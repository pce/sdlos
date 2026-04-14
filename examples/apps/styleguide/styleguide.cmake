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

# Font Awesome 6 Solid — icon font for nav bar buttons (PREV / NEXT / PLAY / PAUSE).
# FA codepoints live in the Unicode Private Use Area (U+F000+) so they never
# collide with glyphs in the primary or Twemoji fallback font.
sdlos_copy_resource_to(styleguide
    "assets/fonts/fa-6-solid-900.otf"
    "data/fonts/fa-6-solid-900.otf")
