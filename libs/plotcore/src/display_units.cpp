// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The default display unit of a channel (display_units.hpp, D-211 (a)).

#include <esses/plotcore/display_units.hpp>

#include <optional>

namespace esses::plot {

namespace {

constexpr std::string_view kRatio = "Ratio";
constexpr std::string_view kPercent = "%";

} // namespace

units::UnitId defaultDisplayUnit(const units::UnitRegistry& registry, units::UnitId stored) {
    if (!registry.contains(stored)) {
        return stored;
    }
    const units::QuantityId q = registry.quantityOf(stored);
    if (!registry.containsQuantity(q)) {
        return stored;
    }
    const units::Quantity& quantity = registry.quantity(q);
    if (quantity.name == kRatio && quantity.base == stored) {
        if (const std::optional<units::UnitId> percent = registry.find(kPercent, q)) {
            return *percent;
        }
    }
    return stored;
}

units::UnitId corrDistDisplayUnit(const units::UnitRegistry& registry, std::string_view distanceOverride,
                                  std::string_view speedUnit) {
    const std::optional<units::QuantityId> length = registry.quantityByName("Distance");
    const std::optional<units::UnitId> metres =
        length ? registry.find("m", *length) : registry.find(std::string_view("m"));
    if (!length || !metres) {
        return metres.value_or(units::kNoUnit);
    }
    if (!distanceOverride.empty()) {
        if (const std::optional<units::UnitId> d = registry.find(distanceOverride, *length)) {
            return *d;
        }
    }
    if (!speedUnit.empty()) {
        if (const std::optional<units::QuantityId> speed = registry.quantityByName("Speed")) {
            if (const std::optional<units::UnitId> s = registry.find(speedUnit, *speed)) {
                const std::string_view symbol = registry.unit(*s).symbol;
                const char* follows = symbol == "km/h" ? "km" : symbol == "mph" ? "mi" : nullptr;
                if (follows != nullptr) {
                    return registry.find(follows, *length).value_or(*metres);
                }
            }
        }
    }
    return *metres;
}

units::UnitId displayUnitFor(const units::UnitRegistry& registry, units::UnitId stored,
                             std::string_view requested) {
    if (!requested.empty() && registry.contains(stored)) {
        if (const std::optional<units::UnitId> id = registry.find(requested, registry.quantityOf(stored))) {
            return *id;
        }
    }
    return defaultDisplayUnit(registry, stored);
}

} // namespace esses::plot
