#include "style_applier.hh"


#include <charconv>
#include <optional>
#include <string_view>


namespace pce::sdlos {

namespace {

// std::from_chars is locale-independent and allocation-free.
[[nodiscard]] static std::optional<float> toFloat(std::string_view s) noexcept
{
    if (s.empty()) return std::nullopt;
    float v{};
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return (ec == std::errc{}) ? std::optional<float>{v} : std::nullopt;
}


[[nodiscard]] static LayoutKind layoutKindForTag(std::string_view tag) noexcept
{
    // Block containers
    if (tag == "div"     || tag == "section"    || tag == "main"   ||
        tag == "header"  || tag == "footer"     || tag == "article"||
        tag == "aside"   || tag == "nav"        || tag == "layout" ||
        tag == "panel"   || tag == "calculator" || tag == "keypad" ||
        tag == "form"    || tag == "fieldset"   || tag == "ul"     ||
        tag == "ol"      || tag == "li"         || tag == "dl")
    {
        return LayoutKind::Block;
    }

    // Flex-row containers
    if (tag == "row" || tag == "hbox" || tag == "toolbar")
        return LayoutKind::FlexRow;

    // Flex-column containers
    if (tag == "col" || tag == "column" || tag == "vbox" || tag == "stack")
        return LayoutKind::FlexColumn;

    // Leaf, caller-managed or None "inline"  span p text label button h1-h6 img image a code pre etc.
    return LayoutKind::None;
}

} // anonymous namespace


void StyleApplier::apply(RenderNode& n) noexcept
{
    if (n.styles.empty()) return;

    // Tag → default LayoutKind like block or inline is treated as None
    {
        const auto tag = n.style("tag");
        if (!tag.empty())
            n.layout_kind = layoutKindForTag(tag);
    }

    // flexDirection overrides tag-derived LayoutKind
    {
        const auto fd = n.style("flexDirection");
        if      (fd == "row")    n.layout_kind = LayoutKind::FlexRow;
        else if (fd == "column") n.layout_kind = LayoutKind::FlexColumn;
    }

    // Geometry
    // width / height: written to both layout_props (for the layout engine)
    //                 and n.w / n.h (for draw callbacks that read them directly).
    if (const auto f = toFloat(n.style("width"))) {
        n.layout_props.width = *f;
        n.w                  = *f;
    }
    if (const auto f = toFloat(n.style("height"))) {
        n.layout_props.height = *f;
        n.h                   = *f;
    }
    // x / y: direct positioning — no layout engine involvement.
    if (const auto f = toFloat(n.style("x"))) n.x = *f;
    if (const auto f = toFloat(n.style("y"))) n.y = *f;

    // Flex item properties
    if (const auto f = toFloat(n.style("flexGrow")))   n.layout_props.flex_grow   = *f;
    if (const auto f = toFloat(n.style("flexShrink"))) n.layout_props.flex_shrink = *f;
    if (const auto f = toFloat(n.style("flexBasis")))  n.layout_props.flex_basis  = *f;

    // Container gap
    if (const auto f = toFloat(n.style("gap"))) n.layout_props.gap = *f;

    // justify / align
    {
        const auto jc = n.style("justifyContent");
        if      (jc == "center")        n.layout_props.justify = LayoutProps::Justify::Center;
        else if (jc == "flex-end"   ||
                 jc == "end")           n.layout_props.justify = LayoutProps::Justify::End;
        else if (jc == "space-between") n.layout_props.justify = LayoutProps::Justify::SpaceBetween;
        else if (jc == "space-around")  n.layout_props.justify = LayoutProps::Justify::SpaceAround;
    }
    {
        const auto ai = n.style("alignItems");
        if      (ai == "center")        n.layout_props.align = LayoutProps::Align::Center;
        else if (ai == "flex-end" ||
                 ai == "end")           n.layout_props.align = LayoutProps::Align::End;
        else if (ai == "flex-start" ||
                 ai == "start")         n.layout_props.align = LayoutProps::Align::Start;
        else if (ai == "stretch")       n.layout_props.align = LayoutProps::Align::Stretch;
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
        if      (ov == "hidden") n.layout_props.overflow = LayoutProps::Overflow::Hidden;
        else if (ov == "scroll") n.layout_props.overflow = LayoutProps::Overflow::Scroll;
    }

    // Visual properties

    // textAlign
    {
        const auto ta = n.style("textAlign");
        if      (ta == "left"  || ta == "start") n.visual_props.text_align = VisualProps::TextAlign::Left;
        else if (ta == "right" || ta == "end")   n.visual_props.text_align = VisualProps::TextAlign::Right;
        else if (ta == "center")                 n.visual_props.text_align = VisualProps::TextAlign::Center;
        // default TextAlign::Center preserves the existing centred-text behaviour
    }

    // fontWeight
    {
        const auto fw = n.style("fontWeight");
        if (fw == "bold" || fw == "700" || fw == "800" || fw == "900")
            n.visual_props.font_weight = VisualProps::FontWeight::Bold;
    }

    // borderRadius
    if (const auto f = toFloat(n.style("borderRadius")))
        n.visual_props.border_radius = *f;

    // padding shorthand — sets all four sides when present
    if (const auto f = toFloat(n.style("padding"))) {
        n.visual_props.padding_left   = *f;
        n.visual_props.padding_right  = *f;
        n.visual_props.padding_top    = *f;
        n.visual_props.padding_bottom = *f;
    }

    // ── objectFit — how an img fills its bounding box ────────────────────────
    {
        const auto of = n.style("objectFit");
        if      (of == "contain") n.visual_props.object_fit = VisualProps::ObjectFit::Contain;
        else if (of == "cover")   n.visual_props.object_fit = VisualProps::ObjectFit::Cover;
        // default ObjectFit::Fill: stretch to exactly w×h (no distortion guard)
    }

        // Individual sides — override the shorthand
    if (const auto f = toFloat(n.style("paddingLeft")))   n.visual_props.padding_left   = *f;
    if (const auto f = toFloat(n.style("paddingRight")))  n.visual_props.padding_right  = *f;
    if (const auto f = toFloat(n.style("paddingTop")))    n.visual_props.padding_top    = *f;
    if (const auto f = toFloat(n.style("paddingBottom"))) n.visual_props.padding_bottom = *f;
}

} // namespace pce::sdlos
