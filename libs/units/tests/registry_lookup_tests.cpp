// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// UnitRegistry lookups, conversions and quantity relations on the shipped table (02 section 2.2 rules 2-4,
// 03 section 3.2): match kinds, white-space collapsing, the spelling normaliser, scoped lookups, family
// rules, byDimension / deltaOf / absoluteOf, and the release fallback for invalid ids.
#include "units_test_support.hpp"

#include <esses/base/assert.hpp>
#include <esses/units/unit_registry.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <string_view>

using namespace esses;
using namespace esses::units;
using test::id;
using test::shipped;

namespace {

std::string symbolOf(std::string_view spelling) {
    const auto found = shipped().find(spelling);
    return found ? shipped().unit(*found).symbol : std::string("<none>");
}

std::atomic<int> gAssertions{0};
void countAssertion(const char*, const char*, int, const char*) {
    ++gAssertions;
}

} // namespace

TEST_CASE("lookup: match kinds in order exact, case-folded, normalised", "[unit][lookup]") {
    const UnitRegistry& reg = shipped();
    CHECK(reg.match("km/h") == UnitMatch{id(reg, "km/h"), MatchKind::Symbol});
    CHECK(reg.match("kph") == UnitMatch{id(reg, "km/h"), MatchKind::Alias});
    CHECK(reg.match("°C") == UnitMatch{id(reg, "deg C"), MatchKind::Alias}); // the display form
    CHECK(reg.match("KPH") == UnitMatch{id(reg, "km/h"), MatchKind::CaseFolded});
    CHECK(reg.match("DEG c") == UnitMatch{id(reg, "deg C"), MatchKind::CaseFolded});
    CHECK(reg.match("ºC") == UnitMatch{id(reg, "deg C"), MatchKind::Normalised});
    CHECK_FALSE(reg.match("furlong/fortnight").has_value());
    CHECK(matchKindName(MatchKind::Symbol) == "symbol");
    CHECK(matchKindName(MatchKind::Alias) == "alias");
    CHECK(matchKindName(MatchKind::CaseFolded) == "case-folded");
    CHECK(matchKindName(MatchKind::Normalised) == "normalised");
}

TEST_CASE("lookup: white space is trimmed and collapsed", "[unit][lookup]") {
    CHECK(symbolOf("  kPa   a ") == "kPa a");
    CHECK(symbolOf("\tdeg\tC\r\n") == "deg C");
    CHECK(symbolOf("P  A/F") == "P A/F");
    CHECK(symbolOf("") == "");
    CHECK(symbolOf(" \t ") == "");
    CHECK(symbolOf("kPa\xC2\xA0"
                   "a") == "kPa a"); // a UTF-8 no-break space folds to a space
}

TEST_CASE("lookup: the spelling normaliser (02 section 2.2 rule 3)", "[unit][lookup]") {
    SECTION("degree glyphs, their mojibake and degree words") {
        CHECK(symbolOf("°F") == "deg F");
        CHECK(symbolOf("øC") == "deg C"); // code page 437 degree read as code page 1252
        CHECK(symbolOf("ØC") == "deg C");
        CHECK(symbolOf("░C") == "deg C"); // code page 1252 degree read as code page 437
        CHECK(symbolOf("ºC") == "deg C"); // masculine ordinal
        CHECK(symbolOf("˚F") == "deg F"); // ring above
        CHECK(symbolOf("℃") == "deg C");  // degree Celsius sign
        CHECK(symbolOf("℉") == "deg F");  // degree Fahrenheit sign
        CHECK(symbolOf("Degrees C") == "deg C");
        CHECK(symbolOf("degreesC") == "deg C");
        CHECK(symbolOf("DegreesF") == "deg F");
        CHECK(symbolOf("°") == "deg");
        CHECK(symbolOf("Degrees") == "deg");
        CHECK(symbolOf("°/s") == "deg/s");
        CHECK(symbolOf("degrees/s") == "deg/s");
        CHECK(symbolOf("°/s²") == "deg/s/s");
        CHECK(symbolOf("øx") == "<none>"); // the mojibake rule needs C or F
    }
    SECTION("micro, ohm, lambda, exponents and multiplication signs") {
        CHECK(symbolOf("µs") == "us"); // micro sign
        CHECK(symbolOf("μs") == "us"); // Greek mu
        CHECK(symbolOf("μL") == "ul");
        CHECK(symbolOf("Ω") == "ohm"); // ohm sign
        CHECK(symbolOf("Ω") == "ohm");
        CHECK(symbolOf("kΩ") == "kohm");
        CHECK(symbolOf("λ") == "lambda");
        CHECK(symbolOf("Λ") == "lambda");
        CHECK(symbolOf("m/s^2") == "m/s/s");
        CHECK(symbolOf("m/s²") == "m/s/s");
        CHECK(symbolOf("rad/s^2") == "rad/s/s");
        CHECK(symbolOf("kg/m^3") == "kg/m3");
        CHECK(symbolOf("kg/m³") == "kg/m3");
        CHECK(symbolOf("m^3") == "m3");
        CHECK(symbolOf("N*m") == "N.m");
        CHECK(symbolOf("N·m") == "N.m");
        CHECK(symbolOf("N⋅m") == "N.m");
        CHECK(symbolOf("lbf·ft") == "lbf.ft");
        CHECK(symbolOf("lb*ft") == "lbf.ft");
        CHECK(symbolOf("ampere*hour") == "Ah");
    }
}

