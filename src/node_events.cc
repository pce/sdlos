#include "node_events.h"
#include "hit_test.h"
#include "render_tree.h"
#include "css_loader.h"

#include <SDL3/SDL.h>

#include <charconv>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace pce::sdlos {

namespace {

[[nodiscard]] static std::optional<uint64_t> toU64(std::string_view s) noexcept
{
    if (s.empty()) return std::nullopt;
    uint64_t v{};
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return (ec == std::errc{}) ? std::optional<uint64_t>{v} : std::nullopt;
}

// Compose b after a. If a is empty, returns b directly.
static std::function<void()> compose(std::function<void()> a,
                                     std::function<void()> b)
{
    if (!a) return b;
    return [a = std::move(a), b = std::move(b)]() { a(); b(); };
}

static void walkTree(RenderTree& tree, NodeHandle root,
                     const std::function<void(NodeHandle, RenderNode&)>& fn)
{
    if (!root.valid()) return;
    RenderNode* n = tree.node(root);
    if (!n) return;
    fn(root, *n);
    for (NodeHandle c = n->child; c.valid(); ) {
        RenderNode* cn = tree.node(c);
        if (!cn) break;
        const NodeHandle next = cn->sibling;
        walkTree(tree, c, fn);
        c = next;
    }
}

struct IntervalState {
    uint64_t    interval_ms  = 0;
    std::string topic;
    uint64_t    last_fire_ms = 0;
};

} // anonymous namespace

// ── dispatchClick ─────────────────────────────────────────────────────────────

/**
 * @brief Dispatches click
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 * @param px    Horizontal coordinate in logical pixels
 * @param py    Vertical coordinate in logical pixels
 * @param bus   Blue channel component [0, 1]
 * @param css   css::StyleSheet * value
 *
 * @return Handle to the node, or k_null_handle on failure
 *
 * @warning Parameter 'css' is a non-const raw pointer — Raw pointer parameter —
 *          ownership is ambiguous; consider std::span (non-owning view),
 *          std::unique_ptr (transfer), or const T* (borrow)
 */
NodeHandle dispatchClick(RenderTree& tree, NodeHandle root,
                         float px, float py,
                         IEventBus& bus,
                         css::StyleSheet* css)
{
    const NodeHandle hit = hitTest(tree, root, px, py);
    if (!hit.valid()) return k_null_handle;

    RenderNode* n = tree.node(hit);
    if (!n) return k_null_handle;

    // toggle-group radio activation — walk up from the hit node to find a
    // node whose *parent* carries toggle-group.  This is robust when the hit
    // is a deep descendant (e.g. a text sub-node inside a .preset div).
    if (css && !css->active_entries.empty()) {
        for (NodeHandle cur = hit; cur.valid(); ) {
            const RenderNode* cn = tree.node(cur);
            if (!cn) break;
            if (cn->parent.valid()) {
                const RenderNode* par = tree.node(cn->parent);
                if (par && !par->style("toggle-group").empty()) {
                    css->activateNode(tree, cur);
                    break;
                }
            }
            cur = cn->parent;
        }
    }

    const auto topic = n->style("onclick");
    if (!topic.empty()) {
        const auto payload = n->style("data-value");
        bus.publish(std::string(topic), std::string(payload));
    }

    return hit;
}


/**
 * @brief Dispatches select
 *
 * @param tree    Red channel component [0, 1]
 * @param handle  Opaque resource handle
 * @param bus     Blue channel component [0, 1]
 */
void dispatchSelect(RenderTree& tree, NodeHandle handle, IEventBus& bus)
{
    RenderNode* n = tree.node(handle);
    if (!n) return;

    const auto topic = n->style("onselect");
    if (topic.empty()) return;

    bus.publish(std::string(topic), std::string(n->style("data-value")));
}



/**
 * @brief Binds node events
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 * @param bus   Blue channel component [0, 1]
 */
void bindNodeEvents(RenderTree& tree, NodeHandle root, IEventBus& bus)
{
    walkTree(tree, root, [&](NodeHandle h, RenderNode& n) {

        // Interval timer: fires bus.publish(ontick, "") every interval-ms.
        {
            const auto interval_sv = n.style("interval-ms");
            const auto tick_sv     = n.style("ontick");

            if (!interval_sv.empty() && !tick_sv.empty()) {
                const auto ms = toU64(interval_sv);
                if (ms && *ms > 0) {
                    auto state          = std::make_shared<IntervalState>();
                    state->interval_ms  = *ms;
                    state->topic        = std::string(tick_sv);
                    state->last_fire_ms = SDL_GetTicks();

                    n.update = compose(std::move(n.update),
                        [&bus, state]() mutable {
                            const uint64_t now = SDL_GetTicks();
                            if (now - state->last_fire_ms >= state->interval_ms) {
                                state->last_fire_ms = now;
                                bus.publish(state->topic, "");
                            }
                        });
                }
            }
        }

        // One-shot reveal: fires once on the first update(), then clears itself.
        {
            const auto reveal_sv = n.style("onreveal");
            if (!reveal_sv.empty()) {
                std::string topic = std::string(reveal_sv);

                n.update = compose(std::move(n.update),
                    [&tree, h, &bus, topic,
                     fired = std::make_shared<bool>(false)]() mutable {
                        if (*fired) return;
                        *fired = true;
                        bus.publish(topic, "");
                        // Clear after returning — not safe to modify update
                        // while it is executing.
                        if (RenderNode* self = tree.node(h))
                            self->update = {};
                    });
            }
        }
    });
}

} // namespace pce::sdlos
