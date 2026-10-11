// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "ld/ld_strings.hpp"

#include <esses/base/utf8.hpp>

#include <algorithm>
#include <array>

namespace esses::formats::ld {
namespace {

// Code points of cp1252 bytes 0x80-0x9F; the five undefined bytes (0x81, 0x8D, 0x8F, 0x90, 0x9D) map to the
// C1 control of the same value, as the reference decoder does.
constexpr std::array<char32_t, 32> kCp1252High = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

std::optional<std::uint8_t> cp1252Byte(char32_t cp) noexcept {
    if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) {
        return static_cast<std::uint8_t>(cp);
    }
    for (std::size_t i = 0; i < kCp1252High.size(); ++i) {
        if (kCp1252High[i] == cp) {
            return static_cast<std::uint8_t>(0x80 + i);
        }
    }
    return std::nullopt;
}

std::string_view asChars(std::span<const std::byte> bytes) noexcept {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// Length of the UTF-8 sequence that `lead` starts (2-4), or 0 when it is not a lead byte.
std::size_t utf8SequenceLength(unsigned char lead) noexcept {
    if (lead >= 0xC2 && lead <= 0xDF) {
        return 2;
    }
    if (lead >= 0xE0 && lead <= 0xEF) {
        return 3;
    }
    if (lead >= 0xF0 && lead <= 0xF4) {
        return 4;
    }
    return 0;
}

// Is `tail` (1-3 bytes) the start of a valid UTF-8 sequence that the field's end cut short?
bool isCutSequence(std::string_view tail) noexcept {
    if (tail.empty()) {
        return false;
    }
    const auto lead = static_cast<unsigned char>(tail[0]);
    const std::size_t need = utf8SequenceLength(lead);
    if (need <= tail.size()) {
        return false;
    }
    for (std::size_t i = 1; i < tail.size(); ++i) {
        const auto c = static_cast<unsigned char>(tail[i]);
        // The second byte of E0, ED, F0 and F4 has a narrower range (no overlongs, surrogates or > U+10FFFF).
        unsigned char lo = 0x80;
        unsigned char hi = 0xBF;
        if (i == 1) {
            if (lead == 0xE0) {
                lo = 0xA0;
            } else if (lead == 0xED) {
                hi = 0x9F;
            } else if (lead == 0xF0) {
                lo = 0x90;
            } else if (lead == 0xF4) {
                hi = 0x8F;
            }
        }
        if (c < lo || c > hi) {
            return false;
        }
    }
    return true;
}

void rtrimSpaces(std::string& s) {
    while (!s.empty() && s.back() == ' ') {
        s.pop_back();
    }
}

// For a field that is not valid UTF-8: the length (1-3) of a cut sequence at its end when everything before
// it is valid UTF-8 and the field shows itself as UTF-8, else 0. A lone lead byte after pure ASCII is not
// enough evidence: in cp1252 it is an accented letter (a cp1252 name that fills its field and ends in "é",
// as Esses' own writer can produce), so that case stays cp1252.
std::size_t cutTailLength(std::string_view chars) noexcept {
    for (std::size_t k = 1; k <= 3 && k <= chars.size(); ++k) {
        const std::string_view tail = chars.substr(chars.size() - k);
        const std::string_view head = chars.substr(0, chars.size() - k);
        if (isCutSequence(tail) && utf8::isValid(head) && (k >= 2 || !utf8::isAscii(head))) {
            return k;
        }
    }
    return 0;
}

} // namespace

std::string_view textEncodingName(TextEncoding e) noexcept {
    switch (e) {
    case TextEncoding::Ascii:
        return "ascii";
    case TextEncoding::Utf8:
        return "utf-8";
    case TextEncoding::Cp1252:
        return "cp1252";
    }
    return "ascii";
}

std::string cp1252ToUtf8(std::span<const std::byte> bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (const std::byte b : bytes) {
        const auto v = static_cast<std::uint8_t>(b);
        if (v < 0x80) {
            out.push_back(static_cast<char>(v));
        } else if (v < 0xA0) {
            utf8::append(out, kCp1252High[static_cast<std::size_t>(v - 0x80)]);
        } else {
            utf8::append(out, static_cast<char32_t>(v));
        }
    }
    return out;
}

std::optional<std::string> utf8ToCp1252(std::string_view s) {
    if (!utf8::isValid(s)) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    while (pos < s.size()) {
        const char32_t cp = utf8::decodeNext(s, pos);
        const auto b = cp1252Byte(cp);
        if (!b) {
            return std::nullopt;
        }
        out.push_back(static_cast<char>(*b));
    }
    return out;
}

FieldText decodeField(std::span<const std::byte> field) {
    FieldText out;
    const auto* first = field.data();
    const auto* last = first + field.size();
    const auto* nul = std::find(first, last, std::byte{0});
    out.terminated = nul != last;
    if (out.terminated) {
        out.trailingBytes = std::any_of(nul + 1, last, [](std::byte b) { return b != std::byte{0}; });
    }
    const std::span<const std::byte> body(first, static_cast<std::size_t>(nul - first));
    const std::string_view chars = asChars(body);
    if (utf8::isAscii(chars)) {
        out.text.assign(chars);
        out.encoding = TextEncoding::Ascii;
    } else if (utf8::isValid(chars)) {
        out.text.assign(chars);
        out.encoding = TextEncoding::Utf8;
    } else if (const std::size_t cut = out.terminated ? 0 : cutTailLength(chars); cut != 0) {
        out.text.assign(chars.substr(0, chars.size() - cut));
        out.encoding = TextEncoding::Utf8;
        out.cutSequence = true;
    } else {
        out.text = cp1252ToUtf8(body);
        out.encoding = TextEncoding::Cp1252;
    }
    rtrimSpaces(out.text);
    return out;
}

EncodedText encodeText(std::string_view utf8In, std::size_t maxBytes) {
    EncodedText out;
    const std::string clean = utf8::isValid(utf8In) ? std::string(utf8In) : utf8::sanitize(utf8In);
    std::string encoded;
    if (utf8::isAscii(clean)) {
        encoded = clean;
    } else {
        // cp1252 only when the bytes cannot be read back as UTF-8 (the reader tries UTF-8 first).
        auto cp = utf8ToCp1252(clean);
        if (cp && !utf8::isValid(*cp)) {
            encoded = std::move(*cp);
        } else {
            encoded = clean;
            out.utf8 = true;
        }
    }
    if (encoded.size() > maxBytes) {
        out.truncated = true;
        std::size_t cut = maxBytes;
        if (out.utf8) {
            // Back up to a code-point boundary: never end inside a multi-byte sequence.
            while (cut > 0 && (static_cast<unsigned char>(encoded[cut]) & 0xC0u) == 0x80u) {
                --cut;
            }
        }
        encoded.resize(cut);
    }
    out.bytes.resize(encoded.size());
    std::transform(encoded.begin(), encoded.end(), out.bytes.begin(),
                   [](char c) { return static_cast<std::byte>(static_cast<unsigned char>(c)); });
    return out;
}

} // namespace esses::formats::ld
