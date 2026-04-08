#include "jade_lexer.h"

#include <cctype>
#include <climits>
#include <stack>

namespace pce::sdlos::jade {

namespace {

static bool isIdentStart(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

static bool isIdentChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
}

// Indent-style detection
//
// First pass over the source to determine the indent unit (spaces per level).
//
// Algorithm — GCD of all observed positive leading-space column counts:
//
//   Indents at 2, 4, 6     → GCD 2 → unit 2
//   Indents at 4, 8, 12    → GCD 4 → unit 4
//   Indents at 2, 4        → GCD 2 → unit 2  (mixed depth, 2-space file)
//   Quirky file: 2 and 3   → GCD 1 → fall back: min indent < 3 → unit 2
//   Quirky file: 3 and 6   → GCD 3 → snap to 4 (nearest of {2,4})
//   Pure-tab file          → unit unused; each tab = one level
//
// The unit is snapped to the nearest member of {2, 4} after GCD computation
// so pathological values (unit=3, unit=7, …) never reach the tokenizer.

struct IndentStyle {
    int  unit      = 2;    // spaces per indent level: 2 or 4
    bool uses_tabs = false;
};

static int gcd_int(int a, int b) noexcept
{
    while (b) { const int t = b; b = a % b; a = t; }
    return a;
}

static IndentStyle detectIndent(std::string_view src) noexcept
{
    IndentStyle style;
    int running_gcd = 0;
    int min_spaces  = INT_MAX;

    std::string_view s = src;
    while (!s.empty()) {
        const auto       nl   = s.find('\n');
        std::string_view line = (nl == std::string_view::npos) ? s : s.substr(0, nl);
        s = (nl == std::string_view::npos) ? std::string_view{} : s.substr(nl + 1);

        int sp = 0, tb = 0;
        for (char c : line) {
            if      (c == ' ')  ++sp;
            else if (c == '\t') ++tb;
            else                break;
        }

        if (tb > 0) style.uses_tabs = true;
        if (sp > 0) {
            if (sp < min_spaces) min_spaces = sp;
            running_gcd = (running_gcd == 0) ? sp : gcd_int(running_gcd, sp);
        }
    }

    // No space-indented lines found, pure-tab or flat; unit stays at 2
    if (min_spaces == INT_MAX) return style;

    if (running_gcd >= 4) {
        style.unit = 4;
    } else if (running_gcd >= 2) {
        style.unit = 2;
    } else {
        style.unit = (min_spaces >= 3) ? 4 : 2;
    }

    return style;
}

// Raw indent counter
//
// Returns the leading-whitespace breakdown of a line:
//   spaces — count of ' ' characters
//   tabs   — count of '\t' characters
//   chars  — total raw characters consumed (used to strip indent from content)
//
// Separating `chars` from the logical column count fixes a pre-existing bug:
// the old code did line.substr(indent) where indent was space + tab*4, but a
// tab is only ONE character — so a tab-indented line had content eaten.

struct RawIndent {
    int         spaces = 0;
    int         tabs   = 0;
    std::size_t chars  = 0;  // raw chars to skip for content stripping
};

static RawIndent countRawIndent(std::string_view line) noexcept
{
    RawIndent r;
    for (char c : line) {
        if      (c == ' ')  { ++r.spaces; ++r.chars; }
        else if (c == '\t') { ++r.tabs;   ++r.chars; }
        else                 break;
    }
    return r;
}

// Level normalisation
//
// Converts raw leading whitespace to a nesting level integer.
//
// Tabs contribute one full level each, independently of the space unit.
// This makes tabs interoperable with either space convention.
//
// Spaces are divided by `unit` and rounded to the nearest integer using
// standard round-half-up:  level = (spaces + unit/2) / unit
//
// Rounding table for unit=2:
//   raw  0 → 0    1 → 1    2 → 1    3 → 2    4 → 2    5 → 3 …
//
// Rounding table for unit=4:
//   raw  0 → 0    1 → 0    2 → 1    3 → 1    4 → 1    5 → 1
//        6 → 2    7 → 2    8 → 2    9 → 2   10 → 3 …
//
// So a user who writes 3 spaces in a 2-space file is rounded to level 2
// (child), and 1 space rounds to level 1 (same as 2 spaces — same child).
// Quirky files are silently tolerated rather than rejected.

static int rawToLevel(int spaces, int tabs, int unit) noexcept
{
    const int space_levels = (unit > 0) ? (spaces + (unit >> 1)) / unit : spaces;
    return tabs + space_levels;
}

// net_paren_delta — net change in paren nesting depth across a string.
//
// Quote-aware: parens that appear inside "..." or '...' values are ignored,
// so an attribute like  onclick="show()"  does not falsely close the list.
//
// Returns:
//   > 0  — more opens than closes (list is still open at end of string)
//   = 0  — balanced
//   < 0  — more closes than opens (malformed, treated as balanced enough)
static int net_paren_delta(std::string_view s) noexcept
{
    int  depth      = 0;
    bool in_quote   = false;
    char quote_char = '\0';
    for (const char c : s) {
        if (in_quote) {
            if (c == quote_char) in_quote = false;
        } else {
            if      (c == '"' || c == '\'') { in_quote = true; quote_char = c; }
            else if (c == '(')              { ++depth; }
            else if (c == ')')              { --depth; }
        }
    }
    return depth;
}

// Attribute list parser
// Called with i pointing to the char *after* the opening '('.
// Advances i past the closing ')'.
// Emits AttrKey / AttrValue pairs; forgiving on malformed input.

static void parseAttrs(std::string_view s, std::size_t& i,
                       std::vector<Token>& out)
{
    const std::size_t n = s.size();

    while (i < n && s[i] != ')') {
        // skip whitespace and commas between attributes
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == ',')) ++i;
        if (i >= n || s[i] == ')') break;

