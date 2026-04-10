#pragma once

#include "render_tree.h"

namespace pce::sdlos {

struct StyleApplier {
    // No-op when styles is empty (fast path for C++-constructed nodes).
    // `px_scale` is the device pixel scale (physical / logical). Style values
    // authored in CSS units are treated as logical pixels here; callers should
    // pass the current renderer pixel scale so visual properties and draw
    // geometry are written in physical pixels while layout_props remain
    // logical for the layout engine.
    /**
     * @brief Applies
     *
     * @param n         RenderNode & value
     * @param px_scale  Uniform scale factor
     */
    static void apply(RenderNode &n, float px_scale = 1.0f) noexcept;
};

/**
 * @brief Swaps a JADE module in the render tree.
 *
 * @param tree       The render tree to modify.
 * @param parent_h   Handle to the parent node where the module will be swapped.
 * @param jade_path  Filesystem path to the .jade file.
 * @return true on success, false on failure.
 */
bool swapJadeModule(RenderTree &tree, NodeHandle parent_h, const std::string &jade_path);

}  // namespace pce::sdlos
