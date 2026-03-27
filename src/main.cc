// Why main is not inside namespace pce::sdlos?
// C++23 §6.9.3.1 requires the entry point named `main` to have external
// linkage at global scope.  Placing it inside any namespace produces a
// mangled symbol the runtime linker cannot find.  All meaningful startup
// logic lives in pce::sdlos::run() instead.

#include "io.h"

#include <cstdlib>    // EXIT_SUCCESS, EXIT_FAILURE

namespace pce::sdlos {


/**
 * @brief run  the real entry point for the tree walk
 * IO is owned on the stack so its destructor runs before the C runtime tears
 * down static storage, giving every component a clean RAII shutdown path.
 *
 * @param param0  Red channel component [0, 1]
 * @param param1  Red channel component [0, 1]
 *
 * @return Integer result; negative values indicate an error code
 */
static int run(int /*argc*/, char* /*argv*/[])
{
    IO io;

    if (!io.boot()) {
        return EXIT_FAILURE;
    }

    io.run();
    io.shutdown();

    return EXIT_SUCCESS;
}

} // namespace pce::sdlos

/**
 * @brief Main entry point bridge, nothing more.
 *
 * @param argc  argument count
 * @param argv  Command-line argument vector
 *
 * @return Integer result; negative values indicate an error code
 */
int main(int argc, char* argv[])
{
    return pce::sdlos::run(argc, argv);
}
