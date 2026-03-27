// Why main is not inside namespace pce::sdlos
// --------------------------------------------
// C++23 §6.9.3.1 requires the entry point named `main` to have external
// linkage at global scope.  Placing it inside any namespace produces a
// mangled symbol the runtime linker cannot find.  All meaningful startup
// logic lives in pce::sdlos::run() instead.

#include "os.hh"

#include <cstdlib>    // EXIT_SUCCESS, EXIT_FAILURE

namespace pce::sdlos {

// ---------------------------------------------------------------------------
// pce::sdlos::run — the real entry point, fully inside the namespace.
//
// OS is owned on the stack so its destructor runs before the C runtime tears
// down static storage, giving every component a clean RAII shutdown path.
// ---------------------------------------------------------------------------
static int run(int /*argc*/, char* /*argv*/[])
{
    OS os;

    if (!os.boot()) {
        return EXIT_FAILURE;
    }

    os.run();
    os.shutdown();

    return EXIT_SUCCESS;
}

} // namespace pce::sdlos

// ---------------------------------------------------------------------------
// main — entry point bridge, nothing more.
// ---------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    return pce::sdlos::run(argc, argv);
}
