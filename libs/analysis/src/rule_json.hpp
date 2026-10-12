// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Private helpers of esses_analysis for reading rule-1 and module-settings documents with esses_base's typed
// JSON readers (violations are thrown as jsonio::JsonViolation and turned into a Result by guardJson at the
// public boundary).
#pragma once

#include <esses/base/json_nlohmann.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::analysis::detail {

inline std::vector<std::string> stringArray(const nlohmann::json& j, const std::string& pointer) {
    if (!j.is_array()) {
        jsonio::violate(pointer, "expected an array of strings");
    }
    std::vector<std::string> out;
    out.reserve(j.size());
    for (std::size_t i = 0; i < j.size(); ++i) {
        out.push_back(jsonio::asString(j[i], jsonio::pointerIndex(pointer, i)));
    }
    return out;
}

// Skips the members every document may carry besides its content.
inline void skipEnvelope(jsonio::ObjectReader& r) {
    r.skip("$schema");
    r.skip("schemaVersion");
    r.skip("$comment");
}

// m/s per unit for the speed units documents may name when no unit registry is at hand (rule documents);
// nullopt for any other spelling.
inline std::optional<double> speedUnitToMps(std::string_view unit) noexcept {
    if (unit == "m/s") {
        return 1.0;
    }
    if (unit == "km/h" || unit == "kph") {
        return 1.0 / 3.6;
    }
    if (unit == "mph") {
        return 0.44704;
    }
    if (unit == "kn" || unit == "kt" || unit == "knot") {
        return 1852.0 / 3600.0;
    }
    if (unit == "ft/s") {
        return 0.3048;
    }
    return std::nullopt;
}

} // namespace esses::analysis::detail
