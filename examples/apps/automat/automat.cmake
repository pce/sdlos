# automat — Crystal Altar sequential composition demo
#
# Registered automatically by the root CMakeLists.txt glob:
#   file(GLOB _app_cmake_files "examples/apps/*/*.cmake")
#
# Resources
#   scene.css     : 3D transform + PBR material overrides per mesh proxy
#   automat.css   : 2D overlay HUD, modal, button styles
#   data/models/  : 5 GLB files loaded by GltfScene::attach() at startup

sdlos_jade_app(automat
    examples/apps/automat/app.jade
    BEHAVIOR examples/apps/automat/automat.cc
    DATA_DIR examples/apps/automat/data
    WIN_W 1280
    WIN_H 800
)

sdlos_copy_resource(automat examples/apps/automat/scene.css)
sdlos_copy_resource(automat examples/apps/automat/automat.css)

