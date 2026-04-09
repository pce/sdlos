#include "css_loader.h"
#include "style_applier.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace pce::sdlos::css {

namespace {

[[nodiscard]]
static std::string_view trimSV(std::string_view s) noexcept
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

[[nodiscard]]
static bool hasClassToken(std::string_view class_sv,
                                        const std::string& token) noexcept
{
    std::size_t pos = 0;
    while (pos < class_sv.size()) {
        const std::size_t end = class_sv.find(' ', pos);
        const std::size_t len = (end == std::string_view::npos)
                                    ? class_sv.size() - pos
                                    : end - pos;
        if (class_sv.substr(pos, len) == token)
            return true;
        pos = (end == std::string_view::npos) ? class_sv.size() : end + 1;
    }
    return false;
}

[[nodiscard]]
static std::string kebabToCamel(std::string_view prop)
{
    // CSS custom properties (--foo-bar) are stored verbatim in the StyleMap so
    // that GltfScene can read them with the same key it writes in setStyle():
    //   CSS:  --rotation-x: -90   →  StyleMap key "--rotation-x"
    //   C++:  n->style("--rotation-x")  → "−90"
    // Only standard layout properties (background-color, flex-grow …) get the
    // camelCase conversion expected by the 2-D layout engine.
    if (prop.size() >= 2 && prop[0] == '-' && prop[1] == '-')
        return std::string(prop);

    std::string result;
    result.reserve(prop.size());
    bool next_upper = false;
    for (const char c : prop) {
        if (c == '-') {
            next_upper = true;
        } else if (next_upper) {
            result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            next_upper = false;
        } else {
            result += c;
        }
    }
    return result;
}

[[nodiscard]]
static std::string stripPx(std::string_view val)
{
    if (val.size() > 2 && val.substr(val.size() - 2) == "px")
        return std::string(val.substr(0, val.size() - 2));
    return std::string(val);
}

[[nodiscard]]
static bool isSimpleSelector(std::string_view sel) noexcept
{
    if (sel.size() < 2) return false;
    if (sel[0] != '.' && sel[0] != '#') return false;

    const std::size_t colon = sel.find(':');
    const std::string_view base = (colon == std::string_view::npos)
                                      ? sel.substr(1)
                                      : sel.substr(1, colon - 1);

    if (base.empty()) return false;

    for (const char c : base) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
            return false;
    }

    if (colon != std::string_view::npos) {
        const std::string_view pseudo = sel.substr(colon + 1);
        if (pseudo != "hover" && pseudo != "focus" && pseudo != "active") return false;
    }

    return true;
}

[[nodiscard]]
static std::pair<float, float>
absolutePos(const RenderTree& tree, NodeHandle h) noexcept
{
    float ax = 0.f, ay = 0.f;
    for (NodeHandle p = h; p.valid(); ) {
        const RenderNode* n = tree.node(p);
        if (!n) break;
        ax += n->x;
        ay += n->y;
        p   = n->parent;
    }
    return {ax, ay};
}

static void walkTree(RenderTree& tree, NodeHandle root,
                     const std::function<void(NodeHandle, RenderNode&)>& fn)
{
    if (!root.valid()) return;

    std::vector<NodeHandle> stack;
    stack.reserve(64);
    stack.push_back(root);

    while (!stack.empty()) {
        const NodeHandle h = stack.back();
        stack.pop_back();

        RenderNode* n = tree.node(h);
        if (!n) continue;

        fn(h, *n);

        std::vector<NodeHandle> children;
        for (NodeHandle c = n->child; c.valid(); ) {
            const RenderNode* cn = tree.node(c);
            if (!cn) break;
            children.push_back(c);
            c = cn->sibling;
        }
        for (auto it = children.rbegin(); it != children.rend(); ++it)
            stack.push_back(*it);
    }
}

} // anonymous namespace

/**
 * @brief Parses
 *
 * @param source  CSS source code
 *
 * @return StyleSheet result
 */
