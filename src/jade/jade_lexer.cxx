#include "jade_lexer.hh"

#include <cctype>
#include <stack>

// JadeLite — Lexer implementation
// Char-by-char, no regex, no intermediate allocations beyond the token vector.

namespace pce::sdlos::jade {

namespace {

// ── Character classification ──────────────────────────────────────────────────

static bool isIdentStart(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

static bool isIdentChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
}

// ── Indent counting ───────────────────────────────────────────────────────────
// Tabs count as 4 spaces (common Jade/Pug convention).

static int countIndent(std::string_view line)
{
    int n = 0;
    for (char c : line) {
        if      (c == ' ')  ++n;
        else if (c == '\t') n += 4;
        else                break;
    }
    return n;
}

// ── Attribute list parser ─────────────────────────────────────────────────────
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
                const char q      = s[i++];
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

// ── Single-line tokenizer ─────────────────────────────────────────────────────
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

    // ── Optional tag name ─────────────────────────────────────────────────
    if (i < n && isIdentStart(s[i])) {
        const std::size_t start = i;
        while (i < n && isIdentChar(s[i])) ++i;
        out.push_back({TokenType::Tag, std::string(s.substr(start, i - start))});
        has_tag = true;
    }

    // ── .class and #id shortcuts (may repeat: div.a.b#id.c) ──────────────
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

    // ── Attribute list ────────────────────────────────────────────────────
    if (i < n && s[i] == '(') {
        ++i; // consume '('
        parseAttrs(s, i, out);
    }

    // ── Inline text — everything remaining after optional whitespace ──────
    while (i < n && s[i] == ' ') ++i;
    if (i < n)
        out.push_back({TokenType::Text, std::string(s.substr(i))});
}

} // anonymous namespace

// ── Lexer::tokenize ───────────────────────────────────────────────────────────

std::vector<Token> Lexer::tokenize() const
{
    std::vector<Token> tokens;
    tokens.reserve(64);

    // Indent stack: tracks the column depth of each open indent level.
    // Bottom element is always 0 (root level).
    std::stack<int> indent_stack;
    indent_stack.push(0);

    std::string_view src = source_;

    while (!src.empty()) {
        // ── Split off one line ────────────────────────────────────────────
        const auto       nl_pos = src.find('\n');
        std::string_view line   = (nl_pos == std::string_view::npos)
                                      ? src
                                      : src.substr(0, nl_pos);
        src = (nl_pos == std::string_view::npos)
                  ? std::string_view{}
                  : src.substr(nl_pos + 1);

        // Strip trailing \r (Windows CRLF)
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        // ── Skip blank / whitespace-only lines ───────────────────────────
        {
            bool blank = true;
            for (char c : line)
                if (c != ' ' && c != '\t') { blank = false; break; }
            if (blank) continue;
        }

        const int indent = countIndent(line);

        // ── Skip comment lines before emitting any indent tokens ─────────
        // A comment must not disturb the indent stack or emit Newline.
        {
            const std::string_view content = line.substr(static_cast<std::size_t>(indent));
            if (content.size() >= 2 && content[0] == '/' && content[1] == '/')
                continue;
        }

        // ── Emit Indent / Dedent ──────────────────────────────────────────
        if (indent > indent_stack.top()) {
            indent_stack.push(indent);
            tokens.push_back({TokenType::Indent, {}});
        } else {
            while (indent < indent_stack.top()) {
                indent_stack.pop();
                tokens.push_back({TokenType::Dedent, {}});
            }
            // If indent == top: same level, no token needed.
        }

        // ── Tokenize the line content (after stripping indent) ────────────
        tokenizeLine(line.substr(static_cast<std::size_t>(indent)), tokens);

        tokens.push_back({TokenType::Newline, {}});
    }

    // ── Flush remaining open indent levels ───────────────────────────────
    while (indent_stack.top() > 0) {
        indent_stack.pop();
        tokens.push_back({TokenType::Dedent, {}});
    }

    tokens.push_back({TokenType::End, {}});
    return tokens;
}

} // namespace pce::sdlos::jade
