// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// NFKC-lite + full case folding; the coverage is documented in casefold.hpp. Tables are written from the
// Unicode Character Database (UnicodeData.txt decomposition mappings, CaseFolding.txt statuses C and F).
#include <esses/base/casefold.hpp>
#include <esses/base/utf8.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>

namespace esses {
namespace {

struct Single {
    char32_t from;
    char32_t to;
};

// Compatibility (and canonical singleton) mappings to a single code point. Sorted by `from`.
constexpr Single kCompatSingles[] = {
    {0x00A0, 0x0020}, {0x00AA, 0x0061}, {0x00B2, 0x0032}, {0x00B3, 0x0033}, {0x00B5, 0x03BC},
    {0x00B9, 0x0031}, {0x00BA, 0x006F}, {0x017F, 0x0073}, {0x03D0, 0x03B2}, {0x03D1, 0x03B8},
    {0x03D2, 0x03A5}, {0x03D5, 0x03C6}, {0x03D6, 0x03C0}, {0x03F0, 0x03BA}, {0x03F1, 0x03C1},
    {0x03F2, 0x03C2}, {0x03F4, 0x0398}, {0x03F5, 0x03B5}, {0x03F9, 0x03A3}, {0x2024, 0x002E},
    {0x202F, 0x0020}, {0x205F, 0x0020}, {0x2070, 0x0030}, {0x2071, 0x0069}, {0x207A, 0x002B},
    {0x207B, 0x2212}, {0x207C, 0x003D}, {0x207D, 0x0028}, {0x207E, 0x0029}, {0x207F, 0x006E},
    {0x208A, 0x002B}, {0x208B, 0x2212}, {0x208C, 0x003D}, {0x208D, 0x0028}, {0x208E, 0x0029},
    {0x2126, 0x03A9}, {0x212A, 0x004B}, {0x212B, 0x00C5}, {0x3000, 0x0020},
};

struct Multi {
    char32_t from;
    std::array<char32_t, 3> to; // zero-terminated when shorter
};

// Compatibility mappings to several code points. Sorted by `from`.
constexpr Multi kCompatMultis[] = {
    {0x00BC, {0x0031, 0x2044, 0x0034}}, {0x00BD, {0x0031, 0x2044, 0x0032}},
    {0x00BE, {0x0033, 0x2044, 0x0034}}, {0x0132, {0x0049, 0x004A, 0}},
    {0x0133, {0x0069, 0x006A, 0}},      {0x013F, {0x004C, 0x00B7, 0}},
    {0x0140, {0x006C, 0x00B7, 0}},      {0x0149, {0x02BC, 0x006E, 0}},
    {0x01C4, {0x0044, 0x017D, 0}},      {0x01C5, {0x0044, 0x017E, 0}},
    {0x01C6, {0x0064, 0x017E, 0}},      {0x01C7, {0x004C, 0x004A, 0}},
    {0x01C8, {0x004C, 0x006A, 0}},      {0x01C9, {0x006C, 0x006A, 0}},
    {0x01CA, {0x004E, 0x004A, 0}},      {0x01CB, {0x004E, 0x006A, 0}},
    {0x01CC, {0x006E, 0x006A, 0}},      {0x01F1, {0x0044, 0x005A, 0}},
    {0x01F2, {0x0044, 0x007A, 0}},      {0x01F3, {0x0064, 0x007A, 0}},
    {0x2025, {0x002E, 0x002E, 0}},      {0x2026, {0x002E, 0x002E, 0x002E}},
    {0x2103, {0x00B0, 0x0043, 0}},      {0x2109, {0x00B0, 0x0046, 0}},
    {0x2116, {0x004E, 0x006F, 0}},      {0x2122, {0x0054, 0x004D, 0}},
    {0xFB00, {0x0066, 0x0066, 0}},      {0xFB01, {0x0066, 0x0069, 0}},
    {0xFB02, {0x0066, 0x006C, 0}},      {0xFB03, {0x0066, 0x0066, 0x0069}},
    {0xFB04, {0x0066, 0x0066, 0x006C}}, {0xFB05, {0x0073, 0x0074, 0}},
    {0xFB06, {0x0073, 0x0074, 0}},
};

// Simple (one-to-one) case foldings that do not follow a range rule. Sorted by `from`.
constexpr Single kFoldSingles[] = {
    {0x00B5, 0x03BC}, {0x0178, 0x00FF}, {0x017F, 0x0073}, {0x0181, 0x0253}, {0x0182, 0x0183},
    {0x0184, 0x0185}, {0x0186, 0x0254}, {0x0187, 0x0188}, {0x0189, 0x0256}, {0x018A, 0x0257},
    {0x018B, 0x018C}, {0x018E, 0x01DD}, {0x018F, 0x0259}, {0x0190, 0x025B}, {0x0191, 0x0192},
    {0x0193, 0x0260}, {0x0194, 0x0263}, {0x0196, 0x0269}, {0x0197, 0x0268}, {0x0198, 0x0199},
    {0x019C, 0x026F}, {0x019D, 0x0272}, {0x019F, 0x0275}, {0x01A6, 0x0280}, {0x01A7, 0x01A8},
    {0x01A9, 0x0283}, {0x01AC, 0x01AD}, {0x01AE, 0x0288}, {0x01AF, 0x01B0}, {0x01B1, 0x028A},
    {0x01B2, 0x028B}, {0x01B3, 0x01B4}, {0x01B5, 0x01B6}, {0x01B7, 0x0292}, {0x01B8, 0x01B9},
    {0x01BC, 0x01BD}, {0x01C4, 0x01C6}, {0x01C5, 0x01C6}, {0x01C7, 0x01C9}, {0x01C8, 0x01C9},
    {0x01CA, 0x01CC}, {0x01CB, 0x01CC}, {0x01F1, 0x01F3}, {0x01F2, 0x01F3}, {0x01F4, 0x01F5},
    {0x01F6, 0x0195}, {0x01F7, 0x01BF}, {0x0220, 0x019E}, {0x023A, 0x2C65}, {0x023B, 0x023C},
    {0x023D, 0x019A}, {0x023E, 0x2C66}, {0x0241, 0x0242}, {0x0243, 0x0180}, {0x0244, 0x0289},
    {0x0245, 0x028C}, {0x0345, 0x03B9}, {0x0370, 0x0371}, {0x0372, 0x0373}, {0x0376, 0x0377},
    {0x037F, 0x03F3}, {0x0386, 0x03AC}, {0x0388, 0x03AD}, {0x0389, 0x03AE}, {0x038A, 0x03AF},
    {0x038C, 0x03CC}, {0x038E, 0x03CD}, {0x038F, 0x03CE}, {0x03C2, 0x03C3}, {0x03CF, 0x03D7},
    {0x03D0, 0x03B2}, {0x03D1, 0x03B8}, {0x03D5, 0x03C6}, {0x03D6, 0x03C0}, {0x03F0, 0x03BA},
    {0x03F1, 0x03C1}, {0x03F4, 0x03B8}, {0x03F5, 0x03B5}, {0x03F7, 0x03F8}, {0x03F9, 0x03F2},
    {0x03FA, 0x03FB}, {0x03FD, 0x037B}, {0x03FE, 0x037C}, {0x03FF, 0x037D}, {0x04C0, 0x04CF},
    {0x10C7, 0x2D27}, {0x10CD, 0x2D2D}, {0x1E9B, 0x1E61}, {0x2126, 0x03C9}, {0x212A, 0x006B},
    {0x212B, 0x00E5}, {0x2132, 0x214E}, {0x2183, 0x2184},
};

enum class RangeRule : std::uint8_t {
    Offset, // every code point in the range folds to cp + delta
    EvenUp, // even code points are upper case and fold to cp + 1
    OddUp   // odd code points are upper case and fold to cp + 1
};

struct FoldRange {
    char32_t first;
    char32_t last;
    RangeRule rule;
    std::int32_t delta;
};

constexpr FoldRange kFoldRanges[] = {
    {0x0041, 0x005A, RangeRule::Offset, 32},                                             // Basic Latin
    {0x00C0, 0x00D6, RangeRule::Offset, 32},                                             // Latin-1
    {0x00D8, 0x00DE, RangeRule::Offset, 32},     {0x0100, 0x012F, RangeRule::EvenUp, 1}, // Latin Extended-A
    {0x0132, 0x0137, RangeRule::EvenUp, 1},      {0x0139, 0x0148, RangeRule::OddUp, 1},
    {0x014A, 0x0177, RangeRule::EvenUp, 1},      {0x0179, 0x017E, RangeRule::OddUp, 1},
    {0x01A0, 0x01A5, RangeRule::EvenUp, 1}, // Latin Extended-B, regular pairs
    {0x01CD, 0x01DC, RangeRule::OddUp, 1},       {0x01DE, 0x01EF, RangeRule::EvenUp, 1},
    {0x01F8, 0x021F, RangeRule::EvenUp, 1},      {0x0222, 0x0233, RangeRule::EvenUp, 1},
    {0x0246, 0x024F, RangeRule::EvenUp, 1},      {0x0391, 0x03A1, RangeRule::Offset, 32}, // Greek and Coptic
    {0x03A3, 0x03AB, RangeRule::Offset, 32},     {0x03D8, 0x03EF, RangeRule::EvenUp, 1},
    {0x0400, 0x040F, RangeRule::Offset, 80}, // Cyrillic
    {0x0410, 0x042F, RangeRule::Offset, 32},     {0x0460, 0x0481, RangeRule::EvenUp, 1},
    {0x048A, 0x04BF, RangeRule::EvenUp, 1},      {0x04C1, 0x04CE, RangeRule::OddUp, 1},
    {0x04D0, 0x052F, RangeRule::EvenUp, 1},      // Cyrillic and Cyrillic Supplement
    {0x0531, 0x0556, RangeRule::Offset, 48},     // Armenian
    {0x10A0, 0x10C5, RangeRule::Offset, 0x1C60}, // Georgian
    {0x1E00, 0x1E95, RangeRule::EvenUp, 1},      // Latin Extended Additional
    {0x1EA0, 0x1EFF, RangeRule::EvenUp, 1},
};

constexpr auto kByFrom = [](const auto& a, const auto& b) { return a.from < b.from; };
static_assert(std::is_sorted(std::begin(kCompatSingles), std::end(kCompatSingles), kByFrom));
static_assert(std::is_sorted(std::begin(kCompatMultis), std::end(kCompatMultis), kByFrom));
static_assert(std::is_sorted(std::begin(kFoldSingles), std::end(kFoldSingles), kByFrom));

template <class Table> const auto* findSorted(const Table& table, char32_t cp) noexcept {
    const auto* it = std::lower_bound(std::begin(table), std::end(table), cp,
                                      [](const auto& entry, char32_t value) { return entry.from < value; });
    return (it != std::end(table) && it->from == cp) ? it : nullptr;
}

// Appends the NFKC-lite mapping of cp; returns false when cp has none.
bool appendCompat(char32_t cp, std::u32string& out) {
    if (cp < 0xA0) {
        return false;
    }
    if (cp >= 0x2000 && cp <= 0x200A) { // en quad ... hair space
        out.push_back(0x0020);
        return true;
    }
    if (cp >= 0x2074 && cp <= 0x2079) { // superscript 4-9
        out.push_back(cp - 0x2074 + 0x0034);
        return true;
    }
    if (cp >= 0x2080 && cp <= 0x2089) { // subscript 0-9
        out.push_back(cp - 0x2080 + 0x0030);
        return true;
    }
    if (cp >= 0xFF01 && cp <= 0xFF5E) { // fullwidth ASCII
        out.push_back(cp - 0xFF01 + 0x0021);
        return true;
    }
    if (const auto* s = findSorted(kCompatSingles, cp)) {
        out.push_back(s->to);
        return true;
    }
    if (const auto* m = findSorted(kCompatMultis, cp)) {
        for (const char32_t c : m->to) {
            if (c != 0) {
                out.push_back(c);
            }
        }
        return true;
    }
    return false;
}

void appendFolded(char32_t cp, std::string& out) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp >= 'A' && cp <= 'Z' ? cp + 32 : cp));
        return;
    }
    switch (cp) { // full foldings (status F)
    case 0x00DF:
    case 0x1E9E:
        out += "ss";
        return;
    case 0x0130:
        out += "i\xCC\x87"; // i + U+0307
        return;
    case 0x01F0:
        out += "j\xCC\x8C"; // j + U+030C
        return;
    case 0x0390:
        utf8::append(out, 0x03B9);
        utf8::append(out, 0x0308);
        utf8::append(out, 0x0301);
        return;
    case 0x03B0:
        utf8::append(out, 0x03C5);
        utf8::append(out, 0x0308);
        utf8::append(out, 0x0301);
        return;
    case 0x0587:
        utf8::append(out, 0x0565);
        utf8::append(out, 0x0582);
        return;
    case 0x1E96:
        out += "h";
        utf8::append(out, 0x0331);
        return;
    case 0x1E97:
        out += "t";
        utf8::append(out, 0x0308);
        return;
    case 0x1E98:
        out += "w";
        utf8::append(out, 0x030A);
        return;
    case 0x1E99:
        out += "y";
        utf8::append(out, 0x030A);
        return;
    case 0x1E9A:
        out += "a";
        utf8::append(out, 0x02BE);
        return;
    default:
        break;
    }
    if (const auto* s = findSorted(kFoldSingles, cp)) {
        utf8::append(out, s->to);
        return;
    }
    for (const FoldRange& r : kFoldRanges) {
        if (cp < r.first || cp > r.last) {
            continue;
        }
        switch (r.rule) {
        case RangeRule::Offset:
            utf8::append(out, static_cast<char32_t>(static_cast<std::int32_t>(cp) + r.delta));
            return;
        case RangeRule::EvenUp:
            utf8::append(out, (cp % 2u == 0u) ? cp + 1u : cp);
            return;
        case RangeRule::OddUp:
            utf8::append(out, (cp % 2u == 1u) ? cp + 1u : cp);
            return;
        }
    }
    utf8::append(out, cp);
}

char asciiLower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

} // namespace

std::string caseFold(std::string_view text) {
    if (utf8::isAscii(text)) {
        std::string out(text);
        std::transform(out.begin(), out.end(), out.begin(), asciiLower);
        return out;
    }
    std::string out;
    out.reserve(text.size() + 4);
    std::u32string compat;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const char32_t cp = utf8::decodeNext(text, pos);
        compat.clear();
        if (appendCompat(cp, compat)) {
            for (const char32_t c : compat) {
                appendFolded(c, out);
            }
        } else {
            appendFolded(cp, out);
        }
    }
    return out;
}

bool ciEquals(std::string_view a, std::string_view b) {
    if (utf8::isAscii(a) && utf8::isAscii(b)) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return asciiLower(x) == asciiLower(y);
               });
    }
    return caseFold(a) == caseFold(b);
}

} // namespace esses
