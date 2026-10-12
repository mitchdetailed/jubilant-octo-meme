// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "doc_io.hpp"

#include <esses/base/atomic_file.hpp>
#include <esses/maths/corrected_speed_distance.hpp>
#include <esses/project/profile.hpp>
#include <esses/project/project_file.hpp>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace esses::project {

namespace {

using detail::json;
using jsonio::asString;
using jsonio::ObjectReader;
using jsonio::violate;

constexpr std::string_view kHashPrefix = "xxh3:";

std::string readHash(const json& j, const std::string& pointer) {
    const std::string& h = asString(j, pointer);
    bool valid = h.size() == kHashPrefix.size() + 32 && std::string_view(h).starts_with(kHashPrefix);
    for (std::size_t i = kHashPrefix.size(); valid && i < h.size(); ++i) {
        valid = (h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f');
    }
    if (!valid) {
        violate(pointer, "expected \"xxh3:\" and 32 lower-case hex digits, found '" + h + "'");
    }
    return h;
}

RangeKey readRangeKey(const json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    RangeKey k;
    {
        ObjectReader s(r.required("source"), r.child("source"));
        k.source.path = asString(s.required("path"), s.child("path"));
        k.source.hash = readHash(s.required("hash"), s.child("hash"));
        s.closed();
    }
    k.layer = detail::optString(r, "layer", "Normal");
    k.groupPath = asString(r.required("groupPath"), r.child("groupPath"));
    k.startMarker = jsonio::asUuid(r.required("startMarker"), r.child("startMarker")).str();
    k.name = detail::optString(r, "name");
    r.closed();
    return k;
}

json rangeKeyToJson(const RangeKey& k) {
    return {{"source", {{"path", k.source.path}, {"hash", k.source.hash}}},
            {"layer", k.layer},
            {"groupPath", k.groupPath},
            {"startMarker", k.startMarker},
            {"name", k.name}};
}

SelectionKeys readSelection(const json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    SelectionKeys s;
    if (const json* m = r.optional("main")) {
        s.main = readRangeKey(*m, r.child("main"));
    }
    if (const json* o = r.optional("overlay0")) {
        s.overlay0 = readRangeKey(*o, r.child("overlay0"));
    }
    if (const json* list = r.optional("overlays")) {
        const std::string lp = r.child("overlays");
        if (!list->is_array()) {
            violate(lp, "overlays is an array");
        }
        for (std::size_t i = 0; i < list->size(); ++i) {
            s.overlays.push_back(readRangeKey((*list)[i], jsonio::pointerIndex(lp, i)));
        }
    }
    s.overlaysHidden = detail::optBool(r, "overlaysHidden", false);
    s.singleColourMain = detail::optBool(r, "singleColourMain", false);
    r.closed();
    return s;
}

json projectToJson(const ProjectFile& p) {
    json root = detail::newDocument("project");
    root["name"] = p.name;
    root["profile"] = p.profile;
    if (!p.options.compact().empty()) {
        root["options"] = jsonio::fromDoc(p.options);
    }
    root["ui"] = {{"layoutLocked", p.layoutLocked}};
    root["mathsOrder"] = p.mathsOrder;
    json builtin = json::object();
    for (const auto& [id, entry] : p.builtinMaths) {
        json e = {{"version", entry.version}, {"enabled", entry.enabled}};
        if (!entry.settings.compact().empty()) {
            e["settings"] = jsonio::fromDoc(entry.settings);
        }
        builtin[id] = std::move(e);
    }
    root["builtinMaths"] = std::move(builtin);
    root["workbooks"] = p.workbooks;
    json recent = json::array();
    for (const RecentSource& s : p.recentSources) {
        recent.push_back(
            {{"path", s.path}, {"importer", s.importer}, {"hash", s.hash}, {"openedAt", s.openedAt}});
    }
    root["recentSources"] = std::move(recent);
    json sel = json::object();
    if (p.lastSelection.main) {
        sel["main"] = rangeKeyToJson(*p.lastSelection.main);
    }
    if (p.lastSelection.overlay0) {
        sel["overlay0"] = rangeKeyToJson(*p.lastSelection.overlay0);
    }
    json overlays = json::array();
    for (const RangeKey& k : p.lastSelection.overlays) {
        overlays.push_back(rangeKeyToJson(k));
    }
    sel["overlays"] = std::move(overlays);
    sel["overlaysHidden"] = p.lastSelection.overlaysHidden;
    sel["singleColourMain"] = p.lastSelection.singleColourMain;
    root["lastSelection"] = std::move(sel);
    if (!p.dataWindow.compact().empty()) {
        root["dataWindow"] = jsonio::fromDoc(p.dataWindow);
    }
    detail::putUnknown(root, p.unknown);
    return root;
}

// "yyyymmdd-hhmmss" in UTC.
std::string backupStamp(std::chrono::system_clock::time_point now) {
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d%02d%02d-%02d%02d%02d", utc.tm_year + 1900, utc.tm_mon + 1,
                  utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return buf;
}

std::u8string toU8(std::string_view s) {
    return {s.begin(), s.end()};
}

std::string fromU8(const std::u8string& s) {
    return {s.begin(), s.end()};
}

// The (stamp, clash number) of a backup name "<stem>.<yyyymmdd-hhmmss>[-n].json"; nullopt for other files.
std::optional<std::pair<std::string, long>> backupKey(const std::string& name, const std::string& stem) {
    if (!name.starts_with(stem + ".") || !name.ends_with(".json")) {
        return std::nullopt;
    }
    const std::string rest = name.substr(stem.size() + 1, name.size() - stem.size() - 1 - 5);
    if (rest.size() < 15) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < 15; ++i) {
        const bool shaped = i == 8 ? rest[i] == '-' : (rest[i] >= '0' && rest[i] <= '9');
        if (!shaped) {
            return std::nullopt;
        }
    }
    long clash = 1;
    if (rest.size() > 15) {
        if (rest[15] != '-' || rest.size() == 16 || rest.size() > 22) {
            return std::nullopt;
        }
        clash = 0;
        for (std::size_t i = 16; i < rest.size(); ++i) {
            if (rest[i] < '0' || rest[i] > '9') {
                return std::nullopt;
            }
            clash = clash * 10 + (rest[i] - '0');
        }
    }
    return std::make_pair(rest.substr(0, 15), clash);
}

Result<void> pruneBackups(const std::filesystem::path& dir, const std::string& stem, int keep) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::pair<std::pair<std::string, long>, fs::path>> mine;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (auto key = backupKey(fromU8(it->path().filename().u8string()), stem)) {
            mine.emplace_back(std::move(*key), it->path());
        }
    }
    if (ec) {
        ErrorContext ctx;
        ctx.path = dir;
        return fail(ErrorCode::Io, "cannot list the backup folder: " + ec.message(), std::move(ctx));
    }
    std::sort(mine.begin(), mine.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const std::size_t keepCount = keep < 0 ? 0 : static_cast<std::size_t>(keep);
    for (std::size_t i = 0; i + keepCount < mine.size(); ++i) {
        fs::remove(mine[i].second, ec);
    }
    return ok();
}

} // namespace