StyleSheet parse(std::string_view source)
{
    // Replace /* ... */ block comments with spaces to preserve brace positions.
    std::string s(source);
    for (std::size_t pos = 0; ; ) {
        const std::size_t open = s.find("/*", pos);
        if (open == std::string::npos) break;
        const std::size_t close = s.find("*/", open + 2);
        if (close == std::string::npos) { s.erase(open); break; }
        std::fill(s.begin() + static_cast<std::ptrdiff_t>(open),
                  s.begin() + static_cast<std::ptrdiff_t>(close + 2), ' ');
        pos = close + 2;
    }

    StyleSheet sheet;
    std::size_t i = 0;

    while (i < s.size()) {
        const std::size_t brace_open = s.find('{', i);
        if (brace_open == std::string::npos) break;

        const std::size_t brace_close = s.find('}', brace_open + 1);
        if (brace_close == std::string::npos) break;

        const std::string_view sel_sv =
            trimSV(std::string_view(s).substr(i, brace_open - i));
        const std::string_view block_sv =
            std::string_view(s).substr(brace_open + 1, brace_close - brace_open - 1);

        i = brace_close + 1;

        if (!isSimpleSelector(sel_sv)) continue;

        Rule rule;
        rule.is_id = (sel_sv[0] == '#');

        const std::size_t colon_pos = sel_sv.find(':');
        if (colon_pos != std::string_view::npos) {
            rule.selector = std::string(sel_sv.substr(1, colon_pos - 1));
            rule.pseudo   = std::string(sel_sv.substr(colon_pos + 1));
        } else {
            rule.selector = std::string(sel_sv.substr(1));
            rule.pseudo   = "";
        }

        std::string_view remaining = block_sv;
        while (!remaining.empty()) {
            const std::size_t semi = remaining.find(';');
            const std::string_view decl = (semi == std::string_view::npos)
                                              ? remaining
                                              : remaining.substr(0, semi);
            remaining = (semi == std::string_view::npos)
                            ? std::string_view{}
                            : remaining.substr(semi + 1);

            const std::size_t colon = decl.find(':');
            if (colon == std::string_view::npos) continue;

            const std::string_view css_prop = trimSV(decl.substr(0, colon));
            const std::string_view css_val  = trimSV(decl.substr(colon + 1));
            if (css_prop.empty() || css_val.empty()) continue;

            const std::string jade_attr = kebabToCamel(css_prop);
            if (jade_attr.empty()) continue;

            const std::string value = stripPx(css_val);
            if (value.empty()) continue;

            rule.props.emplace_back(jade_attr, value);
        }

        if (!rule.props.empty())
            sheet.rules.push_back(std::move(rule));
    }

    return sheet;
}


/**
 * @brief Loads recursive
 *
 * Forward declaration for recursive @import handling.
 *
 * @param path     Filesystem path
 * @param visited  Iterator position
 *
 * @return StyleSheet result
 */
static StyleSheet loadRecursive(const std::string& path,
                                 std::unordered_set<std::string>& visited);

/**
 * @brief Loads recursive
 *
 * @param path     Filesystem path
 * @param visited  Iterator position
 *
 * @return StyleSheet result
 */
static StyleSheet loadRecursive(const std::string& path,
                                 std::unordered_set<std::string>& visited)
{
    // Guard against circular @import chains.
    if (!visited.insert(path).second) return {};

    std::ifstream ifs(path);
    if (!ifs) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    const std::string src = ss.str();

    const std::filesystem::path base_dir =
        std::filesystem::path(path).parent_path();

    StyleSheet result;
    std::string remaining;
    remaining.reserve(src.size());

    // Strip @import directives and collect imported stylesheets first
    // (imported rules precede the current file's rules in cascade order).
    for (std::size_t i = 0; i < src.size(); ) {
        if (src[i] == '@' && src.compare(i, 7, "@import") == 0) {
            const std::size_t end = src.find(';', i);
            if (end == std::string::npos) break;  // malformed — stop

            const std::string_view stmt =
                std::string_view(src).substr(i, end - i + 1);

            const std::size_t q1 = stmt.find_first_of("\"'");
            if (q1 != std::string_view::npos) {
                const char q = stmt[q1];
                const std::size_t q2 = stmt.find(q, q1 + 1);
                if (q2 != std::string_view::npos) {
                    const std::string rel(stmt.substr(q1 + 1, q2 - q1 - 1));
                    const std::string abs =
                        std::filesystem::path(rel).is_absolute()
                            ? rel
                            : (base_dir / rel).string();
                    const StyleSheet imp = loadRecursive(abs, visited);
                    for (const auto& r : imp.rules)
                        result.rules.push_back(r);
                }
            }
            i = end + 1;
        } else {
            remaining += src[i++];
        }
    }

    // Parse the file content (with @import lines already stripped).
    StyleSheet main = parse(remaining);
    for (auto& r : main.rules)
        result.rules.push_back(std::move(r));

    return result;
}

