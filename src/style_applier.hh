#pragma once

#include "render_tree.hh"

namespace pce::sdlos {

struct StyleApplier {
    // No-op when styles is empty (fast path for C++-constructed nodes).
    static void apply(RenderNode& n) noexcept;
};

} // namespace pce::sdlos