TEST_CASE("lookup: g is the gram and G the acceleration unit; scoped lookups fold both", "[unit][lookup]") {
    const UnitRegistry& reg = shipped();
    const QuantityId mass = *reg.quantityByName("Mass");
    const QuantityId accel = *reg.quantityByName("Acceleration");
    CHECK(reg.quantityOf(id(reg, "g")) == mass);
    CHECK(reg.quantityOf(id(reg, "G")) == accel);
    CHECK(reg.quantityOf(id(reg, "GRAM")) == mass);
    CHECK(reg.quantityOf(id(reg, "g/s")) == *reg.quantityByName("Mass Flow"));
    // A dictionary that says Acceleration + "g" (05 section 8.1) means the acceleration unit.
    CHECK(reg.find("g", accel) == reg.find("G"));
    CHECK(reg.match("g", accel) == UnitMatch{id(reg, "G"), MatchKind::CaseFolded});
    CHECK(reg.find("G", mass) == reg.find("g"));
    CHECK(reg.match("G", accel) == UnitMatch{id(reg, "G"), MatchKind::Symbol});
    CHECK(reg.find("m/s^2", accel) == reg.find("m/s/s"));
    CHECK_FALSE(reg.find("kph", accel).has_value());
    CHECK_FALSE(reg.find("G", static_cast<QuantityId>(60000)).has_value());
}

TEST_CASE("lookup: pressures, absolute by default, gauge and differential as Pressure Delta",
          "[unit][lookup]") {
    const UnitRegistry& reg = shipped();
    const QuantityId abs = *reg.quantityByName("Pressure");
    const QuantityId delta = *reg.quantityByName("Pressure Delta");
    for (const char* s : {"kPa", "KPA", "kPaA", "bar", "Bar", "psi", "PSI", "MPa", "mbar", "mBar", "Mbar",
                          "millibar", "kilopascal", "inHg", "hPa"}) {
        INFO(s);
        CHECK(reg.quantityOf(id(reg, s)) == abs);
    }
    for (const char* s :
         {"kPa g", "kPag", "dKPA", "dkPa", "dbar", "dMPa", "psi g", "psig", "bar g", "PSI (sg)"}) {
        INFO(s);
        CHECK(reg.quantityOf(id(reg, s)) == delta);
    }
    CHECK(reg.deltaOf(abs) == delta);
    CHECK(reg.absoluteOf(delta) == abs);
    CHECK_FALSE(reg.deltaOf(delta).has_value());
    CHECK_FALSE(reg.absoluteOf(abs).has_value());
}

