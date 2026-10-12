// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "doc_io.hpp"

#include <esses/base/atomic_file.hpp>

#include <algorithm>
#include <string>

namespace esses::project::detail {

using jsonio::asInt;
using jsonio::asString;
using jsonio::violate;

void readSchemaHeader(jsonio::ObjectReader& r, std::string_view name, std::vector<std::string>& log) {
    const std::string id = std::string(name) + "-1";
    if (r.optional("$schema") == nullptr) {
        log.push_back("no \"$schema\": read as " + id + " (02 section 6.6 rule 4)");
    } else {
        jsonio::checkSchemaId(r.value(), name, 1, r.pointer());
    }
    if (const json* v = r.optional("schemaVersion")) {
        const std::int64_t version = asInt(*v, r.child("schemaVersion"));
        if (version < 1) {
            violate(r.child("schemaVersion"), "schemaVersion must be at least 1");
        }
        if (version > 1) {
            log.push_back(id + " schemaVersion " + std::to_string(version) +
                          " is newer than this build's 1; read as 1, unknown members preserved");
        }
    } else {
        log.push_back("no \"schemaVersion\": read as " + id + " version 1");
    }
}

json newDocument(std::string_view name) {
    json root = json::object();
    root["$schema"] = "urn:esses:" + std::string(name) + "-1";
    root["schemaVersion"] = 1;
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