        // read key (ident chars only)
        const std::size_t kstart = i;
        while (i < n && isIdentChar(s[i])) ++i;
        if (i == kstart) { ++i; continue; } // unknown char — skip (forgiving)

        std::string key(s.substr(kstart, i - kstart));

        // skip whitespace before '='
        while (i < n && s[i] == ' ') ++i;

        std::string val;
        if (i < n && s[i] == '=') {
            ++i; // consume '='
            while (i < n && s[i] == ' ') ++i; // skip whitespace after '='

            if (i < n && (s[i] == '"' || s[i] == '\'')) {
                // quoted value
                const char        q      = s[i++];
                const std::size_t vstart = i;
                while (i < n && s[i] != q) ++i;
                val = std::string(s.substr(vstart, i - vstart));
                if (i < n) ++i; // consume closing quote
            } else {
                // unquoted value — ends at space, comma, or ')'
                const std::size_t vstart = i;
                while (i < n && s[i] != ' ' && s[i] != ',' && s[i] != ')') ++i;
                val = std::string(s.substr(vstart, i - vstart));
            }
        }

        out.push_back({TokenType::AttrKey,   std::move(key)});
        out.push_back({TokenType::AttrValue, std::move(val)});
    }

    if (i < n && s[i] == ')') ++i; // consume ')'
}

// Single-line tokenizer
// Receives the line *after* the leading indent has been stripped.
// Does NOT emit Indent / Dedent / Newline — those are added by tokenize().

static void tokenizeLine(std::string_view s, std::vector<Token>& out)
{
    if (s.empty()) return;

    // Comment: skip the entire line
    if (s.size() >= 2 && s[0] == '/' && s[1] == '/') return;

    // Pipe literal text: "| some text"
    if (s[0] == '|') {
        std::size_t i = 1;
        while (i < s.size() && s[i] == ' ') ++i; // trim leading space
        if (i < s.size())
            out.push_back({TokenType::Text, std::string(s.substr(i))});
        return;
    }

    std::size_t       i       = 0;
    const std::size_t n       = s.size();
    bool              has_tag = false;

    // Optional tag name
    if (i < n && isIdentStart(s[i])) {
        const std::size_t start = i;
        while (i < n && isIdentChar(s[i])) ++i;
        out.push_back({TokenType::Tag, std::string(s.substr(start, i - start))});
        has_tag = true;
    }

    //  .class and #id shortcuts (may repeat: div.a.b#id.c)
    while (i < n && (s[i] == '.' || s[i] == '#')) {
        if (!has_tag) {
            // Bare ".card" or "#main" — implicit div
            out.push_back({TokenType::Tag, "div"});
            has_tag = true;
        }
        const char        sigil = s[i++];
        const std::size_t start = i;
        while (i < n && isIdentChar(s[i])) ++i;
        if (i > start) {
            out.push_back({
                sigil == '.' ? TokenType::Class : TokenType::Id,
                std::string(s.substr(start, i - start))
            });
        }
    }

    //  Attribute list
    if (i < n && s[i] == '(') {
        ++i; // consume '('
        parseAttrs(s, i, out);
    }

    //  Inline text — everything remaining after optional whitespace
    while (i < n && s[i] == ' ') ++i;
    if (i < n)
        out.push_back({TokenType::Text, std::string(s.substr(i))});
}

} // anonymous namespace


/**
 * @brief Tokenize
 *
 * @return Integer result; negative values indicate an error code
 */
