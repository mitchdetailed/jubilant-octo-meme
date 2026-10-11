// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/utf8.hpp>

namespace esses::utf8 {
namespace {

constexpr bool isContinuation(unsigned char c) noexcept {
    return (c & 0xC0u) == 0x80u;
}

constexpr bool isSurrogate(char32_t cp) noexcept {
    return cp >= 0xD800u && cp <= 0xDFFFu;
}

} // namespace

char32_t decodeNext(std::string_view s, std::size_t& pos) noexcept {
    if (pos >= s.size()) {
        ++pos; // callers loop while pos < size; never stall
        return kReplacement;
    }
    const auto c0 = static_cast<unsigned char>(s[pos]);
    if (c0 < 0x80u) {
        ++pos;
        return c0;
    }
    std::size_t len = 0;
    char32_t cp = 0;
    char32_t min = 0;
    if (c0 >= 0xC2u && c0 <= 0xDFu) {
        len = 2;
        cp = c0 & 0x1Fu;
        min = 0x80u;
    } else if (c0 >= 0xE0u && c0 <= 0xEFu) {
        len = 3;
        cp = c0 & 0x0Fu;
        min = 0x800u;
    } else if (c0 >= 0xF0u && c0 <= 0xF4u) {
        len = 4;
        cp = c0 & 0x07u;
        min = 0x10000u;
    } else {
        ++pos; // continuation byte without a lead, C0/C1 overlong lead, or F5..FF
        return kReplacement;
    }
    // Consume the maximal valid prefix of the sequence; stop at the first byte that cannot continue it.
    std::size_t i = 1;
    for (; i < len; ++i) {
        if (pos + i >= s.size()) {
            break;
        }
        const auto c = static_cast<unsigned char>(s[pos + i]);
        if (!isContinuation(c)) {
            break;
        }
        // Second-byte restrictions that rule out overlongs, surrogates and > U+10FFFF early.
        if (i == 1) {
            if ((c0 == 0xE0u && c < 0xA0u) || (c0 == 0xEDu && c > 0x9Fu) || (c0 == 0xF0u && c < 0x90u) ||
                (c0 == 0xF4u && c > 0x8Fu)) {
                break;
            }
        }
        cp = (cp << 6) | (c & 0x3Fu);
    }
    if (i < len) {
        pos += i;
        return kReplacement;
    }
    pos += len;
    if (cp < min || isSurrogate(cp) || cp > 0x10FFFFu) {
        return kReplacement;
    }
    return cp;
}

void append(std::string& out, char32_t cp) {
    if (isSurrogate(cp) || cp > 0x10FFFFu) {
        cp = kReplacement;
    }
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

std::string encode(char32_t cp) {
    std::string out;
    append(out, cp);
    return out;
}

bool isAscii(std::string_view s) noexcept {
    for (const char c : s) {
        if (static_cast<unsigned char>(c) >= 0x80u) {
            return false;
        }
    }
    return true;
}

bool isValid(std::string_view s) noexcept {
    std::size_t pos = 0;
    while (pos < s.size()) {
        const auto c0 = static_cast<unsigned char>(s[pos]);
        if (c0 < 0x80u) {
            ++pos;
            continue;
        }
        const std::size_t before = pos;
        const char32_t cp = decodeNext(s, pos);
        if (cp == kReplacement) {
            // A literal U+FFFD (EF BF BD) is valid; anything else that decoded to it was not.
            if (!(pos - before == 3 && s.substr(before, 3) == "\xEF\xBF\xBD")) {
                return false;
            }
        }
    }
    return true;
}

std::string sanitize(std::string_view s) {
    if (isValid(s)) {
        return std::string(s);
    }
    std::string out;
    out.reserve(s.size() + 8);
    std::size_t pos = 0;
    while (pos < s.size()) {
        append(out, decodeNext(s, pos));
    }
    return out;
}

std::size_t countCodePoints(std::string_view s) noexcept {
    std::size_t n = 0;
    std::size_t pos = 0;
    while (pos < s.size()) {
        static_cast<void>(decodeNext(s, pos));
        ++n;
    }
    return n;
}

std::u32string toUtf32(std::string_view s) {
    std::u32string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    while (pos < s.size()) {
        out.push_back(decodeNext(s, pos));
    }
    return out;
}

std::string fromUtf32(std::u32string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char32_t cp : s) {
        append(out, cp);
    }
    return out;
}

std::u16string toUtf16(std::string_view s) {
    std::u16string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    while (pos < s.size()) {
        const char32_t cp = decodeNext(s, pos);
        if (cp >= 0x10000u) {
            const char32_t v = cp - 0x10000u;
            out.push_back(static_cast<char16_t>(0xD800u + (v >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00u + (v & 0x3FFu)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::string fromUtf16(std::u16string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char32_t u = s[i];
        if (u >= 0xD800u && u <= 0xDBFFu && i + 1 < s.size() && s[i + 1] >= 0xDC00u && s[i + 1] <= 0xDFFFu) {
            const char32_t lo = s[i + 1];
            append(out, 0x10000u + ((u - 0xD800u) << 10) + (lo - 0xDC00u));
            ++i;
        } else {
            append(out, isSurrogate(u) ? kReplacement : u);
        }
    }
    return out;
}

#if defined(ESSES_OS_WINDOWS)
static_assert(sizeof(wchar_t) == sizeof(char16_t));

std::wstring toWide(std::string_view s) {
    const std::u16string u = toUtf16(s);
    return {u.begin(), u.end()};
}

std::string fromWide(std::wstring_view s) {
    std::u16string u;
    u.reserve(s.size());
    for (const wchar_t c : s) {
        u.push_back(static_cast<char16_t>(c));
    }
    return fromUtf16(u);
}
#endif

} // namespace esses::utf8

namespace esses {

std::filesystem::path pathFromUtf8(std::string_view utf8) {
#if defined(ESSES_OS_WINDOWS)
    return {utf8::toWide(utf8)};
#else
    return {std::string(utf8)}; // POSIX paths are byte strings; ours are UTF-8
#endif
}

std::string pathToUtf8(const std::filesystem::path& path) {
#if defined(ESSES_OS_WINDOWS)
    return utf8::fromWide(path.native());
#else
    return path.native();
#endif
}

} // namespace esses
