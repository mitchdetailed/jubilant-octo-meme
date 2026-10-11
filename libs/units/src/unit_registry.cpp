// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// UnitRegistry lookups, relations and conversions. Loading lives in registry_loader.cpp.
#include "spelling.hpp"

#include <esses/base/assert.hpp>
#include <esses/units/unit_registry.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace esses::units {

namespace {

ErrorContext detailContext(const char* detail) {
    ErrorContext ctx;
    ctx.detail = std::string(detail);
    return ctx;
}

} // namespace

std::string_view matchKindName(MatchKind kind) noexcept {
    switch (kind) {
    case MatchKind::Symbol:
        return "symbol";
    case MatchKind::Alias:
        return "alias";
    case MatchKind::CaseFolded:
        return "case-folded";
    case MatchKind::Normalised:
        return "normalised";
    }
    return "unknown";
}

std::optional<UnitId> UnitRegistry::find(std::string_view symbolOrAlias) const {
    if (const auto m = match(symbolOrAlias)) {
        return m->unit;
    }
    return std::nullopt;
}

std::optional<UnitId> UnitRegistry::find(std::string_view symbolOrAlias, QuantityId within) const {
    if (const auto m = match(symbolOrAlias, within)) {
        return m->unit;
    }
    return std::nullopt;
}

std::optional<UnitMatch> UnitRegistry::match(std::string_view symbolOrAlias,
                                             std::optional<QuantityId> within) const {
    const std::string key = spelling::collapse(symbolOrAlias);
    if (within) {
        if (!containsQuantity(*within)) {
            return std::nullopt;
        }
        return matchScoped(key, *within);
    }
    if (const auto it = exact_.find(key); it != exact_.end()) {
        const UnitId id = it->second;
        return UnitMatch{id, units_[id].symbol == key ? MatchKind::Symbol : MatchKind::Alias};
    }
    if (const auto it = folded_.find(spelling::folded(key)); it != folded_.end()) {
        return UnitMatch{it->second, MatchKind::CaseFolded};
    }
    if (const auto it = normalised_.find(spelling::normalised(key)); it != normalised_.end()) {
        return UnitMatch{it->second, MatchKind::Normalised};
    }
    return std::nullopt;
}

std::optional<UnitMatch> UnitRegistry::matchScoped(std::string_view collapsed, QuantityId within) const {
    const std::span<const UnitId> candidates = unitsOfQuantity_[within];
    for (const UnitId id : candidates) {
        for (const std::string& s : keys_[id].exact) {
            if (s == collapsed) {
                return UnitMatch{id, units_[id].symbol == s ? MatchKind::Symbol : MatchKind::Alias};
            }
        }
    }
    const std::string f = spelling::folded(collapsed);
    for (const UnitId id : candidates) {
        for (const std::string& s : keys_[id].folded) {
            if (s == f) {
                return UnitMatch{id, MatchKind::CaseFolded};
            }
        }
    }
    const std::string n = spelling::normalised(collapsed);
    for (const UnitId id : candidates) {
        for (const std::string& s : keys_[id].normalised) {
            if (s == n) {
                return UnitMatch{id, MatchKind::Normalised};
            }
        }
    }
    return std::nullopt;
}

const Unit& UnitRegistry::unit(UnitId id) const {
    ESSES_ASSERT_MSG(contains(id), "UnitRegistry::unit: unknown unit id");
    return contains(id) ? units_[id] : units_[kNoUnit];
}

const Quantity& UnitRegistry::quantity(QuantityId id) const {
    ESSES_ASSERT_MSG(containsQuantity(id), "UnitRegistry::quantity: unknown quantity id");
    return containsQuantity(id) ? quantities_[id] : quantities_[kUnitless];
}

QuantityId UnitRegistry::quantityOf(UnitId id) const {
    return unit(id).qty;
}

std::optional<QuantityId> UnitRegistry::quantityByName(std::string_view name) const {
    if (const auto it = quantityNames_.find(spelling::folded(name)); it != quantityNames_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::span<const UnitId> UnitRegistry::unitsOf(QuantityId id) const {
    ESSES_ASSERT_MSG(containsQuantity(id), "UnitRegistry::unitsOf: unknown quantity id");
    if (!containsQuantity(id)) {
        return {};
    }
    return unitsOfQuantity_[id];
}

Result<Converter> UnitRegistry::converter(UnitId from, UnitId to) const {
    if (!contains(from) || !contains(to)) {
        return fail(ErrorCode::Invalid, "unit conversion with an unknown unit id", detailContext("unit.id"));
    }
    if (from == to) {
        return Converter{};
    }
    const Unit& uf = units_[from];
    const Unit& ut = units_[to];
    const Quantity& qf = quantities_[uf.qty];
    const Quantity& qt = quantities_[ut.qty];
    if (qf.id != qt.id && (qf.family != qt.family || qf.dim != qt.dim)) {
        return fail(ErrorCode::Invalid,
                    "cannot convert '" + uf.symbol + "' (" + qf.name + ") to '" + ut.symbol + "' (" +
                        qt.name + "): the unit families differ",
                    detailContext("unit.family"));
    }
    const Affine& f = affine_[from];
    const Affine& t = affine_[to];
    return Converter{t.fromA * f.toA, t.fromA * f.toB + t.fromB};
}

Converter UnitRegistry::toBase(UnitId id) const {
    ESSES_ASSERT_MSG(contains(id), "UnitRegistry::toBase: unknown unit id");
    if (!contains(id)) {
        return Converter{};
    }
    return Converter{affine_[id].toA, affine_[id].toB};
}

Converter UnitRegistry::fromBase(UnitId id) const {
    ESSES_ASSERT_MSG(contains(id), "UnitRegistry::fromBase: unknown unit id");
    if (!contains(id)) {
        return Converter{};
    }
    return Converter{affine_[id].fromA, affine_[id].fromB};
}

std::optional<QuantityId> UnitRegistry::deltaOf(QuantityId id) const {
    if (!containsQuantity(id)) {
        return std::nullopt;
    }
    return quantities_[id].deltaFamily;
}

std::optional<QuantityId> UnitRegistry::absoluteOf(QuantityId id) const {
    if (!containsQuantity(id)) {
        return std::nullopt;
    }
    return absoluteOf_[id];
}

std::optional<QuantityId> UnitRegistry::byDimension(const Dimension& dim, std::string_view familyHint) const {
    if (dim.isUnknown()) {
        return std::nullopt;
    }
    const std::string hint = familyHint.empty() ? std::string() : spelling::folded(familyHint);
    std::optional<QuantityId> firstNonDelta;
    std::optional<QuantityId> first;
    for (const Quantity& q : quantities_) {
        if (q.dim != dim) {
            continue;
        }
        if (!hint.empty() && (spelling::folded(q.family) == hint || spelling::folded(q.name) == hint)) {
            return q.id;
        }
        if (!first) {
            first = q.id;
        }
        if (!firstNonDelta && !q.isDelta) {
            firstNonDelta = q.id;
        }
    }
    return firstNonDelta ? firstNonDelta : first;
}

} // namespace esses::units
