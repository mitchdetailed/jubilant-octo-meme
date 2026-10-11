// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The shipped table data/units.json: AC-M0-09 (07 section 3: round trip x -> base -> x within 1e-9 relative
// for every unit, delta <-> absolute refused, every alias resolves case-insensitively), 02 section 8 item 2
// (no spelling shared between units, the design documents' spellings resolve, AFR resolves to lambda) and the
// 02 section 2.3 acceptance (load time, round-trip property test), plus schema validation of the file.
#include "units_test_support.hpp"

#include <esses/units/unit_registry.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace esses;
using namespace esses::units;
using test::id;
using test::shipped;

namespace {

class CollectingErrors final : public nlohmann::json_schema::basic_error_handler {
public:
    void error(const nlohmann::json::json_pointer& ptr, const nlohmann::json& instance,
               const std::string& message) override {
        basic_error_handler::error(ptr, instance, message);
        messages.push_back(ptr.to_string() + ": " + message);
    }
    std::vector<std::string> messages;
};

std::string asciiLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
    return s;
}

std::string asciiUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; });
    return s;
}

std::vector<std::string> spellingsOf(const Unit& u) {
    std::vector<std::string> out{u.symbol};
    out.insert(out.end(), u.aliases.begin(), u.aliases.end());
    if (!u.display.empty()) {
        out.push_back(u.display);
    }
    return out;
}

// Distance in units in the last place between two finite doubles of the same sign.
std::uint64_t ulps(double a, double b) {
    if (a == b) {
        return 0;
    }
    if (std::signbit(a) != std::signbit(b)) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const auto ia = std::bit_cast<std::int64_t>(a);
    const auto ib = std::bit_cast<std::int64_t>(b);
    return static_cast<std::uint64_t>(ia > ib ? ia - ib : ib - ia);
}

} // namespace

TEST_CASE("units.json: validates against schemas/units-1.schema.json", "[unit][shipped]") {
    const auto schema = nlohmann::json::parse(test::readFile(test::schemaPath()));
    const auto table = nlohmann::json::parse(test::readFile(test::shippedTablePath()));
    nlohmann::json_schema::json_validator validator;
    validator.set_root_schema(schema);
    CollectingErrors errors;
    validator.validate(table, errors);
    INFO("first schema error: " << (errors.messages.empty() ? std::string("none") : errors.messages.front()));
    CHECK(errors.messages.empty());
    CHECK_FALSE(static_cast<bool>(errors));
}

TEST_CASE("units.json: structure of the shipped table", "[unit][shipped]") {
    const UnitRegistry& reg = shipped();
    REQUIRE(reg.quantityCount() >= 25);
    REQUIRE(reg.unitCount() >= 100);
    CHECK(reg.quantity(kUnitless).name == "Unitless");
    CHECK(reg.unit(kNoUnit).symbol.empty());
    CHECK(reg.unit(kNoUnit).qty == kUnitless);
    for (const Quantity& q : reg.quantities()) {
        INFO("quantity " << q.name);
        CHECK(q.dim.isKnown());
        const auto own = reg.unitsOf(q.id);
        REQUIRE_FALSE(own.empty());
        CHECK(std::find(own.begin(), own.end(), q.base) != own.end());
        CHECK(reg.unit(q.base).scalar == 1.0);
        CHECK(reg.unit(q.base).offset == 0.0);
        CHECK(reg.toBase(q.base).identity());
        CHECK(reg.quantityByName(q.name) == q.id);
        for (const UnitId u : own) {
            CHECK(reg.unit(u).qty == q.id);
        }
        if (q.deltaFamily) {
            CHECK(reg.quantity(*q.deltaFamily).isDelta);
            CHECK(reg.quantity(*q.deltaFamily).dim == q.dim);
        }
    }
    for (const Unit& u : reg.units()) {
        INFO("unit " << u.symbol);
        CHECK((u.compat.empty() || u.compat == "legacy"));
        CHECK(std::isfinite(u.scalar));
        CHECK(u.scalar != 0.0);
        CHECK(std::isfinite(u.offset));
    }
    // The delta pairs of 02 section 2.1.
    CHECK(reg.deltaOf(*reg.quantityByName("Temperature")) == reg.quantityByName("Temperature Delta"));
    CHECK(reg.deltaOf(*reg.quantityByName("Pressure")) == reg.quantityByName("Pressure Delta"));
}

