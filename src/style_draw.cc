#include "style_draw.h"

#include "text_renderer.h"

#include <charconv>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace pce::sdlos {

namespace {

// ── Color ─────────────────────────────────────────────────────────────────────

struct RGBAf {
    float r = 0.f, g = 0.f, b = 0.f, a = 1.f;
};

[[nodiscard]]
static bool parseHexColor(std::string_view s, RGBAf &out) noexcept {
    if (s.empty() || s[0] != '#')
        return false;
    s.remove_prefix(1);

    auto nib = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9')
            return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f')
            return static_cast<uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F')
            return static_cast<uint8_t>(c - 'A' + 10);
        return 0;
    };

    // #rrggbbaa — 8 hex digits, explicit alpha
    if (s.size() == 8) {
        out.r = static_cast<float>((nib(s[0]) << 4) | nib(s[1])) / 255.f;
        out.g = static_cast<float>((nib(s[2]) << 4) | nib(s[3])) / 255.f;
        out.b = static_cast<float>((nib(s[4]) << 4) | nib(s[5])) / 255.f;
        out.a = static_cast<float>((nib(s[6]) << 4) | nib(s[7])) / 255.f;
        return true;
    }
    // #rrggbb — 6 hex digits, opaque
    if (s.size() == 6) {
        out.r = static_cast<float>((nib(s[0]) << 4) | nib(s[1])) / 255.f;
        out.g = static_cast<float>((nib(s[2]) << 4) | nib(s[3])) / 255.f;
        out.b = static_cast<float>((nib(s[4]) << 4) | nib(s[5])) / 255.f;
        out.a = 1.f;
        return true;
    }
    // #rgba — 4 hex digits, shorthand with alpha
    if (s.size() == 4) {
        const uint8_t rn = nib(s[0]), gn = nib(s[1]), bn = nib(s[2]), an = nib(s[3]);
        out.r = static_cast<float>((rn << 4) | rn) / 255.f;
        out.g = static_cast<float>((gn << 4) | gn) / 255.f;
        out.b = static_cast<float>((bn << 4) | bn) / 255.f;
        out.a = static_cast<float>((an << 4) | an) / 255.f;
        return true;
    }
    // #rgb — 3 hex digits, opaque shorthand
    if (s.size() == 3) {
        const uint8_t rn = nib(s[0]), gn = nib(s[1]), bn = nib(s[2]);
        out.r = static_cast<float>((rn << 4) | rn) / 255.f;
        out.g = static_cast<float>((gn << 4) | gn) / 255.f;
        out.b = static_cast<float>((bn << 4) | bn) / 255.f;
        out.a = 1.f;
        return true;
    }
    return false;
}

// ── Numeric ───────────────────────────────────────────────────────────────────

[[nodiscard]]
static float toFloat(std::string_view s) noexcept {
    if (s.empty())
        return 0.f;
    float v = 0.f;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

// Count UTF-8 code points (not bytes) — used for rough text width estimation.
[[nodiscard]]
static std::size_t utf8Len(std::string_view s) noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80)
            i += 1;
        else if ((c & 0xE0) == 0xC0)
            i += 2;
        else if ((c & 0xF0) == 0xE0)
            i += 3;
        else if ((c & 0xF8) == 0xF0)
            i += 4;
        else
            i += 1;
        ++n;
    }
    return n;
}

// Coordinate transform
//   Layout stores parent-relative x/y.  Walk the parent chain to accumulate
//   the absolute screen position for use in draw calls.

[[nodiscard]]
static std::pair<float, float> absolutePos(const RenderTree &tree, NodeHandle handle) noexcept {
    float ax = 0.f, ay = 0.f;
    for (NodeHandle h = handle; h.valid();) {
        const RenderNode *n = tree.node(h);
        if (!n)
            break;
        ax += n->x;
        ay += n->y;
        h   = n->parent;
    }
    return {ax, ay};
}

