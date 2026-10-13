// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The maths document model (maths-1; 03 sections 4.1-4.3, D-022, D-024): a module holds constants,
// enumerations and items, an optional enable condition over the source's details, and its origin (global
// module of the project, local sidecar maths, transient filters, built-in registration). MathModuleIo
// (math_module_io.hpp) reads and writes the JSON document; esses_project validates it against SchemaRegistry
// first (03 section 1).
#pragma once

#include <esses/maths/math_item.hpp>
#include <esses/model/opaque_json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace esses::maths {

struct MathConstant {
    std::string name;
    UnitValue value;
    bool enabled = true;
    std::string comment;
    friend bool operator==(const MathConstant&, const MathConstant&) = default;
};

struct MathEnumeration {
    std::string name;
    std::vector<std::pair<std::string, std::int64_t>> entries; // JSON [{"name", "value"}], duplicates refused
    friend bool operator==(const MathEnumeration&, const MathEnumeration&) = default;
};

struct MathModule {
    enum class Origin : std::uint8_t { Global, Local, Transient, Builtin };

    static constexpr std::string_view kLocalId = "local";          // sidecar localMaths (02 section 5.1)
    static constexpr std::string_view kTransientId = "~transient"; // never saved (03 section 4.2)

    std::string id; // "user", "esses-circuit", "local", "~transient", "esses.corrected_speed_distance"
    bool enabled = true;
    std::optional<std::string> condition; // expression over details, a Bool scalar (03 section 4.3 rule 8)
    std::string precision = "single";     // "single" | "double" (03 section 6.5)
    std::vector<MathConstant> constants;
    std::vector<MathEnumeration> enumerations;
    std::vector<MathItem> items;
    Origin origin = Origin::Global; // not persisted: set by whoever loads the module
    model::OpaqueJson unknown;      // unrecognised top-level members, preserved (02 section 6.1)

    friend bool operator==(const MathModule&, const MathModule&) = default;
};

} // namespace esses::maths
