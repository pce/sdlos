#pragma once

#include <cstdint>
#include <string>

namespace pce::sdlos::jade {

enum class TokenType : uint8_t {
    Indent,     // indentation increased — emitted before the child line's tokens
    Dedent,     // indentation decreased — one per level closed
    Newline,
    Tag,
    Class,      // from .card  → value = "card"
    Id,         // from #main  → value = "main"
    AttrKey,    // always followed by AttrValue
    AttrValue,
    Text,
    End,        // always last
};

struct Token {
    TokenType   type  = TokenType::End;
    std::string value;
};

} // namespace pce::sdlos::jade
