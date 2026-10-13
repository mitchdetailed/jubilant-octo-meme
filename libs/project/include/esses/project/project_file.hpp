// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ProjectFile: <Project>/project.json (schema project-1; 02 sections 6.1-6.2, 06 sections 5.4-5.5 and 18,
// D-019). M1 types the members the walking skeleton reads (profile, builtinMaths, workbooks, recentSources,
// lastSelection, ui.layoutLocked) and keeps `options` and `dataWindow` as opaque JSON until the Options
// dialog and the Data-window filters land (M2); every other member is preserved in `unknown`. RangeKey is the
// persisted form of model::RangeRef (D-014 identity (groupPath, startMarker)); restore resolves by (hash,
// layer, groupPath, startMarker), then by (path, layer, groupPath, name) (06 section 5.5).
//
// JSON written: name, profile, options (when not null), ui {layoutLocked}, mathsOrder, builtinMaths (each
// {version, enabled, settings when not null}), workbooks, recentSources, lastSelection {main and overlay0
// when set, overlays, overlaysHidden, singleColourMain}, dataWindow (when not null), then the unknown
// members. The typed sub-objects are closed (an unknown member there is Invalid, as in
// schemas/project-1.schema.json); `options` and `dataWindow` are kept verbatim. `workbooks` holds workbook
// names; a workbook's file is workbooks/<name>.workbook.json (02 section 6.1).
#pragma once

#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/model/opaque_json.hpp> // compatibility: model::OpaqueJson names JsonDoc

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::project {

struct SourceKey {
    std::string path; // relative to the project folder when possible
    std::string hash; // "xxh3:<32 hex digits>" (SourceId)
    friend bool operator==(const SourceKey&, const SourceKey&) = default;
};

struct RangeKey {
    SourceKey source;
    std::string layer = "Normal";
    std::string groupPath;
    std::string startMarker; // MarkerId UUID text
    std::string name;
    friend bool operator==(const RangeKey&, const RangeKey&) = default;
};

struct SelectionKeys { // project.json "lastSelection"
    std::optional<RangeKey> main;
    std::optional<RangeKey> overlay0;
    std::vector<RangeKey> overlays;
    bool overlaysHidden = false;
    bool singleColourMain = false;
    friend bool operator==(const SelectionKeys&, const SelectionKeys&) = default;
};

struct RecentSource {
    std::string path;
    std::string importer; // "motec.ld"
    std::string hash;
    std::string openedAt; // RFC 3339
    friend bool operator==(const RecentSource&, const RecentSource&) = default;
};

struct BuiltinMathsEntry { // project.json builtinMaths.<moduleId>
    int version = 1;
    bool enabled = true;
    JsonDoc settings; // validated by schemas/builtin-<id>-<version>.schema.json; null = the module defaults
    friend bool operator==(const BuiltinMathsEntry&, const BuiltinMathsEntry&) = default;
};

struct ProjectFile {
    std::string name;
    std::string profile = "circuit";
    JsonDoc options;           // options.* (02 section 6.2 key table)
    bool layoutLocked = false; // ui.layoutLocked
    std::vector<std::string> mathsOrder;
    std::map<std::string, BuiltinMathsEntry, std::less<>> builtinMaths;
    std::vector<std::string> workbooks;
    std::vector<RecentSource> recentSources;
    SelectionKeys lastSelection;
    JsonDoc dataWindow; // filters, applyToAllSources (06 section 5.4)
    JsonDoc unknown;    // unrecognised top-level members, preserved

    static Result<ProjectFile> parse(std::string_view json, std::vector<std::string>& log);
    static Result<ProjectFile> load(const std::filesystem::path& path, std::vector<std::string>& log);
    std::string toJson() const; // canonical text; "" on a non-finite number
    // An explicit Save of <Project>/project.json: saveWithBackup(path.parent_path(), path, toJson()).
    Result<void> save(const std::filesystem::path& path) const;
    // A new project (name = folder name) with the profile's defaults: the options of 02 section 6.2 at their
    // defaults, mathsOrder = the profile's defaultMaths, builtinMaths["esses.corrected_speed_distance"] = {1,
    // true, the module's default settings}, dataWindow {filters {default: all}, applyToAllSources false}.
    // `workbooks` stays empty: the caller copies the profile's defaultWorkbook into the project and adds its
    // name.
    static ProjectFile makeDefault(std::string projectName, std::string profileId = "circuit");

    friend bool operator==(const ProjectFile&, const ProjectFile&) = default;
};

// The explicit-Save write of one project document (02 sections 6.1 and 6.8 rule 2, D-070): `bytes` is written
// atomically to `target`; when `target` existed with other bytes, its previous content is then copied to
// <projectFolder>/backup/<target file name without ".json">.<yyyymmdd-hhmmss UTC>.json (a "-2", "-3", ...
// suffix on a clash) and only the newest `keep` backups of that file are kept. Io on a failed write; a failed
// backup copy is reported after the document itself was written.
Result<void> saveWithBackup(const std::filesystem::path& projectFolder, const std::filesystem::path& target,
                            std::string_view bytes,
                            std::chrono::system_clock::time_point now = std::chrono::system_clock::now(),
                            int keep = 5);

} // namespace esses::project