Result<ProjectFile> ProjectFile::parse(std::string_view json, std::vector<std::string>& log) {
    return detail::parseDocument<ProjectFile>(json, [&](const detail::json& root) -> Result<ProjectFile> {
        ObjectReader r(root, "");
        detail::readSchemaHeader(r, "project", log);
        ProjectFile p;
        p.name = asString(r.required("name"), r.child("name"));
        p.profile = detail::optString(r, "profile", "circuit");
        if (const detail::json* o = r.optional("options")) {
            p.options = detail::objectDoc(*o, r.child("options"));
        }
        if (const detail::json* u = r.optional("ui")) {
            ObjectReader ur(*u, r.child("ui"));
            p.layoutLocked = detail::optBool(ur, "layoutLocked", false);
            ur.closed();
        }
        p.mathsOrder = detail::optStringArray(r, "mathsOrder");
        if (const detail::json* b = r.optional("builtinMaths")) {
            ObjectReader br(*b, r.child("builtinMaths"));
            for (auto it = b->begin(); it != b->end(); ++it) {
                const std::string ep = br.child(it.key());
                if (!detail::isLowerId(it.key(), '_', 2)) {
                    violate(ep,
                            "a built-in module id is dotted snake_case (\"esses.corrected_speed_distance\")");
                }
                ObjectReader er(it.value(), ep);
                BuiltinMathsEntry e;
                if (const detail::json* v = er.optional("version")) {
                    e.version = jsonio::asInt32(*v, er.child("version"));
                    if (e.version < 1) {
                        violate(er.child("version"), "a module version is at least 1");
                    }
                }
                e.enabled = detail::optBool(er, "enabled", true);
                if (const detail::json* s = er.optional("settings")) {
                    e.settings = detail::objectDoc(*s, er.child("settings"));
                }
                er.closed();
                p.builtinMaths.emplace(it.key(), std::move(e));
            }
        }
        p.workbooks = detail::optStringArray(r, "workbooks");
        if (const detail::json* list = r.optional("recentSources")) {
            const std::string lp = r.child("recentSources");
            if (!list->is_array()) {
                violate(lp, "recentSources is an array");
            }
            for (std::size_t i = 0; i < list->size(); ++i) {
                ObjectReader sr((*list)[i], jsonio::pointerIndex(lp, i));
                RecentSource s;
                s.path = asString(sr.required("path"), sr.child("path"));
                s.importer = detail::optString(sr, "importer");
                s.hash =
                    sr.optional("hash") != nullptr ? readHash(*sr.optional("hash"), sr.child("hash")) : "";
                s.openedAt = detail::optString(sr, "openedAt");
                sr.closed();
                p.recentSources.push_back(std::move(s));
            }
        }
        if (const detail::json* s = r.optional("lastSelection")) {
            p.lastSelection = readSelection(*s, r.child("lastSelection"));
        }
        if (const detail::json* d = r.optional("dataWindow")) {
            p.dataWindow = detail::objectDoc(*d, r.child("dataWindow"));
        }
        p.unknown = detail::unknownMembers(r);
        return p;
    });
}

