// Single-pass line-oriented parser for pipeline.pug.
//
// Grammar (line-oriented, indentation ignored at pipeline level):
//
//   file        := line*
//   line        := blank | comment | declaration
//   blank       := (' ' | '\t')* newline
//   comment     := ('//' .*) newline
//   declaration := tag '#' id '(' attrs? ')' newline
//   tag         := 'variant' | 'resource' | 'pass'
//   id          := [a-z][a-z0-9_]*
//   attrs       := attr (' '+ attr)*
//   attr        := key '=' '"' value '"'
//   key         := [a-zA-Z_][a-zA-Z0-9_-]*
//   value       := [^"]*
//
// Multi-line attribute lists are supported: a declaration whose opening '('
// has no matching ')' on the same line is folded with subsequent lines
// until the paren closes.

#include "pug_parser.h"

#include <charconv>
#include <optional>
#include <vector>

namespace pce::sdlos::fg::pug {


namespace {

[[nodiscard]] static constexpr std::string_view
trim(std::string_view s) noexcept
{
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last  = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

[[nodiscard]] static bool
starts_with_tag(std::string_view line, std::string_view tag) noexcept
{
    if (!line.starts_with(tag)) return false;
    if (line.size() <= tag.size()) return false;
    return line[tag.size()] == '#';
}

// Extract key="value" pairs from the content between the outermost parens.
// Handles quoted values containing spaces or '#', and empty blocks.
[[nodiscard]] static StyleMap
parse_attrs(std::string_view block) noexcept
{
    StyleMap    result;
    std::size_t pos = 0;
    const auto  n   = block.size();

    while (pos < n) {
        while (pos < n && (block[pos] == ' ' || block[pos] == '\t' ||
                           block[pos] == '\r' || block[pos] == '\n'))
            ++pos;
        if (pos >= n) break;

        // Stop if we hit a closing paren — it belongs to the pass declaration.
        if (block[pos] == ')') break;

        const auto key_start = pos;
        while (pos < n && block[pos] != '=' && block[pos] != ' ' &&
               block[pos] != '\t' && block[pos] != ')')
            ++pos;
        if (pos >= n || block[pos] != '=') break;

        std::string key{ trim(block.substr(key_start, pos - key_start)) };
        if (key.empty()) break;

        ++pos; // skip '='

        if (pos >= n || block[pos] != '"') break;
        ++pos; // skip opening '"'

        const auto val_start = pos;
        while (pos < n && block[pos] != '"')
            ++pos;

        std::string val{ block.substr(val_start, pos - val_start) };
        if (pos < n) ++pos; // skip closing '"'

        // Log the attribute name and value for debugging.
        // Result is std::map<std::string, std::string> in pug_parser.h.
        result.insert_or_assign(std::move(key), std::move(val));
    }

    return result;
}

// Split a space-delimited string into tokens.  Used for reads="lit depth".
[[nodiscard]] static std::vector<std::string>
split_words(std::string_view s) noexcept
{
    std::vector<std::string> result;
    std::size_t pos = 0;
    const auto  n   = s.size();
    while (pos < n) {
        while (pos < n && s[pos] == ' ') ++pos;
        if (pos >= n) break;
        const auto start = pos;
        while (pos < n && s[pos] != ' ') ++pos;
        result.emplace_back(s.substr(start, pos - start));
    }
    return result;
}

[[nodiscard]] static TexFormat
parse_tex_format(std::string_view s) noexcept
{
    if (s == "rgba16f") return TexFormat::RGBA16F;
    if (s == "depth32") return TexFormat::Depth32F;
    return TexFormat::RGBA8;
}

[[nodiscard]] static TexSize
parse_tex_size(std::string_view s) noexcept
{
    return (s == "swapchain") ? TexSize::Swapchain : TexSize::Fixed;
}

[[nodiscard]] static constexpr bool
parse_bool(std::string_view s) noexcept
{
    return s != "false" && s != "0" && s != "no";
}

[[nodiscard]] static uint32_t
parse_uint(std::string_view s) noexcept
{
    uint32_t v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

struct LineParser {
    std::string_view line;     ///< trimmed, without newline
    std::size_t      line_no;  ///< 1-based, for error messages

    // Extracts id and attr map from a declaration after the tag prefix.
    // Returns false if the line is malformed.
    [[nodiscard]] bool
    extract_id_and_attrs(std::size_t  tag_len,
                         std::string& id_out,
                         StyleMap&    attrs_out) const noexcept
    {
        if (line.size() <= tag_len + 1) return false;
        const auto after_tag = line.substr(tag_len + 1); // +1 for '#'

        const auto paren_open = after_tag.find('(');
        if (paren_open == std::string_view::npos) {
            // No attributes — id is the rest of the line.
            id_out = std::string{ trim(after_tag) };
            return !id_out.empty();
        }

        id_out = std::string{ trim(after_tag.substr(0, paren_open)) };
        if (id_out.empty()) return false;

        const auto paren_close = after_tag.rfind(')');
        if (paren_close == std::string_view::npos || paren_close <= paren_open)
            return false;

        const auto attr_block =
            after_tag.substr(paren_open + 1, paren_close - paren_open - 1);
        attrs_out = parse_attrs(attr_block);
        return true;
    }

    [[nodiscard]] std::optional<VariantDesc>
    try_variant() const noexcept
    {
        if (!starts_with_tag(line, "variant")) return std::nullopt;
        VariantDesc vd;
        StyleMap    attrs;
        if (!extract_id_and_attrs(7, vd.id, attrs)) return std::nullopt;
        vd.defines = std::move(attrs);
        return vd;
    }

    [[nodiscard]] std::optional<ResourceDesc>
    try_resource() const noexcept
    {
        if (!starts_with_tag(line, "resource")) return std::nullopt;
        ResourceDesc rd;
        StyleMap     attrs;
        if (!extract_id_and_attrs(8, rd.id, attrs)) return std::nullopt;
        if (const auto* v = attrs.find("format")) rd.format = parse_tex_format(*v);
        if (const auto* v = attrs.find("size"))   rd.size   = parse_tex_size(*v);
        if (const auto* v = attrs.find("w"))      rd.w      = parse_uint(*v);
        if (const auto* v = attrs.find("h"))      rd.h      = parse_uint(*v);
        return rd;
    }

    // Meta-keys (shader / reads / writes / enabled) are consumed here.
    // All remaining keys become Bucket-C float params.
    [[nodiscard]] std::optional<PassDesc>
    try_pass() const noexcept
    {
        if (!starts_with_tag(line, "pass")) return std::nullopt;
        PassDesc pd;
        StyleMap attrs;
        if (!extract_id_and_attrs(4, pd.id, attrs)) return std::nullopt;

        static constexpr std::string_view k_meta[] = {
            "enabled", "reads", "shader", "writes"
        };
        auto is_meta = [](std::string_view k) {
            for (auto m : k_meta) if (k == m) return true;
            return false;
        };

        if (const auto* v = attrs.find("shader"))  pd.shader_key = *v;
        if (const auto* v = attrs.find("writes"))  pd.writes     = *v;
        if (const auto* v = attrs.find("enabled")) pd.enabled    = parse_bool(*v);
        if (const auto* v = attrs.find("reads"))   pd.reads      = split_words(*v);

        for (const auto& e : attrs)
            if (!is_meta(e.key))
                pd.params.insert_or_assign(e.key, e.val);

        return pd;
    }
};

} // anonymous namespace

[[nodiscard]]
std::expected<ParseResult, std::string>
/**
 * @brief Parses
 *
 * @param src  Source operand or data
 *
 * @return Integer result; negative values indicate an error code
 */
parse(std::string_view src) noexcept
{
    ParseResult result;

    std::size_t pos     = 0;
    std::size_t line_no = 0;

    while (pos <= src.size()) {
        const auto end = src.find('\n', pos);
        const auto raw = (end == std::string_view::npos)
                             ? src.substr(pos)
                             : src.substr(pos, end - pos);
        pos = (end == std::string_view::npos) ? src.size() + 1 : end + 1;
        ++line_no;

        const auto line = trim(raw);
        if (line.empty() || line.starts_with("//")) continue;

        // Fold multi-line declarations: if '(' is opened but ')' is not on
        // the same line, consume continuation lines until the paren closes.
        std::string      folded_buf;
        std::string_view effective_line = line;

        const bool is_decl = starts_with_tag(line, "pass")     ||
                             starts_with_tag(line, "resource") ||
                             starts_with_tag(line, "variant");

        if (is_decl &&
            line.find('(') != std::string_view::npos &&
            line.find(')') == std::string_view::npos)
        {
            folded_buf = std::string(line);
            while (pos <= src.size() &&
                   folded_buf.find(')') == std::string::npos)
            {
                const auto cont_end = src.find('\n', pos);
                const auto cont_raw = (cont_end == std::string_view::npos)
                                          ? src.substr(pos)
                                          : src.substr(pos, cont_end - pos);
                pos = (cont_end == std::string_view::npos)
                          ? src.size() + 1
                          : cont_end + 1;
                ++line_no;
                const auto cont_line = trim(cont_raw);
                if (!cont_line.empty() && !cont_line.starts_with("//")) {
                    folded_buf += ' ';
                    folded_buf += std::string(cont_line);
                }
            }
            effective_line = folded_buf;
        }

        LineParser lp{ effective_line, line_no };

        if (auto v = lp.try_variant())  { result.variants.push_back(std::move(*v));  continue; }
        if (auto r = lp.try_resource()) { result.resources.push_back(std::move(*r)); continue; }
        if (auto p = lp.try_pass())     { result.passes.push_back(std::move(*p));    continue; }

        // Unknown tags are silently skipped (lenient / forward-compatible).
        // To make the parser strict, return an unexpected error here instead.
    }

    return result;
}

} // namespace pce::sdlos::fg::pug
