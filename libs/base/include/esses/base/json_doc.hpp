// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// JsonDoc (01 section 2.1, 02 section 6.5, 08 section 6.3): an immutable JSON value and the owner of the
// single canonical serialisation of 02 section 6.5 rule 1, used by every document kind, probe_json, fixtures
// and CLI goldens:
//   * UTF-8 without BOM, LF line ends, two-space indent, one space after ':' and after ',' inside an inline
//     array, trailing newline, no trailing white space;
//   * object members sorted by key in UTF-8 code-unit order, except that "$schema" and then "schemaVersion"
//     come first when present (known and unknown members alike, so no schema metadata is needed);
//   * arrays keep their order; an array is written one element per line when any element is an object (or a
//     multi-line array), otherwise inline: [0, 1250000];
//   * integers without a decimal point; other numbers in shortest round-trip form (std::to_chars), -0 written
//     as 0; a non-finite number is an error (schemas use null for an invalid value);
//   * strings escape '"', '\\' and the control characters (\b \f \n \r \t, else \u00XX); everything else, '/'
//     and non-ASCII included, is written as is; invalid UTF-8 is replaced by U+FFFD.
//
// The value is held as its compact canonical text (the same member order and number and string forms with no
// white space at all), so equal values compare equal and no JSON library type appears in this header.
// Libraries that read or build JSON with nlohmann-json convert through <esses/base/json_nlohmann.hpp>
// (jsonio::toDoc / jsonio::fromDoc); model::OpaqueJson is an alias of this type.
#pragma once

#include <esses/base/result.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace esses {

namespace jsonio {
struct DocAccess; // constructs JsonDoc values from already-canonical text (json_doc.cpp)
} // namespace jsonio

class JsonDoc {
public:
    JsonDoc() = default; // the absent value (isNull() == true)

    // Parses any JSON text (a UTF-8 BOM is skipped). Errors: Format with context.offset (byte) on a syntax
    // error or a nesting deeper than kMaxJsonDepth, Invalid when a number is not finite.
    static Result<JsonDoc> parse(std::string_view json);
    // Reads and parses a file. Errors: Io (context.path) when it cannot be read; parse() errors with
    // context.path set.
    static Result<JsonDoc> load(const std::filesystem::path& path);

    bool isNull() const noexcept { return text_.empty() || text_ == "null"; }
    bool isObject() const noexcept { return !text_.empty() && text_.front() == '{'; }
    bool isArray() const noexcept { return !text_.empty() && text_.front() == '['; }
    bool isString() const noexcept { return !text_.empty() && text_.front() == '"'; }

    // Compact canonical text ("" for the absent value).
    const std::string& compact() const noexcept { return text_; }
    // The canonical document form (two-space indent, LF, trailing newline); "null\n" for the absent value.
    std::string canonical() const;
    // canonical() written with AtomicFile (temporary file + flush + rename).
    Result<void> saveAtomic(const std::filesystem::path& path) const;

    // The "$schema" member of an object ("" when absent or not a string) and its "schemaVersion" (0 when
    // absent or not an integer).
    std::string schemaId() const;
    int schemaVersion() const;

    friend bool operator==(const JsonDoc&, const JsonDoc&) = default;

private:
    friend struct jsonio::DocAccess;
    explicit JsonDoc(std::string compactText) : text_(std::move(compactText)) {}
    std::string text_;
};

} // namespace esses
