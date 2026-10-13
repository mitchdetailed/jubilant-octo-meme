// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Shared reading and writing of the esses_project documents (private): the "$schema"/"schemaVersion" header
// of 02 section 6.6, unknown top-level members (02 section 6.1 Document<T>::unknown), canonical text (02
// section 6.5 rule 1, esses_base's writer) and file access. Violations are thrown as jsonio::JsonViolation
// and turned into Results by parseDocument().
#pragma once

#include <esses/base/json_doc.hpp>
#include <esses/base/json_nlohmann.hpp>
#include <esses/base/result.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace esses::project::detail {

using nlohmann::json;

// Reads "$schema" and "schemaVersion" of the root object (marking both as read): another schema is Invalid,
// another major Unsupported; a missing "$schema" is read as <name>-1 and logged (02 section 6.6 rule 4), a
// schemaVersion above 1 is logged (no migrations before M2; unknown members are preserved).
void readSchemaHeader(jsonio::ObjectReader& r, std::string_view name, std::vector<std::string>& log);
// {"$schema": "urn:esses:<name>-1", "schemaVersion": 1}.
json newDocument(std::string_view name);

// The members `r` did not read, as a JsonDoc (null when there are none).
JsonDoc unknownMembers(const jsonio::ObjectReader& r);
// Adds the members of `unknown` (an object or null) to `root`.
void putUnknown(json& root, const JsonDoc& unknown);
// A JsonDoc of `j`; throws Invalid at `pointer` when it cannot be represented.
JsonDoc docOrViolate(const json& j, const std::string& pointer);
// `j` as an object JsonDoc; throws Invalid at `pointer` when it is not an object.
JsonDoc objectDoc(const json& j, const std::string& pointer);

// Typed helpers on top of jsonio's readers.
std::string optString(jsonio::ObjectReader& r, std::string_view key, std::string fallback = {});
bool optBool(jsonio::ObjectReader& r, std::string_view key, bool fallback);
std::vector<std::string> stringArray(const json& j, const std::string& pointer);
std::vector<std::string> optStringArray(jsonio::ObjectReader& r, std::string_view key);
// A string from a fixed vocabulary; throws Invalid naming the allowed values otherwise.
std::string enumString(const json& j, const std::string& pointer,
                       std::initializer_list<std::string_view> allowed);

// Lower-case identifiers: segments [a-z][a-z0-9<extra>]* separated by '.', between minSegments and
// maxSegments of them ("data.toggle_domain" with '_', 2..; "circuit.beacon-pair" with '-', 2..;
// "time_distance" with '_', 1..1).
bool isLowerId(std::string_view s, char extra, int minSegments, int maxSegments = 1000) noexcept;

// Parses `text` and runs `fn(root)` under jsonio::guardJson.
template <class T, class F> Result<T> parseDocument(std::string_view text, F&& fn) {
    auto parsed = jsonio::parseJson(text);
    if (!parsed) {
        return Failure{parsed.error()};
    }
    const json& root = *parsed;
    return jsonio::guardJson([&]() -> Result<T> { return fn(root); });
}

// Reads `path` and runs `parse(text)`; errors carry context.path.
template <class T, class F> Result<T> loadDocument(const std::filesystem::path& path, F&& parse) {
    auto text = jsonio::readFile(path);
    if (!text) {
        return Failure{text.error()};
    }
    Result<T> doc = parse(*text);
    if (!doc) {
        Error e = doc.error();
        e.context.path = path;
        return Failure{std::move(e)};
    }
    return doc;
}

// The canonical document text of `root`, "" when it holds a non-finite number.
std::string canonicalOrEmpty(const json& root);
// canonicalJson(root) written atomically to `path`.
Result<void> saveCanonical(const json& root, const std::filesystem::path& path);
// The file name of `path` as UTF-8.
std::string fileNameUtf8(const std::filesystem::path& path);

} // namespace esses::project::detail