// AC-M0-09 (1): x -> base -> x for every unit. The tolerance is 1e-9 relative to the larger of |x| and the
// unit's offset, the magnitude the affine map works at (x = 1e-6 deg C passes through 273.15 K). Values span
// 1e-6 ... 1e9 in both signs, log-uniform, plus fixed edge values.
TEST_CASE("AC-M0-09: every unit round-trips x -> base -> x within 1e-9 relative",
          "[unit][shipped][property]") {
    const UnitRegistry& reg = shipped();
    std::mt19937_64 rng(test::testSeed());
    std::uniform_real_distribution<double> exponent(-6.0, 9.0);
    std::bernoulli_distribution negative(0.5);
    constexpr int kPerUnit = 20000;
    double worst = 0.0;
    std::string worstUnit;
    std::uint64_t worstUlpsScaleOnly = 0;
    for (const Unit& u : reg.units()) {
        const Converter to = reg.toBase(u.id);
        const Converter from = reg.fromBase(u.id);
        std::vector<double> values{0.0, 1.0, -1.0, u.offset, -u.offset, 1e-200, 123456.789};
        for (int k = 0; k < kPerUnit; ++k) {
            const double v = std::pow(10.0, exponent(rng));
            values.push_back(negative(rng) ? -v : v);
        }
        for (const double x : values) {
            const double back = from(to(x));
            const double scale =
                std::max({std::abs(x), std::abs(u.offset), std::numeric_limits<double>::min()});
            const double rel = std::abs(back - x) / scale;
            if (rel > worst) {
                worst = rel;
                worstUnit = u.symbol;
            }
            if (u.offset == 0.0 && x != 0.0) {
                worstUlpsScaleOnly = std::max(worstUlpsScaleOnly, ulps(back, x));
            }
            // and base -> unit -> base
            const double y = x;
            const double baseBack = to(from(y));
            const double baseScale =
                std::max({std::abs(y), std::abs(to(0.0)), std::numeric_limits<double>::min()});
            if (std::abs(baseBack - y) / baseScale > 1e-9) {
                FAIL_CHECK("base -> " << u.symbol << " -> base drifts for " << y << ": " << baseBack);
            }
        }
    }
    INFO("worst relative error " << worst << " (" << worstUnit << "), worst ulps without offset "
                                 << worstUlpsScaleOnly);
    CHECK(worst <= 1e-9);
    // 02 section 2.3 acceptance: units without an offset round-trip within about 1 ulp over these 2 million
    // values (two roundings plus the rounding of 1/scalar bound it by 2; 1 is what the shipped table shows).
    CHECK(worstUlpsScaleOnly <= 2);
}

// The same through converter(from, to) and back for every pair of units of every quantity.
TEST_CASE("AC-M0-09: unit-to-unit conversions round-trip within 1e-9 relative", "[unit][shipped][property]") {
    const UnitRegistry& reg = shipped();
    std::mt19937_64 rng(test::testSeed() ^ 0x9E3779B97F4A7C15ull);
    std::uniform_real_distribution<double> exponent(-3.0, 6.0);
    for (const Quantity& q : reg.quantities()) {
        for (const UnitId a : reg.unitsOf(q.id)) {
            for (const UnitId b : reg.unitsOf(q.id)) {
                const auto there = reg.converter(a, b);
                const auto back = reg.converter(b, a);
                REQUIRE(there.ok());
                REQUIRE(back.ok());
                const double offsets = std::max(std::abs(reg.unit(a).offset), std::abs(reg.unit(b).offset));
                for (int k = 0; k < 200; ++k) {
                    const double x = std::pow(10.0, exponent(rng)) * (k % 2 == 0 ? 1.0 : -1.0);
                    const double r = (*back)((*there)(x));
                    const double scale =
                        std::max({std::abs(x), offsets * (1.0 + std::abs((*there).a)), 1e-300});
                    if (std::abs(r - x) / scale > 1e-9) {
                        FAIL_CHECK(reg.unit(a).symbol << " -> " << reg.unit(b).symbol
                                                      << " -> back drifts for " << x << ": " << r);
                    }
                }
            }
        }
    }
}

