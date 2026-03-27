#include "layout_debug.hh"
#include "../text_renderer.hh"

#include <algorithm>
#include <format>
#include <vector>

namespace pce::sdlos::debug {

namespace {

struct DebugColor { float r, g, b; };

[[nodiscard]]
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

[[nodiscard]]
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

[[nodiscard]]
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

[[nodiscard]]
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

[[nodiscard]]
std::string primaryLabel(const RenderNode& n)
{
    return std::format("{} {:.0f},{:.0f} {:.0f}x{:.0f}",
                       kindAbbrev(n.layout_kind),
                       n.x, n.y,
                       n.w, n.h);
}

[[nodiscard]]
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

void drawLayoutDebug(RenderContext&           ctx,
                     RenderTree&              tree,
                     NodeHandle               root,
                     const LayoutDebugConfig& cfg)
{
    if (!root.valid()) return;

    // Push children left-to-right then reverse the newly added segment so
    // the leftmost child sits on top of the stack (popped first = preorder).
    std::vector<NodeHandle> stack;
    stack.reserve(64);
    stack.push_back(root);

    while (!stack.empty()) {
        const NodeHandle h = stack.back();
        stack.pop_back();

        if (h == cfg.skip) continue;

        RenderNode* n = tree.node(h);
        if (!n) continue;   // stale handle — skip silently

        const bool is_none     = (n->layout_kind == LayoutKind::None);
        const bool has_area    = (n->w > 0.f && n->h > 0.f);
        const bool should_draw = has_area && (!is_none || cfg.show_none);

        if (should_draw) {
            const DebugColor c = colorForKind(n->layout_kind);

            ctx.drawRect(n->x, n->y, n->w, n->h,
                         c.r, c.g, c.b, cfg.fill_alpha);

            ctx.drawRectOutline(n->x, n->y, n->w, n->h,
                                cfg.border_width,
                                c.r, c.g, c.b, 0.90f);

            if (cfg.show_labels) {
                const float lx = n->x + cfg.border_width + 3.f;
                float       ly = n->y + cfg.border_width + 2.f;

                drawLabel(ctx, primaryLabel(*n),
                          lx, ly, cfg.label_size,
                          c.r, c.g, c.b);

                const std::string sec = secondaryLabel(*n);
                if (!sec.empty()) {
                    ly += cfg.label_size * 1.3f;
                    drawLabel(ctx, sec,
                              lx, ly, cfg.label_size * 0.90f,
                              c.r * 0.85f, c.g * 0.85f, c.b * 0.85f);
                }
            }
        }

        const auto base = static_cast<std::ptrdiff_t>(stack.size());

        for (NodeHandle c = n->child; c.valid(); ) {
            const RenderNode* cn = tree.node(c);
            if (!cn) break;
            stack.push_back(c);
            c = cn->sibling;
        }

        std::reverse(stack.begin() + base, stack.end());
    }
}

} // namespace pce::sdlos::debug
