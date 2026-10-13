// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The maths document model (maths-1; 03 section 4.1), M1 subset: a module holds items, and M1 needs only
// items of kind Builtin (the built-in module registered per source, D-016). Expression, Filter, ScaleOffset,
// the tables, constants, enumerations and MathModuleIo arrive with the language in M2 and extend these
// structs.
#pragma once

#include <esses/model/opaque_json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace esses::maths {

enum class ItemKind : std::uint8_t {
    Expression,
    Filter,
    ScaleOffset,
    Table2D,
    Table3D,
    Builtin,
    Plugin,
    Regression
};

struct MathItem {
    ItemKind kind = ItemKind::Expression;
    std::string name; // output channel name; for a builtin item the module id
    bool enabled = true;
    std::string comment;
    // Builtin (03 section 9): {"kind": "builtin", "module": id, "version": n, "enabled", "settings": {...}}
    std::string module;
    int moduleVersion = 0;
    model::OpaqueJson settings;
};

struct MathModule {
    std::string id; // "user", "esses-circuit", "esses.corrected_speed_distance", "~transient"
    bool enabled = true;
    std::optional<std::string> condition; // M2
    std::vector<MathItem> items;
};

} // namespace esses::maths
