// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The channel documents of a project folder (02 section 6.3, D-013): overrides-1 (ChannelOverrides),
// aliases-1 (Aliases) and mappings-1 (Mappings), read through the shared load pipeline of doc_io.hpp (header,
// migrations, typed reader, SchemaRegistry) and written canonically (02 section 6.5 rule 1), so a canonical
// document round-trips byte-identically (AC-M2-07). {v, unit} values keep the unit symbol as written.

#include "../doc_io.hpp"

#include <esses/base/casefold.hpp>
#include <esses/project/aliases.hpp>
#include <esses/project/channel_overrides.hpp>
#include <esses/project/mappings.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace esses::project {

namespace {

using detail::json;
using jsonio::asString;
using jsonio::ObjectReader;
using jsonio::violate;

constexpr detail::DocKind kOverridesKind{"overrides", 1};
constexpr detail::DocKind kAliasesKind{"aliases", 1};
constexpr detail::DocKind kMappingsKind{"mappings", 1};

constexpr int kMaxDps = 15;

std::string nonEmptyString(const json& j, const std::string& pointer, std::string_view what) {
    const std::string& s = asString(j, pointer);
    if (s.empty()) {
        violate(pointer, std::string(what) + " is not empty");
    }
    return s;
}

int readDps(const json& j, const std::string& pointer) {
    const int dps = jsonio::asInt32(j, pointer);
    if (dps < 0 || dps > kMaxDps) {
        violate(pointer, "dps lies in 0..15");
    }
    return dps;
}

int readColourIndex(const json& j, const std::string& pointer) {
    const int index = jsonio::asInt32(j, pointer);
    if (index < 0) {
        violate(pointer, "a colour index is not negative");
    }
    return index;
}

// {"v": number, "unit": symbol ("" = unitless)}; the symbol is kept as written.
model::Quantityd readUnitValue(const json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    model::Quantityd q;
    q.v = jsonio::asNumber(r.required("v"), r.child("v"));
    q.symbol = asString(r.required("unit"), r.child("unit"));
    r.closed();
    return q;
}

json unitValueToJson(const model::Quantityd& q) {
    return {{"v", q.v}, {"unit", q.symbol}};
}

// warn / fault: {"min"?: {v, unit}, "max"?: {v, unit}}.
JsonDoc readLimits(const json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    for (const std::string_view key : {std::string_view("min"), std::string_view("max")}) {
        if (const json* v = r.optional(key)) {
            (void)readUnitValue(*v, r.child(key));
        }
    }
    r.closed();
    return detail::objectDoc(j, pointer);
}

ChannelOverrides::Entry readEntry(const json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    ChannelOverrides::Entry e;
    if (const json* v = r.optional("displayUnit")) {
        e.displayUnit = asString(*v, r.child("displayUnit"));
    }
    if (const json* v = r.optional("dps")) {
        e.dps = readDps(*v, r.child("dps"));
    }
    if (const json* v = r.optional("colourIndex")) {
        e.colourIndex = readColourIndex(*v, r.child("colourIndex"));
    }
    if (const json* v = r.optional("autoScale")) {
        e.autoScale = jsonio::asBool(*v, r.child("autoScale"));
    }
    if (const json* v = r.optional("scaleMin")) {
        e.scaleMin = readUnitValue(*v, r.child("scaleMin"));
    }
    if (const json* v = r.optional("scaleMax")) {
        e.scaleMax = readUnitValue(*v, r.child("scaleMax"));
    }
    if (const json* v = r.optional("interp")) {
        e.interp = detail::enumString(*v, r.child("interp"), {"Linear", "Closest", "Previous", "Next"});
    }
    if (const json* v = r.optional("warn")) {
        e.warn = readLimits(*v, r.child("warn"));
    }
    if (const json* v = r.optional("fault")) {
        e.fault = readLimits(*v, r.child("fault"));
    }
    e.unknown = detail::unknownMembers(r);
    return e;
}

json entryToJson(const ChannelOverrides::Entry& e) {
    json j = json::object();
    if (e.displayUnit) {
        j["displayUnit"] = *e.displayUnit;
    }
    if (e.dps) {
        j["dps"] = *e.dps;
    }
    if (e.colourIndex) {
        j["colourIndex"] = *e.colourIndex;
    }
    if (e.autoScale) {
        j["autoScale"] = *e.autoScale;
    }
    if (e.scaleMin) {
        j["scaleMin"] = unitValueToJson(*e.scaleMin);
    }
    if (e.scaleMax) {
        j["scaleMax"] = unitValueToJson(*e.scaleMax);
    }
    if (e.interp) {
        j["interp"] = *e.interp;
    }
    if (!e.warn.isNull()) {
        j["warn"] = jsonio::fromDoc(e.warn);
    }
    if (!e.fault.isNull()) {
        j["fault"] = jsonio::fromDoc(e.fault);
    }
    detail::putUnknown(j, e.unknown);
    return j;
}

json overridesToJson(const ChannelOverrides& doc) {
    json root = detail::newDocument("overrides", std::max(doc.schemaVersion, kOverridesKind.current));
    json channels = json::object();
    for (const auto& [name, entry] : doc.channels) {
        channels[name] = entryToJson(entry);
    }
    root["channels"] = std::move(channels);
    detail::putUnknown(root, doc.unknown);
    return root;
}

json aliasesToJson(const Aliases& doc) {
    json root = detail::newDocument("aliases", std::max(doc.schemaVersion, kAliasesKind.current));
    json rows = json::array();
    for (const model::AliasRow& row : doc.table.rows) {
        json names = json::array();
        json enabled = json::array();
        for (const model::AliasCandidate& c : row.candidates) {
            names.push_back(c.name);
            enabled.push_back(c.enabled);
        }
        json j = {{"name", row.name}, {"candidates", std::move(names)}, {"enabled", std::move(enabled)}};
        if (row.colourIndex >= 0) {
            j["colourIndex"] = row.colourIndex;
        }
        if (row.dps) {
            j["dps"] = *row.dps;
        }
        rows.push_back(std::move(j));
    }
    root["aliases"] = std::move(rows);
    detail::putUnknown(root, doc.unknown);
    return root;
}

json mappingsToJson(const Mappings& doc) {
    json root = detail::newDocument("mappings", std::max(doc.schemaVersion, kMappingsKind.current));
    json rows = json::array();
    for (const Mappings::Row& row : doc.rows) {
        json j = {{"from", row.from}, {"to", row.to}};
        if (row.unit) {
            j["unit"] = *row.unit;
        }
        if (row.unitlessOnly) {
            j["unitlessOnly"] = true;
        }
        rows.push_back(std::move(j));
    }
    root["mappings"] = std::move(rows);
    detail::putUnknown(root, doc.unknown);
    return root;
}

const json& requiredArray(ObjectReader& r, std::string_view key) {
    const json& a = r.required(key);
    if (!a.is_array()) {
        violate(r.child(key), std::string(key) + " is an array");
    }
    return a;
}

} // namespace

