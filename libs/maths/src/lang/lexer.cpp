// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Lexer (03 section 2.1, D-020). One pass over the UTF-8 source in the C locale; every lexical error is a
// diagnostic and the lexer carries on after the offending text, so the parser sees a usable token stream and
// the editor gets every error at once. Readings recorded in docs/maths-compat.md: a quoted name runs to the
// next quote of its kind (newlines included, no escapes), a number directly followed by a letter is malformed
// (`2x`, `1e`), the keyword operators are recognised in any letter case, only undotted identifiers can be
// keywords, and non-ASCII characters are accepted only inside quotes and brackets.

#include "codes.hpp"
#include "lang_util.hpp"

#include <esses/maths/lexer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace esses::maths {

namespace {

using lang::isDigit;
using lang::isIdentChar;
using lang::isIdentStart;
using lang::makeDiag;
using lang::spanOf;

constexpr std::array<std::string_view, 9> kKeywordOps{"AND", "OR",  "NOT", "LT", "LTE",
                                                      "GT",  "GTE", "EQ",  "NEQ"};

bool isBlank(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// Length of the UTF-8 sequence that starts with `lead` (1 for ASCII and for stray continuation bytes).
std::size_t utf8Length(unsigned char lead) noexcept {
    if (lead >= 0xF0 && lead <= 0xF7) {
        return 4;
    }
    if (lead >= 0xE0) {
        return lead <= 0xEF ? 3 : 1;
    }
    if (lead >= 0xC2) {
        return 2;
    }
    return 1;
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && isBlank(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && isBlank(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

Lexer::Lexer(std::string_view source) : source_(source) {
    // A UTF-8 byte order mark at the start is not part of the expression.
    if (source_.size() >= 3 && static_cast<unsigned char>(source_[0]) == 0xEF &&
        static_cast<unsigned char>(source_[1]) == 0xBB && static_cast<unsigned char>(source_[2]) == 0xBF) {
        pos_ = 3;
    }
}

Token Lexer::next(Diagnostics& diags) {
    const std::size_t size = source_.size();
    for (;;) {
        // Blanks and comments.
        while (pos_ < size) {
            const char c = source_[pos_];
            if (isBlank(c)) {
                ++pos_;
            } else if (c == '/' && pos_ + 1 < size && source_[pos_ + 1] == '/') {
                while (pos_ < size && source_[pos_] != '\n') {
                    ++pos_;
                }
            } else if (c == '/' && pos_ + 1 < size && source_[pos_ + 1] == '*') {
                const std::size_t close = source_.find("*/", pos_ + 2);
                if (close == std::string_view::npos) {
                    diags.push_back(makeDiag(codes::kCommentUnclosed, Diagnostic::Error,
                                             spanOf(pos_, pos_ + 2), "comment '/*' is never closed",
                                             "end the comment with '*/'"));
                    pos_ = size;
                } else {
                    pos_ = close + 2;
                }
            } else {
                break;
            }
        }

        Token tok;
        const std::size_t start = pos_;
        if (pos_ >= size) {
            tok.kind = TokenKind::End;
            tok.span = spanOf(size, size);
            return tok;
        }
        const char c = source_[pos_];

        // 'Channel name' and "text": no escapes; an unclosed quote takes the rest of the line.
        if (c == '\'' || c == '"') {
            const std::size_t close = source_.find(c, pos_ + 1);
            tok.kind = c == '\'' ? TokenKind::ChannelRef : TokenKind::StrLit;
            if (close == std::string_view::npos) {
                std::size_t eol = source_.find('\n', pos_ + 1);
                if (eol == std::string_view::npos) {
                    eol = size;
                }
                tok.text = std::string(trim(source_.substr(pos_ + 1, eol - pos_ - 1)));
                pos_ = eol;
                diags.push_back(makeDiag(
                    codes::kQuoteMissing, Diagnostic::Error, spanOf(start, pos_),
                    c == '\'' ? "channel name has no closing quote" : "text has no closing double quote",
                    c == '\'' ? "write the name as 'Name'" : "write the text as \"text\""));
            } else {
                tok.text = std::string(source_.substr(pos_ + 1, close - pos_ - 1));
                pos_ = close + 1;
            }
            tok.span = spanOf(start, pos_);
            return tok;
        }

        // [unit]: the content between the brackets, blanks trimmed; the parser checks where it may stand.
        if (c == '[') {
            std::size_t close = pos_ + 1;
            while (close < size && source_[close] != ']' && source_[close] != '\n' && source_[close] != '[') {
                ++close;
            }
            tok.kind = TokenKind::UnitSuffix;
            if (close >= size || source_[close] != ']') {
                tok.text = std::string(trim(source_.substr(pos_ + 1, close - pos_ - 1)));
                pos_ = close;
                diags.push_back(makeDiag(codes::kUnitBracket, Diagnostic::Error, spanOf(start, pos_),
                                         "unit '[' has no closing ']'", "write the unit as [km/h]"));
            } else {
                tok.text = std::string(trim(source_.substr(pos_ + 1, close - pos_ - 1)));
                pos_ = close + 1;
            }
            tok.span = spanOf(start, pos_);
            return tok;
        }
        if (c == ']') {
            ++pos_;
            diags.push_back(makeDiag(codes::kUnitBracket, Diagnostic::Error, spanOf(start, pos_),
                                     "']' without a matching '['",
                                     "a unit is written [km/h] after a channel"));
            continue;
        }

        // Numbers: [0-9]+ is an integer; a decimal point or an exponent makes it decimal.
        if (isDigit(c) || (c == '.' && pos_ + 1 < size && isDigit(source_[pos_ + 1]))) {
            bool decimal = false;
            while (pos_ < size && isDigit(source_[pos_])) {
                ++pos_;
            }
            if (pos_ < size && source_[pos_] == '.') {
                decimal = true;
                ++pos_;
                while (pos_ < size && isDigit(source_[pos_])) {
                    ++pos_;
                }
            }
            if (pos_ < size && (source_[pos_] == 'e' || source_[pos_] == 'E')) {
                std::size_t p = pos_ + 1;
                if (p < size && (source_[p] == '+' || source_[p] == '-')) {
                    ++p;
                }
                if (p < size && isDigit(source_[p])) {
                    decimal = true;
                    pos_ = p;
                    while (pos_ < size && isDigit(source_[pos_])) {
                        ++pos_;
                    }
                }
            }
            tok.kind = decimal ? TokenKind::DecLit : TokenKind::IntLit;
            tok.text = std::string(source_.substr(start, pos_ - start));
            if (pos_ < size && (isIdentChar(source_[pos_]) || source_[pos_] == '.')) {
                // `2x`, `1e`, `1.5.2`: malformed; the number keeps its leading part for recovery.
                const std::size_t numEnd = pos_;
                while (pos_ < size && (isIdentChar(source_[pos_]) || source_[pos_] == '.')) {
                    ++pos_;
                }
                diags.push_back(
                    makeDiag(codes::kNumberFormat, Diagnostic::Error, spanOf(start, pos_),
                             "malformed number '" + std::string(source_.substr(start, pos_ - start)) + "'",
                             "numbers are written 12, 0.5, .5, 1e3 or 2.5e-3"));
                tok.span = spanOf(start, numEnd);
                return tok;
            }
            tok.span = spanOf(start, pos_);
            return tok;
        }

        // Identifiers (dotted for external functions) and the keyword operators and constants.
        if (isIdentStart(c)) {
            ++pos_;
            for (;;) {
                while (pos_ < size && isIdentChar(source_[pos_])) {
                    ++pos_;
                }
                if (pos_ + 1 < size && source_[pos_] == '.' && isIdentStart(source_[pos_ + 1])) {
                    pos_ += 2;
                    continue;
                }
                break;
            }
            tok.text = std::string(source_.substr(start, pos_ - start));
            tok.span = spanOf(start, pos_);
            tok.kind = TokenKind::Ident;
            if (tok.text.find('.') == std::string::npos) {
                const std::string upper = lang::toUpperAscii(tok.text);
                for (const std::string_view k : kKeywordOps) {
                    if (upper == k) {
                        tok.kind = TokenKind::KeywordOp;
                        tok.text = upper;
                        break;
                    }
                }
                const std::string lower = lang::toLowerAscii(tok.text);
                if (lower == "rise" || lower == "fall") {
                    tok.kind = TokenKind::KeywordConst;
                    tok.text = lower;
                }
            }
            return tok;
        }

        // Operators and punctuation.
        const char n1 = pos_ + 1 < size ? source_[pos_ + 1] : '\0';
        const auto op = [&](std::size_t len, TokenKind kind) {
            tok.kind = kind;
            tok.text = std::string(source_.substr(pos_, len));
            pos_ += len;
            tok.span = spanOf(start, pos_);
            return tok;
        };
        switch (c) {
        case '(':
            return op(1, TokenKind::LParen);
        case ')':
            return op(1, TokenKind::RParen);
        case ',':
            return op(1, TokenKind::Comma);
        case '+':
        case '-':
        case '*':
        case '/':
        case '^':
        case '~':
            return op(1, TokenKind::Operator);
        case '<':
        case '>':
        case '!':
            return op(n1 == '=' ? 2 : 1, TokenKind::Operator);
        case '&':
            return op(n1 == '&' ? 2 : 1, TokenKind::Operator);
        case '|':
            return op(n1 == '|' ? 2 : 1, TokenKind::Operator);
        case '=':
            if (n1 == '=') {
                return op(2, TokenKind::Operator);
            }
            ++pos_;
            diags.push_back(makeDiag(codes::kUnexpectedChar, Diagnostic::Error, spanOf(start, pos_),
                                     "'=' is not an operator", "compare with '=='"));
            continue;
        default:
            break;
        }

        // Anything else: one diagnostic per character (a whole UTF-8 sequence), then carry on.
        const auto lead = static_cast<unsigned char>(c);
        const std::size_t len = std::min(utf8Length(lead), size - pos_);
        const std::string_view seq = source_.substr(pos_, len);
        pos_ += len;
        std::string hint;
        if (seq == "\xE2\x80\x98" || seq == "\xE2\x80\x99") {
            hint = "use the straight quote ' around channel names";
        } else if (seq == "\xE2\x80\x9C" || seq == "\xE2\x80\x9D") {
            hint = "use the straight double quote \" around text";
        } else if (seq == "\xC2\xA0") {
            hint = "a non-breaking space; use an ordinary space";
        } else if (c == '.') {
            hint = "a decimal number needs a digit after the point";
        }
        std::string shown =
            lead < 0x20 || lead == 0x7F ? std::string("control character") : "'" + std::string(seq) + "'";
        diags.push_back(makeDiag(codes::kUnexpectedChar, Diagnostic::Error, spanOf(start, pos_),
                                 "unexpected " + shown, std::move(hint)));
    }
}

std::vector<Token> Lexer::tokenize(std::string_view source, Diagnostics& diags) {
    Lexer lexer(source);
    std::vector<Token> out;
    for (;;) {
        out.push_back(lexer.next(diags));
        if (out.back().kind == TokenKind::End) {
            return out;
        }
    }
}

} // namespace esses::maths
