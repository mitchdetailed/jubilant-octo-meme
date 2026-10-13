// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// UnitMappingProfile: unitmap-1 reading, canonical writing and rule resolution (05 section 8.2, 02 section
// 6.3, schemas/unitmap-1.schema.json). load(canonical) -> toJson() reproduces the bytes (AC-M2-07): every
// member a rule states is written back, unknown top-level members are kept.
#include <esses/base/atomic_file.hpp>
#include <esses/base/casefold.hpp>
#include <esses/base/json_nlohmann.hpp>
#include <esses/base/utf8.hpp>
#include <esses/formats/text_profile.hpp>
#include <esses/formats/unit_mapping_profile.hpp>

#include <string>
#include <utility>

namespace esses::formats {
namespace {

using jsonio::asString;
using jsonio::ObjectReader;
using jsonio::violate;
using json = nlohmann::json;

std::string_view matchName(UnitMappingProfile::Match m) {
    switch (m) {
    case UnitMappingProfile::Match::NameOnly:
        return "nameOnly";
    case UnitMappingProfile::Match::UnitOnly:
        return "unitOnly";
    case UnitMappingProfile::Match::NameAndUnit:
        break;
    }
    return "nameAndUnit";
}

UnitMappingProfile parseProfile(const json& j, std::vector<std::string>& log) {
    if (!j.is_object()) {
        violate("", "a unit map is a JSON object");
    }
    jsonio::checkSchemaId(j, "unitmap", 1);
    ObjectReader r(j, "");
    r.skip("$schema");
    (void)jsonio::asInt(r.required("schemaVersion"), r.child("schemaVersion"));
    UnitMappingProfile p;
    p.importer = asString(r.required("importer"), r.child("importer"));
    const json& rules = r.required("rules");
    if (!rules.is_array()) {
        violate(r.child("rules"), "expected an array");
    }
    for (std::size_t i = 0; i < rules.size(); ++i) {
        const std::string pi = jsonio::pointerIndex(r.child("rules"), i);
        ObjectReader q(rules[i], pi);
        UnitMappingProfile::Rule rule;
        const std::string& m = asString(q.required("match"), q.child("match"));
        if (m == "nameOnly") {
            rule.match = UnitMappingProfile::Match::NameOnly;
        } else if (m == "unitOnly") {
            rule.match = UnitMappingProfile::Match::UnitOnly;
        } else if (m == "nameAndUnit") {
            rule.match = UnitMappingProfile::Match::NameAndUnit;
        } else {
            violate(q.child("match"), "match must be nameOnly, unitOnly or nameAndUnit");
        }
        if (const json* v = q.optional("name")) {
            rule.name = asString(*v, q.child("name"));
        }
        if (const json* v = q.optional("vendorUnit")) {
            rule.vendorUnit = asString(*v, q.child("vendorUnit"));
        }
        rule.quantity = asString(q.required("quantity"), q.child("quantity"));
        rule.unit = asString(q.required("unit"), q.child("unit"));
        if (const json* v = q.optional("analysisName")) {
            rule.analysisName = asString(*v, q.child("analysisName"));
        }
        q.closed();
        if (rule.match != UnitMappingProfile::Match::UnitOnly && rule.name.empty()) {
            violate(pi, std::string(matchName(rule.match)) + " needs a name");
        }
        if (rule.match != UnitMappingProfile::Match::NameOnly && rule.vendorUnit.empty()) {
            violate(pi, std::string(matchName(rule.match)) + " needs a vendorUnit");
        }
        p.rules.push_back(std::move(rule));
    }
    json unknown = r.unread();
    if (!unknown.empty()) {
        for (const auto& [key, value] : unknown.items()) {
            log.push_back("unit map '" + p.importer + "': unknown member '" + key + "' kept");
        }
        if (auto doc = jsonio::toDoc(unknown)) {
            p.unknown = std::move(doc).value();
        }
    }
    return p;
}

std::string foldUnit(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return caseFold(s);
}

} // namespace

Result<UnitMappingProfile> UnitMappingProfile::parse(std::string_view text, std::vector<std::string>& log) {
    auto j = jsonio::parseJson(text);
    if (!j) {
        return Failure{j.error()};
    }
    return jsonio::guardJson([&]() -> Result<UnitMappingProfile> { return parseProfile(*j, log); });
}

Result<UnitMappingProfile> UnitMappingProfile::load(const std::filesystem::path& path,
                                                    std::vector<std::string>& log) {
    auto text = jsonio::readFile(path);
    if (!text) {
        return Failure{text.error()};
    }
    auto p = parse(*text, log);
    if (!p) {
        Error e = p.error();
        e.context.path = path;
        e.message = pathToUtf8(path) + ": " + e.message;
        return Failure{std::move(e)};
    }
    return p;
}

Result<std::string> UnitMappingProfile::toJson() const {
    json j = json::object();
    if (!unknown.isNull()) {
        j = jsonio::fromDoc(unknown);
        if (!j.is_object()) {
            j = json::object();
        }
    }
    j["$schema"] = std::string(kSchemaId);
    j["schemaVersion"] = 1;
    j["importer"] = importer;
    json rs = json::array();
    for (const Rule& r : rules) {
        json x = json::object();
        x["match"] = std::string(matchName(r.match));
        if (!r.name.empty()) {
            x["name"] = r.name;
        }
        if (!r.vendorUnit.empty()) {
            x["vendorUnit"] = r.vendorUnit;
        }
        x["quantity"] = r.quantity;
        x["unit"] = r.unit;
        if (!r.analysisName.empty()) {
            x["analysisName"] = r.analysisName;
        }
        rs.push_back(std::move(x));
    }
    j["rules"] = std::move(rs);
    return jsonio::canonicalJson(j);
}

Result<void> UnitMappingProfile::save(const std::filesystem::path& path) const {
    auto text = toJson();
    if (!text) {
        return Failure{text.error()};
    }
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    return AtomicFile::write(path, std::string_view(*text));
}

std::optional<UnitMappingProfile::Resolution>
UnitMappingProfile::resolve(std::string_view loggedName, std::string_view vendorUnit,
                            const units::UnitRegistry& units) const {
    const std::string name = caseFold(loggedName);
    const std::string unit = foldUnit(vendorUnit);
    const auto applies = [&](const Rule& r) {
        switch (r.match) {
        case Match::NameAndUnit:
            return caseFold(r.name) == name && foldUnit(r.vendorUnit) == unit;
        case Match::NameOnly:
            return caseFold(r.name) == name;
        case Match::UnitOnly:
            return !unit.empty() && foldUnit(r.vendorUnit) == unit;
        }
        return false;
    };
    // Most specific first; within one kind, file order. A rule whose quantity or unit the registry does not
    // know is skipped.
    for (const Match kind : {Match::NameAndUnit, Match::NameOnly, Match::UnitOnly}) {
        for (const Rule& r : rules) {
            if (r.match != kind || !applies(r)) {
                continue;
            }
            const auto q = units.quantityByName(r.quantity);
            if (!q) {
                continue;
            }
            const auto u = units.find(r.unit, *q);
            if (!u) {
                continue;
            }
            Resolution res;
            res.quantity = *q;
            res.unit = *u;
            if (!r.analysisName.empty()) {
                res.analysisName = r.analysisName;
            }
            res.rule = &r;
            return res;
        }
    }
    return std::nullopt;
}

std::string UnitMappingProfile::fileStem(std::string_view importerId) {
    if (importerId.starts_with(TextProfile::kImporterIdPrefix)) {
        importerId.remove_prefix(TextProfile::kImporterIdPrefix.size());
    }
    return std::string(importerId);
}

} // namespace esses::formats