// ---- ChannelOverrides -------------------------------------------------------------------------------------

bool ChannelOverrides::Entry::empty() const noexcept {
    return !displayUnit && !dps && !colourIndex && !autoScale && !scaleMin && !scaleMax && !interp &&
           warn.isNull() && fault.isNull() && unknown.isNull();
}

const ChannelOverrides::Entry* ChannelOverrides::find(std::string_view channel) const {
    if (const auto it = channels.find(channel); it != channels.end()) {
        return &it->second;
    }
    for (const auto& [name, entry] : channels) {
        if (ciEquals(name, channel)) {
            return &entry;
        }
    }
    return nullptr;
}

void ChannelOverrides::set(std::string_view channel, Entry entry) {
    auto it = channels.find(channel);
    if (it == channels.end()) {
        for (auto i = channels.begin(); i != channels.end(); ++i) {
            if (ciEquals(i->first, channel)) {
                it = i;
                break;
            }
        }
    }
    if (entry.empty()) {
        if (it != channels.end()) {
            channels.erase(it);
        }
        return;
    }
    if (it != channels.end()) {
        it->second = std::move(entry);
    } else {
        channels.emplace(std::string(channel), std::move(entry));
    }
}

bool ChannelOverrides::clear(std::string_view channel) {
    const bool had = find(channel) != nullptr;
    set(channel, Entry{});
    return had;
}