TEST_CASE("lookup: conversions between units of one family", "[unit][lookup]") {
    const UnitRegistry& reg = shipped();
    const auto conv = [&](std::string_view from, std::string_view to) {
        auto c = reg.converter(id(reg, from), id(reg, to));
        INFO(from << " -> " << to << ": " << (c.ok() ? std::string() : c.error().toString()));
        REQUIRE(c.ok());
        return *c;
    };
    CHECK(conv("km/h", "m/s")(36.0) == Catch::Approx(10.0).epsilon(1e-15));
    CHECK(conv("mph", "km/h")(60.0) == Catch::Approx(96.56064).epsilon(1e-15));
    CHECK(conv("deg C", "deg F")(100.0) == Catch::Approx(212.0).epsilon(1e-15));
    CHECK(conv("deg F", "K")(-459.67) == Catch::Approx(0.0).margin(1e-12));
    CHECK(conv("K", "deg C")(0.0) == Catch::Approx(-273.15).epsilon(1e-15));
    CHECK(conv("dF", "dC")(9.0) == Catch::Approx(5.0).epsilon(1e-15));
    CHECK(conv("psi", "kPa")(1.0) == Catch::Approx(6.894757293168361).epsilon(1e-15));
    CHECK(conv("bar", "psi a")(1.0) == Catch::Approx(14.503773773020923).epsilon(1e-15));
    CHECK(conv("psi g", "kPa g")(10.0) == Catch::Approx(68.94757293168361).epsilon(1e-15));
    CHECK(conv("rpm", "rad/s")(60.0) == Catch::Approx(6.283185307179586).epsilon(1e-15));
    CHECK(conv("1000rpm", "rpm")(7.5) == Catch::Approx(7500.0).epsilon(1e-15));
    CHECK(conv("deg/s", "rpm")(360.0) == Catch::Approx(60.0).epsilon(1e-15));
    CHECK(conv("G", "m/s/s")(1.0) == 9.80665);
    CHECK(conv("lbf", "N")(1.0) == 4.4482216152605);
    CHECK(conv("lbf.ft", "N.m")(100.0) == Catch::Approx(135.58179483314004).epsilon(1e-15));
    CHECK(conv("hp", "kW")(1.0) == Catch::Approx(0.74569987158227022).epsilon(1e-15));
    CHECK(conv("in", "mm")(1.0) == Catch::Approx(25.4).epsilon(1e-15));
    CHECK(conv("mi", "km")(1.0) == Catch::Approx(1.609344).epsilon(1e-15));
    CHECK(conv("USgal", "l")(1.0) == Catch::Approx(3.785411784).epsilon(1e-15));
    CHECK(conv("l/h", "cc/min")(6.0) == Catch::Approx(100.0).epsilon(1e-15));
    CHECK(conv("%", "ratio")(50.0) == Catch::Approx(0.5).epsilon(1e-15));
    CHECK(conv("%Trim", "ratio")(5.0) == Catch::Approx(1.05).epsilon(1e-15));
    CHECK(conv("Trim", "%Trim")(-0.1) == Catch::Approx(-10.0).epsilon(1e-14));
    CHECK(conv("dBTDC", "deg")(10.0) ==
          Catch::Approx(-10.0).epsilon(1e-15)); // advance is negative crank angle
    CHECK(conv("dRTD", "deg")(2.0) == Catch::Approx(2.0).epsilon(1e-15));
    CHECK(conv("Ah", "A.s")(1.0) == 3600.0);
    CHECK(conv("kWh", "MJ")(1.0) == Catch::Approx(3.6).epsilon(1e-15));
    CHECK(conv("km/l", "m/m3")(1.0) == Catch::Approx(1e6).epsilon(1e-15));
    // Fuel consumption is the reciprocal of fuel economy, a quantity of its own (AC-M2-01 user maths).
    CHECK(conv("L/100km", "m3/m")(5.0) == Catch::Approx(5e-8).epsilon(1e-15));
    CHECK_FALSE(reg.converter(id(reg, "l/100km"), id(reg, "km/l")).ok());
    CHECK(conv("kg/l", "kg/m3")(0.75) == Catch::Approx(750.0).epsilon(1e-15));
    // Identity is exact.
    CHECK(conv("deg C", "deg C").identity());
    CHECK(conv("deg C", "°C").identity());
}

TEST_CASE("lookup: AFR = lambda x stoichiometric ratio (D-010, D-068)", "[unit][lookup]") {
    const UnitRegistry& reg = shipped();
    const QuantityId afr = *reg.quantityByName("Air Fuel Ratio");
    CHECK(reg.quantity(afr).base == id(reg, "lambda"));
    CHECK(reg.unit(id(reg, "lambda")).label() == "λ");
    CHECK(id(reg, "LA") == id(reg, "lambda"));
    const struct {
        const char* unit;
        double stoich;
    } fuels[] = {{"P A/F", 14.7}, {"G A/F", 14.7}, {"A A/F", 6.4}, {"D A/F", 14.5}, {"LPG A/F", 15.5}};
    for (const auto& f : fuels) {
        INFO(f.unit);
        const UnitId u = id(reg, f.unit);
        CHECK(reg.quantityOf(u) == afr);
        // Stoichiometric mixture: lambda 1 displays as the fuel's stoichiometric ratio, and back.
        CHECK(reg.fromBase(u)(1.0) == Catch::Approx(f.stoich).epsilon(1e-15));
        CHECK(reg.toBase(u)(f.stoich) == Catch::Approx(1.0).epsilon(1e-15));
        CHECK(reg.toBase(u)(f.stoich * 0.85) == Catch::Approx(0.85).epsilon(1e-15)); // rich
        CHECK(reg.unit(u).isCompat());
    }
    // The vehicle-specific ratio is not a unit: a bare "AFR" names no fuel and stays unmapped.
    CHECK_FALSE(reg.find("AFR").has_value());
    // AFR is its own family: neither Ratio nor Unitless converts into it.
    CHECK_FALSE(reg.converter(id(reg, "ratio"), id(reg, "lambda")).ok());
}