TEST_CASE("AC-M0-09: temperature delta <-> absolute conversion is refused", "[unit][shipped]") {
    const UnitRegistry& reg = shipped();
    const QuantityId temperature = *reg.quantityByName("Temperature");
    const QuantityId delta = *reg.quantityByName("Temperature Delta");
    for (const UnitId a : reg.unitsOf(temperature)) {
        for (const UnitId d : reg.unitsOf(delta)) {
            INFO(reg.unit(a).symbol << " <-> " << reg.unit(d).symbol);
            const auto there = reg.converter(a, d);
            const auto back = reg.converter(d, a);
            REQUIRE_FALSE(there.ok());
            REQUIRE_FALSE(back.ok());
            CHECK(there.error().code == ErrorCode::Invalid);
            CHECK(there.error().context.detail == std::string("unit.family"));
            CHECK(back.error().context.detail == std::string("unit.family"));
        }
    }
    // Pressure follows the same rule (02 section 2.2 rule 2).
    const QuantityId pressure = *reg.quantityByName("Pressure");
    const QuantityId pressureDelta = *reg.quantityByName("Pressure Delta");
    for (const UnitId a : reg.unitsOf(pressure)) {
        for (const UnitId d : reg.unitsOf(pressureDelta)) {
            CHECK_FALSE(reg.converter(a, d).ok());
        }
    }
    // Within each family conversion works.
    CHECK(reg.converter(id(reg, "deg C"), id(reg, "deg F")).ok());
    CHECK(reg.converter(id(reg, "dC"), id(reg, "dF")).ok());
}

// AC-M0-09 (3): every spelling (symbol, alias, display form) resolves to its unit as written, in ASCII lower
// and upper case and padded with white space. The one exception is a case variant that is exactly a
// caseSensitive spelling of another unit (lower-case "G" is "g", the gram), which resolves to that unit by
// design.
TEST_CASE("AC-M0-09: all aliases resolve case-insensitively", "[unit][shipped]") {
    const UnitRegistry& reg = shipped();
    std::map<std::string, UnitId> exactOnly;
    for (const Unit& u : reg.units()) {
        for (const std::string& s : u.caseSensitive) {
            exactOnly.emplace(s, u.id);
        }
    }
    std::size_t checked = 0;
    for (const Unit& u : reg.units()) {
        for (const std::string& s : spellingsOf(u)) {
            const bool sensitive =
                std::find(u.caseSensitive.begin(), u.caseSensitive.end(), s) != u.caseSensitive.end();
            std::vector<std::string> variants{s, "  " + s + " "};
            if (!sensitive) {
                variants.push_back(asciiLower(s));
                variants.push_back(asciiUpper(s));
                variants.push_back(" " + asciiUpper(s) + "\t");
            }
            for (const std::string& v : variants) {
                INFO("unit '" << u.symbol << "', spelling '" << s << "', variant '" << v << "'");
                const auto other = exactOnly.find(std::string(v));
                const UnitId expected = (other != exactOnly.end() && v != s) ? other->second : u.id;
                const auto found = reg.find(v);
                REQUIRE(found.has_value());
                CHECK(*found == expected);
                ++checked;
            }
        }
    }
    CHECK(checked > 500);
    // The documented exception, explicitly.
    CHECK(reg.unit(id(reg, "g")).symbol == "g");
    CHECK(reg.unit(id(reg, "G")).symbol == "G");
}

TEST_CASE("units.json: no spelling belongs to two units (02 section 8 item 2)", "[unit][shipped]") {
    const UnitRegistry& reg = shipped();
    std::map<std::string, UnitId> exact;
    for (const Unit& u : reg.units()) {
        for (const std::string& s : spellingsOf(u)) {
            const auto [it, inserted] = exact.emplace(s, u.id);
            INFO("spelling '" << s << "'");
            CHECK((inserted || it->second == u.id));
        }
    }
    // Case-insensitive uniqueness holds for everything except the declared caseSensitive spellings.
    std::set<std::string> declared;
    for (const Unit& u : reg.units()) {
        declared.insert(u.caseSensitive.begin(), u.caseSensitive.end());
    }
    CHECK(declared == std::set<std::string>{"g"});
}

