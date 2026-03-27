// main.cxx — Program entry point.
//
// Why main is not inside namespace pce::sdlos
// ----------------------------------------
// The C++ standard (ISO C++23 §6.9.3.1) requires that the program entry point
// named `main` has external linkage at global scope. Placing it inside any
// namespace gives it a mangled symbol the runtime linker cannot find, producing
// a "undefined reference to `main`" (or equivalent) link error.
//
// Instead, all meaningful startup logic lives in pce::sdlos::run(). This file is
// intentionally a thin bridge: C runtime → C++ namespace. Nothing else belongs
// here. Adding logic directly to main would put it outside the namespace and
// outside the reach of the rest of the codebase's conventions.

#include "os.hh"

#include <cstdlib>    // EXIT_SUCCESS, EXIT_FAILURE

namespace pce::sdlos {

// ---------------------------------------------------------------------------
// pce::sdlos::run — the real entry point, fully inside the namespace.
//
// Owns the OS value on the stack so that its destructor runs before the C
// runtime tears down static storage, giving every component a clean shutdown
// path through normal C++ RAII even if run() exits via an exception.
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
