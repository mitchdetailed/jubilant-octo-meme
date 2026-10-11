// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The canonical JSON writer of 02 section 6.5 rule 1, private to esses_model until esses_base's JsonDoc
// exists (it is the same algorithm JsonDoc::canonical() is specified to implement):
//   * UTF-8 without BOM, LF line ends, two-space indent, one space after ':' and after ',' inside an inline
//     array, trailing newline;
//   * object members sorted by key in UTF-8 code-unit order, except that "$schema" and then "schemaVersion"
//     come first when present (known and unknown members alike);
//   * arrays keep their order; an array is written one element per line when any element is an object (or a
//     multi-line array), otherwise inline: [0, 1250000];
//   * integers without a decimal point; other numbers in shortest round-trip form (std::to_chars), -0 written
//     as 0; a non-finite number is an error (schemas use null for an invalid value);
//   * strings escape '"', '\\' and the control characters (\b \f \n \r \t, else \u00XX); everything else,
//     '/' and non-ASCII included, is written as is; invalid UTF-8 is replaced by U+FFFD.
// The compact form uses the same member order and number and string forms with no white space at all.
#pragma once

#include <esses/base/result.hpp>
#include <esses/model/opaque_json.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace esses::model {

// Constructs OpaqueJson values from inside the library (OpaqueJson's text constructor is private).
struct OpaqueJsonAccess {
    static OpaqueJson make(std::string compactText) { return OpaqueJson(std::move(compactText)); }
};

namespace detail {

// Errors: Invalid ("non-finite number at <pointer>") when a number is NaN or infinite.
Result<std::string> canonicalJson(const nlohmann::json& value);        // pretty, trailing LF
Result<std::string> compactCanonicalJson(const nlohmann::json& value); // no white space, no LF

Result<OpaqueJson> toOpaque(const nlohmann::json& value);
// The JSON value of an OpaqueJson (null for the absent value). The text is always valid, it was produced by
// compactCanonicalJson.
nlohmann::json fromOpaque(const OpaqueJson& value);

// Byte comparison used for member order: "$schema" < "schemaVersion" < every other key in UTF-8 code-unit
// order.
bool canonicalKeyLess(std::string_view a, std::string_view b) noexcept;

} // namespace detail
} // namespace esses::model
