#pragma once

#include "render_tree.hh"
#include "style_applier.hh"

#include <string>
#include <string_view>
#include <vector>

namespace pce::sdlos::css {

/// One parsed CSS block.  Rules with a non-empty pseudo are skipped by
/// applyTo() and handled at runtime via buildHover() / tickHover().
struct Rule {
    std::string selector;
    bool        is_id  = false;
    std::string pseudo;   ///< "" | "hover" | "focus"
    std::vector<std::pair<std::string, std::string>> props; ///< jade-attr → value
};

struct HoverEntry {
    NodeHandle handle;
    bool       hovered  = false;
    std::vector<std::pair<std::string, std::string>> on_enter;
    std::vector<std::pair<std::string, std::string>> on_leave;
};


struct StyleSheet {
    std::vector<Rule>       rules;
    std::vector<HoverEntry> hover;

    [[nodiscard]] bool        empty() const { return rules.empty(); }
    [[nodiscard]] std::size_t size()  const { return rules.size();  }

    void applyTo(RenderTree& tree, NodeHandle root);

    void buildHover(RenderTree& tree, NodeHandle root);

    /// Compare phys_x/y against each hover entry's screen rect.
    /// Pass -1, -1 to force all entries unhovered (e.g. WINDOW_MOUSE_LEAVE).
    void tickHover(RenderTree& tree, float phys_x, float phys_y);
};

[[nodiscard]] StyleSheet parse(std::string_view source);

[[nodiscard]] StyleSheet load(const std::string& path);

} // namespace pce::sdlos::css
