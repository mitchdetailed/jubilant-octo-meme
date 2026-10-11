// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelResolver (02 section 4.6; D-013, D-132 (f)): turns a channel name as written in a workbook, a maths
// expression or an override into a channel of one layer, in D-013's order
//     Mappings (from -> to) -> Aliases (ordered candidates) -> canonical name (case-folded)
//     -> logged name (case-folded) -> Missing
// and records every step in a ResolutionTrace for the Channel Status window.
//
// Aliases: candidates are tried in order, disabled ones skipped. Each candidate is resolved through the whole
// resolver except the alias being expanded, so a candidate spelled like its own alias reaches the logged or
// maths channel of that name, and a candidate naming another alias follows it. Revisiting an alias of the
// chain stops with "Error: alias cycle 'A' -> 'B' -> 'A'" (no loop). A candidate that is Missing or whose
// channel has status Error is skipped (W-ALIAS-FALLBACK). The result carries an Alias descriptor with the
// alias's own colour index and dps.
//
// The resolver holds Qt-free POD tables; esses_project's Mappings / Aliases documents (layer 2) convert to
// them on load and on every edit.
#pragma once

#include <esses/model/channel.hpp>
#include <esses/model/ids.hpp>
#include <esses/model/layer.hpp>
#include <esses/units/unit_registry.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace esses::model {

struct ResolutionStep {
    enum class Kind : std::uint8_t { Dictionary, Mapping, Alias, CaseFold, LoggedName, Missing };
    Kind kind = Kind::Missing;
    std::string from;
    std::string to;
    std::string note; // "exact", "renamed at load", "skipped: missing", "Error: alias cycle ...", ...

    friend bool operator==(const ResolutionStep&, const ResolutionStep&) = default;
};
// "dictionary", "mapping", "alias", "case-fold", "logged name", "missing"
std::string_view resolutionStepKindName(ResolutionStep::Kind k) noexcept;

struct ResolutionTrace {
    std::string requested;
    std::vector<ResolutionStep> steps;
    std::optional<ChannelId> result; // id of the channel found (the aliased channel for an alias)
    std::string unitLabel;           // the unit of the result when the resolver has a registry

    bool resolved() const noexcept { return result.has_value(); }
    // "'Wheel Speed' → alias → 'Wheel Speed FL' (km/h)"; skipped candidates and exact matches are not shown,
    // a failure ends with " → missing" and the error note when there is one.
    std::string summary() const;
};

struct MappingRow {
    std::string from; // logged spelling, matched by caseFold
    std::string to;   // analysis name
    std::optional<units::UnitId> unit;
    bool unitlessOnly = false; // stamp the unit only on channels that have none
};
struct MappingTable {
    std::vector<MappingRow> rows;
};

struct AliasCandidate {
    std::string name;
    bool enabled = true;
};
struct AliasRow {
    std::string name; // matched by caseFold
    std::vector<AliasCandidate> candidates;
    int colourIndex = -1;
    std::optional<int> dps; // D-074; nullopt = the resolved channel's dps
};
struct AliasTable {
    std::vector<AliasRow> rows;
};

class ChannelResolver {
public:
    explicit ChannelResolver(MappingTable mappings = {}, AliasTable aliases = {},
                             const units::UnitRegistry* registry = nullptr);

    struct Resolution {
        const Channel* channel = nullptr; // the logged / maths / ... channel found; null when unresolved
        ResolutionTrace trace;
        std::optional<ChannelDescriptor> alias; // set when the name resolved through an alias
    };
    // `source` only feeds the Alias descriptor's ChannelId (channelIdOther(Alias, source, layer, alias)).
    Resolution resolve(std::string_view name, const DataLayer& layer, const SourceId& source = {}) const;

    const MappingRow* findMapping(std::string_view from) const;
    const AliasRow* findAlias(std::string_view name) const;
    const MappingTable& mappings() const noexcept { return mappings_; }
    const AliasTable& aliases() const noexcept { return aliases_; }

private:
    struct Outcome;
    Outcome resolveName(std::string_view name, const DataLayer& layer, const AliasRow* excluded,
                        std::vector<const AliasRow*>& chain, std::vector<ResolutionStep>& steps) const;
    void lookupChannel(std::string_view name, const DataLayer& layer, Outcome& out,
                       std::vector<ResolutionStep>& steps) const;

    MappingTable mappings_;
    AliasTable aliases_;
    const units::UnitRegistry* registry_ = nullptr;
    std::unordered_map<std::string, std::size_t> mappingIndex_; // caseFold(from) -> row (first wins)
    std::unordered_map<std::string, std::size_t> aliasIndex_;   // caseFold(name) -> row (first wins)
};

// The Alias descriptor of 02 section 4.6 rule 17: the target's descriptor renamed to the alias, source Alias,
// the alias's colour index and dps (else the target's), no source encoding, id channelIdOther(Alias, ...).
ChannelDescriptor makeAliasDescriptor(const ChannelDescriptor& target, const AliasRow& alias,
                                      const SourceId& source, std::string_view layer);

// Load-time Mappings (02 section 4.6 rule 16): every channel whose loggedName matches a row's `from`
// (case-folded) is renamed to `to`; when the row has a unit (and, with unitlessOnly, the channel has none)
// the unit is stamped, the quantity follows from the registry and source becomes Mapped with id
// channelIdOther(Mapped, ...); otherwise source stays Logged. Rebuilds the layer index.
struct MappingOutcome {
    std::uint32_t channel = 0;
    std::string from;
    std::string to;
    bool unitStamped = false;
};
std::vector<MappingOutcome> applyMappings(DataLayer& layer, const MappingTable& mappings,
                                          const SourceId& source,
                                          const units::UnitRegistry* registry = nullptr);

} // namespace esses::model
