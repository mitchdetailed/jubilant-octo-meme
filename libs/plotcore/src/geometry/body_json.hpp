// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Internal JSON helpers of the component bodies (src/geometry/): the shared definitions of the component
// schemas (unitValue, scaleSpec, colourBands, colour) read with esses_base's typed readers (a violation
// throws jsonio::JsonViolation, turned into a Result by jsonio::guardJson at the public parse functions), and
// the "typed members written over `rest`" rule every body's toJson() follows (TimeDistanceBody's rule: a
// member the source had is replaced, a member it lacked is added only when it differs from its default, so
// parse(x).toJson() == x for every canonical x).
#pragma once

#include <esses/analysis/band_axis.hpp>
#include <esses/base/json_doc.hpp>
#include <esses/base/json_nlohmann.hpp>
#include <esses/plotcore/component_bodies.hpp>

#include <nlohmann/json.hpp>

#include <span>
#include <string>
#include <vector>

namespace esses::plot::detail {

analysis::UnitValue readUnitValue(const nlohmann::json& j, const std::string& pointer);
nlohmann::json unitValueJson(const analysis::UnitValue& value);
ScaleSpec readScaleSpec(const nlohmann::json& j, const std::string& pointer);
nlohmann::json scaleSpecJson(const ScaleSpec& scale);
std::vector<ColourBand> readColourBands(const nlohmann::json& j, const std::string& pointer);
nlohmann::json colourBandsJson(std::span<const ColourBand> bands);
Rgba readColour(const nlohmann::json& j, const std::string& pointer);

// The JSON of a BandAxis member: `old` (the member as the source wrote it) when it reads back as `axis`, so
// an unchanged axis keeps its form (BandAxis::toJson writes every member), else axis.toJson().
nlohmann::json bandAxisJson(const nlohmann::json* old, const analysis::BandAxis& axis);

// The object `rest` holds (an empty object for anything else).
nlohmann::json mergeOver(const JsonDoc& rest);
// `j` as a JsonDoc; `fallback` when it cannot be written (a non-finite number).
JsonDoc docOf(const nlohmann::json& j, const JsonDoc& fallback);

// obj[key] = value when obj has the member or value differs from its default.
template <class T> void put(nlohmann::json& obj, const char* key, const T& value, const T& defaultValue) {
    if (obj.contains(key) || !(value == defaultValue)) {
        obj[key] = value;
    }
}

// The object member `key` of `obj` (an empty object when absent or not an object).
inline nlohmann::json childObject(const nlohmann::json& obj, const char* key) {
    const auto it = obj.find(key);
    return it != obj.end() && it->is_object() ? *it : nlohmann::json::object();
}

// Stores `child` at obj[key] when obj had the member or `child` is not empty.
inline void putObject(nlohmann::json& obj, const char* key, nlohmann::json child) {
    if (obj.contains(key) || !child.empty()) {
        obj[key] = std::move(child);
    }
}

} // namespace esses::plot::detail
