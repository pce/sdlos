# clima — Weather dashboard with optional city 3D mesh background
#
# Registered automatically by the root CMakeLists.txt glob:
#   file(GLOB _app_cmake_files "examples/apps/*/*.cmake")
#
# clima.css is copied flat to the binary directory so it can be loaded at
# runtime via SDL_GetBasePath() / "clima.css".
#
# City meshes are lazily loaded from data/models/<city>/ by clima.cc
# when a city button with an available mesh is selected.
# The DATA_DIR copy already places everything from examples/apps/clima/data/
# beside the binary; the explicit sdlos_copy_resource_to rules below keep
# assets/city-meshes/ as the canonical mesh source and make the intent clear.

sdlos_jade_app(clima
    examples/apps/clima/clima.jade
    BEHAVIOR examples/apps/clima/clima.cc
    DATA_DIR examples/apps/clima/data
    WIN_W 900
    WIN_H 620
)

sdlos_copy_resource(clima examples/apps/clima/clima.css)