/**
 * @brief Loads
 *
 * @param path  Filesystem path
 *
 * @return StyleSheet result
 */
StyleSheet load(const std::string& path)
{
    std::unordered_set<std::string> visited;
    return loadRecursive(path, visited);
}

/**
 * @brief Applies to
 *
 * @param tree      Red channel component [0, 1]
 * @param root      Red channel component [0, 1]
 * @param px_scale  Uniform scale factor
 */
void StyleSheet::applyTo(RenderTree& tree, NodeHandle root, float px_scale)
{
    if (!root.valid() || rules.empty()) return;

    bool any_base = false;
    for (const Rule& r : rules)
        if (r.pseudo.empty()) { any_base = true; break; }
    if (!any_base) return;

    walkTree(tree, root, [this, px_scale](NodeHandle /*h*/, RenderNode& n) {
        const std::string_view id_sv    = n.style("id");
        const std::string_view class_sv = n.style("class");
        if (id_sv.empty() && class_sv.empty()) return;

        bool any_added = false;

        for (const Rule& rule : rules) {
            if (!rule.pseudo.empty()) continue;

            const bool matched = rule.is_id
                ? (!id_sv.empty()    && id_sv    == rule.selector)
                : (!class_sv.empty() && hasClassToken(class_sv, rule.selector));
            if (!matched) continue;

            for (const auto& [jade_attr, value] : rule.props) {
                if (!n.hasStyle(jade_attr)) {
                    n.setStyle(jade_attr, value);
                    any_added = true;
                }
            }
        }

        if (any_added)
            StyleApplier::apply(n, px_scale);
    });
}

/**
 * @brief Builds hover
 *
 * @param tree      Red channel component [0, 1]
 * @param root      Red channel component [0, 1]
 * @param px_scale  Uniform scale factor
 */
void StyleSheet::buildHover(RenderTree& tree, NodeHandle root, float px_scale)
{
    hover.clear();

    bool any_pseudo = false;
    for (const Rule& r : rules)
        if (!r.pseudo.empty()) { any_pseudo = true; break; }
    if (!any_pseudo) return;

    walkTree(tree, root, [this](NodeHandle h, RenderNode& n) {
        const std::string_view id_sv    = n.style("id");
        const std::string_view class_sv = n.style("class");
        if (id_sv.empty() && class_sv.empty()) return;

        for (const Rule& rule : rules) {
            if (rule.pseudo.empty()) continue;

            const bool matched = rule.is_id
                ? (!id_sv.empty()    && id_sv    == rule.selector)
                : (!class_sv.empty() && hasClassToken(class_sv, rule.selector));
            if (!matched) continue;

            HoverEntry entry;
            entry.handle = h;

            for (const auto& [attr, hover_val] : rule.props) {
                entry.on_enter.emplace_back(attr, hover_val);
                // Snapshot current value so it can be restored on pointer-leave.
                entry.on_leave.emplace_back(attr, std::string(n.style(attr)));
            }

            hover.push_back(std::move(entry));
        }
    });
}

/**
 * @brief Ticks one simulation frame for hover
 *
 * @param tree      Red channel component [0, 1]
 * @param phys_x    Opaque resource handle
 * @param phys_y    Opaque resource handle
 * @param px_scale  Uniform scale factor
 */
void StyleSheet::tickHover(RenderTree& tree, float phys_x, float phys_y, float px_scale)
{
    for (HoverEntry& entry : hover) {
        RenderNode* n = tree.node(entry.handle);
        if (!n) continue;

        const auto [ax, ay] = absolutePos(tree, entry.handle);
        const bool now = (phys_x >= ax && phys_x < ax + n->w &&
                          phys_y >= ay && phys_y < ay + n->h);

        if (now == entry.hovered) continue;
        entry.hovered = now;

        for (const auto& [attr, val] : (now ? entry.on_enter : entry.on_leave))
            n->setStyle(attr, val);

        // If a hover style changed properties that affect layout (width, height, flex, etc.),
        // we must mark the node as layout-dirty so the changes are reflected in the next frame.
        // StyleApplier::apply() writes both the logical layout_props and the physical n->w/h.
        StyleApplier::apply(*n, px_scale);
        tree.markLayoutDirty(entry.handle);
    }
}