Result<ProjectFile> ProjectFile::load(const std::filesystem::path& path, std::vector<std::string>& log) {
    return detail::loadDocument<ProjectFile>(
        path, [&](std::string_view text) { return ProjectFile::parse(text, log); });
}

std::string ProjectFile::toJson() const {
    return detail::canonicalOrEmpty(projectToJson(*this));
}

Result<void> ProjectFile::save(const std::filesystem::path& path) const {
    auto text = jsonio::canonicalJson(projectToJson(*this));
    if (!text) {
        Error e = text.error();
        e.context.path = path;
        return Failure{std::move(e)};
    }
    return saveWithBackup(path.parent_path(), path, *text);
}

ProjectFile ProjectFile::makeDefault(std::string projectName, std::string profileId) {
    ProjectFile p;
    p.name = std::move(projectName);
    p.profile = std::move(profileId);
    // 02 section 6.2: every option at its default.
    const json options = {{"general",
                           {{"saveOnExit", true},
                            {"autosaveMinutes", 2},
                            {"loadPreviousData", true},
                            {"selectFastestOnLoad", false},
                            {"looseChannelSearch", false},
                            {"autoShowMessagesOnError", true},
                            {"reduceCpuDuringAnimation", false}}},
                          {"autoscale", {{"includeUntrusted", false}, {"includeOverlays", true}}},
                          {"timeFormat", "m:ss.fff"},
                          {"lapTimeFormat", "m:ss.fff"},
                          {"distanceUnit", "km"},
                          {"fastLapTolerancePct", 1.0},
                          {"autoCentreCursor", false},
                          {"animation", {{"rate", 1.0}}},
                          {"lapStretch", {{"enabled", true}, {"tolerancePct", 5.0}}},
                          {"trackMaps", {{"autoGenerateOnOpen", true}}},
                          {"maths", {{"precision", "single"}, {"compat", {{"unaryMinusAbovePower", false}}}}},
                          {"reports", {{"includeUntrusted", false}, {"probeSec", 5}}}};
    p.options = jsonio::toDoc(options).valueOr(JsonDoc{});
    p.mathsOrder = Profile::circuitDefault().defaultMaths;
    BuiltinMathsEntry csd;
    csd.version = maths::CorrectedSpeedDistanceModule::kVersion;
    csd.enabled = true;
    const maths::CorrectedSpeedDistanceModule module;
    const JsonDoc defaults = module.defaultSettings();
    if (defaults.isObject()) {
        csd.settings = defaults;
    }
    p.builtinMaths.emplace(std::string(maths::CorrectedSpeedDistanceModule::kId), std::move(csd));
    const json dataWindow = {{"filters", {{"default", {{"kind", "all"}}}}}, {"applyToAllSources", false}};
    p.dataWindow = jsonio::toDoc(dataWindow).valueOr(JsonDoc{});
    return p;
}

Result<void> saveWithBackup(const std::filesystem::path& projectFolder, const std::filesystem::path& target,
                            std::string_view bytes, std::chrono::system_clock::time_point now, int keep) {
    namespace fs = std::filesystem;
    std::optional<std::string> previous;
    std::error_code ec;
    if (fs::exists(target, ec)) {
        auto old = jsonio::readFile(target);
        if (!old) {
            return Failure{old.error()};
        }
        if (*old != bytes) {
            previous = std::move(old).value();
        }
    }
    if (auto written = AtomicFile::write(target, bytes); !written) {
        return written;
    }
    if (!previous) {
        return ok();
    }
    const fs::path dir = projectFolder / "backup";
    fs::create_directories(dir, ec);
    if (ec) {
        ErrorContext ctx;
        ctx.path = dir;
        return fail(ErrorCode::Io, "cannot create the backup folder: " + ec.message(), std::move(ctx));
    }
    std::string stem = fromU8(target.filename().u8string());
    if (stem.ends_with(".json")) {
        stem.resize(stem.size() - 5);
    }
    const std::string stamp = backupStamp(now);
    const std::string base = stem + "." + stamp;
    fs::path copy = dir / toU8(base + ".json");
    for (int n = 2; fs::exists(copy, ec); ++n) {
        std::string name = base;
        name += '-';
        name += std::to_string(n);
        name += ".json";
        copy = dir / toU8(name);
    }
    if (auto backedUp = AtomicFile::write(copy, *previous); !backedUp) {
        return backedUp;
    }
    return pruneBackups(dir, stem, keep);
}

} // namespace esses::project
