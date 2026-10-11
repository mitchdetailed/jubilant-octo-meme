// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// UTF-8 helpers. Strings below layer 4 are UTF-8 std::string (D-007). Decoding is strict (no overlong forms,
// no surrogates, nothing above U+10FFFF); each maximal invalid subsequence decodes to U+FFFD.
//
// Paths: on Windows a std::filesystem::path is never built from a narrow std::string (01 section 9); use
// pathFromUtf8()/pathToUtf8(), which convert through UTF-16 without exceptions.
#pragma once

#include <esses/base/config.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace esses::utf8 {

inline constexpr char32_t kReplacement = 0xFFFD; // U+FFFD REPLACEMENT CHARACTER

bool isValid(std::string_view s) noexcept;
// Decodes the code point at s[pos] and advances pos (by at least one byte). Invalid input -> kReplacement.
char32_t decodeNext(std::string_view s, std::size_t& pos) noexcept;
// Appends cp; surrogates and values above U+10FFFF are written as U+FFFD.
void append(std::string& out, char32_t cp);
std::string encode(char32_t cp);
// Replaces every invalid sequence with U+FFFD.
std::string sanitize(std::string_view s);
std::size_t countCodePoints(std::string_view s) noexcept;
bool isAscii(std::string_view s) noexcept;

std::u16string toUtf16(std::string_view s);   // invalid input -> U+FFFD
std::string fromUtf16(std::u16string_view s); // unpaired surrogates -> U+FFFD
std::u32string toUtf32(std::string_view s);
std::string fromUtf32(std::u32string_view s);

#if defined(ESSES_OS_WINDOWS)
std::wstring toWide(std::string_view s);
std::string fromWide(std::wstring_view s);
#endif

} // namespace esses::utf8

namespace esses {

std::filesystem::path pathFromUtf8(std::string_view utf8);
std::string pathToUtf8(const std::filesystem::path& path);

} // namespace esses
