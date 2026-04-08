#include "layout_debug.h"
#include "../text_renderer.h"

#include <algorithm>
#include <format>
#include <vector>

namespace pce::sdlos::debug {

namespace {

struct DebugColor { float r, g, b; };

constexpr DebugColor colorForKind(LayoutKind kind) noexcept
{
    switch (kind) {
        case LayoutKind::None:       return {0.55f, 0.55f, 0.58f};  // gray
        case LayoutKind::Block:      return {1.00f, 0.55f, 0.10f};  // orange
        case LayoutKind::FlexRow:    return {0.15f, 0.50f, 1.00f};  // blue
        case LayoutKind::FlexColumn: return {0.10f, 0.80f, 0.32f};  // green
        case LayoutKind::Grid:       return {0.75f, 0.20f, 0.90f};  // purple
    }
    return {0.50f, 0.50f, 0.50f};
}

constexpr const char* kindAbbrev(LayoutKind kind) noexcept
{
    switch (kind) {
        case LayoutKind::None:       return "··";
        case LayoutKind::Block:      return "BL";
        case LayoutKind::FlexRow:    return "FR";
        case LayoutKind::FlexColumn: return "FC";
        case LayoutKind::Grid:       return "GD";
    }
    return "??";
}

constexpr const char* justifyAbbrev(LayoutProps::Justify j) noexcept
{
    switch (j) {
        case LayoutProps::Justify::Start:        return "S";
        case LayoutProps::Justify::Center:       return "C";
        case LayoutProps::Justify::End:          return "E";
        case LayoutProps::Justify::SpaceBetween: return "SB";
        case LayoutProps::Justify::SpaceAround:  return "SA";
    }
    return "?";
}

constexpr const char* alignAbbrev(LayoutProps::Align a) noexcept
{
    switch (a) {
        case LayoutProps::Align::Start:   return "S";
        case LayoutProps::Align::Center:  return "C";
        case LayoutProps::Align::End:     return "E";
        case LayoutProps::Align::Stretch: return "X";
    }
    return "?";
}



std::string secondaryLabel(const RenderNode& n)
{
    switch (n.layout_kind) {
        case LayoutKind::FlexRow:
        case LayoutKind::FlexColumn:
            return std::format("j={} a={} g={:.0f}",
                               justifyAbbrev(n.layout_props.justify),
                               alignAbbrev  (n.layout_props.align),
                               n.layout_props.gap);
        case LayoutKind::Block:
            if (n.layout_props.gap > 0.f)
                return std::format("g={:.0f}", n.layout_props.gap);
            return {};
        default:
            return {};
    }
}

void drawLabel(RenderContext& ctx,
               const std::string& text,
               float lx, float ly,
               float size,
               float cr, float cg, float cb)
{
    if (text.empty())                  return;
    if (!ctx.text_renderer)            return;
    if (!ctx.text_renderer->isReady()) return;

    // Query glyph dimensions before drawing the background pill.
    const GlyphTexture gt = ctx.text_renderer->ensureTexture(text, size);
    if (!gt.valid()) return;

    const float pad_h = 4.f;
    const float pad_v = 2.f;
    const float bw    = static_cast<float>(gt.width)  + pad_h * 2.f;
    const float bh    = static_cast<float>(gt.height) + pad_v * 2.f;

    ctx.drawRect(lx - pad_h, ly - pad_v, bw, bh,
                 0.04f, 0.04f, 0.05f, 0.82f);

    ctx.drawText(text, lx, ly, size, cr, cg, cb, 1.0f);
}

} // anonymous namespace

/**
 * @brief Draws layout debug
 *
 * @param ctx   Execution or rendering context
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 * @param cfg   Configuration options struct
 */
void drawLayoutDebug(RenderContext&           ctx,
                     RenderTree&              tree,
                     NodeHandle               root,
                     const LayoutDebugConfig& cfg)
{
    if (!root.valid()) return;

    // Each stack entry carries the node handle plus the accumulated absolute
    // origin of its *parent*.  Adding n->x / n->y gives the node's own
    // absolute screen position, which is what drawRect / drawText expect.
    struct Entry {
        NodeHandle h;
        float      pax;   // parent absolute x
        float      pay;   // parent absolute y
    };

    std::vector<Entry> stack;
    stack.reserve(64);

    // Start at root: root's own position contributes to its children.
    // We push root with pax/pay = 0 so its absolute origin = (0+root->x, 0+root->y).
    stack.push_back({root, 0.f, 0.f});

    while (!stack.empty()) {
        const auto [h, pax, pay] = stack.back();
        stack.pop_back();

        if (h == cfg.skip) continue;

        RenderNode* n = tree.node(h);
        if (!n) continue;   // stale handle — skip silently

        // Absolute position of this node.
        const float ax = pax + n->x;
        const float ay = pay + n->y;

        const bool is_none     = (n->layout_kind == LayoutKind::None);
        const bool has_area    = (n->w > 0.f && n->h > 0.f);
        const bool should_draw = has_area && (!is_none || cfg.show_none);

        if (should_draw) {
            const DebugColor c = colorForKind(n->layout_kind);

            ctx.drawRect(ax, ay, n->w, n->h,
                         c.r, c.g, c.b, cfg.fill_alpha);

            ctx.drawRectOutline(ax, ay, n->w, n->h,
                                cfg.border_width,
                                c.r, c.g, c.b, 0.90f);

            if (cfg.show_labels) {
                const float lx = ax + cfg.border_width + 3.f;
                float       ly = ay + cfg.border_width + 2.f;

                // primaryLabel now receives absolute coords for the position
                // annotation so the overlay matches what you see on screen.
                drawLabel(ctx,
                          std::format("{} {:.0f},{:.0f} {:.0f}x{:.0f}",
                                      kindAbbrev(n->layout_kind),
                                      ax, ay, n->w, n->h),
                          lx, ly, cfg.label_size,
                          c.r, c.g, c.b);

                const std::string sec = secondaryLabel(*n);
                if (!sec.empty()) {
                    ly += cfg.label_size * 1.3f;
                    drawLabel(ctx, sec,
                              lx, ly, cfg.label_size * 0.90f,
                              c.r * 0.85f, c.g * 0.85f, c.b * 0.85f);
                }

                // Optional: show the node id / class in a third line.
                {
                    const auto id  = n->style("id");
                    const auto cls = n->style("class");
                    std::string tag_info;
                    if (!id.empty())  { tag_info += '#'; tag_info += id;  }
                    if (!cls.empty()) {
                        if (!tag_info.empty()) tag_info += ' ';
                        tag_info += '.'; tag_info += cls;
                    }
                    if (!tag_info.empty()) {
                        ly += cfg.label_size * 1.2f;
                        drawLabel(ctx, tag_info,
                                  lx, ly, cfg.label_size * 0.85f,
                                  0.90f, 0.90f, 0.55f);
                    }
                }
            }
        }

        // Push children: their parent absolute origin is (ax, ay).
        const auto base = static_cast<std::ptrdiff_t>(stack.size());

        for (NodeHandle c = n->child; c.valid(); ) {
            const RenderNode* cn = tree.node(c);
            if (!cn) break;
            stack.push_back({c, ax, ay});
            c = cn->sibling;
        }

        std::reverse(stack.begin() + base, stack.end());
    }
}

} // namespace pce::sdlos::debug
