// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "spelling.hpp"

#include <esses/base/casefold.hpp>
#include <esses/base/utf8.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace esses::units::spelling {

namespace {

bool isAsciiSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

bool isAsciiLetter(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isAsciiLetter(char32_t c) noexcept {
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z');
}

bool isAsciiDigit(char32_t c) noexcept {
    return c >= U'0' && c <= U'9';
}

bool startsWordAt(std::string_view s, std::size_t i) noexcept {
    return i == 0 || !isAsciiLetter(s[i - 1]);
}

// Code points rewritten before case folding: NFKC folds U+00BA (masculine ordinal, a common stand-in for the
// degree sign) to 'o', and U+02DA (ring above) is another stand-in; both become U+00B0.
std::string preFold(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    while (pos < s.size()) {
        char32_t cp = utf8::decodeNext(s, pos);
        if (cp == 0x00BA || cp == 0x02DA) {
            cp = 0x00B0;
        }
        utf8::append(out, cp);
    }
    return out;
}

// Code-point rules on the folded string.
std::string glyphRules(std::string_view folded) {
    const std::u32string cps = utf8::toUtf32(folded);
    std::string out;
    out.reserve(folded.size() + 8);
    for (std::size_t i = 0; i < cps.size(); ++i) {
        const char32_t c = cps[i];
        const bool nextIsTemperatureLetter = i + 1 < cps.size() &&
                                             (cps[i + 1] == U'c' || cps[i + 1] == U'f') &&
                                             (i + 2 == cps.size() || !isAsciiLetter(cps[i + 2]));
        switch (c) {
        case 0x00B0: // degree sign
            out += "deg";
            break;
        case 0x00F8: // o with stroke: the degree sign of code page 437 read as code page 1252
        case 0x2591: // light shade: the degree sign of code page 1252 read as code page 437
            if (nextIsTemperatureLetter) {
                out += "deg";
            } else {
                utf8::append(out, c);
            }
            break;
        case 0x03BC: // Greek mu (the micro sign folds to it)
            out += 'u';
            break;
        case 0x03C9: // Greek omega (capital omega and the ohm sign fold to it)
            out += "ohm";
            break;
        case 0x03BB: // Greek lambda
            out += "lambda";
            break;
        case U'*':
        case 0x00B7: // middle dot
        case 0x22C5: // dot operator
        case 0x00D7: // multiplication sign
            out += '.';
            break;
        case U'^':
            if (i + 1 < cps.size() &&
                (isAsciiDigit(cps[i + 1]) || cps[i + 1] == U'-' || cps[i + 1] == U'+')) {
                break; // m/s^2 -> m/s2
            }
            out += '^';
            break;
        default:
            utf8::append(out, c);
            break;
        }
    }
    return out;
}

// ASCII word rules: "degrees"/"degree" -> "deg"; "deg" + c|f|k ending a word -> "deg c".
std::string wordRules(std::string_view s) {
    std::string words;
    words.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        if (startsWordAt(s, i) && s.substr(i, 7) == "degrees") {
            words += "deg";
            i += 7;
        } else if (startsWordAt(s, i) && s.substr(i, 6) == "degree") {
            words += "deg";
            i += 6;
        } else {
            words += s[i];
            ++i;
        }
    }
    std::string out;
    out.reserve(words.size() + 2);
    const std::string_view w = words;
    for (std::size_t i = 0; i < w.size();) {
        if (startsWordAt(w, i) && w.substr(i, 3) == "deg" && i + 3 < w.size() &&
            (w[i + 3] == 'c' || w[i + 3] == 'f' || w[i + 3] == 'k') &&
            (i + 4 == w.size() || !isAsciiLetter(w[i + 4]))) {
            out += "deg ";
            out += w[i + 3];
            i += 4;
        } else {
            out += w[i];
            ++i;
        }
    }
    return out;
}

bool hasValidLayout(std::string_view s) noexcept {
    if (s.empty() || s.front() == ' ' || s.back() == ' ') {
        return false;
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
        if (c == ' ' && i + 1 < s.size() && s[i + 1] == ' ') {
            return false;
        }
    }
    return utf8::isValid(s);
}

} // namespace

std::string collapse(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool pendingSpace = false;
    for (const char c : s) {
        if (isAsciiSpace(c)) {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out += ' ';
            pendingSpace = false;
        }
        out += c;
    }
    return out;
}

std::string folded(std::string_view s) {
    return collapse(caseFold(collapse(s)));
}

std::string normalised(std::string_view s) {
    return collapse(wordRules(glyphRules(folded(preFold(s)))));
}

bool isValidSymbol(std::string_view s) noexcept {
    if (s.empty()) {
        return true;
    }
    if (s.front() == ' ' || s.back() == ' ') {
        return false;
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c < ' ' || c > '~') {
            return false;
        }
        if (c == ' ' && s[i + 1] == ' ') {
            return false;
        }
    }
    return true;
}

bool isValidSpelling(std::string_view s) noexcept {
    return hasValidLayout(s);
}

bool isValidName(std::string_view s) noexcept {
    if (s.empty() || isAsciiSpace(s.front()) || isAsciiSpace(s.back())) {
        return false;
    }
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) {
            return false;
        }
    }
    return utf8::isValid(s);
}

bool isValidTag(std::string_view s) noexcept {
    if (s.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!alnum && !(i > 0 && c == '-')) {
            return false;
        }
    }
    return true;
}

} // namespace esses::units::spelling
