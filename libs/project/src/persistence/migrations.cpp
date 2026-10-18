// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Migrations (02 section 6.6, D-015, D-019, D-072) and the shipped steps. One real step ships in M2:
// project-1 1 -> 2 (D-162), which fixes the key form of the Data-window filters (project.json
// dataWindow.filters) to "default" or the source's quick hash as "xxh3:<32 lower-case hex digits>", the form
// of every other hash in project-1 and sidecar-1. A version-1 key of 32 bare hex digits (the "SourceId hex"
// spelling of 02 section 6.2's key table) gains the prefix; any other key moves, with its filter, to
// x-esses-legacy.dataWindowFilters (rule 3: a step never deletes information).

#include <esses/base/json_nlohmann.hpp>
#include <esses/project/migrations.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace esses::project {

namespace {

using nlohmann::json;

bool isHex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool isBareHex32(std::string_view s) noexcept {
    if (s.size() != 32) {
        return false;
    }
    for (const char c : s) {
        if (!isHex(c)) {
            return false;
        }
    }
    return true;
}

bool isHashText(std::string_view s) noexcept {
    if (!s.starts_with("xxh3:") || s.size() != 37) {
        return false;
    }
    for (const char c : s.substr(5)) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'F') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

Result<JsonDoc> invalidAt(std::string pointer, std::string message) {
    ErrorContext ctx;
    ctx.detail = std::move(pointer);
    return fail(ErrorCode::Invalid, std::move(message), std::move(ctx));
}

// project-1 1 -> 2: the Data-window filter keys (D-162).
Result<JsonDoc> projectFilterKeys(JsonDoc doc) {
    json root = jsonio::fromDoc(doc);
    const auto dataWindow = root.find("dataWindow");
    if (dataWindow == root.end() || !dataWindow->is_object()) {
        return doc;
    }
    const auto filters = dataWindow->find("filters");
    if (filters == dataWindow->end() || !filters->is_object()) {
        return doc;
    }
    json kept = json::object();
    json legacy = json::object();
    // Keys already in the version-2 form first, so a bare key never replaces its prefixed twin.
    for (auto it = filters->begin(); it != filters->end(); ++it) {
        if (it.key() == "default" || isHashText(it.key())) {
            kept[it.key()] = it.value();
        }
    }
    for (auto it = filters->begin(); it != filters->end(); ++it) {
        if (it.key() == "default" || isHashText(it.key())) {
            continue;
        }
        if (isBareHex32(it.key())) {
            const std::string key = "xxh3:" + lower(it.key());
            if (!kept.contains(key)) {
                kept[key] = it.value();
                continue;
            }
        }
        legacy[it.key()] = it.value();
    }
    *filters = std::move(kept);
    if (!legacy.empty()) {
        json& bag = root["x-esses-legacy"];
        if (bag.is_null()) {
            bag = json::object();
        }
        if (!bag.is_object()) {
            return invalidAt("/x-esses-legacy", "x-esses-legacy is not an object");
        }
        json& moved = bag["dataWindowFilters"];
        if (moved.is_null()) {
            moved = json::object();
        }
        if (!moved.is_object()) {
            return invalidAt("/x-esses-legacy/dataWindowFilters",
                             "x-esses-legacy.dataWindowFilters is not an object");
        }
        for (auto it = legacy.begin(); it != legacy.end(); ++it) {
            moved[it.key()] = it.value();
        }
    }
    return jsonio::toDoc(root);
}

} // namespace

Migrations& Migrations::instance() {
    static Migrations migrations = [] {
        Migrations m;
        m.add("project-1", 1, projectFilterKeys);
        return m;
    }();
    return migrations;
}

void Migrations::add(std::string_view schema, int fromVersion, Fn fn) {
    steps_.insert_or_assign(std::make_pair(std::string(schema), fromVersion), std::move(fn));
}

int Migrations::latest(std::string_view schema) const {
    int newest = 1;
    for (const auto& step : steps_) {
        if (step.first.first == schema) {
            newest = std::max(newest, step.first.second + 1);
        }
    }
    return newest;
}

Result<JsonDoc> Migrations::upgrade(JsonDoc doc, std::string_view schema, int toVersion,
                                    std::vector<std::string>& log) const {
    const std::string key(schema);
    ErrorContext ctx;
    ctx.schemaId = "urn:esses:" + key;
    if (!doc.isObject()) {
        ctx.detail = "";
        return fail(ErrorCode::Invalid, key + ": the document is not a JSON object", std::move(ctx));
    }
    int version = doc.schemaVersion();
    if (version == 0) {
        version = 1; // absent: the oldest version of the kind (02 section 6.6 rule 4)
    }
    if (version > toVersion) {
        return fail(ErrorCode::Unsupported,
                    key + " schemaVersion " + std::to_string(version) + " is newer than " +
                        std::to_string(toVersion),
                    std::move(ctx));
    }
    while (version < toVersion) {
        const auto step = steps_.find(std::make_pair(key, version));
        if (step == steps_.end()) {
            return fail(ErrorCode::Unsupported,
                        "no migration step " + key + " " + std::to_string(version) + " -> " +
                            std::to_string(version + 1),
                        std::move(ctx));
        }
        Result<JsonDoc> next = step->second(std::move(doc));
        if (!next) {
            Error e = next.error();
            e.message =
                key + " " + std::to_string(version) + " -> " + std::to_string(version + 1) + ": " + e.message;
            if (!e.context.schemaId) {
                e.context.schemaId = ctx.schemaId;
            }
            return Failure{std::move(e)};
        }
        if (!next->isObject()) {
            return fail(ErrorCode::Internal,
                        "migration step " + key + " " + std::to_string(version) + " did not return an object",
                        std::move(ctx));
        }
        json root = jsonio::fromDoc(*next);
        root["schemaVersion"] = version + 1;
        auto stamped = jsonio::toDoc(root);
        if (!stamped) {
            return Failure{stamped.error()};
        }
        doc = std::move(stamped).value();
        log.push_back(key + ": migrated schemaVersion " + std::to_string(version) + " -> " +
                      std::to_string(version + 1) + " (02 section 6.6)");
        ++version;
    }
    return doc;
}

} // namespace esses::project