// Spellings used by the design documents (02, 03, 04, 05 examples and the 05 dictionaries) resolve to the
// expected quantity.
TEST_CASE("units.json: the design documents' spellings resolve", "[unit][shipped]") {
    const UnitRegistry& reg = shipped();
    const struct {
        const char* spelling;
        const char* quantity;
    } cases[] = {
        {"rpm", "Angular Speed"},
        {"RPM", "Angular Speed"},
        {"r/min", "Angular Speed"},
        {"1000rpm", "Angular Speed"},
        {"rpm1000", "Angular Speed"},
        {"krpm", "Angular Speed"},
        {"rad/s", "Angular Speed"},
        {"km/h", "Speed"},
        {"kph", "Speed"},
        {"KPH", "Speed"},
        {"km/hr", "Speed"},
        {"mph", "Speed"},
        {"MPH", "Speed"},
        {"mi/h", "Speed"},
        {"m/s", "Speed"},
        {"mm/s", "Speed"},
        {"m", "Distance"},
        {"km", "Distance"},
        {"mm", "Distance"},
        {"%", "Ratio"},
        {"ratio", "Ratio"},
        {"Trim", "Ratio"},
        {"%Trim", "Ratio"},
        {"s", "Time"},
        {"bar", "Pressure"},
        {"bar a", "Pressure"},
        {"kPa a", "Pressure"},
        {"kpa", "Pressure"},
        {"psi", "Pressure"},
        {"PSI", "Pressure"},
        {"kPa g", "Pressure Delta"},
        {"psi g", "Pressure Delta"},
        {"dKPA", "Pressure Delta"},
        {"kPag", "Pressure Delta"},
        {"G", "Acceleration"},
        {"m/s/s", "Acceleration"},
        {"N", "Force"},
        {"C", "Temperature"},
        {"deg C", "Temperature"},
        {"°C", "Temperature"},
        {"degC", "Temperature"},
        {"øC", "Temperature"},
        {"░C", "Temperature"},
        {"deg F", "Temperature"},
        {"K", "Temperature"},
        {"dC", "Temperature Delta"},
        {"dF", "Temperature Delta"},
        {"dK", "Temperature Delta"},
        {"lambda", "Air Fuel Ratio"},
        {"LA", "Air Fuel Ratio"},
        {"P A/F", "Air Fuel Ratio"},
        {"kg", "Mass"},
        {"deg/s", "Angular Speed"},
        {"deg", "Angle"},
        {"dBTDC", "Angle"},
        {"V", "Voltage"},
        {"N.m", "Torque"},
        {"Nm", "Torque"},
        {"J", "Energy"},
        {"Hz", "Frequency"},
        {"µs", "Time"},
        {"m³/s", "Volume Flow"},
        {"", "Unitless"},
    };
    for (const auto& c : cases) {
        INFO("'" << c.spelling << "' as " << c.quantity);
        const auto u = reg.find(c.spelling);
        REQUIRE(u.has_value());
        CHECK(reg.quantity(reg.quantityOf(*u)).name == c.quantity);
    }
    // 05 section 8.1 dictionary rows give a quantity with the unit: Acceleration + "g" is the acceleration
    // unit.
    const QuantityId accel = *reg.quantityByName("Acceleration");
    CHECK(reg.find("g", accel) == reg.find("G"));
    // Compat (legacy) units are flagged; ordinary ones are not.
    CHECK(reg.unit(id(reg, "%Trim")).isCompat());
    CHECK(reg.unit(id(reg, "1000rpm")).isCompat());
    CHECK(reg.unit(id(reg, "dBTDC")).isCompat());
    CHECK_FALSE(reg.unit(id(reg, "rpm")).isCompat());
    CHECK_FALSE(reg.unit(id(reg, "kPa a")).isCompat());
}

// 02 section 2.3 acceptance: the registry loads the full table in < 5 ms. Asserted in optimised builds only
// (best of 10 runs, so a busy machine does not fail it); debug builds report the time.
TEST_CASE("units.json: loads in under 5 ms", "[unit][shipped]") {
    const std::string text = test::readFile(test::shippedTablePath());
    double best = 1e9;
    for (int run = 0; run < 10; ++run) {
        const auto t0 = std::chrono::steady_clock::now();
        auto r = UnitRegistry::load(text);
        const auto t1 = std::chrono::steady_clock::now();
        REQUIRE(r.ok());
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
#if defined(NDEBUG) && !defined(ESSES_FORCE_ASSERTS)
    INFO("best load time " << best << " ms");
    CHECK(best < 5.0);
#else
    WARN("best load time " << best << " ms (debug build; asserted in optimised builds)");
#endif
}
