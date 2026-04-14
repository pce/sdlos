#ifndef SDLOS_JADE_ENTRY
#  error "SDLOS_JADE_ENTRY must be defined by the build system (path to .jade file)"
#endif

#ifndef SDLOS_APP_NAME
#  define SDLOS_APP_NAME "sdlos-app"
#endif

#ifndef SDLOS_WIN_W
#  define SDLOS_WIN_W 375
#endif
#ifndef SDLOS_WIN_H
#  define SDLOS_WIN_H 667
#endif

#include "render_tree.h"
#include "sdl_renderer.h"
#include "event_bus.h"
#include "hit_test.h"
#include "node_events.h"
#include "jade/jade_parser.h"
#include "style_draw.h"
#include "i_event_bus.h"
#include "debug/layout_debug.h"
#include "css_loader.h"
#include "vfs/vfs.h"
#include "gltf/gltf_scene.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

// ConsoleState — in-app debug log ring buffer.
//
// Defined at file scope (before #include SDLOS_APP_BEHAVIOR) so that
// sdlos_log() is visible inside behaviour fragments at compile time.
// Toggle console visibility with ` (backtick).
// Toggle layout overlay with F1.

struct ConsoleState {
    static constexpr int   kMaxLines = 14;
    static constexpr float kLineH    = 15.f;
    static constexpr float kFontSz   = 11.f;
    static constexpr float kPad      = 5.f;
    static constexpr float kTitleH   = kLineH + kPad;
    static constexpr float kSelectorH = 22.f;
    static constexpr float kTotalH    = kTitleH + kMaxLines * kLineH + kPad + kSelectorH;

    std::deque<std::string> lines;
    bool visible      = false;
    bool layout_debug = false;

    // Inspector / node selector
    // TODO without console in release
    // Press '/' when console is open to enter selector mode.
    // Type #id  .class  or bare word to search live.
    // Press Enter to commit, n/N (or Shift+n) to step through matches,
    // Esc to clear.
    std::string selector_text;                              // live query buffer
    std::vector<pce::sdlos::NodeHandle> selector_matches;  // DFS result list
    int  selector_idx  = -1;   // currently highlighted match (-1 = none)
    bool selector_mode = false; // true while '/' is capturing text
};

static ConsoleState g_console;

// sdlos_log — callable from behaviour fragments included into this TU.
void sdlos_log(std::string_view msg)
{
    std::clog << "[sdlos] " << msg << "\n";
    g_console.lines.emplace_back(msg);
    while (static_cast<int>(g_console.lines.size()) > ConsoleState::kMaxLines)
        g_console.lines.pop_front();
}

// jade_app_init — per-app behaviour hook.
//
// Signature contract:
//   tree        — fully-parsed, style-bound RenderTree (safe to query / mutate)
//   root        — handle to the virtual root returned by jade::parse()
//   bus         — live EventBus; call bus.subscribe() here, not at global scope
//   renderer    — the active SDLRenderer; call SetFontPath() etc. when needed
//   out_handler — assign a raw-SDL-event handler here when you need to receive
//                 mouse / keyboard events directly (e.g. to drive a TextArea or
//                 NumberDragger).  The host stores it in SceneState; it is
//                 cleared automatically at the start of every loadScene() call
//                 so stale closures from the old tree can never fire.
//                 Return true from the handler to consume the event (host
//                 shortcuts are skipped); return false to let normal processing
//                 continue.
//
// Called once per scene load: on startup AND after every sdlos:navigate event.
// Do NOT call SDL_PollEvent or render from here.
//
// VFS access:
//   Each app receives a reference to the host's pce::vfs::Vfs.
//   Standard mounts:
//     asset://  -> read-only binary data directory (SDL_GetBasePath() + "data")
//     user://   -> read-write app data directory (SDL_GetPrefPath())
//   Common usage:
//     auto text = vfs.read_text("asset://config.json");
//

#ifdef SDLOS_APP_BEHAVIOR
void jade_app_vfs_init(pce::vfs::Vfs& vfs);
#  include SDLOS_APP_BEHAVIOR
#endif

// Default implementation of the VFS hook, used if the behavior doesn't provide one.
// We use a separate TU or a weak symbol if possible, but for this unified TU
// pattern, we'll just check if a macro was defined by the behavior.
#ifndef SDLOS_BEHAVIOR_VFS_INIT
void jade_app_vfs_init(pce::vfs::Vfs& /*vfs*/) {}
#endif

#ifndef SDLOS_APP_BEHAVIOR
void jade_app_init(pce::sdlos::RenderTree&               /*tree*/,
                   pce::sdlos::NodeHandle                 /*root*/,
                   pce::sdlos::IEventBus&                 /*bus*/,
                   pce::sdlos::SDLRenderer&               /*renderer*/,
                   std::function<bool(const SDL_Event&)>& /*out_handler*/) {}
#endif

// File-local helpers

