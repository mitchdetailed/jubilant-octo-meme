// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Lexer (03 section 2.1, D-020): UTF-8 source in the C locale. Tokens: 'Channel' references, [unit] suffixes,
// integer and decimal literals (distinct: integers select points, decimals seconds in window arguments),
// "string" literals, identifiers (dotted for external functions), the case-insensitive keyword operators
// AND OR NOT LT LTE GT GTE EQ NEQ and constants rise/fall, operators and punctuation. Comments (// and /* */)
// are dropped with positions preserved. Lexical errors are reported as diagnostics (03 section 2.5 codes).
#pragma once

#include <esses/maths/diagnostics.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace esses::maths {

enum class TokenKind : std::uint8_t {
    ChannelRef,   // '...'          text = the name between the quotes
    UnitSuffix,   // [...]          text = the unit between the brackets ("" for [])
    IntLit,       // [0-9]+
    DecLit,       // 1.5, .5, 1., 1e3
    StrLit,       // "..."          text = the content
    Ident,        // function names, a.b.c for external functions
    KeywordOp,    // AND OR NOT LT LTE GT GTE EQ NEQ (text upper-cased)
    KeywordConst, // rise fall (text lower-cased)
    Operator,     // + - * / ^ < <= > >= == != ! && || ~ & |
    LParen,
    RParen,
    Comma,
    End // end of input (always the last token)
};

struct Token {
    TokenKind kind = TokenKind::End;
    Span span; // byte offsets into the source
    std::string text;
};

class Lexer {
public:
    explicit Lexer(std::string_view source);

    // The next token; End at (and after) the end of input. Lexical errors are appended to `diags` and the
    // lexer resynchronises after the offending character.
    Token next(Diagnostics& diags);

    // Every token of `source`, ending with End.
    static std::vector<Token> tokenize(std::string_view source, Diagnostics& diags);

private:
    std::string_view source_;
    std::size_t pos_ = 0;
};

} // namespace esses::maths
