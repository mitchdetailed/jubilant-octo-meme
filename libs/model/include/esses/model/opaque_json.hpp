// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// OpaqueJson: a JSON value the model carries without interpreting it: a sidecar layer's localMaths module
// (maths-1, validated by esses_project), the reserved runs/stages members, unknown top-level members of a
// document (02 section 6.1 Document<T>::unknown) and the source's probe_json.
//
// It stands in for esses_base's JsonDoc (01 section 2.1), which is still a reserved name in M0: the value is
// held as its compact canonical text (object members sorted by UTF-8 code units with "$schema" and
// "schemaVersion" first, no white space, shortest round-trip numbers), so two equal values compare equal and
// no JSON library type crosses the esses_model boundary. When JsonDoc lands, members of this type switch to
// it.
#pragma once

#include <esses/base/result.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace esses::model {

class OpaqueJson {
public:
    OpaqueJson() = default; // the absent value (isNull() == true)

    // Parses any JSON text. Errors: Format with context.offset (byte) on a syntax error, Invalid when a
    // number is not finite.
    static Result<OpaqueJson> parse(std::string_view json);

    bool isNull() const noexcept { return text_.empty() || text_ == "null"; }
    bool isObject() const noexcept { return !text_.empty() && text_.front() == '{'; }
    bool isArray() const noexcept { return !text_.empty() && text_.front() == '['; }

    // Compact canonical text ("" for the absent value).
    const std::string& compact() const noexcept { return text_; }
    // The canonical document form of 02 section 6.5 rule 1 (two-space indent, LF, trailing newline);
    // "null\n" for the absent value.
    std::string canonical() const;

    friend bool operator==(const OpaqueJson&, const OpaqueJson&) = default;

private:
    friend struct OpaqueJsonAccess;
    explicit OpaqueJson(std::string compactText) : text_(std::move(compactText)) {}
    std::string text_;
};

} // namespace esses::model
