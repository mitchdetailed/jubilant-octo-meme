// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "doc_io.hpp"

#include <esses/base/atomic_file.hpp>
#include <esses/project/migrations.hpp>
#include <esses/project/schema_registry.hpp>

#include <algorithm>
#include <string>

namespace esses::project::detail {

using jsonio::asInt;
using jsonio::asString;
using jsonio::violate;

DocVersion prepareDocument(json& root, const DocKind& kind, std::vector<std::string>& log) {
    const std::string id = kind.id();
    if (!root.is_object()) {
        violate("", "a " + id + " document is a JSON object");
    }
    if (root.contains("$schema") && !root["$schema"].is_null()) {
        jsonio::checkSchemaId(root, kind.name, 1, "");
    } else {
        log.push_back("no \"$schema\": read as " + id + " (02 section 6.6 rule 4)");
    }
    DocVersion v;
    if (root.contains("schemaVersion") && !root["schemaVersion"].is_null()) {
        const std::int64_t read = asInt(root["schemaVersion"], "/schemaVersion");
        if (read < 1) {
            violate("/schemaVersion", "schemaVersion must be at least 1");
        }
        v.read = read > 1'000'000 ? 1'000'000 : static_cast<int>(read);
    } else {
        log.push_back("no \"schemaVersion\": read as " + id + " version 1");
    }
    // Read leniently (rule 4): from here the root carries the header it is read as (and migrated from).
    root["$schema"] = "urn:esses:" + id;
    root["schemaVersion"] = v.read;
    v.write = std::max(v.read, kind.current);
    if (v.read > kind.current) {
        log.push_back(id + " schemaVersion " + std::to_string(v.read) + " is newer than this build's " +
                      std::to_string(kind.current) + "; read as it is, its version kept on save");
        return v;
    }
    if (v.read < kind.current) {
        auto doc = jsonio::toDoc(root);
        if (!doc) {
            violate("", doc.error().message);
        }
        auto upgraded = Migrations::instance().upgrade(std::move(doc).value(), id, kind.current, log);
        if (!upgraded) {
            const Error& e = upgraded.error();
            violate(e.context.detail.value_or(std::string()), e.message, e.code,
                    e.context.schemaId.value_or(std::string()));
        }
        root = jsonio::fromDoc(*upgraded);
        v.migrated = true;
    }
    return v;
}

void validateDocument(const json& doc, const DocKind& kind) {
    auto value = jsonio::toDoc(doc);
    if (!value) {
        violate("", value.error().message);
    }
    auto valid = SchemaRegistry::instance().validate(*value, kind.id());
    if (!valid && valid.error().code != ErrorCode::NotFound) {
        const Error& e = valid.error();
        violate(e.context.detail.value_or(std::string()), e.message);
    }
}

Error newerMinorError(Error e, const DocKind& kind, const DocVersion& version) {
    if (version.read <= kind.current || e.code != ErrorCode::Invalid) {
        return e;
    }
    e.code = ErrorCode::Unsupported;
    e.message =
        kind.id() + " schemaVersion " + std::to_string(version.read) + " was written by a newer build (" +
        "this one reads up to " + std::to_string(kind.current) +
        ") and holds content this build does not understand; the file is left untouched: " + e.message;
    e.context.schemaId = "urn:esses:" + kind.id();
    return e;
}

void markSchemaHeader(jsonio::ObjectReader& r) {
    r.skip("$schema");
    r.skip("schemaVersion");
}

json newDocument(std::string_view name, int version) {
    json root = json::object();
    root["$schema"] = "urn:esses:" + std::string(name) + "-1";
    root["schemaVersion"] = version;
    return root;
}

JsonDoc unknownMembers(const jsonio::ObjectReader& r) {
    json rest = r.unread();
    if (rest.empty()) {
        return {};
    }
    return docOrViolate(rest, r.pointer());
}

void putUnknown(json& root, const JsonDoc& unknown) {
    if (!unknown.isObject()) {
        return;
    }
    const json members = jsonio::fromDoc(unknown);
    for (auto it = members.begin(); it != members.end(); ++it) {
        if (!root.contains(it.key())) { // a typed member always wins
            root[it.key()] = it.value();
        }
    }
}

JsonDoc docOrViolate(const json& j, const std::string& pointer) {
    auto doc = jsonio::toDoc(j);
    if (!doc) {
        violate(pointer, doc.error().message);
    }
    return std::move(doc).value();
}

JsonDoc objectDoc(const json& j, const std::string& pointer) {
    if (!j.is_object()) {
        violate(pointer, "expected an object");
    }
    return docOrViolate(j, pointer);
}

std::string optString(jsonio::ObjectReader& r, std::string_view key, std::string fallback) {
    if (const json* v = r.optional(key)) {
        return asString(*v, r.child(key));
    }
    return fallback;
}

bool optBool(jsonio::ObjectReader& r, std::string_view key, bool fallback) {
    if (const json* v = r.optional(key)) {
        return jsonio::asBool(*v, r.child(key));
    }
    return fallback;
}

std::vector<std::string> stringArray(const json& j, const std::string& pointer) {
    if (!j.is_array()) {
        violate(pointer, "expected an array of strings");
    }
    std::vector<std::string> out;
    out.reserve(j.size());
    for (std::size_t i = 0; i < j.size(); ++i) {
        out.push_back(asString(j[i], jsonio::pointerIndex(pointer, i)));
    }
    return out;
}

std::vector<std::string> optStringArray(jsonio::ObjectReader& r, std::string_view key) {
    if (const json* v = r.optional(key)) {
        return stringArray(*v, r.child(key));
    }
    return {};
}

std::string enumString(const json& j, const std::string& pointer,
                       std::initializer_list<std::string_view> allowed) {
    const std::string& s = asString(j, pointer);
    if (std::find(allowed.begin(), allowed.end(), std::string_view(s)) == allowed.end()) {
        std::string list;
        for (const std::string_view a : allowed) {
            list += list.empty() ? "" : ", ";
            list += a;
        }
        violate(pointer, "'" + s + "' is not one of " + list);
    }
    return s;
}

bool isLowerId(std::string_view s, char extra, int minSegments, int maxSegments) noexcept {
    bool segmentStart = true;
    int segments = 1;
    for (const char c : s) {
        if (c == '.') {
            if (segmentStart) {
                return false;
            }
            segmentStart = true;
            ++segments;
            continue;
        }
        const bool lower = c >= 'a' && c <= 'z';
        const bool tail = c == extra || (c >= '0' && c <= '9');
        if (segmentStart ? !lower : !(lower || tail)) {
            return false;
        }
        segmentStart = false;
    }
    return !segmentStart && segments >= minSegments && segments <= maxSegments;
}

std::string canonicalOrEmpty(const json& root) {
    auto text = jsonio::canonicalJson(root);
    return text ? std::move(text).value() : std::string();
}

Result<void> saveCanonical(const json& root, const std::filesystem::path& path) {
    auto text = jsonio::canonicalJson(root);
    if (!text) {
        Error e = text.error();
        e.context.path = path;
        return Failure{std::move(e)};
    }
    return AtomicFile::write(path, *text);
}

std::string fileNameUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.filename().u8string();
    return {u8.begin(), u8.end()};
}

} // namespace esses::project::detail