Result<ChannelOverrides> ChannelOverrides::parse(std::string_view json, std::vector<std::string>& log) {
    return detail::readDocument<ChannelOverrides>(
        json, kOverridesKind, log,
        [&](const detail::json& root, const detail::DocVersion& version) -> Result<ChannelOverrides> {
            ObjectReader r(root, "");
            detail::markSchemaHeader(r);
            ChannelOverrides doc;
            doc.schemaVersion = version.write;
            const detail::json& channelsJson = r.required("channels");
            const std::string cp = r.child("channels");
            if (!channelsJson.is_object()) {
                violate(cp, "channels is an object keyed by channel name");
            }
            std::map<std::string, std::string, std::less<>> folded; // caseFold -> first spelling
            for (auto it = channelsJson.begin(); it != channelsJson.end(); ++it) {
                const std::string ep = jsonio::pointerChild(cp, it.key());
                if (it.key().empty()) {
                    violate(ep, "a channel name is not empty");
                }
                const auto [prev, inserted] = folded.emplace(caseFold(it.key()), it.key());
                if (!inserted) {
                    log.push_back("overrides: channels '" + prev->second + "' and '" + it.key() +
                                  "' differ only in case; lookups find the exact spelling first");
                }
                doc.channels.emplace(it.key(), readEntry(it.value(), ep));
            }
            doc.unknown = detail::unknownMembers(r);
            return doc;
        },
        overridesToJson);
}

Result<ChannelOverrides> ChannelOverrides::load(const std::filesystem::path& path,
                                                std::vector<std::string>& log) {
    return detail::loadDocument<ChannelOverrides>(
        path, [&](std::string_view text) { return ChannelOverrides::parse(text, log); });
}

std::string ChannelOverrides::toJson() const {
    return detail::canonicalOrEmpty(overridesToJson(*this));
}

Result<void> ChannelOverrides::save(const std::filesystem::path& path) const {
    return detail::saveCanonical(overridesToJson(*this), path);
}

// ---- Aliases ----------------------------------------------------------------------------------------------

Result<Aliases> Aliases::parse(std::string_view json, std::vector<std::string>& log) {
    return detail::readDocument<Aliases>(
        json, kAliasesKind, log,
        [&](const detail::json& root, const detail::DocVersion& version) -> Result<Aliases> {
            ObjectReader r(root, "");
            detail::markSchemaHeader(r);
            Aliases doc;
            doc.schemaVersion = version.write;
            const detail::json& rows = requiredArray(r, "aliases");
            const std::string rp = r.child("aliases");
            std::set<std::string, std::less<>> names;
            for (std::size_t i = 0; i < rows.size(); ++i) {
                const std::string ip = jsonio::pointerIndex(rp, i);
                ObjectReader ar(rows[i], ip);
                model::AliasRow row;
                row.name = nonEmptyString(ar.required("name"), ar.child("name"), "an alias name");
                if (!names.insert(caseFold(row.name)).second) {
                    violate(ar.child("name"), "alias '" + row.name +
                                                  "' is defined twice (names compare "
                                                  "case-insensitively)");
                }
                const detail::json& candidates = requiredArray(ar, "candidates");
                if (candidates.empty()) {
                    violate(ar.child("candidates"), "an alias has at least one candidate");
                }
                for (std::size_t c = 0; c < candidates.size(); ++c) {
                    model::AliasCandidate cand;
                    cand.name = nonEmptyString(candidates[c], jsonio::pointerIndex(ar.child("candidates"), c),
                                               "a candidate name");
                    row.candidates.push_back(std::move(cand));
                }
                if (const detail::json* enabled = ar.optional("enabled")) {
                    const std::string ep = ar.child("enabled");
                    if (!enabled->is_array() || enabled->size() != candidates.size()) {
                        violate(ep, "enabled is an array of one flag per candidate");
                    }
                    for (std::size_t c = 0; c < enabled->size(); ++c) {
                        row.candidates[c].enabled =
                            jsonio::asBool((*enabled)[c], jsonio::pointerIndex(ep, c));
                    }
                }
                if (const detail::json* v = ar.optional("colourIndex")) {
                    row.colourIndex = readColourIndex(*v, ar.child("colourIndex"));
                }
                if (const detail::json* v = ar.optional("dps")) {
                    row.dps = readDps(*v, ar.child("dps"));
                }
                ar.closed();
                doc.table.rows.push_back(std::move(row));
            }
            doc.unknown = detail::unknownMembers(r);
            for (const model::AliasRow& row : doc.table.rows) {
                if (auto cycle = doc.cycleThrough(row.name)) {
                    log.push_back("aliases: '" + row.name + "' closes a cycle (" + *cycle +
                                  "); the resolver stops it with an error");
                }
            }
            return doc;
        },
        aliasesToJson);
}

Result<Aliases> Aliases::load(const std::filesystem::path& path, std::vector<std::string>& log) {
    return detail::loadDocument<Aliases>(path,
                                         [&](std::string_view text) { return Aliases::parse(text, log); });
}

std::string Aliases::toJson() const {
    return detail::canonicalOrEmpty(aliasesToJson(*this));
}

