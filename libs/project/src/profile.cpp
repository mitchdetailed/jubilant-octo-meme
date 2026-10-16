// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "doc_io.hpp"

#include <esses/project/profile.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <utility>

namespace esses::project {

namespace {

constexpr detail::DocKind kKind{"profile", 1};

using detail::json;
using jsonio::asString;
using jsonio::ObjectReader;
using jsonio::violate;

DataView readView(const json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    DataView v;
    v.title = asString(r.required("title"), r.child("title"));
    if (v.title.empty()) {
        violate(r.child("title"), "a data view title is not empty");
    }
    v.isDefault = detail::optBool(r, "default", false);
    v.tree = detail::stringArray(r.required("tree"), r.child("tree"));
    v.outingDetail = detail::optString(r, "outingDetail");
    r.closed();
    return v;
}

ProfileDetail readDetail(const json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    ProfileDetail d;
    d.id = asString(r.required("id"), r.child("id"));
    if (d.id.empty()) {
        violate(r.child("id"), "a detail id is not empty");
    }
    d.type = detail::enumString(r.required("type"), r.child("type"), {"string", "number"});
    d.unit = detail::optString(r, "unit");
    if (const json* dps = r.optional("dps")) {
        d.dps = jsonio::asInt32(*dps, r.child("dps"));
        if (*d.dps < 0 || *d.dps > 15) {
            violate(r.child("dps"), "dps lies in 0..15");
        }
    }
    if (const json* def = r.optional("default")) {
        d.defaultValue = jsonio::asNumber(*def, r.child("default"));
    }
    if (d.type == "string" && (!d.unit.empty() || d.dps || d.defaultValue)) {
        violate(pointer, "a string detail has no unit, dps or numeric default");
    }
    r.closed();
    return d;
}

json profileToJson(const Profile& p) {
    json root = detail::newDocument("profile", std::max(p.schemaVersion, kKind.current));
    root["id"] = p.id;
    root["name"] = p.name;
    root["rangeKinds"] = p.rangeKinds;
    root["primaryRanges"] = p.primaryRanges;
    root["primaryMarkers"] = p.primaryMarkers;
    root["rules"] = p.rules;
    json views = json::array();
    for (const DataView& v : p.dataViews) {
        json jv = {{"title", v.title}, {"tree", v.tree}};
        if (v.isDefault) {
            jv["default"] = true;
        }
        if (!v.outingDetail.empty()) {
            jv["outingDetail"] = v.outingDetail;
        }
        views.push_back(std::move(jv));
    }
    root["dataViews"] = std::move(views);
    root["reportHeadings"] = p.reportHeadings;
    root["defaultMaths"] = p.defaultMaths;
    root["defaultWorkbook"] = p.defaultWorkbook;
    json details = json::array();
    for (const ProfileDetail& d : p.details) {
        json jd = {{"id", d.id}, {"type", d.type}};
        if (!d.unit.empty()) {
            jd["unit"] = d.unit;
        }
        if (d.dps) {
            jd["dps"] = *d.dps;
        }
        if (d.defaultValue) {
            jd["default"] = *d.defaultValue;
        }
        details.push_back(std::move(jd));
    }
    root["details"] = std::move(details);
    if (!p.terms.compact().empty()) {
        root["terms"] = jsonio::fromDoc(p.terms);
    }
    detail::putUnknown(root, p.unknown);
    return root;
}

ProfileDetail numberDetail(std::string id, std::string unit, int dps,
                           std::optional<double> def = std::nullopt) {
    ProfileDetail d;
    d.id = std::move(id);
    d.type = "number";
    d.unit = std::move(unit);
    d.dps = dps;
    d.defaultValue = def;
    return d;
}

ProfileDetail stringDetail(std::string id) {
    ProfileDetail d;
    d.id = std::move(id);
    d.type = "string";
    return d;
}

} // namespace

const DataView* Profile::defaultView() const noexcept {
    for (const DataView& v : dataViews) {
        if (v.isDefault) {
            return &v;
        }
    }
    return dataViews.empty() ? nullptr : &dataViews.front();
}

const ProfileDetail* Profile::findDetail(std::string_view detailId) const noexcept {
    for (const ProfileDetail& d : details) {
        if (d.id == detailId) {
            return &d;
        }
    }
    return nullptr;
}

Result<Profile> Profile::parse(std::string_view json, std::vector<std::string>& log) {
    return detail::readDocument<Profile>(
        json, kKind, log,
        [&](const detail::json& root, const detail::DocVersion& version) -> Result<Profile> {
            ObjectReader r(root, "");
            detail::markSchemaHeader(r);
            Profile p;
            p.schemaVersion = version.write;
            p.id = asString(r.required("id"), r.child("id"));
            if (!detail::isLowerId(p.id, '-', 1, 1)) {
                violate(r.child("id"), "a profile id is lower case (\"circuit\"), found '" + p.id + "'");
            }
            p.name = asString(r.required("name"), r.child("name"));
            p.rangeKinds = detail::optStringArray(r, "rangeKinds");
            p.primaryRanges = detail::optString(r, "primaryRanges", "Outings:Laps");
            p.primaryMarkers = detail::optString(r, "primaryMarkers", "Beacons");
            p.rules = detail::optStringArray(r, "rules");
            for (std::size_t i = 0; i < p.rules.size(); ++i) {
                if (!detail::isLowerId(p.rules[i], '-', 2)) {
                    violate(jsonio::pointerIndex(r.child("rules"), i),
                            "a rule id is dotted lower case (\"circuit.beacon-pair\"), found '" + p.rules[i] +
                                "'");
                }
            }
            if (const detail::json* views = r.optional("dataViews")) {
                const std::string vp = r.child("dataViews");
                if (!views->is_array()) {
                    violate(vp, "dataViews is an array");
                }
                int defaults = 0;
                for (std::size_t i = 0; i < views->size(); ++i) {
                    p.dataViews.push_back(readView((*views)[i], jsonio::pointerIndex(vp, i)));
                    defaults += p.dataViews.back().isDefault ? 1 : 0;
                }
                if (defaults > 1) {
                    violate(vp, "at most one data view is the default");
                }
            }
            p.reportHeadings = detail::optStringArray(r, "reportHeadings");
            p.defaultMaths = detail::optStringArray(r, "defaultMaths");
            p.defaultWorkbook = detail::optString(r, "defaultWorkbook");
            if (const detail::json* detailsJson = r.optional("details")) {
                const std::string dp = r.child("details");
                if (!detailsJson->is_array()) {
                    violate(dp, "details is an array");
                }
                std::set<std::string, std::less<>> ids;
                for (std::size_t i = 0; i < detailsJson->size(); ++i) {
                    ProfileDetail d = readDetail((*detailsJson)[i], jsonio::pointerIndex(dp, i));
                    if (!ids.insert(d.id).second) {
                        violate(jsonio::pointerIndex(dp, i), "detail '" + d.id + "' is listed twice");
                    }
                    p.details.push_back(std::move(d));
                }
            }
            if (const detail::json* t = r.optional("terms")) {
                p.terms = detail::objectDoc(*t, r.child("terms"));
            }
            p.unknown = detail::unknownMembers(r);
            return p;
        },
        profileToJson);
}

Result<Profile> Profile::load(const std::filesystem::path& path, std::vector<std::string>& log) {
    return detail::loadDocument<Profile>(path,
                                         [&](std::string_view text) { return Profile::parse(text, log); });
}

std::string Profile::toJson() const {
    return detail::canonicalOrEmpty(profileToJson(*this));
}

Result<void> Profile::save(const std::filesystem::path& path) const {
    return detail::saveCanonical(profileToJson(*this), path);
}

Profile Profile::circuitDefault() {
    Profile p;
    p.id = "circuit";
    p.name = "Circuit";
    p.rangeKinds = {"Outing", "Lap", "Section"};
    p.primaryRanges = "Outings:Laps";
    p.primaryMarkers = "Beacons";
    p.rules = {"circuit.detail-groups", "circuit.whole-outing", "circuit.beacon-pair",
               "circuit.track-sections"};
    DataView laps;
    laps.title = "Laps";
    laps.isDefault = true;
    laps.tree = {"Events", "Vehicles", "Sessions", "Outings", "Outings:Laps"};
    laps.outingDetail = "'File Name'";
    DataView outings;
    outings.title = "Outings";
    outings.tree = {"Events", "Vehicles", "Sessions", "Outings"};
    p.dataViews = {laps, outings};
    p.reportHeadings = {"Events",
                        "Vehicles",
                        "Sessions",
                        "Driver",
                        "Vehicle Id",
                        "Vehicle Number",
                        "Outings",
                        "Outings:Laps",
                        "Outings:Laps:Track Sections:Default",
                        "Outings:Laps:Track Sections:Default:Corners",
                        "Outings:Laps:Track Sections:Default:Straights"};
    p.defaultMaths = {"esses-circuit"};
    p.defaultWorkbook = "circuit-default";
    p.details = {stringDetail("Driver"),
                 stringDetail("Venue"),
                 numberDetail("Venue Length", "km", 3),
                 numberDetail("Start Lap", "", 0),
                 numberDetail("Vehicle Wheelbase", "m", 3),
                 numberDetail("Vehicle Track", "m", 3),
                 numberDetail("Vehicle Weight", "kg", 0),
                 numberDetail("Diff Ratio", "ratio", 2),
                 numberDetail("Stoich Ratio", "ratio", 1, 14.7)};
    p.terms =
        JsonDoc::parse(R"({"editor": "Lap Editor", "range": "Lap", "ranges": "Laps"})").valueOr(JsonDoc{});
    return p;
}

} // namespace esses::project
