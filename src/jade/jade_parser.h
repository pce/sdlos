#pragma once

#include "../render_tree.h"

#include <string_view>

namespace pce::sdlos::jade {

/**
 * @brief Parses
 *
 * Parse source into tree. Returns a virtual FlexColumn root whose children
 * are the top-level elements. Forgiving: unknown constructs are silently skipped.
 *
 * @param source
 * @param tree
 *
 * @return Handle to the node, or k_null_handle on failure
 */
[[nodiscard]]
NodeHandle parse(std::string_view source, RenderTree &tree);

}  // namespace pce::sdlos::jade
