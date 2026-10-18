// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Shared reading and writing of the esses_project documents (private): the load pipeline of 02 section 6.6
// rule 2 (header, migrations, typed reading, SchemaRegistry validation; readDocument()), unknown top-level
// members (02 section 6.1 Document<T>::unknown), canonical text (02 section 6.5 rule 1, esses_base's writer)
// and file access. Violations are thrown as jsonio::JsonViolation and turned into Results by readDocument()
// and parseDocument().
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

// A document kind of this library: its schema name ("project", major 1) and the newest minor this build reads
// and writes (02 section 6.6 rule 1).
struct DocKind {
    std::string_view name;
    int current = 1;
    std::string id() const { return std::string(name) + "-1"; } // "project-1"
};

// The header outcome of prepareDocument().
struct DocVersion {
    int read = 1;          // "schemaVersion" as found (1 when absent)
    int write = 1;         // the minor a save writes: max(read, current); a newer minor is kept (R-38)
    bool migrated = false; // a registered migration ran (the document is dirty, 02 section 6.6 rule 2)
};

// Steps 1-3 of 02 section 6.6 rule 2 on a parsed root: an object; "$schema" of `kind` (another schema
// Invalid, another major Unsupported; absent: read as <name>-1 and logged, rule 4); "schemaVersion" an
// integer >= 1 (absent: 1, logged); then Migrations::instance() from that minor up to kind.current, which
// replaces `root` and appends one log line per step. A newer minor is logged and read as it is. Throws
// JsonViolation.
DocVersion prepareDocument(json& root, const DocKind& kind, std::vector<std::string>& log);
// Validates `doc` against <name>-1 through SchemaRegistry::instance() when the registry has that schema (a
// build without a schemas directory leaves the typed reader as the only check). Throws Invalid at the JSON
// pointer of the first error, the message listing every error.
void validateDocument(const json& doc, const DocKind& kind);
// The error of a document whose minor is newer than this build's when the typed reader or the schema refused
// it: Invalid becomes Unsupported (a newer build wrote members this one does not know; the file is never
// overwritten with a loss, R-38), other errors pass unchanged.
Error newerMinorError(Error e, const DocKind& kind, const DocVersion& version);

// Marks "$schema" and "schemaVersion" of the root object as read (prepareDocument() checked them).
void markSchemaHeader(jsonio::ObjectReader& r);
// {"$schema": "urn:esses:<name>-1", "schemaVersion": <version>}.
json newDocument(std::string_view name, int version = 1);

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

// The load pipeline of 02 section 6.6 rule 2 for a document of `kind`: parses `text`, runs prepareDocument()
// (header and migrations) and the typed reader `fn(root, version)`, then validates the document as it will be
// written, `serialise(typed)`, with validateDocument(): the typed readers are closed except for the members
// they keep verbatim (unknown members, opaque option blocks), so this checks those against the schema while
// the reader's own leniencies (02 section 6.6 rule 4: a missing "$schema", a workbook name taken from the
// file name, an explicit null for an absent optional) stay accepted. Everything runs under jsonio::guardJson.
template <class T, class F, class S>
Result<T> readDocument(std::string_view text, const DocKind& kind, std::vector<std::string>& log, F&& fn,
                       S&& serialise) {
    auto parsed = jsonio::parseJson(text);
    if (!parsed) {
        return Failure{parsed.error()};
    }
    json root = std::move(parsed).value();
    DocVersion version;
    auto header = jsonio::guardJson([&]() -> Result<void> {
        version = prepareDocument(root, kind, log);
        return ok();
    });
    if (!header) {
        return Failure{header.error()};
    }
    const json& prepared = root;
    Result<T> doc = jsonio::guardJson([&]() -> Result<T> {
        Result<T> typed = fn(prepared, version);
        if (typed) {
            validateDocument(serialise(*typed), kind);
        }
        return typed;
    });
    if (!doc) {
        return Failure{newerMinorError(doc.error(), kind, version)};
    }
    return doc;
}

// Parses `text` and runs `fn(root)` under jsonio::guardJson (a document fragment without the header).
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
