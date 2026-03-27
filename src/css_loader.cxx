#include "css_loader.hh"
#include "style_applier.hh"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <vector>

namespace pce::sdlos::css {

namespace {

[[nodiscard]] static std::string_view trimSV(std::string_view s) noexcept
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

[[nodiscard]] static bool hasClassToken(std::string_view class_sv,
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

[[nodiscard]] static std::string kebabToCamel(std::string_view prop)
{
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

[[nodiscard]] static std::string stripPx(std::string_view val)
{
    if (val.size() > 2 && val.substr(val.size() - 2) == "px")
        return std::string(val.substr(0, val.size() - 2));
    return std::string(val);
}

[[nodiscard]] static bool isSimpleSelector(std::string_view sel) noexcept
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
        if (pseudo != "hover" && pseudo != "focus") return false;
    }

    return true;
}

[[nodiscard]] static std::pair<float, float>
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

StyleSheet load(const std::string& path)
{
    std::ifstream ifs(path);
    if (!ifs) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return parse(ss.str());
}

void StyleSheet::applyTo(RenderTree& tree, NodeHandle root)
{
    if (!root.valid() || rules.empty()) return;

    bool any_base = false;
    for (const Rule& r : rules)
        if (r.pseudo.empty()) { any_base = true; break; }
    if (!any_base) return;

    walkTree(tree, root, [this](NodeHandle /*h*/, RenderNode& n) {
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
            StyleApplier::apply(n);
    });
}

void StyleSheet::buildHover(RenderTree& tree, NodeHandle root)
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

void StyleSheet::tickHover(RenderTree& tree, float phys_x, float phys_y)
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

        StyleApplier::apply(*n);
        n->dirty_render = true;
    }
}

} // namespace pce::sdlos::css
