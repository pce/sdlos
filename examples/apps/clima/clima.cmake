# clima — Weather dashboard with optional city 3D mesh background
#
# Registered automatically by the root CMakeLists.txt glob:
#   file(GLOB _app_cmake_files "examples/apps/*/*.cmake")
#
# clima.css is copied flat to the binary directory so it can be loaded at
# runtime via SDL_GetBasePath() / "clima.css".
#
# City meshes are lazily loaded from data/models/cities/<city>/ by clima.cc
# when a city button with an available mesh is selected.

sdlos_jade_app(clima
    examples/apps/clima/clima.jade
    BEHAVIOR examples/apps/clima/clima.cc
    DATA_DIR examples/apps/clima/data
    WIN_W 900
    WIN_H 620
)

sdlos_copy_resource(clima examples/apps/clima/clima.css)

# ── City meshes — Amsterdam ───────────────────────────────────────────────────
sdlos_copy_resource_to(clima
    "assets/city-meshes/amsterdam/amsterdam.gltf"
    "data/models/cities/amsterdam/amsterdam.gltf")
sdlos_copy_resource_to(clima
    "assets/city-meshes/amsterdam/gltf_buffer_0.bin"
    "data/models/cities/amsterdam/gltf_buffer_0.bin")
sdlos_copy_resource_to(clima
    "assets/city-meshes/amsterdam/gltf_buffer_1.bin"
    "data/models/cities/amsterdam/gltf_buffer_1.bin")

# ── City meshes — Kyoto ───────────────────────────────────────────────────────
sdlos_copy_resource_to(clima
    "assets/city-meshes/kyoto/kyoto.gltf"
    "data/models/cities/kyoto/kyoto.gltf")
sdlos_copy_resource_to(clima
    "assets/city-meshes/kyoto/gltf_buffer_0.bin"
    "data/models/cities/kyoto/gltf_buffer_0.bin")
sdlos_copy_resource_to(clima
    "assets/city-meshes/kyoto/gltf_buffer_1.bin"
    "data/models/cities/kyoto/gltf_buffer_1.bin")
