// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Small helpers shared by the esses-cli commands and esses-gen-fixtures: whole-file I/O through UTF-8 paths,
// the canonical JSON text of 02 section 6.5 rule 1 (through esses_model's one canonical writer),
// exception-free JSON parsing, and the reversible file-name encoding of channel names used under
// expected.channels/.
#pragma once

#include <esses/base/result.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace esses::cli {

// UTF-8 <-> path without the narrow std::string constructor (01 section 9).
std::string u8(const std::filesystem::path& path);
std::filesystem::path p8(std::string_view utf8);

Result<std::string> readFile(const std::filesystem::path& path);
// Atomic write (temporary file + rename); creates missing parent directories.
Result<void> writeFile(const std::filesystem::path& path, std::string_view bytes);

// Parses JSON text without letting an exception escape. Format with the byte offset on a syntax error.
Result<nlohmann::json> parseJson(std::string_view text);
Result<nlohmann::json> loadJson(const std::filesystem::path& path);

// The canonical document text of a JSON value: "$schema" and "schemaVersion" first, other keys sorted by
// UTF-8 code units, two-space indent, LF, shortest round-trip numbers, trailing LF (02 section 6.5 rule 1).
// Invalid when the value holds a non-finite number.
Result<std::string> canonicalText(const nlohmann::json& value);

// File-name encoding of a channel name: ASCII letters, digits, space and ._-()+,=@[]{}#&!'~ are kept, every
// other byte (path separators, reserved characters, '%', non-ASCII UTF-8 bytes) becomes %XX in upper-case
// hex, and a trailing space or dot is encoded too (Windows drops them). percentDecode() inverts it.
std::string percentEncode(std::string_view utf8);
std::string percentDecode(std::string_view text);

// "a,b,,c" -> {"a","b","c"}
std::vector<std::string> splitList(std::string_view text, char separator = ',');

} // namespace esses::cli
