// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Fixed-width .ld string fields (05 section 3, amended 2026-10-10; port specification Q-W09, Q-W19, Q-W20):
// a field is NUL-padded and bounded by its width (a full field has no NUL). Reading cuts at the first NUL,
// decodes UTF-8 when the bytes are valid UTF-8 and cp1252 otherwise, and right-trims spaces. Writing encodes
// cp1252 when every character is representable (and the bytes cannot be mistaken for UTF-8), else UTF-8, and
// truncates on code-point boundaries.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace esses::formats::ld {

enum class TextEncoding : std::uint8_t { Ascii, Utf8, Cp1252 };
std::string_view textEncodingName(TextEncoding e) noexcept; // "ascii", "utf-8", "cp1252"

struct FieldText {
    std::string text; // UTF-8: cut at the first NUL, decoded, right-trimmed of spaces
    TextEncoding encoding = TextEncoding::Ascii;
    bool terminated = true;     // the field holds a NUL
    bool trailingBytes = false; // non-zero bytes after the first NUL (stale buffer)
    bool cutSequence = false;   // a full field ended inside a UTF-8 sequence; the partial tail was dropped
};

// Decodes one fixed-width field: UTF-8 when valid, else cp1252 (05 section 3.4). A full field (no NUL) whose
// only UTF-8 fault is an incomplete sequence in its last 1-3 bytes was cut by a byte-truncating writer (port
// specification Q-W19/Q-W20): it decodes as UTF-8 without the partial tail (cutSequence), provided the field
// shows itself as UTF-8 (a multi-byte sequence before the tail, or a tail of 2-3 bytes).
FieldText decodeField(std::span<const std::byte> field);
// cp1252 -> UTF-8; the five undefined bytes map to the C1 control of the same value.
std::string cp1252ToUtf8(std::span<const std::byte> bytes);
// UTF-8 -> cp1252; nullopt when a code point has no cp1252 byte or the input is not valid UTF-8.
std::optional<std::string> utf8ToCp1252(std::string_view utf8);

struct EncodedText {
    std::vector<std::byte> bytes; // without a NUL
    bool utf8 = false;            // non-ASCII text written as UTF-8 (cp1252 could not represent it)
    bool truncated = false;       // cut to maxBytes on a code-point boundary
};
// Encodes for a field of at most maxBytes bytes (the caller pads with NUL; maxBytes == width leaves a full
// field without a NUL, width - 1 always keeps one).
EncodedText encodeText(std::string_view utf8, std::size_t maxBytes);

} // namespace esses::formats::ld
