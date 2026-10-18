// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// UnitMappingProfile (05 section 8.2, 02 section 6.3, D-028, D-029; schema unitmap-1,
// <Project>/channels/unitmaps/<importerStem>.json): the user's answers to the Mapping panel. Rules map
// (logged name, vendor unit) to (quantity, unit) and optionally an analysis name, which becomes a project
// Mapping (D-013) rather than a rename inside the importer. Rules are tried most specific first
// (nameAndUnit > nameOnly > unitOnly); names compare NFKC case-folded; a vendor unit that already resolves
// through UnitRegistry needs no rule. A profile is complete for a file when every channel has a quantity and
// unit afterwards, in which case the Mapping panel is skipped silently (AC-M2-11). This class is the
// unitmap-1 document type (load/save with canonical bytes, AC-M2-07).
#pragma once

#include <esses/base/result.hpp>
#include <esses/model/opaque_json.hpp>
#include <esses/units/unit_registry.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::formats {

class UnitMappingProfile {
public:
    static constexpr std::string_view kSchemaId = "urn:esses:unitmap-1";

    enum class Match : std::uint8_t { NameOnly, UnitOnly, NameAndUnit }; // "nameOnly" | "unitOnly" | ...
    struct Rule {
        Match match = Match::NameAndUnit;
        std::string name;       // logged name (NameOnly, NameAndUnit)
        std::string vendorUnit; // the file's spelling (UnitOnly, NameAndUnit)
        std::string quantity;   // target quantity name
        std::string unit;       // target unit symbol
        std::string analysisName;
        friend bool operator==(const Rule&, const Rule&) = default;
    };
    struct Resolution {
        units::QuantityId quantity = units::kUnitless;
        units::UnitId unit = units::kNoUnit;
        std::optional<std::string> analysisName;
        const Rule* rule = nullptr;
    };

    std::string importer; // importer id the map belongs to
    std::vector<Rule> rules;
    model::OpaqueJson unknown; // unrecognised top-level members, preserved

    static Result<UnitMappingProfile> parse(std::string_view json, std::vector<std::string>& log);
    static Result<UnitMappingProfile> load(const std::filesystem::path& path, std::vector<std::string>& log);
    Result<std::string> toJson() const;                         // canonical text
    Result<void> save(const std::filesystem::path& path) const; // atomic

    // The most specific rule for a channel, resolved against the registry; nullopt when none applies or its
    // quantity/unit is unknown to the registry.
    std::optional<Resolution> resolve(std::string_view loggedName, std::string_view vendorUnit,
                                      const units::UnitRegistry& units) const;

    // The file stem of an importer id: "text.profile:generic.csv" -> "generic.csv", "motec.ld" -> "motec.ld"
    // (':' is not allowed in Windows file names; 05 section 8.2).
    static std::string fileStem(std::string_view importerId);
};

} // namespace esses::formats
