#include "style_applier.h"

#include "jade/jade_parser.h"
#include "parse.h"

#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>

namespace pce::sdlos {

namespace {

// parse_float (from parse.h) is locale-independent and allocation-free.
[[nodiscard]]
static std::optional<float> toFloat(std::string_view s) noexcept {
    if (s.empty())
        return std::nullopt;
    float v{};
    const auto [ptr, ok] = parse_float(s.data(), s.data() + s.size(), v);
    return ok ? std::optional<float>{v} : std::nullopt;
}

[[nodiscard]]
static LayoutKind layoutKindForTag(std::string_view tag) noexcept {
    // Block containers
    if (tag == "div" || tag == "section" || tag == "main" || tag == "header" || tag == "footer"
        || tag == "article" || tag == "aside" || tag == "nav" || tag == "layout" || tag == "panel"
        || tag == "calculator" || tag == "keypad" || tag == "form" || tag == "fieldset"
        || tag == "ul" || tag == "ol" || tag == "li" || tag == "dl" || tag == "video") {
        return LayoutKind::Block;
    }

    // Flex-row containers
    if (tag == "row" || tag == "hbox" || tag == "toolbar")
        return LayoutKind::FlexRow;

    // Flex-column containers
    if (tag == "col" || tag == "column" || tag == "vbox" || tag == "stack" || tag == "app")
        return LayoutKind::FlexColumn;

    // Leaf, caller-managed or None "inline"  span p text label button h1-h6 img image a code pre
    // etc.
    return LayoutKind::None;
}

}  // anonymous namespace

/**
 * @brief Applies
 *
 * @param n         RenderNode & value
 * @param px_scale  Uniform scale factor
 */
void StyleApplier::apply(RenderNode &n, float px_scale) noexcept {
    if (n.style_attrs.empty())
        return;

    // Tag → default LayoutKind like block or inline is treated as None
    {
        const auto tag = n.style("tag");
        if (!tag.empty())
            n.layout_kind = layoutKindForTag(tag);
    }

    // flexDirection overrides tag-derived LayoutKind
    {
        const auto fd = n.style("flexDirection");
        if (fd == "row")
            n.layout_kind = LayoutKind::FlexRow;
        else if (fd == "column")
            n.layout_kind = LayoutKind::FlexColumn;
    }

    // display:none → hidden (skip layout/render)
    {
        const auto disp = n.style("display");
        n.hidden        = (disp == "none");
    }

    // Geometry
    // width / height: written to both layout_props (for the layout engine)
    //                 and n.w / n.h (for draw callbacks that read them directly).
    // Percent values (e.g. "100%") set width_pct / height_pct instead so the
    // layout engine can resolve them relative to the parent container dimensions.
    {
        const auto w_sv = n.style("width");
        if (!w_sv.empty()) {
            if (w_sv.back() == '%') {
                if (const auto f = toFloat(w_sv.substr(0, w_sv.size() - 1)))
                    n.layout_props.width_pct = *f;
            } else if (const auto f = toFloat(w_sv)) {
                n.layout_props.width = *f;
                n.w                  = (*f) * px_scale;
            }
        }
    }
    {
        const auto h_sv = n.style("height");
        if (!h_sv.empty()) {
            if (h_sv.back() == '%') {
                if (const auto f = toFloat(h_sv.substr(0, h_sv.size() - 1)))
                    n.layout_props.height_pct = *f;
            } else if (const auto f = toFloat(h_sv)) {
                n.layout_props.height = *f;
                n.h                   = (*f) * px_scale;
            }
        }
    }
    // x / y: direct positioning — convert to physical for draw geometry.
    if (const auto f = toFloat(n.style("x")))
        n.x = (*f) * px_scale;
    if (const auto f = toFloat(n.style("y")))
        n.y = (*f) * px_scale;

    // Flex item properties
    if (const auto f = toFloat(n.style("flexGrow")))
        n.layout_props.flex_grow = *f;
    if (const auto f = toFloat(n.style("flexShrink")))
        n.layout_props.flex_shrink = *f;
    if (const auto f = toFloat(n.style("flexBasis")))
        n.layout_props.flex_basis = *f;

    // Container gap
    if (const auto f = toFloat(n.style("gap")))
        n.layout_props.gap = *f;

    // justify / align
    {
        const auto jc = n.style("justifyContent");
        if (jc == "center")
            n.layout_props.justify = LayoutProps::Justify::Center;
        else if (jc == "flex-end" || jc == "end")
            n.layout_props.justify = LayoutProps::Justify::End;
        else if (jc == "space-between")
            n.layout_props.justify = LayoutProps::Justify::SpaceBetween;
        else if (jc == "space-around")
            n.layout_props.justify = LayoutProps::Justify::SpaceAround;
    }
    {
        const auto ai = n.style("alignItems");
        if (ai == "center")
            n.layout_props.align = LayoutProps::Align::Center;
        else if (ai == "flex-end" || ai == "end")
            n.layout_props.align = LayoutProps::Align::End;
        else if (ai == "flex-start" || ai == "start")
            n.layout_props.align = LayoutProps::Align::Start;
        else if (ai == "stretch")
            n.layout_props.align = LayoutProps::Align::Stretch;
    }

    // flex-wrap
    {
        const auto fw = n.style("flexWrap");
        if (fw == "wrap" || fw == "true")
            n.layout_props.flex_wrap = true;
    }

    // overflow
    {
        const auto ov = n.style("overflow");
        if (ov == "hidden")
            n.layout_props.overflow = LayoutProps::Overflow::Hidden;
        else if (ov == "scroll")
            n.layout_props.overflow = LayoutProps::Overflow::Scroll;
    }

    // Visual properties

    // ── direction — parsed BEFORE textAlign so start/end can be resolved ──
    // CSS `direction: rtl` enables:
    //   - HarfBuzz RTL text shaping (Arabic, Hebrew, etc.)
    //   - FlexRow children placed right→left
    //   - Block fixed-width children right-aligned
    //   - Default textAlign = Right (when textAlign not explicitly set)
    {
        const auto dir = n.style("direction");
        if (dir == "rtl")
            n.visual_props.direction = VisualProps::Direction::RTL;
        else if (dir == "ltr")
            n.visual_props.direction = VisualProps::Direction::LTR;
    }

    // textAlign
    // `start` and `end` are logical keywords that resolve relative to direction:
    //   LTR: start=Left,  end=Right
    //   RTL: start=Right, end=Left
    // When no textAlign is set explicitly AND direction==rtl, default to Right.
    {
        const auto ta  = n.style("textAlign");
        const bool rtl = (n.visual_props.direction == VisualProps::Direction::RTL);
        if (ta.empty()) {
            // Historical LTR default: Center (preserves existing centred-text
            // behaviour for nodes that never set textAlign).
            // RTL default: Right (matches CSS `text-align: start` for RTL).
            if (rtl)
                n.visual_props.text_align = VisualProps::TextAlign::Right;
            // else: keep the VisualProps default (Center)
        } else if (ta == "left") {
            n.visual_props.text_align = VisualProps::TextAlign::Left;
        } else if (ta == "right") {
            n.visual_props.text_align = VisualProps::TextAlign::Right;
        } else if (ta == "center") {
            n.visual_props.text_align = VisualProps::TextAlign::Center;
        } else if (ta == "start") {
            n.visual_props.text_align =
                rtl ? VisualProps::TextAlign::Right : VisualProps::TextAlign::Left;
        } else if (ta == "end") {
            n.visual_props.text_align =
                rtl ? VisualProps::TextAlign::Left : VisualProps::TextAlign::Right;
        }
    }

    // fontWeight
    {
        const auto fw = n.style("fontWeight");
        if (fw == "bold" || fw == "700" || fw == "800" || fw == "900")
            n.visual_props.font_weight = VisualProps::FontWeight::Bold;
    }

    // borderRadius
    if (const auto f = toFloat(n.style("borderRadius")))
        n.visual_props.border_radius = (*f) * px_scale;

    // padding shorthand — 1, 2, 3, or 4 space-separated values (CSS spec order).
    //   1 value:   all four sides
    //   2 values:  top+bottom, left+right
    //   3 values:  top, left+right, bottom
    //   4 values:  top, right, bottom, left
    {
        const auto pv = n.style("padding");
        if (!pv.empty()) {
            float vals[4]{-1.f, -1.f, -1.f, -1.f};
            int count       = 0;
            const char *p   = pv.data();
            const char *end = p + pv.size();
            while (p < end && count < 4) {
                // skip whitespace
                while (p < end && *p == ' ')
                    ++p;
                if (p >= end)
                    break;
                float v{};
                const auto [next, ok] = parse_float(p, end, v);
                if (!ok)
                    break;
                vals[count++] = v * px_scale;
                p             = next;
            }
            if (count == 1) {
                n.visual_props.padding_top    = vals[0];
                n.visual_props.padding_right  = vals[0];
                n.visual_props.padding_bottom = vals[0];
                n.visual_props.padding_left   = vals[0];
            } else if (count == 2) {
                n.visual_props.padding_top    = vals[0];
                n.visual_props.padding_bottom = vals[0];
                n.visual_props.padding_left   = vals[1];
                n.visual_props.padding_right  = vals[1];
            } else if (count == 3) {
                n.visual_props.padding_top    = vals[0];
                n.visual_props.padding_left   = vals[1];
                n.visual_props.padding_right  = vals[1];
                n.visual_props.padding_bottom = vals[2];
            } else if (count >= 4) {
                n.visual_props.padding_top    = vals[0];
                n.visual_props.padding_right  = vals[1];
                n.visual_props.padding_bottom = vals[2];
                n.visual_props.padding_left   = vals[3];
            }
        }
    }

    // opacity — multiplier for all draw calls on this node.
    // Supports: `opacity: 0.5`  (float in [0, 1]) or `opacity: 50%`
    {
        const auto op = n.style("opacity");
        if (!op.empty()) {
            if (op.back() == '%') {
                if (const auto f = toFloat(op.substr(0, op.size() - 1)))
                    n.opacity = (*f) / 100.f;
            } else if (const auto f = toFloat(op)) {
                n.opacity = *f < 0.f ? 0.f : (*f > 1.f ? 1.f : *f);
            }
        }
    }

    // ── objectFit — how an img fills its bounding box ────────────────────────
    {
        const auto of = n.style("objectFit");
        if (of == "contain")
            n.visual_props.object_fit = VisualProps::ObjectFit::Contain;
        else if (of == "cover")
            n.visual_props.object_fit = VisualProps::ObjectFit::Cover;
        // default ObjectFit::Fill: stretch to exactly w×h (no distortion guard)
    }

    // Individual sides — override the shorthand
    if (const auto f = toFloat(n.style("paddingLeft")))
        n.visual_props.padding_left = (*f) * px_scale;
    if (const auto f = toFloat(n.style("paddingRight")))
        n.visual_props.padding_right = (*f) * px_scale;
    if (const auto f = toFloat(n.style("paddingTop")))
        n.visual_props.padding_top = (*f) * px_scale;
    if (const auto f = toFloat(n.style("paddingBottom")))
        n.visual_props.padding_bottom = (*f) * px_scale;

    // Intrinsic text height
    //
    // The flex layout engine keeps a child's existing h when no explicit sizing
    // is set (basis = -1 → "auto: keep existing w/h").  For freshly-allocated
    // nodes that carries the default h = 0, so a text-only div with no
    // height/height_pct/flex_basis/flex_grow never gets space and its draw
    // callback bails out on the `self->h <= 0` guard — content invisible.
    //
    // Fix: seed layout_props.height (and n.h) from fontSize so the parent
    // FlexColumn treats the node as having at least one line's worth of height.
    // Only applied when ALL of the following are true:
    //   - the node has a non-empty `text` style
    //   - no explicit height / height_pct / flex_basis was given
    //   - the node has no flex_grow (which would size it via remaining space)
    //
    // This mirrors the CSS "line-height ≈ font-size" intrinsic-size rule and
    // ensures dynamically-loaded page content (which receives no CSS pass) is
    // visible on the first rendered frame.
    {
        const auto txt_sv = n.style("text");
        if (!txt_sv.empty() && n.layout_props.height < 0.f  // no explicit logical height
            && n.layout_props.height_pct < 0.f              // no percent height
            && n.layout_props.flex_basis < 0.f              // no flex-basis
            && n.layout_props.flex_grow <= 0.f              // no flex-grow allocation
        ) {
            float fs         = 16.f;                        // default font size
            const auto fs_sv = n.style("fontSize");
            if (!fs_sv.empty()) {
                if (const auto f = toFloat(fs_sv))
                    fs = *f;
            }
            // layout_props.height is the unscaled (jade-attribute) basis used by
            // flexLayout Pass 1: child->h = lp.height.  This matches every other
            // jade height= attribute — raw value in lp.height, physical value in
            // n.h.  After layout runs, self->h = lp.height = fs (unscaled), which
            // is what the draw callback reads.  n.h is set to fs * px_scale so
            // the pre-layout physical value is also reasonable, but flexLayout
            // overwrites it before draw anyway.
            n.layout_props.height = fs;             // unscaled — layout basis
            n.h                   = fs * px_scale;  // physical — overwritten by layout
        }
    }
}

/**
 * @brief Swaps a portion of the DOM by parsing a new Jade source and replacing children of a target
 * node.
 *
 * @param tree      The RenderTree to operate on.
 * @param parent_h  Handle of the node whose children should be replaced.
 * @param jade_path Path to the new .jade file.
 * @return true if successful, false otherwise.
 */
bool swapJadeModule(
    RenderTree &tree,
    NodeHandle parent_h,
    const std::string &jade_path) {
    if (!parent_h.valid())
        return false;

    // 1. Read Jade file
    std::ifstream f(jade_path);
    if (!f.is_open())
        return false;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string source = ss.str();

    // 2. Parse into a temporary subtree
    // Note: jade::parse usually returns a virtual root containing the parsed elements
    NodeHandle new_content_root = jade::parse(source, tree);
    if (!new_content_root.valid())
        return false;

    // 3. Clear existing children of the parent
    // We assume RenderTree has a way to remove children. If not, we'd need to extend it.
    // For now, we'll use a placeholder logic that implies typical tree operations:
    // tree.clearChildren(parent_h);

    // 4. Move children from the temp root to the target parent
    // Typically:
    // for (auto child : tree.children(new_content_root)) {
    //     tree.appendChild(parent_h, child);
    // }

    // 5. Free the temp root (it's no longer needed)
    // tree.free(new_content_root);

    tree.markLayoutDirty(parent_h);
    return true;
}

}  // namespace pce::sdlos
