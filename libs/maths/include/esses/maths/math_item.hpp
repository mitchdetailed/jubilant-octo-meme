// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathItem and the value types it is built from (maths-1; 03 sections 4.1 and 4.4, D-010, D-019, D-024). One
// item is one output channel definition of a module: an expression, a simple Filter, a Scale & Offset, a 2D
// or (v1.0) 3D table, a built-in module reference, or a preserved plugin/regression item. MathModuleIo
// (math_module_io.hpp) reads and writes the JSON form; numbers carry their unit (UnitValue, D-010).
//
// M2 contract (07 section 5.1): every member of 03 section 4.1 exists here. The M1 member `module` is spelled
// `moduleId` as 03 section 4.1 writes it, and moduleVersion defaults to 1 like the maths-1 document.
#pragma once

#include <esses/base/rate.hpp>
#include <esses/model/opaque_json.hpp>
#include <esses/store/types.hpp>
#include <esses/units/unit_registry.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace esses::maths {

// A persisted number with its unit: {"v": 8000, "unit": "rpm"} (D-010).
struct UnitValue {
    double v = 0.0;
    units::UnitId unit = units::kNoUnit;
    friend bool operator==(const UnitValue&, const UnitValue&) = default;
};

struct DisplayDefaults {                 // 03 section 4.1
    units::UnitId unit = units::kNoUnit; // kNoUnit: inherit (Corr Speed: the first input's display unit)
    int dps = 2;
    int colourIndex = 0;
    friend bool operator==(const DisplayDefaults&, const DisplayDefaults&) = default;
};

// "auto" | {num, den} | a plain Hz number (normalised to {hz, 1} on load; 03 section 6.4 rule 1).
struct RateSpec {
    bool automatic = true;
    Rate rate{};
    friend bool operator==(const RateSpec&, const RateSpec&) = default;
};

// table2d / table3d (03 section 4.4): breakpoints ascending (else E-TABLE-AXIS), at least 2 per axis;
// `values` is len(x) for table2d and len(x) x len(y) row-major for table3d (v1.0).
struct TableSpec {
    enum class Interp : std::uint8_t { Linear, Next, Previous }; // "linear" | "next" | "previous"
    enum class OutOfRange : std::uint8_t { Clamp, Invalid };     // "clamp" (default) | "invalid"
    struct Axis {
        std::string expr; // a full expression ("'Damper Pos FL'[mm]")
        units::UnitId unit = units::kNoUnit;
        std::vector<double> breakpoints;
        friend bool operator==(const Axis&, const Axis&) = default;
    };
    Axis x;                // table2d "axis", table3d "x"
    std::optional<Axis> y; // table3d only
    units::UnitId valuesUnit = units::kNoUnit;
    std::vector<double> values; // flattened; row-major for table3d
    bool rowMajor = true;
    Interp interpolation = Interp::Linear;
    OutOfRange outOfRange = OutOfRange::Clamp;
    friend bool operator==(const TableSpec&, const TableSpec&) = default;
};

enum class ItemKind : std::uint8_t {
    Expression,  // "expression"
    Filter,      // "filter"
    ScaleOffset, // "scale_offset"
    Table2D,     // "table2d"
    Table3D,     // "table3d" (v1.0; loads with E-KIND-UNSUPPORTED before)
    Builtin,     // "builtin"
    Plugin,      // "plugin" (post-v1.0; E-PLUGIN-UNAVAILABLE, preserved)
    Regression   // "regression" (v1.0; E-KIND-UNSUPPORTED, preserved)
};

struct MathItem {
    ItemKind kind = ItemKind::Expression;
    std::string name; // output channel name (any character except '); Filter/ScaleOffset: the input channel
    bool enabled = true;
    std::string comment;

    // Expression (and the display/result fields of Filter, ScaleOffset and the tables)
    std::string expr;
    RateSpec rate;
    units::QuantityId quantity = units::kUnitless;
    units::UnitId resultUnit = units::kNoUnit;
    DisplayDefaults display;
    store::Interp interp = store::Interp::Linear;
    std::optional<std::string> enumeration;
    struct Applies {
        bool logged = true;
        bool telemetry = false; // stored, unused before v1.1
        friend bool operator==(const Applies&, const Applies&) = default;
    } applies;

    // Filter (simple Filter = smooth on the chain input, 03 section 4.4)
    struct Window {
        std::optional<int> points;
        std::optional<double> seconds;
        friend bool operator==(const Window&, const Window&) = default;
    } window;

    // ScaleOffset: 'N' * scale + offset (offset applied as a delta in the input's base unit)
    double scale = 1.0;
    UnitValue offset;

    // Table2D / Table3D
    TableSpec table;

    // Builtin / Plugin: {"kind": "builtin", "module": id, "version": n, "settings": {...}}
    std::string moduleId;
    int moduleVersion = 1;
    model::OpaqueJson settings;

    std::optional<std::string> precision; // "single" | "double"; the module's precision otherwise
    model::OpaqueJson xI2;                // JSON key "x-i2" (D-019): imported attributes not understood

    friend bool operator==(const MathItem&, const MathItem&) = default;
};

} // namespace esses::maths