namespace {

namespace fs = std::filesystem;

std::atomic<bool> g_quit{false};

extern "C" void on_signal(int) noexcept { g_quit.store(true); }

[[nodiscard]] std::string readFile(const char* path)
{
    std::ifstream ifs(path);
    if (!ifs) {
        std::cerr << "[jade_host] cannot open: " << path << "\n";
        return {};
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

// Walk the tree and resolve every relative `src` attribute to an absolute
// path so ImageCache::ensureTexture() works regardless of the working dir.
static void resolveAssetPaths(pce::sdlos::RenderTree& tree,
                               pce::sdlos::NodeHandle   root,
                               const fs::path&          base_dir)
{
    if (!root.valid()) return;
    pce::sdlos::RenderNode* n = tree.node(root);
    if (!n) return;

    // scene3d nodes carry src= pointing to a .glb file.
    // Their path is resolved by GltfScene::attach() against the binary base-path
    // (SDL_GetBasePath()), not against the jade source directory.
    // Skip them here so the src stays as a relative data-dir path.
    const bool isScene3D = (n->style("tag") == "scene3d");

    if (!isScene3D) {
        const auto src = n->style("src");
        if (!src.empty()) {
            // Skip VFS URIs (scheme://path) — they are already fully qualified and
            // must be routed through the mount system, not joined with base_dir.
            // A bare relative path (no "://") is resolved against the jade file's
            // directory so that  img(src="data/img/hero.png")  works as before.
            const bool is_vfs_uri = (src.find("://") != std::string_view::npos);
            const fs::path p(src);
            if (!is_vfs_uri && p.is_relative())
                n->setStyle("src", (base_dir / p).lexically_normal().string());
        }
    }

    for (pce::sdlos::NodeHandle c = n->child; c.valid(); ) {
        pce::sdlos::RenderNode* cn = tree.node(c);
        if (!cn) break;
        const pce::sdlos::NodeHandle next = cn->sibling;
        resolveAssetPaths(tree, c, base_dir);
        c = next;
    }
}

// Load a font from the app's data/fonts/ directory.
// The directory is <SDL_GetBasePath()>/data/fonts/ — i.e. the build-tree
// location that sdlos_jade_app(DATA_DIR ...) copies fonts into at build time.
static void loadAppFonts(pce::sdlos::SDLRenderer& renderer)
{
    pce::sdlos::TextRenderer* tr = renderer.GetTextRenderer();
    if (!tr) return;

    const std::string base = renderer.GetDataBasePath();
    if (base.empty()) return;

    const fs::path fonts_dir = fs::path(base) / "data" / "fonts";
    if (!fs::exists(fonts_dir)) return;

    // First successful load wins.
    const fs::path candidates[] = {
        fonts_dir / "Ubuntu-Regular.ttf",
        fonts_dir / "UbuntuSans-Regular.ttf",
        fonts_dir / "InterVariable.ttf",
        fonts_dir / "Inter-Regular.ttf",
        fonts_dir / "Roboto-Regular.ttf",
        fonts_dir / "LiberationSans-Regular.ttf",
    };

    bool loaded = false;
    for (const auto& p : candidates) {
        if (fs::exists(p)) {
            if (tr->loadFont(p.string(), 16.f)) {
                sdlos_log("[jade_host] app font: " + p.filename().string());
                loaded = true;
                break;
            }
        }
    }

    // Emoji fallback — TwemojiMozilla.ttf provides color emoji via COLR tables.
    // SDL3_ttf's TTF_AddFallbackFont() chains it so any missing glyph (☀️ ❄️ …)
    // falls through to Twemoji automatically.
    if (loaded) {
        // Try several common Twemoji file-name variants in priority order.
        // The DATA_DIR copy preserves whatever name the file has on disk,
        // so we probe all known spellings rather than hard-coding one.
        const fs::path twemoji_candidates[] = {
            fonts_dir / "TwemojiMozilla.ttf",          // canonical name
            fonts_dir / "Twemoji.Mozilla.ttf",          // trimmed variant with dots
            fonts_dir / "TwemojiMozilla.subset.ttf",    // pre-subsetted smaller build
            fonts_dir / "twemoji.ttf",                  // lowercase fallback
        };
        for (const auto& twemoji : twemoji_candidates) {
            if (fs::exists(twemoji)) {
                if (tr->addFallbackFont(twemoji.string(), 16.f))
                    sdlos_log("[jade_host] emoji font: " + twemoji.filename().string());
                break;  // first successful load wins
            }
        }
    }
}

/// SceneState — owns the current jade scene.
//
// Lives in main() so its address (and the address of scene.tree) is stable
// for the entire process lifetime.  Overlay lambdas capture SceneState* so
// they keep working correctly after a scene reload: the pointer never changes,
// only the tree contents are replaced in-place by loadScene().
struct SceneState {
    std::unique_ptr<pce::sdlos::RenderTree> tree{
        std::make_unique<pce::sdlos::RenderTree>()};
    pce::sdlos::NodeHandle root{pce::sdlos::k_null_handle};
    pce::sdlos::NodeHandle layout_dbg_h{pce::sdlos::k_null_handle};
    pce::sdlos::NodeHandle console_h{pce::sdlos::k_null_handle};
    pce::sdlos::NodeHandle highlight_h{pce::sdlos::k_null_handle};

    pce::sdlos::css::StyleSheet css_sheet;

    // Raw SDL event hook set by jade_app_init() via the out_handler parameter.
    // Stored here (not as a global) so its lifetime is exactly the scene's
    // lifetime — cleared at the top of every loadScene() before the old tree
    // is destroyed, so captured references can never dangle.
    std::function<bool(const SDL_Event&)> raw_event_handler;

    // Host-managed GltfScene — created automatically when the jade scene
    // contains one or more scene3d nodes with src= attributes and the
    // behavior's jade_app_init() did NOT install its own scene3d hook.
    // Cleared at the start of every loadScene() before the tree is replaced.
    std::shared_ptr<pce::sdlos::gltf::GltfScene> host_scene3d;
};

// Walk the RenderTree from `root` (DFS) and collect all nodes whose
// id / class / tag matches `query`.
//
//   #foo  → nodes whose id  == "foo"
//   .foo  → nodes whose class contains "foo"
//   foo   → nodes whose id == "foo" OR class contains "foo" OR tag == "foo"
//
// Results are stored in `out` (cleared first).
static void runSelector(
    const std::string& query,
    pce::sdlos::RenderTree& tree,
    pce::sdlos::NodeHandle root,
    std::vector<pce::sdlos::NodeHandle>& out)
{
    using NH = pce::sdlos::NodeHandle;

    out.clear();
    if (query.empty()) return;

    const bool by_id    = query.size() > 1 && query[0] == '#';
    const bool by_class = query.size() > 1 && query[0] == '.';
    const std::string key = (by_id || by_class) ? query.substr(1) : query;

    std::vector<NH> stk;
    stk.reserve(64);
    stk.push_back(root);

    while (!stk.empty()) {
        const NH h = stk.back();
        stk.pop_back();
        const pce::sdlos::RenderNode* n = tree.node(h);
        if (!n) continue;

        const std::string id  = std::string(n->style("id"));
        const std::string cls = std::string(n->style("class"));
        const std::string tag = std::string(n->style("tag"));

        bool match = false;
        if (by_id)
            match = (id == key);
        else if (by_class)
            match = !cls.empty() && cls.find(key) != std::string::npos;
        else
            match = (id == key)
                 || (!cls.empty() && cls.find(key) != std::string::npos)
                 || (tag == key && !tag.empty());

        if (match) out.push_back(h);

        // LCRS children — push in reverse so left-most child is visited first
        std::vector<NH> children;
        for (NH c = n->child; c != pce::sdlos::k_null_handle; ) {
            const pce::sdlos::RenderNode* cn = tree.node(c);
            if (!cn) break;
            children.push_back(c);
            c = cn->sibling;
        }
        for (auto it = children.rbegin(); it != children.rend(); ++it)
            stk.push_back(*it);
    }
}

// Attach the two host-owned overlay nodes (layout debug + console) on top
// of the scene.  Must be called after jade_app_init() so app nodes are below.
static void addOverlays(SceneState& scene)
{
    SceneState* sp = &scene;

    // Layout-debug overlay
    scene.layout_dbg_h = scene.tree->alloc();
    if (pce::sdlos::RenderNode* nd = scene.tree->node(scene.layout_dbg_h)) {
        nd->draw = [sp](pce::sdlos::RenderContext& ctx) {
            if (!g_console.layout_debug) return;
            pce::sdlos::debug::LayoutDebugConfig cfg;
            cfg.show_none    = false;
            cfg.show_labels  = true;
            cfg.label_size   = 10.f;
            cfg.fill_alpha   = 0.07f;
            cfg.border_width = 1.0f;
            pce::sdlos::debug::drawLayoutDebug(ctx, *sp->tree, sp->root, cfg);
        };
        nd->update = [sp]() {
            if (!g_console.layout_debug) return;
            if (pce::sdlos::RenderNode* self = sp->tree->node(sp->layout_dbg_h))
                self->dirty_render = true;
        };
        scene.tree->appendChild(scene.root, scene.layout_dbg_h);
    }

    // Highlight overlay — yellow outline + info badge over the selected node.
    scene.highlight_h = scene.tree->alloc();
    if (pce::sdlos::RenderNode* nd = scene.tree->node(scene.highlight_h)) {
        nd->draw = [sp](pce::sdlos::RenderContext& ctx) {
            if (g_console.selector_idx < 0) return;
            if (g_console.selector_idx >=
                    static_cast<int>(g_console.selector_matches.size())) return;

            const pce::sdlos::NodeHandle mh =
                g_console.selector_matches[g_console.selector_idx];
            const pce::sdlos::RenderNode* mn = sp->tree->node(mh);
            if (!mn) return;

            const float x = mn->x, y = mn->y, w = mn->w, h = mn->h;
            if (w <= 0.f || h <= 0.f) return;

            constexpr float bw = 2.f;
            // Yellow outline (four edge rects)
            ctx.drawRect(x,          y,          w,  bw, 1.f, 0.90f, 0.f, 1.f);
            ctx.drawRect(x,          y + h - bw, w,  bw, 1.f, 0.90f, 0.f, 1.f);
            ctx.drawRect(x,          y,          bw, h,  1.f, 0.90f, 0.f, 1.f);
            ctx.drawRect(x + w - bw, y,          bw, h,  1.f, 0.90f, 0.f, 1.f);
            // Faint yellow fill
            ctx.drawRect(x, y, w, h, 1.f, 0.90f, 0.f, 0.07f);

            // Build info string: #id .class <tag> W×H @x,y
            std::string info;
            const auto id_sv  = mn->style("id");
            const auto cls_sv = mn->style("class");
            const auto tag_sv = mn->style("tag");
            if (!id_sv.empty())  { info += "#"; info += id_sv;  info += "  "; }
            if (!cls_sv.empty()) { info += "."; info += cls_sv; info += "  "; }
            if (!tag_sv.empty() && tag_sv != "div") {
                info += "<"; info += tag_sv; info += ">  ";
            }
            info += std::to_string(static_cast<int>(w))
                  + "\xC3\x97"                               // ×
                  + std::to_string(static_cast<int>(h));
            info += "  @";
            info += std::to_string(static_cast<int>(x));
            info += ",";
            info += std::to_string(static_cast<int>(y));

            // Badge: above the node if room, otherwise below
            constexpr float kBadgeH  = 14.f;
            constexpr float kBadgeFz = 10.f;
            const float bx = x + 3.f;
            const float by = (y >= kBadgeH + 3.f)
                           ? y - kBadgeH - 1.f
                           : y + h + 3.f;
            const float badge_w = static_cast<float>(info.size()) * 6.2f + 8.f;
            ctx.drawRect(bx - 3.f, by - 1.f, badge_w, kBadgeH + 2.f,
                         0.04f, 0.04f, 0.04f, 0.88f);
            ctx.drawText(info.c_str(), bx, by, kBadgeFz,
                         1.f, 0.90f, 0.f, 1.f);
        };
        nd->update = [sp]() {
            // Keep redrawing when a match is selected
            if (g_console.selector_idx < 0) return;
            if (pce::sdlos::RenderNode* self = sp->tree->node(sp->highlight_h))
                self->dirty_render = true;
        };
        scene.tree->appendChild(scene.root, scene.highlight_h);
    }

    // Console overlay
    scene.console_h = scene.tree->alloc();
    if (pce::sdlos::RenderNode* nd = scene.tree->node(scene.console_h)) {
        nd->draw = [](pce::sdlos::RenderContext& ctx) {
            if (!g_console.visible) return;

            const float vw = ctx.viewport_w;
            const float vh = ctx.viewport_h;
            const float y0 = vh - ConsoleState::kTotalH;

            // Background + top border + title bar
            ctx.drawRect(0.f, y0, vw, ConsoleState::kTotalH,
                         0.04f, 0.07f, 0.04f, 0.92f);
            ctx.drawRect(0.f, y0, vw, 1.f,
                         0.20f, 0.85f, 0.20f, 0.70f);
            ctx.drawRect(0.f, y0, vw, ConsoleState::kTitleH,
                         0.00f, 0.30f, 0.00f, 0.80f);
            ctx.drawText("sdlos console   [`] toggle   [F1] layout   [/] select   [n/N] step",
                         ConsoleState::kPad, y0 + 2.f,
                         ConsoleState::kFontSz,
                         0.40f, 1.00f, 0.40f, 1.00f);

            // Log lines (oldest first = top of panel)
            float ly = y0 + ConsoleState::kTitleH + 2.f;
            for (const auto& line : g_console.lines) {
                ctx.drawText(line,
                             ConsoleState::kPad, ly,
                             ConsoleState::kFontSz,
                             0.72f, 0.95f, 0.72f, 1.00f);
                ly += ConsoleState::kLineH;
            }
            // ── Selector row ─────────────────────────────────────────────────
            {
                const float sel_y =
                    y0 + ConsoleState::kTotalH - ConsoleState::kSelectorH;
                // Row background
                ctx.drawRect(0.f, sel_y, vw, ConsoleState::kSelectorH,
                             0.00f, 0.20f, 0.00f, 0.75f);
                ctx.drawRect(0.f, sel_y, vw, 1.f,
                             0.20f, 0.85f, 0.20f, 0.40f);  // thin separator

                const float ty = sel_y + (ConsoleState::kSelectorH - ConsoleState::kFontSz) * 0.5f;
                const float pl = ConsoleState::kPad;

                // Prompt glyph
                ctx.drawText(g_console.selector_mode ? "\xe2\x96\xba " : "/ ",
                             pl, ty, ConsoleState::kFontSz,
                             0.40f, 1.00f, 0.40f, 1.00f);

                // Query text (+ blinking-cursor underscore in selector mode)
                const std::string disp = g_console.selector_text
                    + (g_console.selector_mode ? "_" : "");
                if (!disp.empty())
                    ctx.drawText(disp.c_str(), pl + 14.f, ty,
                                 ConsoleState::kFontSz,
                                 0.90f, 0.90f, 0.90f, 1.00f);

                // Right-side: match count / hint
                if (!g_console.selector_matches.empty()) {
                    const std::string cnt =
                        std::to_string(g_console.selector_idx + 1)
                        + "/" + std::to_string(g_console.selector_matches.size())
                        + "  n/N=step";
                    ctx.drawText(cnt.c_str(), vw - 120.f, ty,
                                 ConsoleState::kFontSz,
                                 0.50f, 1.00f, 0.50f, 1.00f);
                } else if (!g_console.selector_text.empty()) {
                    ctx.drawText("no match",
                                 vw - 65.f, ty, ConsoleState::kFontSz,
                                 1.00f, 0.40f, 0.40f, 1.00f);
                } else {
                    ctx.drawText("/ search   #id  .class  tag",
                                 vw - 175.f, ty, ConsoleState::kFontSz,
                                 0.28f, 0.52f, 0.28f, 1.00f);
                }
            }
        };
        nd->update = [sp]() {
            // Only dirty when the console is open — keeps idle-frame skip
            // working when the panel is hidden.
            if (!g_console.visible) return;
            if (pce::sdlos::RenderNode* self = sp->tree->node(sp->console_h))
                self->dirty_render = true;
        };
        scene.tree->appendChild(scene.root, scene.console_h);
    }
}

/// loadScene — parse a jade file and fully wire up the scene.
//
// Used for both the initial load and every sdlos:navigate transition.
//
//  1. Detach the old scene from the renderer (safe to call with null).
//  2. Reset the EventBus — drops all old behaviour callbacks so they cannot
//     fire against a tree that is about to be destroyed.
//  3. Re-subscribe the host's sdlos:navigate handler on the fresh bus so the
//     new behaviour can trigger further navigations.
//  4. Replace the RenderTree in-place (SceneState address stays stable so
//     overlay lambdas capturing SceneState* continue to work).
//  5. Parse jade → attach to renderer → resolve asset paths → load app fonts.
//  6. Bind draw/node callbacks → call jade_app_init (behaviour gets renderer).
//  7. Auto-wire scene3d: if jade_app_init() did NOT install a scene3d hook but
//     the tree contains scene3d nodes with src= attributes, the host creates a
//     GltfScene, inits it, attaches it, and installs both the render hook and
//     the per-frame tick.  Behaviors that need a custom camera or advanced
//     scene control simply call renderer.setScene3DHook() inside jade_app_init
//     as before — that takes priority and skips the auto-wire.
//  8. Handle _font / _font_size jade attributes (behaviour has the last word).
//  9. Add host overlays on top of the app scene.
//
static bool loadScene(const std::string&       jade_path,
                       SceneState&              scene,
                       pce::sdlos::SDLRenderer& renderer,
                       pce::sdlos::EventBus&    events,
                       std::string&             next_jade_path,
                       pce::vfs::Vfs& vfs)
{
    // 1. Detach old scene before destroying the tree.
    renderer.SetScene(nullptr, pce::sdlos::k_null_handle);

    // 1b. Release GPU textures that belonged to the previous scene.
    //
    // This must happen AFTER SetScene(nullptr) — no draw callbacks are
    // executing at this point, so it is safe to free the textures.
    // SDL3 GPU defers the actual GPU-side release until all in-flight
    // command buffers complete, so there is no GPU/CPU race here.
    //
    // Textures tagged with scope "" (permanent — UI icons, shared assets)
    // are untouched.  Only textures tagged "scene" are released.
    if (pce::sdlos::ImageCache* ic = renderer.GetImageCache()) {
        ic->evict_scope("scene");
        // New textures loaded for this scene will be tagged "scene".
        ic->set_scope("scene");
    }

    // 2. Drop all subscriptions — old lambdas capture refs into the old tree.
    events.reset();

    // 2b. Clear the raw-event hook — it may capture refs into the old tree.
    scene.raw_event_handler = nullptr;

    // 2c. Clear 3D hooks — lambdas may capture old app state.
    renderer.setScene3DHook(nullptr);
    renderer.setGpuPreShutdownHook(nullptr);

    // 2d. Release the host-managed GltfScene from the previous scene.
    //     Must happen AFTER the hooks are cleared (they captured a shared_ptr
    //     to it) so the destructor runs while the GPU is idle between frames.
    scene.host_scene3d.reset();

    // 3. Re-subscribe the host navigation handler on the fresh bus.
    //    The new behaviour can now call:
    //      bus.publish("sdlos:navigate", "path/to/next.jade");
    events.subscribe("sdlos:navigate", [&next_jade_path](const std::string& path) {
        next_jade_path = path;
        sdlos_log("[jade_host] navigate → " + path);
    });

    // 4. Replace the tree — unique_ptr swap keeps SceneState address stable.
    scene.tree         = std::make_unique<pce::sdlos::RenderTree>();
    scene.root         = pce::sdlos::k_null_handle;
    scene.layout_dbg_h = pce::sdlos::k_null_handle;
    scene.console_h    = pce::sdlos::k_null_handle;
    scene.highlight_h = pce::sdlos::k_null_handle;
    // Clear stale selector matches — handles from the old tree are invalid.
    g_console.selector_matches.clear();
    g_console.selector_idx  = -1;
    g_console.selector_mode = false;
    g_console.selector_text.clear();

    // 5a. Parse jade source.
    const std::string source = readFile(jade_path.c_str());
    if (source.empty()) {
        std::cerr << "[jade_host] empty or missing jade file: " << jade_path << "\n";
        return false;
    }

    scene.root = pce::sdlos::jade::parse(source, *scene.tree);
    if (!scene.root.valid()) {
        std::cerr << "[jade_host] jade parse produced no root: " << jade_path << "\n";
        return false;
    }

    // 5b. Attach to renderer (layout sizing needs the scene to be set).
    renderer.SetScene(scene.tree.get(), scene.root);

    // 5c. Resolve relative src= attributes before bindDrawCallbacks().
    const fs::path jade_dir = fs::path(jade_path).parent_path();
    resolveAssetPaths(*scene.tree, scene.root, jade_dir);

    // 5d. Load a font from the jade app's own data/fonts/ if present.
    //     Only apps that bundle a DATA_DIR will have this directory.
    loadAppFonts(renderer);

    scene.css_sheet = {};
    {
        const fs::path css_path = fs::path(jade_path).replace_extension(".css");
            if (fs::exists(css_path)) {
                scene.css_sheet = pce::sdlos::css::load(css_path.string());
                if (!scene.css_sheet.empty()) {
                    scene.css_sheet.applyTo(*scene.tree, scene.root, renderer.pixelScaleX());
                    sdlos_log("[jade_host] css: " + css_path.filename().string()
                              + "  rules=" + std::to_string(scene.css_sheet.size()));
                }
            }
    }

    // 6. Bind draw callbacks and node-event wiring, then call the behaviour.
    pce::sdlos::bindDrawCallbacks(*scene.tree, scene.root);
    pce::sdlos::bindNodeEvents(*scene.tree, scene.root, events);

    jade_app_vfs_init(vfs);
    jade_app_init(*scene.tree, scene.root, events, renderer, scene.raw_event_handler);

    // 7. Auto-wire scene3d nodes (built-in GltfScene resolution).
    //
    // If jade_app_init() already installed a scene3d hook the behavior is in
    // full control — skip auto-wiring entirely.
    //
    // Otherwise: scan the tree for any node with tag="scene3d".  If at least
    // one is found, create a host-managed GltfScene, init the GPU pipeline,
    // attach all scene3d nodes (loads their src= GLB files), and wire the
    // per-frame render + tick hooks.  A sane default camera is set; behaviors
    // that need a different viewpoint can call renderer.setScene3DHook() to
    // replace it later (or use a bus event to drive the camera reactively).
    if (!renderer.hasScene3DHook()) {
        // Quick scan — look for any scene3d node in the tree.
        bool has_scene3d = false;
        {
            std::vector<pce::sdlos::NodeHandle> stk;
            stk.push_back(scene.root);
            while (!stk.empty() && !has_scene3d) {
                const pce::sdlos::NodeHandle h = stk.back();
                stk.pop_back();
                const pce::sdlos::RenderNode* n = scene.tree->node(h);
                if (!n) continue;
                if (n->style("tag") == "scene3d") { has_scene3d = true; break; }
                for (pce::sdlos::NodeHandle c = n->child; c.valid(); ) {
                    const pce::sdlos::RenderNode* cn = scene.tree->node(c);
                    if (!cn) break;
                    stk.push_back(c);
                    c = cn->sibling;
                }
            }
        }

        if (has_scene3d) {
            auto sc3d = std::make_shared<pce::sdlos::gltf::GltfScene>();
            const std::string& bp = renderer.GetDataBasePath();
            const SDL_GPUTextureFormat sc_fmt = renderer.GetSwapchainFormat();
            const bool ok = sc3d->init(renderer.GetDevice(),
                                       renderer.GetShaderFormat(),
                                       bp,
                                       sc_fmt);
            if (ok) {
                sc3d->attach(*scene.tree, scene.root, bp);

                // Default camera — orbit at yaw=30°, pitch=20°, dist=5 units,
                // target at (0,0,0).  Behaviors can override by calling
                // renderer.setScene3DHook() or camera.orbit/setOrbitTarget().
                sc3d->camera().perspective(45.f, 16.f / 9.f);
                sc3d->camera().orbit(30.f, 20.f, 5.f);

                scene.host_scene3d = sc3d;

                // Capture the tree pointer — safe because the hook is always
                // cleared (step 2c) before the tree is replaced in loadScene().
                pce::sdlos::RenderTree* tree_ptr = scene.tree.get();

                renderer.setScene3DHook(
                    [sc3d, tree_ptr]
                    (SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* swap,
                     float vw, float vh)
                    {
                        sc3d->tick(*tree_ptr, vw, vh);
                        sc3d->render(cmd, swap, vw, vh);
                    });

                renderer.setGpuPreShutdownHook([sc3d]() {
                    sc3d->shutdown();
                });

                sdlos_log("[jade_host] scene3d: auto-wired host GltfScene");
            } else {
                sdlos_log("[jade_host] scene3d: GltfScene init failed — "
                          "check data/shaders/msl/pbr_mesh.{vert,frag}.metal");
            }
        }
    }

    if (!scene.css_sheet.empty()) {
        scene.css_sheet.buildHover(*scene.tree, scene.root, renderer.pixelScaleX());
        if (!scene.css_sheet.hover.empty())
            sdlos_log("[jade_host] css hover: "
                      + std::to_string(scene.css_sheet.hover.size()) + " entries");
        scene.css_sheet.buildActive(*scene.tree, scene.root, renderer.pixelScaleX());
        if (!scene.css_sheet.active_entries.empty())
            sdlos_log("[jade_host] css active: "
                      + std::to_string(scene.css_sheet.active_entries.size()) + " entries");
    }

    // 8. _font / _font_size / _emoji_font jade attributes — behaviour has the last word.
    //    Declare fonts directly on the root node, e.g.:
    //      col#my-root(_font="data/fonts/Ubuntu-Regular.ttf"
    //                  _font_size="16"
    //                  _emoji_font="data/fonts/TwemojiMozilla.ttf")
    //    _font       — primary typeface (relative to GetDataBasePath())
    //    _font_size  — point size for both primary and emoji fonts
    //    _emoji_font — emoji / fallback font chained via TTF_AddFallbackFont
    if (pce::sdlos::RenderNode* rn = scene.tree->node(scene.root)) {
        float sz = 17.f;
        const std::string sz_s{ rn->style("_font_size") };
        if (!sz_s.empty()) {
            try { sz = std::stof(sz_s); } catch (...) {}
        }

        const std::string fp{ rn->style("_font") };
        if (!fp.empty())
            renderer.SetFontPath(fp, sz);

        const std::string efp{ rn->style("_emoji_font") };
        if (!efp.empty())
            renderer.AddFallbackFontPath(efp, sz);
    }

    // 9. Host overlays always render on top of the app scene.
    addOverlays(scene);

    sdlos_log(std::string("[jade_host] loaded '")
              + jade_path + "' — "
              + std::to_string(scene.tree->nodeCount()) + " nodes");
    return true;
}

} // anonymous namespace


int main(int argc, char* argv[])
{

#if BUILD_TYPE_DEBUG
    // Jade file can be overridden at runtime: ./shade other.jade
    const char* jade_path = (argc > 1) ? argv[1] : SDLOS_JADE_ENTRY;
#else
    const char* jade_path = SDLOS_JADE_ENTRY;
#endif
    std::cout << "[jade_host] jade: " << jade_path << "\n";

    /// SDL init
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_CAMERA)) {
        std::cerr << "[jade_host] SDL_Init failed: " << SDL_GetError() << "\n";
        return EXIT_FAILURE;
    }

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    ///  Window
#ifdef BUILD_TYPE_DEBUG
    SDL_Window* window = SDL_CreateWindow(
        SDLOS_APP_NAME, SDLOS_WIN_W, SDLOS_WIN_H,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
#else
    SDL_Window* window = SDL_CreateWindow(
        SDLOS_APP_NAME, 0, 0,
        static_cast<SDL_WindowFlags>(SDL_WINDOW_FULLSCREEN));
#endif

    if (!window) {
        std::cerr << "[jade_host] SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        return EXIT_FAILURE;
    }

    /// Renderer
    // HiDPI pixel scale is owned by SDLRenderer: computed inside Initialize()
    // and refreshed on display-change events via RefreshPixelScale().
    pce::sdlos::SDLRenderer renderer;
    if (!renderer.Initialize(window)) {
        std::cerr << "[jade_host] renderer init failed\n";
        SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    sdlos_log(std::string("[jade_host] pixel scale ")
              + std::to_string(renderer.pixelScaleX()) + "x"
              + std::to_string(renderer.pixelScaleY()));

    // Data base path
    // SDL_GetBasePath() → directory of the running binary with trailing '/'.
    // CMake copies each app's data/ folder there as a post-build step, so
    // relative src= paths in jade and data/shaders/ paths resolve correctly.
    // Stored in base_path for use in runtime navigation path resolution.

    std::string base_path;
    {
        const char* sdl_base = SDL_GetBasePath();
        base_path = sdl_base ? sdl_base : "";
        if (!base_path.empty()) {
            renderer.SetDataBasePath(base_path);
            sdlos_log("[jade_host] base: " + base_path);
        }
    }


    // Host VFS
    // Mount the binary-dir as the "asset" scheme and pref-path as "user".
    // the ``asset`` scheme.  Behaviours and the host may consult

    pce::vfs::Vfs host_vfs;
    if (!base_path.empty()) {
        host_vfs.mount_local("asset", std::filesystem::path(base_path));
        // Also provide a "user://" scheme for persistent app data using SDL_GetPrefPath
        // char* pref_path = SDL_GetPrefPath("pce", SDLOS_APP_NAME);
        // if (pref_path) {
        //    host_vfs.mount_local("user", std::filesystem::path(pref_path));
        //    SDL_free(pref_path);
        // }
        host_vfs.set_default_scheme("asset");
        sdlos_log(std::string("[jade_host] vfs: asset:// -> ") + base_path);
    }

    // Scene state and EventBus
    // SceneState lives here so its address (and &scene.tree) is stable for
    // the entire process lifetime.  Overlay lambdas capture SceneState*.
    SceneState scene;
    pce::sdlos::EventBus events;

    // Set by the sdlos:navigate bus handler (subscribed inside loadScene).
    // Checked at the bottom of the main loop after every render tick.
    std::string next_jade_path;

    if (!loadScene(jade_path, scene, renderer, events, next_jade_path, host_vfs)) {
        renderer.Shutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    std::cout << "[jade_host] running '" << SDLOS_APP_NAME
              << "' — " << scene.tree->nodeCount() << " nodes"
              << "  (` = console, F1 = layout debug)\n";

    //  Main loop
    SDL_Event event;

    while (!g_quit.load()) {

        //  Event pump
        while (SDL_PollEvent(&event)) {

            if (event.type == SDL_EVENT_QUIT) {
                g_quit.store(true);
                break;
            }

            // HiDPI — refresh scale when window moves to a different display.
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED)
            {
                renderer.RefreshPixelScale();
                sdlos_log(std::string("[jade_host] scale updated to ")
                          + std::to_string(renderer.pixelScaleX()) + "x"
                          + std::to_string(renderer.pixelScaleY()));
            }

            if (event.type == SDL_EVENT_MOUSE_MOTION) {
                const float phys_x = event.motion.x * renderer.pixelScaleX();
                const float phys_y = event.motion.y * renderer.pixelScaleY();
                scene.css_sheet.tickHover(*scene.tree, phys_x, phys_y, renderer.pixelScaleX());
            }

            if (event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE)
                scene.css_sheet.tickHover(*scene.tree, -1.f, -1.f, renderer.pixelScaleX());

            // Live selector text input — only consumed when selector mode active.
            if (g_console.selector_mode && event.type == SDL_EVENT_TEXT_INPUT) {
                g_console.selector_text += event.text.text;
                runSelector(g_console.selector_text, *scene.tree, scene.root,
                            g_console.selector_matches);
                g_console.selector_idx =
                    g_console.selector_matches.empty() ? -1 : 0;
                if (pce::sdlos::RenderNode* nd = scene.tree->node(scene.console_h))
                    nd->dirty_render = true;
                if (pce::sdlos::RenderNode* nd = scene.tree->node(scene.highlight_h))
                    nd->dirty_render = true;
            }

            // Behavior raw-event hook — runs before host keyboard handlers.
            // Returns true only when the behavior consumed the event (e.g. a
            // focused TextArea absorbed a KEY_DOWN).  Mouse events return false
            // so dispatchClick always runs below.
            const bool behavior_consumed =
                scene.raw_event_handler && scene.raw_event_handler(event);

            // Mouse clicks — SDL logical coords → physical layout coords.
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                event.button.button == SDL_BUTTON_LEFT)
            {
                const float px = event.button.x * renderer.pixelScaleX();
                const float py = event.button.y * renderer.pixelScaleY();

                const pce::sdlos::NodeHandle hit =
                    pce::sdlos::dispatchClick(*scene.tree, scene.root,
                                              px, py, events, &scene.css_sheet);

                // When the console is open, log what was hit for debugging.
                if (g_console.visible && hit.valid()) {
                    const pce::sdlos::RenderNode* hn = scene.tree->node(hit);
                    if (hn) {
                        const auto id  = hn->style("id");
                        const auto cls = hn->style("class");
                        const auto oc  = hn->style("onclick");
                        const auto dv  = hn->style("data-value");
                        std::string msg = "[click] ";
                        msg += std::to_string(static_cast<int>(px));
                        msg += ",";
                        msg += std::to_string(static_cast<int>(py));
                        if (!id.empty())  { msg += " id=";  msg += id;  }
                        if (!cls.empty()) { msg += " cls="; msg += cls; }
                        if (!oc.empty())  { msg += " -> ";  msg += oc;
                            if (!dv.empty()) { msg += "("; msg += dv; msg += ")"; }
                        }
                        sdlos_log(msg);
                    }
                }
            }

            // Keyboard shortcuts — skipped when the behavior already consumed
            // the key (e.g. a focused TextArea editor is active).
            if (!behavior_consumed && event.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Scancode sc = event.key.scancode;

                // ` — toggle console visibility.
                if (sc == SDL_SCANCODE_GRAVE) {
                    g_console.visible = !g_console.visible;
                    if (pce::sdlos::RenderNode* nd = scene.tree->node(scene.console_h))
                        nd->dirty_render = true;
                }

                // F1 — toggle layout debug overlay.
                else if (sc == SDL_SCANCODE_F1) {
                    g_console.layout_debug = !g_console.layout_debug;
                    g_console.visible      = true;
                    sdlos_log(std::string("[layout] overlay ")
                              + (g_console.layout_debug ? "ON" : "OFF"));
                    if (pce::sdlos::RenderNode* nd = scene.tree->node(scene.layout_dbg_h))
                        nd->dirty_render = true;
                    if (pce::sdlos::RenderNode* nd = scene.tree->node(scene.console_h))
                        nd->dirty_render = true;
                }

                // / — enter selector mode (when console open, not already in mode)
                else if (sc == SDL_SCANCODE_SLASH && g_console.visible
                         && !g_console.selector_mode) {
                    g_console.selector_mode = true;
                    SDL_StartTextInput(window);
                    if (pce::sdlos::RenderNode* nd = scene.tree->node(scene.console_h))
                        nd->dirty_render = true;
                }

                // Backspace — erase last character from selector query
                else if (sc == SDL_SCANCODE_BACKSPACE
                         && g_console.visible && g_console.selector_mode) {
                    if (!g_console.selector_text.empty())
                        g_console.selector_text.pop_back();
                    runSelector(g_console.selector_text, *scene.tree, scene.root,
                                g_console.selector_matches);
                    g_console.selector_idx =
                        g_console.selector_matches.empty() ? -1 : 0;
                    if (pce::sdlos::RenderNode* nd = scene.tree->node(scene.console_h))
                        nd->dirty_render = true;
                    if (pce::sdlos::RenderNode* nd = scene.tree->node(scene.highlight_h))
                        nd->dirty_render = true;
                }

                // Enter — commit selector query (exit typing mode, keep results)
                else if (sc == SDL_SCANCODE_RETURN && g_console.visible) {
                    if (g_console.selector_mode) {
                        g_console.selector_mode = false;
                        SDL_StopTextInput(window);
                        if (g_console.selector_idx < 0
                                && !g_console.selector_matches.empty())
                            g_console.selector_idx = 0;
                        if (pce::sdlos::RenderNode* nd = scene.tree->node(scene.console_h))
                            nd->dirty_render = true;
                    }
                }

                // Escape — exit selector mode (clear), or close console
                else if (sc == SDL_SCANCODE_ESCAPE) {
                    if (g_console.selector_mode) {
                        g_console.selector_mode = false;
                        SDL_StopTextInput(window);
                    }
                    if (!g_console.selector_text.empty() || g_console.selector_idx >= 0) {
                        g_console.selector_text.clear();
                        g_console.selector_matches.clear();
                        g_console.selector_idx = -1;
                        if (pce::sdlos::RenderNode* nd = scene.tree->node(scene.highlight_h))
                            nd->dirty_render = true;
                    } else {
                        g_console.visible = false;
                    }
                    if (pce::sdlos::RenderNode* nd = scene.tree->node(scene.console_h))
                        nd->dirty_render = true;
                }

                // n — next match   N (Shift+n) — previous match
                else if (sc == SDL_SCANCODE_N && g_console.visible
                         && !g_console.selector_mode
                         && !g_console.selector_matches.empty()) {
                    const int sz = static_cast<int>(g_console.selector_matches.size());
                    const bool prev = (event.key.mod & SDL_KMOD_SHIFT) != 0;
                    if (prev)
                        g_console.selector_idx = (g_console.selector_idx - 1 + sz) % sz;
                    else
                        g_console.selector_idx = (g_console.selector_idx + 1) % sz;
                    if (pce::sdlos::RenderNode* nd = scene.tree->node(scene.console_h))
                        nd->dirty_render = true;
                    if (pce::sdlos::RenderNode* nd = scene.tree->node(scene.highlight_h))
                        nd->dirty_render = true;
                }
            }
        }

        //  Render
        const double t = static_cast<double>(SDL_GetTicks()) * 0.001;
        renderer.Render(t);

        //  Scene navigation
        // A behaviour publishes "sdlos:navigate" with a jade file path.
        // The bus handler (subscribed in loadScene) sets next_jade_path.
        // We act here — after the render pass, before the next frame — so
        // the old scene is fully done before the tree is replaced.
        //
        // Navigation in a behaviour looks like:
        //   bus.publish("sdlos:navigate", "data/slides/slide2.jade");
        //   bus.publish("sdlos:navigate", "/absolute/path/to/settings.jade");
        //
        // Relative paths are resolved against the binary directory (base_path)
        // so they follow the same convention as data/ asset paths.
        if (!next_jade_path.empty()) {
            const std::string target =
                fs::path(next_jade_path).is_absolute()
                    ? next_jade_path
                    : base_path + next_jade_path;
            next_jade_path.clear();

            sdlos_log("[jade_host] loading scene: " + target);
            if (!loadScene(target, scene, renderer, events, next_jade_path, host_vfs)) {
                // Target failed — reload the original so the host is never
                // left with no scene attached to the renderer.
                sdlos_log("[jade_host] scene load failed, reloading: "
                          + std::string(jade_path));
                loadScene(jade_path, scene, renderer, events, next_jade_path, host_vfs);
            }

            std::cout << "[jade_host] scene → " << target
                      << "  (" << scene.tree->nodeCount() << " nodes)\n";
        }

        SDL_Delay(16);   // ~60 fps; replace with vsync / frame timing later
    }

    //  Shutdown
    renderer.Shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();

    std::cout << "[jade_host] '" << SDLOS_APP_NAME << "' exited cleanly\n";
    return EXIT_SUCCESS;
}
