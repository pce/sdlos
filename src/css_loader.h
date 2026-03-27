#pragma once

#include "render_tree.h"
#include "style_applier.h"

#include <string>
#include <string_view>
#include <vector>

namespace pce::sdlos::css {

/// One parsed CSS block.  Rules with a non-empty pseudo are skipped by
/// applyTo() and handled at runtime via buildHover() / tickHover().
/// runtime handling is work in progress
struct Rule {
    std::string selector;
    bool        is_id  = false;
    std::string pseudo;   ///< "" | "hover" | "focus" | "active"
    std::vector<std::pair<std::string, std::string>> props; ///< jade-attr → value
};

struct HoverEntry {
    NodeHandle handle;
    bool       hovered  = false;
    std::vector<std::pair<std::string, std::string>> on_enter;
    std::vector<std::pair<std::string, std::string>> on_leave;
};

/// Radio-group active state — one entry per child of a toggle-group parent.
/// on_enter  = the :active CSS rule's property values (applied when activated).
/// on_leave  = the CSS *base* values for the same properties (never snapshots
///             run-time state, so behavior-set styles don't corrupt the reset).
struct ActiveEntry {
    NodeHandle handle;
    NodeHandle group_parent;   ///< the node bearing toggle-group attr
    bool       active = false;
    std::vector<std::pair<std::string, std::string>> on_enter;
    std::vector<std::pair<std::string, std::string>> on_leave;
};


struct StyleSheet {
    std::vector<Rule>        rules;
    std::vector<HoverEntry>  hover;
    std::vector<ActiveEntry> active_entries;

    /**
     * @brief Empty
     *
     * @return true on success, false on failure
     */
    bool        empty() const { return rules.empty(); }
    /**
     * @brief Size
     *
     * @return Integer result; negative values indicate an error code
     */
    std::size_t size()  const { return rules.size();  }

    /**
     * @brief Applies to
     *
     * @param tree      Red channel component [0, 1]
     * @param root      Red channel component [0, 1]
     * @param px_scale  Uniform scale factor
     */
    void applyTo(RenderTree& tree, NodeHandle root, float px_scale = 1.0f);

    /**
     * @brief Builds hover
     *
     * @param tree      Red channel component [0, 1]
     * @param root      Red channel component [0, 1]
     * @param px_scale  Uniform scale factor
     */
    void buildHover(RenderTree& tree, NodeHandle root, float px_scale = 1.0f);

    /// Compare phys_x/y against each hover entry's screen rect.
    /// Pass -1, -1 to force all entries unhovered (e.g. WINDOW_MOUSE_LEAVE).
    void tickHover(RenderTree& tree, float phys_x, float phys_y, float px_scale = 1.0f);

    /// Scan children of every toggle-group node; build ActiveEntry records
    /// from matching :active CSS rules.  Call after applyTo() and after
    /// jade_app_init() so the tree layout is stable.
    void buildActive(RenderTree& tree, NodeHandle root, float px_scale = 1.0f);

    /// Radio-style toggle: deactivate all siblings in the same toggle-group,
    /// activate `clicked`.  No-op when clicked has no ActiveEntry.
    void activateNode(RenderTree& tree, NodeHandle clicked, float px_scale = 1.0f);
};

/**
 * @brief Parses
 *
 * @param source  Red channel component [0, 1]
 *
 * @return StyleSheet result
 */
StyleSheet parse(std::string_view source);

/// Load a CSS file.  Supports @import "relative/path.css"; directives
/// (resolved relative to the loading file's directory; circular imports
/// are silently skipped).
StyleSheet load(const std::string& path);

} // namespace pce::sdlos::css
