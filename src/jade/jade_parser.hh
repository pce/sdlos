#pragma once

// JadeLite parser — single pass, no intermediate tree.
// Allocates RenderNodes directly into the given RenderTree.
// StyleApplier::apply() is called on each node before it is attached.
// See architecture.md § JadeLite Parser for details.

#include "../render_tree.hh"

#include <string_view>

namespace pce::sdlos::jade {

// Parse source into tree. Returns a virtual FlexColumn root whose children
// are the top-level elements. Forgiving: unknown constructs are silently skipped.
[[nodiscard]]
NodeHandle parse(std::string_view source, RenderTree& tree);

} // namespace pce::sdlos::jade