std::vector<Token> Lexer::tokenize() const
{
    // detect indent style
    //
    // Scans the entire source once, reads leading whitespace on each line, and
    // computes the GCD of all positive space-indent column counts.  The result
    // is the `unit` used for level normalisation in Pass 2.
    const IndentStyle style = detectIndent(source_);


    std::vector<Token> tokens;
    tokens.reserve(64);

    // Indent stack: stores the *normalised level* of each open indent scope.
    // Bottom element is always 0 (root level).
    //
    // Using levels (not raw column counts) makes the comparisons below unit-
    // and tab-independent: level 1 is "one indent in" regardless of whether
    // the file uses 2 spaces, 4 spaces, or tabs.
    std::stack<int> indent_stack;
    indent_stack.push(0);

    std::string_view src = source_;

    while (!src.empty()) {
        // Split off one line
        const auto       nl_pos = src.find('\n');
        std::string_view line   = (nl_pos == std::string_view::npos)
                                      ? src
                                      : src.substr(0, nl_pos);
        src = (nl_pos == std::string_view::npos)
                  ? std::string_view{}
                  : src.substr(nl_pos + 1);

        // Strip trailing \r (Windows CRLF)
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        // Skip blank / whitespace-only lines
        {
            bool blank = true;
            for (char c : line)
                if (c != ' ' && c != '\t') { blank = false; break; }
            if (blank) continue;
        }

        //  Measure leading whitespace
        //
        // `ri.chars` is the number of raw characters to skip for content
        // stripping — NOT the same as the logical column count when tabs are
        // present.  Using ri.chars instead of the old `indent` value fixes the
        // bug where a tab-indented line had content characters accidentally
        // consumed by line.substr(4).
        const RawIndent ri      = countRawIndent(line);
        const int       level   = rawToLevel(ri.spaces, ri.tabs, style.unit);
        const std::string_view content = line.substr(ri.chars);

        // Skip comment lines before emitting any indent tokens
        // A comment must not disturb the indent stack or emit Newline.
        if (content.size() >= 2 && content[0] == '/' && content[1] == '/')
            continue;

        // Emit Indent / Dedent
        //
        // Comparisons use normalised levels so the thresholds are consistent
        // regardless of the raw whitespace in the source.
        //
        // Orphaned / over-dedented levels:
        //   If, after popping all levels below `level`, the stack top is still
        //   below `level` (i.e. the author jumped back to an intermediate depth
        //   that was never pushed), we do NOT emit an extra Indent.  The line
        //   is silently snapped to the nearest ancestor level — the same
        //   behaviour as the old column-based code.  This avoids a spurious
        //   Dedent + Indent pair that would misattach the node in the parser.
        if (level > indent_stack.top()) {
            indent_stack.push(level);
            tokens.push_back({TokenType::Indent, {}});
        } else {
            while (level < indent_stack.top()) {
                indent_stack.pop();
                tokens.push_back({TokenType::Dedent, {}});
            }
            // level == stack.top()  →  same depth, no token.
            // level >  stack.top()  →  orphaned quirky line, snap silently.
        }

        // Multi-line attribute folding
        // Real Pug/Jade allows wrapping long attribute lists across lines:
        //
        //   div(id="canvas"
        //       flexGrow="1"
        //       _shader="preset_a")
        //
        // When the opening line has unmatched '(' we consume continuation
        // lines directly from src until the parens balance.  Those lines are
        // intentionally NOT processed by the main loop — they produce no
        // Indent / Dedent / Newline tokens of their own; they are part of the
        // parent tag declaration.
        std::string      folded_buf;
        std::string_view effective_content = content;
        {
            int depth = net_paren_delta(content);
            if (depth > 0) {
                folded_buf = std::string(content);
                while (!src.empty() && depth > 0) {
                    // Pull one raw continuation line out of src.
                    const auto cont_nl  = src.find('\n');
                    auto       cont_raw = (cont_nl == std::string_view::npos)
                                             ? src
                                             : src.substr(0, cont_nl);
                    src = (cont_nl == std::string_view::npos)
                              ? std::string_view{}
                              : src.substr(cont_nl + 1);

                    if (!cont_raw.empty() && cont_raw.back() == '\r')
                        cont_raw.remove_suffix(1);

                    // Strip leading indent — we only want the content.
                    const auto cont_ri      = countRawIndent(cont_raw);
                    const auto cont_content = cont_raw.substr(cont_ri.chars);

                    // Skip comment-only continuation lines without breaking fold.
                    if (cont_content.size() >= 2 &&
                        cont_content[0] == '/' && cont_content[1] == '/')
                        continue;

                    if (!cont_content.empty()) {
                        folded_buf += ' ';
                        folded_buf += std::string(cont_content);
                        depth += net_paren_delta(cont_content);
                    }
                }
                effective_content = folded_buf;
            }
        }

        // Tokenize the line content (indent already stripped)
        tokenizeLine(effective_content, tokens);

        tokens.push_back({TokenType::Newline, {}});
    }

    // Flush remaining open indent levels
    while (indent_stack.top() > 0) {
        indent_stack.pop();
        tokens.push_back({TokenType::Dedent, {}});
    }

    tokens.push_back({TokenType::End, {}});
    return tokens;
}

} // namespace pce::sdlos::jade