Result<void> Aliases::save(const std::filesystem::path& path) const {
    return detail::saveCanonical(aliasesToJson(*this), path);
}

std::optional<std::string> Aliases::cycleThrough(std::string_view name) const {
    // The resolver's rule (02 section 4.6 rule 17): a candidate spelled like the alias being expanded reaches
    // the channel of that name; a candidate naming an alias already being expanded closes a cycle.
    std::map<std::string, const model::AliasRow*, std::less<>> byName;
    for (const model::AliasRow& row : table.rows) {
        byName.emplace(caseFold(row.name), &row);
    }
    const auto start = byName.find(caseFold(name));
    if (start == byName.end()) {
        return std::nullopt;
    }
    std::vector<const model::AliasRow*> chain;
    std::optional<std::string> found;
    // Depth-first in candidate order; the first cycle found is reported.
    auto visit = [&](auto&& self, const model::AliasRow* alias) -> void {
        chain.push_back(alias);
        for (const model::AliasCandidate& cand : alias->candidates) {
            if (found) {
                break;
            }
            const std::string key = caseFold(cand.name);
            if (key == caseFold(alias->name)) {
                continue; // the channel of that name, not the alias itself
            }
            const auto next = byName.find(key);
            if (next == byName.end()) {
                continue;
            }
            const auto again = std::find(chain.begin(), chain.end(), next->second);
            if (again != chain.end()) {
                std::string text;
                for (auto it = again; it != chain.end(); ++it) {
                    text += (*it)->name + " -> ";
                }
                found = text + next->second->name;
                break;
            }
            self(self, next->second);
        }
        chain.pop_back();
    };
    visit(visit, start->second);
    return found;
}

// ---- Mappings ---------------------------------------------------------------------------------------------

Result<Mappings> Mappings::parse(std::string_view json, std::vector<std::string>& log) {
    return detail::readDocument<Mappings>(
        json, kMappingsKind, log,
        [&](const detail::json& root, const detail::DocVersion& version) -> Result<Mappings> {
            ObjectReader r(root, "");
            detail::markSchemaHeader(r);
            Mappings doc;
            doc.schemaVersion = version.write;
            const detail::json& rowsJson = requiredArray(r, "mappings");
            const std::string rp = r.child("mappings");
            std::set<std::string, std::less<>> froms;
            for (std::size_t i = 0; i < rowsJson.size(); ++i) {
                ObjectReader mr(rowsJson[i], jsonio::pointerIndex(rp, i));
                Row row;
                row.from = nonEmptyString(mr.required("from"), mr.child("from"), "a logged name");
                if (!froms.insert(caseFold(row.from)).second) {
                    violate(mr.child("from"), "'" + row.from +
                                                  "' is mapped twice (names compare "
                                                  "case-insensitively)");
                }
                row.to = nonEmptyString(mr.required("to"), mr.child("to"), "an analysis name");
                if (const detail::json* u = mr.optional("unit")) {
                    row.unit = nonEmptyString(*u, mr.child("unit"), "a unit symbol");
                }
                row.unitlessOnly = detail::optBool(mr, "unitlessOnly", false);
                mr.closed();
                doc.rows.push_back(std::move(row));
            }
            doc.unknown = detail::unknownMembers(r);
            return doc;
        },
        mappingsToJson);
}

Result<Mappings> Mappings::load(const std::filesystem::path& path, std::vector<std::string>& log) {
    return detail::loadDocument<Mappings>(path,
                                          [&](std::string_view text) { return Mappings::parse(text, log); });
}

std::string Mappings::toJson() const {
    return detail::canonicalOrEmpty(mappingsToJson(*this));
}

Result<void> Mappings::save(const std::filesystem::path& path) const {
    return detail::saveCanonical(mappingsToJson(*this), path);
}

model::MappingTable Mappings::toTable(const units::UnitRegistry& units, std::vector<std::string>& log) const {
    model::MappingTable table;
    table.rows.reserve(rows.size());
    for (const Row& row : rows) {
        model::MappingRow out;
        out.from = row.from;
        out.to = row.to;
        out.unitlessOnly = row.unitlessOnly;
        if (row.unit.has_value() && !row.unit->empty()) {
            out.unit = units.find(*row.unit);
            if (!out.unit.has_value()) {
                log.push_back("mappings: unit '" + *row.unit + "' of '" + row.from +
                              "' is unknown; not stamped");
            }
        }
        table.rows.push_back(std::move(out));
    }
    return table;
}

} // namespace esses::project
