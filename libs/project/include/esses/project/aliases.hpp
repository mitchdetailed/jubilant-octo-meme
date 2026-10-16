// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Aliases (02 sections 4.6 and 6.3, D-013, D-132; schema aliases-1, <Project>/channels/aliases.json): a name
// that resolves to the first usable of an ordered list of candidates (each with its own enable flag), with
// the alias's own colour index and DPS. The document converts to esses_model's Qt-free model::AliasTable on
// load and on every edit (esses_model never includes an esses_project header, 01 section 2.2 rule 1).
// Circular alias references are refused by the editor (06 section 13.1 item 7) and stopped by ChannelResolver
// at run time.
#pragma once

#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/model/resolver.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::project {

class Aliases {
public:
    static constexpr std::string_view kSchemaId = "urn:esses:aliases-1";

    model::AliasTable table; // rows in file order; candidates with their enable flags
    int schemaVersion = 1;
    JsonDoc unknown; // unrecognised top-level members, preserved

    static Result<Aliases> parse(std::string_view json, std::vector<std::string>& log);
    static Result<Aliases> load(const std::filesystem::path& path, std::vector<std::string>& log);
    std::string toJson() const; // canonical text
    Result<void> save(const std::filesystem::path& path) const;

    // The alias chain `name` would expand into when it closes a cycle ("A -> B -> A"), else nullopt.
    std::optional<std::string> cycleThrough(std::string_view name) const;
};

} // namespace esses::project