TEST_CASE("lookup: conversions across families are refused", "[unit][lookup]") {
    const UnitRegistry& reg = shipped();
    const auto refused = [&](std::string_view from, std::string_view to) {
        auto c = reg.converter(id(reg, from), id(reg, to));
        INFO(from << " -> " << to);
        REQUIRE_FALSE(c.ok());
        CHECK(c.error().code == ErrorCode::Invalid);
        CHECK(c.error().context.detail == std::string("unit.family"));
    };
    refused("deg C", "dC"); // Temperature -> Temperature Delta
    refused("dC", "deg C");
    refused("K", "dK");
    refused("deg F", "dF");
    refused("kPa a", "kPa g"); // Pressure -> Pressure Delta
    refused("psi g", "psi");
    refused("N.m", "J");  // Torque -> Energy
    refused("rpm", "Hz"); // Angular Speed -> Frequency
    refused("deg", "%");  // Angle -> Ratio
    refused("ratio", ""); // Ratio -> Unitless
    refused("m", "s");    // different dimensions
    auto bad = reg.converter(kNoUnit, static_cast<UnitId>(60000));
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().context.detail == std::string("unit.id"));
}

TEST_CASE("lookup: quantity names, aliases and byDimension", "[unit][lookup]") {
    const UnitRegistry& reg = shipped();
    CHECK(reg.quantityByName("Angular Velocity") == reg.quantityByName("Angular Speed"));
    CHECK(reg.quantityByName("energy & work") == reg.quantityByName("Energy"));
    CHECK(reg.quantityByName("Length") == reg.quantityByName("Distance"));
    CHECK(reg.quantityByName("Unitless") == kUnitless);
    CHECK_FALSE(reg.quantityByName("Nonsense").has_value());

    const Dimension none = Dimension::none();
    const Dimension perTime = Dimension::basis(Dimension::Time, -1);
    const Dimension temperature = Dimension::basis(Dimension::Temperature);
    const Dimension torque{{2, 1, -2, 0, 0, 0, 0}};
    CHECK(reg.byDimension(none) == kUnitless);
    CHECK(reg.byDimension(none, "Angle") == reg.quantityByName("Angle"));
    CHECK(reg.byDimension(none, "air fuel ratio") == reg.quantityByName("Air Fuel Ratio"));
    CHECK(reg.byDimension(perTime) == reg.quantityByName("Frequency"));
    CHECK(reg.byDimension(perTime, "Angular Speed") == reg.quantityByName("Angular Speed"));
    CHECK(reg.byDimension(temperature) == reg.quantityByName("Temperature"));
    CHECK(reg.byDimension(temperature, "Temperature Delta") == reg.quantityByName("Temperature Delta"));
    CHECK(reg.byDimension(torque) == reg.quantityByName("Energy"));
    CHECK(reg.byDimension(torque, "Torque") == reg.quantityByName("Torque"));
    CHECK(reg.byDimension(torque, "Nonsense") == reg.quantityByName("Energy"));
    CHECK(reg.byDimension(Dimension{{1, 0, -1, 0, 0, 0, 0}}) == reg.quantityByName("Speed"));
    CHECK_FALSE(reg.byDimension(Dimension::unknown()).has_value());
    CHECK_FALSE(reg.byDimension(Dimension{{5, 5, 5, 0, 0, 0, 0}}).has_value());
    CHECK(reg.deltaOf(*reg.quantityByName("Temperature")) == reg.quantityByName("Temperature Delta"));
    CHECK(reg.absoluteOf(*reg.quantityByName("Temperature Delta")) == reg.quantityByName("Temperature"));
    CHECK_FALSE(reg.deltaOf(*reg.quantityByName("Speed")).has_value());
    CHECK_FALSE(reg.deltaOf(static_cast<QuantityId>(60000)).has_value());
}

TEST_CASE("lookup: invalid ids assert and fall back to the unitless unit", "[unit][lookup]") {
    const UnitRegistry& reg = shipped();
    gAssertions = 0;
    const AssertHandler previous = setAssertHandler(&countAssertion);
    const auto bad = static_cast<UnitId>(reg.unitCount());
    CHECK_FALSE(reg.contains(bad));
    CHECK(reg.unit(bad).id == kNoUnit);
    CHECK(reg.quantity(static_cast<QuantityId>(reg.quantityCount())).id == kUnitless);
    CHECK(reg.quantityOf(bad) == kUnitless);
    CHECK(reg.toBase(bad).identity());
    CHECK(reg.fromBase(bad).identity());
    CHECK(reg.unitsOf(static_cast<QuantityId>(reg.quantityCount())).empty());
    setAssertHandler(previous);
#if ESSES_ASSERTS_ENABLED
    CHECK(gAssertions.load() == 6);
#else
    CHECK(gAssertions.load() == 0);
#endif
}