/**
 * @brief Builds active
 *
 * @param tree      Red channel component [0, 1]
 * @param root      Red channel component [0, 1]
 * @param px_scale  Uniform scale factor
 */
void StyleSheet::buildActive(RenderTree& tree, NodeHandle root, float px_scale)
{
    active_entries.clear();

    bool any_active = false;
    for (const Rule& r : rules)
        if (r.pseudo == "active") { any_active = true; break; }
    if (!any_active) return;

    walkTree(tree, root, [this, &tree](NodeHandle h, RenderNode& n) {
        if (n.style("toggle-group").empty()) return;

        // h is a toggle-group parent; examine its direct children only.
        for (NodeHandle c = n.child; c.valid(); ) {
            RenderNode* cn = tree.node(c);
            const NodeHandle next_c = cn ? cn->sibling : k_null_handle;

            if (cn) {
                const std::string_view id_sv    = cn->style("id");
                const std::string_view class_sv = cn->style("class");

                for (const Rule& rule : rules) {
                    if (rule.pseudo != "active") continue;

                    const bool matched = rule.is_id
                        ? (!id_sv.empty()    && id_sv    == rule.selector)
                        : (!class_sv.empty() && hasClassToken(class_sv, rule.selector));
                    if (!matched) continue;

                    ActiveEntry entry;
                    entry.handle       = c;
                    entry.group_parent = h;
                    entry.active       = false;

                    for (const auto& [attr, active_val] : rule.props) {
                        entry.on_enter.emplace_back(attr, active_val);

                        // Derive the "off" value from CSS *base* rules only —
                        // never snapshot current node state (behaviors may have
                        // already mutated it by the time buildActive() runs).
                        std::string base_val;
                        for (const Rule& base : rules) {
                            if (!base.pseudo.empty()) continue;
                            const bool bm = base.is_id
                                ? (!id_sv.empty()    && id_sv    == base.selector)
                                : (!class_sv.empty() && hasClassToken(class_sv, base.selector));
                            if (!bm) continue;
                            for (const auto& [ba, bv] : base.props)
                                if (ba == attr) { base_val = bv; break; }
                            if (!base_val.empty()) break;
                        }
                        entry.on_leave.emplace_back(attr, std::move(base_val));
                    }

                    active_entries.push_back(std::move(entry));
                    break; // first matching :active rule per node wins
                }
            }

            c = next_c;
        }
    });

    // Second pass: auto-activate any entry whose jade node carries active="1".
    // This lets the jade author declare the initial selection without any
    // behavior C++ code:
    //   div.preset(onclick="…" data-value="…" active="1") Label
    // Radio constraint: at most one item per group should be marked; if more
    // than one is found, each is activated in order (last one wins visually —
    // document that only one should be set per group).
    for (ActiveEntry& e : active_entries) {
        const RenderNode* cn = tree.node(e.handle);
        if (!cn || cn->style("active").empty()) continue;

        e.active = true;
        if (RenderNode* nm = tree.node(e.handle)) {
            for (const auto& [attr, val] : e.on_enter)
                nm->setStyle(attr, val);
            StyleApplier::apply(*nm, px_scale);
            tree.markLayoutDirty(e.handle);
        }
    }
}

/**
 * @brief Activate node
 *
 * @param tree      Red channel component [0, 1]
 * @param clicked   Render-tree node handle
 * @param px_scale  Uniform scale factor
 */
void StyleSheet::activateNode(RenderTree& tree, NodeHandle clicked, float px_scale)
{
    if (active_entries.empty()) return;

    // Find which group the clicked node belongs to.
    NodeHandle group_parent = k_null_handle;
    for (const auto& e : active_entries)
        if (e.handle == clicked) { group_parent = e.group_parent; break; }
    if (!group_parent.valid()) return;

    // Radio-style: exactly one entry active at a time within the group.
    for (ActiveEntry& entry : active_entries) {
        if (entry.group_parent != group_parent) continue;

        RenderNode* n = tree.node(entry.handle);
        if (!n) continue;

        const bool should_activate = (entry.handle == clicked);
        if (should_activate == entry.active) continue; // no change

        entry.active = should_activate;

        for (const auto& [attr, val] : (should_activate ? entry.on_enter : entry.on_leave))
            n->setStyle(attr, val);

        StyleApplier::apply(*n, px_scale);
        tree.markLayoutDirty(entry.handle);
    }
}

} // namespace pce::sdlos::css