// Tree walk
static void
walkTree(RenderTree &tree, NodeHandle h, const std::function<void(NodeHandle, RenderNode &)> &fn) {
    if (!h.valid())
        return;
    RenderNode *n = tree.node(h);
    if (!n)
        return;
    fn(h, *n);
    for (NodeHandle c = n->child; c.valid();) {
        RenderNode *cn = tree.node(c);
        if (!cn)
            break;
        const NodeHandle next = cn->sibling;
        walkTree(tree, c, fn);
        c = next;
    }
}

}  // anonymous namespace

// bindDrawCallbacks
/**
 * @brief Binds draw callbacks
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 */
void bindDrawCallbacks(RenderTree &tree, NodeHandle root) {
    walkTree(tree, root, [&tree](NodeHandle h, RenderNode &n) {
        const std::string bgStr = std::string(n.style("backgroundColor"));

        RGBAf bg_probe{0.f, 0.f, 0.f, 0.f};
        const bool hasBg     = parseHexColor(bgStr, bg_probe);
        const bool hasTxt    = !n.style("text").empty();
        const bool hasBorder = !n.style("borderWidth").empty() && !n.style("borderColor").empty();
        const bool hasClip   = (n.layout_props.overflow == LayoutProps::Overflow::Hidden);
        // scene3d nodes use src= to name a .glb file — handled by GltfScene::attach(),
        // not by the image pipeline.  Skip them here so ImageCache is never asked
        // to load a binary mesh file as a 2D texture.
        const bool isScene3D = (n.style("tag") == "scene3d");
        // src is a parse-time-only attribute (img nodes); captured at bind time
        // so the path string is stored in the closure without a per-frame lookup.
        const bool hasSrc = !isScene3D && !n.style("src").empty();
        // True when a custom fragment shader is named at parse/bind time.
        // Enables per-frame re-rendering for time-based animation (jitter etc.).
        const bool hasShader      = hasSrc && !n.style("_shader").empty();
        const bool hasVideo       = !n.style("_video").empty();
        const bool hasVideoShader = hasVideo && !n.style("_shader").empty();

        // Nothing visual and no clip, skip installing callbacks entirely.
        if (!hasBg && !hasTxt && !hasBorder && !hasClip && !hasSrc && !hasVideo)
            return;

        n.draw = [&tree, h, hasBg, hasClip, hasSrc, hasShader, hasVideo, hasVideoShader](
                     RenderContext &ctx) {
            const RenderNode *self = tree.node(h);
            if (!self || self->w <= 0.f || self->h <= 0.f)
                return;

            const auto [ax, ay] = absolutePos(tree, h);

            // opacity
            // Multiplied into every alpha below.  Re-read each frame so that
            // an Animated<float> stored in StyleMap via setStyle("opacity", ...)
            // takes effect immediately.  Clamp to [0, 1].
            const float op = [&]() noexcept -> float {
                const auto sv = self->style("opacity");
                if (sv.empty())
                    return 1.f;
                float v = 1.f;
                std::from_chars(sv.data(), sv.data() + sv.size(), v);
                return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
            }();

            // background
            // Re-read each frame so setStyle("backgroundColor", ...) is
            // reflected immediately.  Skip the draw call when alpha ≈ 0 to
            // avoid a GPU draw for invisible transparent fills.
            if (hasBg) {
                const auto bgSv = self->style("backgroundColor");
                RGBAf bg{0.f, 0.f, 0.f, 0.f};
                if (!bgSv.empty() && parseHexColor(bgSv, bg) && bg.a * op > 0.001f)
                    ctx.drawRect(ax, ay, self->w, self->h, bg.r, bg.g, bg.b, bg.a * op);
            }

            // image
            // Draw after the background fill so the image appears on top of any
            // placeholder colour.  src is constant for img nodes — read it once
            // and forward to drawImage which handles the ImageCache lookup.
            if (hasSrc) {
                const auto src = self->style("src");
                if (!src.empty()) {
                    // Re-read _shader each frame so behavior can switch presets.
                    const auto shdr = self->style("_shader");
                    if (hasShader && !shdr.empty()) {
                        // Build NodeShaderParams from _shader_* style attrs.
                        pce::sdlos::NodeShaderParams sp{};
                        sp.u_width  = self->w;
                        sp.u_height = self->h;
                        {
                            const auto fx = self->style("_shader_focusX");
                            const auto fy = self->style("_shader_focusY");
                            sp.u_focusX   = fx.empty() ? 0.5f : toFloat(fx);
                            sp.u_focusY   = fy.empty() ? 0.5f : toFloat(fy);
                        }
                        {
                            const auto p0 = self->style("_shader_param0");
                            const auto p1 = self->style("_shader_param1");
                            const auto p2 = self->style("_shader_param2");
                            sp.u_param0   = p0.empty() ? 50.f : toFloat(p0);
                            sp.u_param1   = p1.empty() ? 0.f : toFloat(p1);
                            sp.u_param2   = p2.empty() ? 0.f : toFloat(p2);
                        }
                        sp.u_time = ctx.time;
                        ctx.drawImageWithShader(
                            src,
                            shdr,
                            ax,
                            ay,
                            self->w,
                            self->h,
                            op,
                            self->visual_props.object_fit,
                            sp);
                    } else {
                        ctx.drawImage(
                            src,
                            ax,
                            ay,
                            self->w,
                            self->h,
                            op,
                            self->visual_props.object_fit);
                    }
                }
            }

            // video (webcam / camera source)
            // Draws the current VideoTexture frame each render tick.
            // _shader applies the same node-shader pipeline as static images.
            if (hasVideo) {
                const auto shdr = self->style("_shader");
                if (hasVideoShader && !shdr.empty()) {
                    pce::sdlos::NodeShaderParams sp{};
                    sp.u_width  = self->w;
                    sp.u_height = self->h;
                    {
                        const auto fx = self->style("_shader_focusX");
                        const auto fy = self->style("_shader_focusY");
                        sp.u_focusX   = fx.empty() ? 0.5f : toFloat(fx);
                        sp.u_focusY   = fy.empty() ? 0.5f : toFloat(fy);
                    }
                    {
                        const auto p0 = self->style("_shader_param0");
                        const auto p1 = self->style("_shader_param1");
                        const auto p2 = self->style("_shader_param2");
                        sp.u_param0   = p0.empty() ? 0.8f : toFloat(p0);
                        sp.u_param1   = p1.empty() ? 0.0f : toFloat(p1);
                        sp.u_param2   = p2.empty() ? 1.0f : toFloat(p2);
                    }
                    sp.u_time = ctx.time;
                    ctx.drawVideoWithShader(shdr, ax, ay, self->w, self->h, op, sp);
                } else {
                    ctx.drawVideo(ax, ay, self->w, self->h, op);
                }
            }

            // border
            // Re-read each frame (supports animated border width/color later).
            {
                const auto bwSv = self->style("borderWidth");
                const auto bcSv = self->style("borderColor");
                if (!bwSv.empty() && !bcSv.empty()) {
                    RGBAf bc{0.f, 0.f, 0.f, 1.f};
                    if (parseHexColor(bcSv, bc)) {
                        const float bw = toFloat(bwSv);
                        if (bw > 0.f)
                            ctx.drawRectOutline(
                                ax,
                                ay,
                                self->w,
                                self->h,
                                bw,
                                bc.r,
                                bc.g,
                                bc.b,
                                bc.a * op);
                    }
                }
            }

            // overflow:hidden — set scissor rect
            // Called after drawing the node's own background and border so
            // those are not clipped.  Children drawn after this callback will
            // be clipped to these bounds.  after_draw() restores the scissor.
            if (hasClip) {
                const SDL_Rect sr{
                    static_cast<int>(ax),
                    static_cast<int>(ay),
                    static_cast<int>(self->w),
                    static_cast<int>(self->h)};
                SDL_SetGPUScissor(ctx.pass, &sr);
            }

            // text
            // Re-read each frame so dynamic setStyle("text", ...) is reflected
            // immediately (used by the calculator behaviour file, etc.).
            const auto txt = self->style("text");
            if (!txt.empty()) {
                const float fs = [&] {
                    const float v = toFloat(self->style("fontSize"));
                    return v > 0.f ? v : 16.f;
                }();

                RGBAf fg{1.f, 1.f, 1.f, 1.f};
                [[maybe_unused]]
                const bool hasFg = parseHexColor(self->style("color"), fg);

                // Prefer measured dimensions from the text renderer (cached
                // after the first call — subsequent frames are cheap lookups).
                // Falls back to the character-width approximation when the
                // renderer is not ready (e.g. TTF unavailable).
                const auto &vp    = self->visual_props;
                const bool is_rtl = (vp.direction == VisualProps::Direction::RTL);
                float tw          = static_cast<float>(utf8Len(txt)) * fs * 0.55f;  // fallback
                float th          = fs;  // fallback height
                if (ctx.text_renderer && ctx.text_renderer->isReady()) {
                    const auto [mw, mh] = ctx.text_renderer->measureText(txt, fs, is_rtl);
                    if (mw > 0)
                        tw = static_cast<float>(mw);
                    if (mh > 0)
                        th = static_cast<float>(mh);
                }

                // Padding-aware content box.
                const float inner_w = self->w - vp.padding_left - vp.padding_right;
                const float inner_h = self->h - vp.padding_top - vp.padding_bottom;

                // Vertical: centred using the actual glyph bounding-box height
                // (th) rather than the raw fontSize value.  TTF_RenderText
                // produces a surface taller than fontSize by the font's
                // ascender + descender + internal leading, so using fontSize
                // here would leave the glyphs visually below centre ("missing
                // padding at the bottom" effect).
                const float ty = ay + vp.padding_top + std::max(0.f, inner_h - th) * 0.5f;

                // Horizontal: governed by textAlign (default = Center).
                float tx;
                switch (vp.text_align) {
                case VisualProps::TextAlign::Left:
                    tx = ax + vp.padding_left;
                    break;
                case VisualProps::TextAlign::Right:
                    tx = ax + self->w - vp.padding_right - tw;
                    break;
                default:  // Center — preserves historical centred-label behaviour
                    tx = ax + vp.padding_left + std::max(0.f, inner_w - tw) * 0.5f;
                    break;
                }

                ctx.drawText(txt, tx, ty, fs, fg.r, fg.g, fg.b, fg.a * op, is_rtl);
            }
        };

        // after_draw: restore scissor after all children
        // Only installed when overflow:hidden is set.  Restores the scissor to
        // the full viewport so sibling nodes outside this clip are unaffected.
        // A proper scissor stack (for nested overflow:hidden) can be added to
        // RenderContext later; for now single-level clipping covers the common
        // case.
        if (hasClip) {
            n.after_draw = [](RenderContext &ctx) {
                const SDL_Rect full{
                    0,
                    0,
                    static_cast<int>(ctx.viewport_w),
                    static_cast<int>(ctx.viewport_h)};
                SDL_SetGPUScissor(ctx.pass, &full);
            };
        }

        // Shader canvas: re-render every frame so time-based effects
        // (Poisson disk jitter, animated noise, etc.) update continuously.
        // Only installed when the node had a non-empty _shader at bind time.
        if (hasShader) {
            n.update = [&tree, h]() {
                if (RenderNode *self = tree.node(h)) {
                    // Only animate while a shader is still active.
                    if (!self->style("_shader").empty())
                        self->dirty_render = true;
                }
            };
        }

        // Video canvas: always re-render so each new frame is shown.
        if (hasVideo) {
            n.update = [&tree, h]() {
                if (RenderNode *self = tree.node(h))
                    self->dirty_render = true;
            };
        }

        // No unconditional self-dirtying here.
        //
        // The UI offscreen texture uses LOADOP_LOAD, so a node that does not
        // mark itself dirty keeps its pixels from the previous frame intact.
        // dirty_render is set by:
        //   - RenderNode constructor default (first frame → drawn once)
        //   - setStyle() → markDirty() → propagates up to ancestors
        //   -  markLayoutDirty() → layout cascade → all affected nodes
        //   - Animated<T> / ontick update callbacks in behaviour files
        //
        // Any existing update callback set by bindNodeEvents() is preserved
        // as-is (node_events uses its own local compose() helper).
    });
}

}  // namespace pce::sdlos
