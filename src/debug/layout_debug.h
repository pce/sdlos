#pragma once

// Color map: None=gray, Block=orange, FlexRow=blue, FlexColumn=green, Grid=purple.
// Label format: "FR 10,20 200x48" — kind, x,y, w×h.
// Pass skip=layout_debug_node_ to exclude the overlay node from drawing over itself.

#include "../render_tree.h"

namespace pce::sdlos::debug {

struct LayoutDebugConfig {
    bool       show_none    = false;          // include None-kind nodes
    bool       show_labels  = true;           // requires loaded font
    float      fill_alpha   = 0.10f;          // 0 = outline only
    float      border_width = 1.5f;
    float      label_size   = 11.f;
    NodeHandle skip         = k_null_handle;  // subtree to exclude
};

/**
 * @brief Draws layout debug
 *
 * @param ctx     Execution or rendering context
 * @param tree    Red channel component [0, 1]
 * @param root    Red channel component [0, 1]
 * @param config  Configuration options struct
 */
void drawLayoutDebug(RenderContext&           ctx,
                     RenderTree&              tree,
                     NodeHandle               root,
                     const LayoutDebugConfig& config = {});

} // namespace pce::sdlos::debug
