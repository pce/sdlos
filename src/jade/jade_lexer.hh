#pragma once

// JadeLite lexer — no regex, no SDL dependency.
// See architecture.md § JadeLite Parser for the token stream contract.

#include "jade_token.hh"

#include <string_view>
#include <vector>

namespace pce::sdlos::jade {

class Lexer {
public:
    explicit Lexer(std::string_view source) noexcept : source_(source) {}

    // Returns a token stream always ending with exactly one End token.
    // Blank lines and //-comment lines produce no tokens (not even Newline).
    [[nodiscard]] std::vector<Token> tokenize() const;

private:
    std::string_view source_;
};

} // namespace pce::sdlos::jade
