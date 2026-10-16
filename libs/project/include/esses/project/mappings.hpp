// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Mappings (02 sections 4.6 and 6.3, D-013; schema mappings-1, <Project>/channels/mappings.json): logged name
// -> analysis name renames, optionally stamping a unit (unitlessOnly: only on a channel without one). The
// Mapping panel's analysis names and the Channel Editor's Mappings dialog write it. The document converts to
// esses_model's Qt-free model::MappingTable on load and on every edit; unit symbols are kept as written so a
// canonical document round-trips byte-identically (AC-M2-07).
#pragma once

#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/model/resolver.hpp>
#include <esses/units/unit_registry.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::project {

class Mappings {
public:
    static constexpr std::string_view kSchemaId = "urn:esses:mappings-1";

    struct Row {
        std::string from;
        std::string to;
        std::optional<std::string> unit; // symbol as written
        bool unitlessOnly = false;
        friend bool operator==(const Row&, const Row&) = default;
    };

    std::vector<Row> rows; // file order
    int schemaVersion = 1;
    JsonDoc unknown; // unrecognised top-level members, preserved

    static Result<Mappings> parse(std::string_view json, std::vector<std::string>& log);
    static Result<Mappings> load(const std::filesystem::path& path, std::vector<std::string>& log);
    std::string toJson() const; // canonical text
    Result<void> save(const std::filesystem::path& path) const;

    // The resolver's table; a unit symbol the registry does not know is left unstamped and logged.
    model::MappingTable toTable(const units::UnitRegistry& units, std::vector<std::string>& log) const;

    friend bool operator==(const Mappings&, const Mappings&) = default;
};

} // namespace esses::project
