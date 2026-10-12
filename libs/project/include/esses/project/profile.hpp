// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Profile: an analysis profile (schema profile-1; 02 section 6.9, D-036). M1 ships the Circuit profile,
// data/profiles/circuit.profile.json: its `rules` list (rule-1 ids, resolved by
// analysis::RuleRunner::fromRuleIds against data/rules/), the Data-window views, report headings, default
// maths and default workbook.
//
// JSON written: id, name, rangeKinds, primaryRanges, primaryMarkers, rules, dataViews (title, tree, "default"
// only when true, outingDetail when not empty), reportHeadings, defaultMaths, defaultWorkbook, details (id,
// type, unit when not empty, dps and default when set), terms (when not null), then the unknown members.
#pragma once

#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/model/opaque_json.hpp> // compatibility: model::OpaqueJson names JsonDoc

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::project {

struct DataView {
    std::string title;             // "Laps"
    bool isDefault = false;        // JSON "default"
    std::vector<std::string> tree; // group paths, outermost first
    std::string outingDetail;      // "'File Name'"
    friend bool operator==(const DataView&, const DataView&) = default;
};

struct ProfileDetail {
    std::string id;   // "Start Lap"
    std::string type; // "string" | "number"
    std::string unit; // symbol, "" for none
    std::optional<int> dps;
    std::optional<double> defaultValue;
    friend bool operator==(const ProfileDetail&, const ProfileDetail&) = default;
};

struct Profile {
    std::string id; // "circuit"
    std::string name;
    std::vector<std::string> rangeKinds;
    std::string primaryRanges = "Outings:Laps";
    std::string primaryMarkers = "Beacons";
    std::vector<std::string> rules; // rule-1 ids in order
    std::vector<DataView> dataViews;
    std::vector<std::string> reportHeadings;
    std::vector<std::string> defaultMaths;
    std::string defaultWorkbook; // "circuit-default"
    std::vector<ProfileDetail> details;
    JsonDoc terms;   // {"range": "Lap", "ranges": "Laps", "editor": "Lap Editor"}
    JsonDoc unknown; // unrecognised top-level members, preserved

    const DataView* defaultView() const noexcept; // the view marked default, else the first, else nullptr
    const ProfileDetail* findDetail(std::string_view id) const noexcept;

    static Result<Profile> parse(std::string_view json, std::vector<std::string>& log);
    static Result<Profile> load(const std::filesystem::path& path, std::vector<std::string>& log);
    std::string toJson() const; // canonical text; "" on a non-finite number
    Result<void> save(const std::filesystem::path& path) const;
    // The shipped Circuit profile built in code (callers without a data directory); equal to
    // Profile::load(data/profiles/circuit.profile.json).
    static Profile circuitDefault();

    friend bool operator==(const Profile&, const Profile&) = default;
};

} // namespace esses::project
