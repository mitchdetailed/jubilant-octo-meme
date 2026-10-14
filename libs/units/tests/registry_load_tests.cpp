// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// UnitRegistry::load / loadFiles: the units-1 rules (schemas/units-1.schema.json and the rules only the
// loader checks), user files, schema versions and error reporting (02 section 2.2 rules 5-6, section 6.6 rule
// 2).
#include "units_test_support.hpp"

#include <esses/base/result.hpp>
#include <esses/units/unit_registry.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace esses;
using namespace esses::units;

namespace {

constexpr std::string_view kUnitlessQuantity =
    R"({"name": "Unitless", "family": "Unitless", "dim": [0,0,0,0,0,0,0], "base": "", "units": [{"symbol": ""}]})";
constexpr std::string_view kSpeed =
    R"({"name": "Speed", "family": "Speed", "dim": [1,0,-1,0,0,0,0], "base": "m/s",
        "units": [{"symbol": "m/s"}, {"symbol": "km/h", "scalar": 3.6, "aliases": ["kph"]}]})";
constexpr std::string_view kTemperature =
    R"({"name": "Temperature", "family": "Temperature", "dim": [0,0,0,0,1,0,0], "base": "K",
        "deltaFamily": "Temperature Delta",
        "units": [{"symbol": "K"}, {"symbol": "deg C", "display": "°C", "offset": -273.15, "aliases": ["C"]}]})";
constexpr std::string_view kTemperatureDelta =
    R"({"name": "Temperature Delta", "family": "Temperature Delta", "dim": [0,0,0,0,1,0,0], "base": "dK",
        "isDelta": true, "units": [{"symbol": "dK"}, {"symbol": "dC"}]})";

// A shipped table: the unitless quantity followed by `quantities` (comma-separated JSON objects).
std::string table(std::string_view quantities,
                  std::string_view head = R"("$schema": "urn:esses:units-1", "schemaVersion": 1)") {
    std::string out = "{" + std::string(head) + R"(, "quantities": [)" + std::string(kUnitlessQuantity);
    if (!quantities.empty()) {
        out += ", " + std::string(quantities);
    }
    return out + "]}";
}

// A user document holding `quantities`.
std::string userDoc(std::string_view quantities) {
    return R"({"$schema": "urn:esses:units-1", "schemaVersion": 1, "quantities": [)" +
           std::string(quantities) + "]}";
}

void requireInvalid(const Result<UnitRegistry>& r, std::string_view pointerPrefix,
                    std::string_view messagePart = {}) {
    REQUIRE_FALSE(r.ok());
    INFO(r.error().toString());
    CHECK(r.error().code == ErrorCode::Invalid);
    REQUIRE(r.error().context.detail.has_value());
    CHECK(r.error().context.detail->starts_with(pointerPrefix));
    CHECK(r.error().context.schemaId == std::string(UnitRegistry::kSchemaId));
    if (!messagePart.empty()) {
        CHECK(r.error().message.find(messagePart) != std::string::npos);
    }
}

std::filesystem::path scratchDir() {
    const std::filesystem::path dir = pathFromUtf8(ESSES_TEST_BINARY_DIR) / "units-scratch";
    std::filesystem::create_directories(dir);
    return dir;
}

void writeFile(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    REQUIRE(out.good());
}

} // namespace

TEST_CASE("registry load: the smallest table is the unitless quantity", "[unit][registry]") {
    auto r = UnitRegistry::load(test::kMinimalTable);
    REQUIRE(r.ok());
    const UnitRegistry& reg = *r;
    CHECK(reg.quantityCount() == 1);
    CHECK(reg.unitCount() == 1);
    CHECK(reg.find("") == kNoUnit);
    CHECK(reg.find("   ") == kNoUnit);
    CHECK(reg.quantity(kUnitless).name == "Unitless");
    CHECK(reg.quantity(kUnitless).base == kNoUnit);
    CHECK(reg.unit(kNoUnit).label().empty());
    CHECK(reg.toBase(kNoUnit).identity());
}

TEST_CASE("registry load: ids follow load order and unitsOf keeps the file order", "[unit][registry]") {
    auto r = UnitRegistry::load(
        table(std::string(kSpeed) + "," + std::string(kTemperature) + "," + std::string(kTemperatureDelta)));
    REQUIRE(r.ok());
    const UnitRegistry& reg = *r;
    REQUIRE(reg.quantityCount() == 4);
    CHECK(reg.unit(1).symbol == "m/s");
    CHECK(reg.unit(2).symbol == "km/h");
    CHECK(reg.unit(3).symbol == "K");
    const QuantityId speed = *reg.quantityByName("Speed");
    CHECK(speed == 1);
    REQUIRE(reg.unitsOf(speed).size() == 2);
    CHECK(reg.unitsOf(speed)[0] == 1);
    CHECK(reg.unitsOf(speed)[1] == 2);
    CHECK(reg.quantity(speed).base == 1);
    const QuantityId temp = *reg.quantityByName("temperature");
    REQUIRE(reg.quantity(temp).deltaFamily.has_value());
    CHECK(reg.quantity(*reg.quantity(temp).deltaFamily).name == "Temperature Delta");
    CHECK(reg.absoluteOf(*reg.deltaOf(temp)) == temp);
    CHECK(reg.unit(*reg.find("deg C")).label() == "°C");
}

TEST_CASE("registry load: syntax errors are Format errors with an offset", "[unit][registry]") {
    auto r = UnitRegistry::load(R"({"quantities": [ )");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Format);
    CHECK(r.error().context.offset.has_value());
    auto notObject = UnitRegistry::load("[1, 2]");
    requireInvalid(notObject, "", "JSON object");
    auto noQuantities = UnitRegistry::load(R"({"$schema": "urn:esses:units-1", "schemaVersion": 1})");
    requireInvalid(noQuantities, "", "quantities");
    auto empty = UnitRegistry::load(R"({"quantities": []})");
    requireInvalid(empty, "/quantities");
}

TEST_CASE("registry load: schema ids and versions", "[unit][registry]") {
    SECTION("accepted spellings of the units-1 id, and a missing $schema") {
        CHECK(UnitRegistry::load(table("", R"("$schema": "esses:units-1", "schemaVersion": 1)")).ok());
        CHECK(
            UnitRegistry::load(
                table("",
                      R"("$schema": "https://example.org/schemas/units-1.schema.json", "schemaVersion": 1)"))
                .ok());
        CHECK(UnitRegistry::load(table("", R"("schemaVersion": 1)")).ok());
        CHECK(
            UnitRegistry::load(table("", R"("x-esses-legacy": {"a": 1}, "note": "top level is open")")).ok());
    }
    SECTION("a newer major is Unsupported and names the schema") {
        auto r = UnitRegistry::load(table("", R"("$schema": "urn:esses:units-2", "schemaVersion": 1)"));
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Unsupported);
        CHECK(r.error().context.schemaId == std::string("urn:esses:units-2"));
    }
    SECTION("another document kind is refused") {
        requireInvalid(
            UnitRegistry::load(table("", R"("$schema": "urn:esses:sidecar-1", "schemaVersion": 1)")),
            "/$schema");
        requireInvalid(UnitRegistry::load(table("", R"("$schema": 7)")), "/$schema");
        requireInvalid(UnitRegistry::load(table("", R"("schemaVersion": 0)")), "/schemaVersion");
        requireInvalid(UnitRegistry::load(table("", R"("schemaVersion": "1")")), "/schemaVersion");
        requireInvalid(UnitRegistry::load(table("", R"("schemaVersion": -3)")), "/schemaVersion");
        requireInvalid(UnitRegistry::load(table("", R"("schemaVersion": 1.5)")), "/schemaVersion");
    }
    SECTION("records are closed in the current minor and open in a newer one") {
        const std::string q =
            R"({"name": "Speed", "family": "Speed", "dim": [1,0,-1,0,0,0,0], "base": "m/s", "futureKey": 1,
                "units": [{"symbol": "m/s", "futureUnitKey": true}]})";
        requireInvalid(UnitRegistry::load(table(q)), "/quantities/1/futureKey", "unknown member");
        auto newer = UnitRegistry::load(table(q, R"("$schema": "urn:esses:units-1", "schemaVersion": 2)"));
        REQUIRE(newer.ok());
        CHECK(newer->find("m/s").has_value());
    }
}

TEST_CASE("registry load: the unitless quantity comes first", "[unit][registry]") {
    requireInvalid(UnitRegistry::load(userDoc(kSpeed)), "/quantities/0", "unitless");
    const std::string wrongFirstUnit =
        R"({"quantities": [{"name": "Unitless", "family": "Unitless", "dim": [0,0,0,0,0,0,0], "base": "",
            "units": [{"symbol": "x"}, {"symbol": ""}]}]})";
    requireInvalid(UnitRegistry::load(wrongFirstUnit), "/quantities/0/units/1/symbol", "reserved");
    const std::string emptyElsewhere =
        R"({"name": "Speed", "family": "Speed", "dim": [1,0,-1,0,0,0,0], "base": "m/s",
            "units": [{"symbol": "m/s"}, {"symbol": ""}]})";
    requireInvalid(UnitRegistry::load(table(emptyElsewhere)), "/quantities/1/units/1/symbol", "reserved");
}

TEST_CASE("registry load: quantity rules", "[unit][registry]") {
    SECTION("base is one of the quantity's units, with scalar 1 and offset 0") {
        requireInvalid(
            UnitRegistry::load(table(R"({"name": "Speed", "family": "Speed", "dim": [1,0,-1,0,0,0,0],
                           "base": "km/h", "units": [{"symbol": "m/s"}]})")),
            "/quantities/1/base", "not one of");
        requireInvalid(
            UnitRegistry::load(table(R"({"name": "Speed", "family": "Speed", "dim": [1,0,-1,0,0,0,0],
                           "base": "km/h", "units": [{"symbol": "km/h", "scalar": 3.6}]})")),
            "/quantities/1/base", "scalar 1 and offset 0");
        requireInvalid(UnitRegistry::load(table(R"({"name": "T", "family": "T", "dim": [0,0,0,0,1,0,0],
                           "base": "C", "units": [{"symbol": "C", "offset": -273.15}]})")),
                       "/quantities/1/base", "scalar 1 and offset 0");
        CHECK(UnitRegistry::load(table(R"({"name": "Speed", "family": "Speed", "dim": [1,0,-1,0,0,0,0],
                  "base": "m/s", "units": [{"symbol": "km/h", "scalar": 3.6}, {"symbol": "m/s", "toBase": 1}]})"))
                  .ok());
    }
    SECTION("required members and their types") {
        requireInvalid(UnitRegistry::load(table(R"({"family": "S", "dim": [1,0,0,0,0,0,0], "base": "m",
                           "units": [{"symbol": "m"}]})")),
                       "/quantities/1", "'name'");
        requireInvalid(UnitRegistry::load(table(R"({"name": "S", "dim": [1,0,0,0,0,0,0], "base": "m",
                           "units": [{"symbol": "m"}]})")),
                       "/quantities/1", "'family'");
        requireInvalid(UnitRegistry::load(table(R"({"name": " S", "family": "S", "dim": [1,0,0,0,0,0,0],
                           "base": "m", "units": [{"symbol": "m"}]})")),
                       "/quantities/1/name");
        requireInvalid(UnitRegistry::load(table(R"({"name": "S", "family": "S", "dim": [1,0,0,0,0,0,128],
                           "base": "m", "units": [{"symbol": "m"}]})")),
                       "/quantities/1/dim/6");
        requireInvalid(UnitRegistry::load(table(R"({"name": "S", "family": "S",
                           "dim": [18446744073709551615,0,0,0,0,0,0], "base": "m", "units": [{"symbol": "m"}]})")),
                       "/quantities/1/dim/0");
        requireInvalid(UnitRegistry::load(table(R"({"name": "S", "family": "S", "dim": [-128,0,0,0,0,0,0],
                           "base": "m", "units": [{"symbol": "m"}]})")),
                       "/quantities/1/dim/0");
        requireInvalid(UnitRegistry::load(table(R"({"name": "S", "family": "S", "dim": [1,0,0,0,0,0,0],
                           "base": "m", "isDelta": 1, "units": [{"symbol": "m"}]})")),
                       "/quantities/1/isDelta");
        requireInvalid(UnitRegistry::load(table(R"(7)")), "/quantities/1", "JSON object");
    }
    SECTION("names and quantity aliases are unique, case-insensitively") {
        requireInvalid(UnitRegistry::load(table(std::string(kSpeed) + R"(, {"name": "speed", "family": "X",
                           "dim": [1,0,0,0,0,0,0], "base": "m", "units": [{"symbol": "m"}]})")),
                       "/quantities/2/name", "already used");
        requireInvalid(UnitRegistry::load(table(std::string(kSpeed) + R"(, {"name": "Length", "aliases":
                           ["SPEED"], "family": "Length", "dim": [1,0,0,0,0,0,0], "base": "m",
                           "units": [{"symbol": "m"}]})")),
                       "/quantities/2/aliases/0", "already used");
        auto r = UnitRegistry::load(table(R"({"name": "Angular Speed", "aliases": ["Angular Velocity"],
                     "family": "Angular Speed", "dim": [0,0,-1,0,0,0,0], "base": "rad/s",
                     "units": [{"symbol": "rad/s"}]})"));
        REQUIRE(r.ok());
        CHECK(r->quantityByName("angular velocity") == r->quantityByName("Angular Speed"));
        CHECK(r->quantity(1).aliases.size() == 1);
        CHECK_FALSE(r->quantityByName("Angular").has_value());
    }
    SECTION("deltaFamily names a delta quantity of the same dimension") {
        requireInvalid(UnitRegistry::load(table(kTemperature)), "/quantities/1/deltaFamily",
                       "names no quantity");
        requireInvalid(UnitRegistry::load(table(std::string(kTemperature) + R"(, {"name": "Temperature Delta",
                           "family": "Temperature Delta", "dim": [0,0,0,0,1,0,0], "base": "dK",
                           "units": [{"symbol": "dK"}]})")),
                       "/quantities/1/deltaFamily", "without isDelta");
        requireInvalid(UnitRegistry::load(table(std::string(kTemperature) + R"(, {"name": "Temperature Delta",
                           "family": "Temperature Delta", "dim": [0,0,0,0,2,0,0], "base": "dK", "isDelta": true,
                           "units": [{"symbol": "dK"}]})")),
                       "/quantities/1/deltaFamily", "different dimension");
        requireInvalid(UnitRegistry::load(table(R"({"name": "D", "family": "D", "dim": [0,0,0,0,1,0,0],
                           "base": "dK", "isDelta": true, "deltaFamily": "D", "units": [{"symbol": "dK"}]})")),
                       "/quantities/1", "no deltaFamily");
        // A forward reference within the document is fine (kTemperature names the delta defined after it).
        CHECK(
            UnitRegistry::load(table(std::string(kTemperature) + "," + std::string(kTemperatureDelta))).ok());
    }
    SECTION("quantities sharing a family share dimension and delta-ness") {
        requireInvalid(
            UnitRegistry::load(table(std::string(kSpeed) + R"(, {"name": "Velocity", "family": "Speed",
                           "dim": [1,0,0,0,0,0,0], "base": "m", "units": [{"symbol": "m"}]})")),
            "/quantities/2/family", "share dimension");
        auto shared = UnitRegistry::load(table(std::string(kSpeed) + R"(, {"name": "Damper Velocity",
                          "family": "Speed", "dim": [1,0,-1,0,0,0,0], "base": "mm/s",
                          "units": [{"symbol": "mm/s", "toBase": 1}]})"));
        REQUIRE(shared.ok());
        // Same family and dimension: conversion across the two quantities is allowed.
        const auto c = shared->converter(*shared->find("km/h"), *shared->find("mm/s"));
        REQUIRE(c.ok());
        CHECK((*c)(3.6) == Catch::Approx(1.0).epsilon(1e-15));
    }
}

TEST_CASE("registry load: unit rules", "[unit][registry]") {
    const auto speedWith = [](std::string_view unit) {
        return table(R"({"name": "Speed", "family": "Speed", "dim": [1,0,-1,0,0,0,0], "base": "m/s",
                         "units": [{"symbol": "m/s"}, )" +
                     std::string(unit) + "]}");
    };
    requireInvalid(UnitRegistry::load(speedWith(R"({"symbol": "km/h", "scalar": 3.6, "toBase": 0.25})")),
                   "/quantities/1/units/1", "mutually exclusive");
    requireInvalid(UnitRegistry::load(speedWith(R"({"symbol": "km/h", "scalar": 0})")),
                   "/quantities/1/units/1/scalar");
    requireInvalid(UnitRegistry::load(speedWith(R"({"symbol": "km/h", "toBase": 0})")),
                   "/quantities/1/units/1/toBase");
    requireInvalid(UnitRegistry::load(speedWith(R"({"symbol": "km/h", "scalar": 1e-320})")),
                   "/quantities/1/units/1", "finite inverse");
    requireInvalid(UnitRegistry::load(speedWith(R"({"symbol": "km/h", "offset": "1"})")),
                   "/quantities/1/units/1/offset");
    requireInvalid(UnitRegistry::load(speedWith(R"({"symbol": "km/h", "factor": 3.6})")),
                   "/quantities/1/units/1/factor", "unknown member");
    requireInvalid(UnitRegistry::load(speedWith(R"({"symbol": "km h "})")), "/quantities/1/units/1/symbol");
    requireInvalid(UnitRegistry::load(speedWith(R"({"symbol": "km  h"})")), "/quantities/1/units/1/symbol");
    requireInvalid(UnitRegistry::load(speedWith(R"({"symbol": "°/s"})")), "/quantities/1/units/1/symbol");
    requireInvalid(UnitRegistry::load(speedWith(R"({"symbol": "km/h", "display": ""})")),
                   "/quantities/1/units/1/display");
    requireInvalid(UnitRegistry::load(speedWith(R"({"symbol": "km/h", "aliases": ["kph", "kph"]})")),
                   "/quantities/1/units/1/aliases/1", "twice");
    requireInvalid(UnitRegistry::load(speedWith(R"({"symbol": "km/h", "aliases": ["km/h"]})")),
                   "/quantities/1/units/1/aliases/0", "repeats the symbol");
    requireInvalid(UnitRegistry::load(speedWith(R"({"symbol": "km/h", "aliases": ["k\tph"]})")),
                   "/quantities/1/units/1/aliases/0");
    requireInvalid(UnitRegistry::load(speedWith(R"({"symbol": "km/h", "caseSensitive": ["kph"]})")),
                   "/quantities/1/units/1/caseSensitive/0", "neither the symbol nor an alias");
    requireInvalid(UnitRegistry::load(speedWith(R"({"symbol": "km/h", "compat": ""})")),
                   "/quantities/1/units/1/compat");
    requireInvalid(UnitRegistry::load(speedWith(R"({"symbol": "km/h", "compat": "Legacy"})")),
                   "/quantities/1/units/1/compat");
    auto good = UnitRegistry::load(speedWith(R"({"symbol": "km/h", "display": "km/h", "compat": "legacy",
                                                 "aliases": ["kph", "KPH"], "note": "free text"})"));
    REQUIRE(good.ok());
    const Unit& kmh = good->unit(*good->find("km/h"));
    CHECK(kmh.isCompat());
    CHECK(kmh.compat == "legacy");
    CHECK(kmh.aliases.size() == 2);
}

TEST_CASE("registry load: no spelling belongs to two units", "[unit][registry]") {
    const std::string mass = R"({"name": "Mass", "family": "Mass", "dim": [0,1,0,0,0,0,0], "base": "kg",
                                 "units": [{"symbol": "kg"}, {"symbol": "g", "scalar": 1000 CS}]})";
    const std::string accel = R"({"name": "Acceleration", "family": "Acceleration", "dim": [1,0,-2,0,0,0,0],
                                  "base": "m/s/s", "units": [{"symbol": "m/s/s"}, {"symbol": "G", "toBase": 9.80665}]})";
    const auto withMass = [&](std::string_view cs) {
        std::string m = mass;
        m.replace(m.find("CS"), 2, cs);
        return table(m + "," + accel);
    };
    SECTION("exactly") {
        requireInvalid(UnitRegistry::load(table(std::string(kSpeed) + R"(, {"name": "Rate", "family": "Rate",
                           "dim": [0,0,-1,0,0,0,0], "base": "1/s", "units": [{"symbol": "1/s", "aliases": ["kph"]}]})")),
                       "/quantities/2/units/0/aliases/0", "already a spelling of");
    }
    SECTION("after case folding, unless the spelling is declared caseSensitive") {
        requireInvalid(UnitRegistry::load(withMass("")), "/quantities/2/units/1/symbol", "case folding");
        auto r = UnitRegistry::load(withMass(R"(, "caseSensitive": ["g"])"));
        REQUIRE(r.ok());
        const QuantityId massQ = *r->quantityByName("Mass");
        const QuantityId accelQ = *r->quantityByName("Acceleration");
        CHECK(r->quantityOf(*r->find("g")) == massQ);
        CHECK(r->quantityOf(*r->find("G")) == accelQ);
        // Scoped lookups fold every spelling of their quantity.
        CHECK(r->quantityOf(*r->find("g", accelQ)) == accelQ);
        CHECK(r->quantityOf(*r->find("G", massQ)) == massQ);
    }
    SECTION("after the normaliser") {
        requireInvalid(
            UnitRegistry::load(table(std::string(kTemperature) + "," + std::string(kTemperatureDelta) +
                                     R"(, {"name": "Angle", "family": "Angle", "dim": [0,0,0,0,0,0,0],
                           "base": "rad", "units": [{"symbol": "rad"}, {"symbol": "deg", "aliases": ["degc"]}]})")),
            "/quantities/3/units/1/aliases/0", "normaliser");
    }
    SECTION("the display form is a spelling too") {
        requireInvalid(
            UnitRegistry::load(table(std::string(kTemperature) + "," + std::string(kTemperatureDelta) +
                                     R"(, {"name": "X", "family": "X", "dim": [0,0,0,0,0,0,0],
                           "base": "x", "units": [{"symbol": "x", "display": "°C"}]})")),
            "/quantities/3/units/0/display", "already a spelling of");
    }
    SECTION("within one quantity, case variants of two units collide even when caseSensitive") {
        requireInvalid(UnitRegistry::load(table(R"({"name": "Mass", "family": "Mass", "dim": [0,1,0,0,0,0,0],
                           "base": "kg", "units": [{"symbol": "kg"}, {"symbol": "KG", "scalar": 2,
                           "caseSensitive": ["KG"]}]})")),
                       "/quantities/1/units/1/symbol", "same quantity");
    }
    SECTION("case variants of one unit are fine") {
        auto r = UnitRegistry::load(table(R"({"name": "Speed", "family": "Speed", "dim": [1,0,-1,0,0,0,0],
                     "base": "m/s", "units": [{"symbol": "m/s"}, {"symbol": "km/h", "scalar": 3.6,
                     "aliases": ["kph", "KPH", "Kph"]}]})"));
        CHECK(r.ok());
    }
}

TEST_CASE("registry load: user files extend the shipped table", "[unit][registry]") {
    const std::string shipped =
        table(std::string(kSpeed) + "," + std::string(kTemperature) + "," + std::string(kTemperatureDelta));
    SECTION("a new quantity and units appended to a shipped quantity") {
        const std::string user = userDoc(
            R"({"name": "Flow Rate", "family": "Flow Rate", "dim": [3,0,-1,0,0,0,0], "base": "m3/s",
                "units": [{"symbol": "m3/s", "display": "m³/s"}, {"symbol": "L/min", "scalar": 60000}]},
               {"name": "Speed", "family": "Speed", "dim": [1,0,-1,0,0,0,0], "base": "m/s",
                "units": [{"symbol": "ft/min", "toBase": 0.00508, "aliases": ["fpm"]}]})");
        auto r = UnitRegistry::load(shipped, user);
        REQUIRE(r.ok());
        const QuantityId speed = *r->quantityByName("Speed");
        REQUIRE(r->unitsOf(speed).size() == 3);
        CHECK(r->unit(r->unitsOf(speed)[2]).symbol == "ft/min");
        CHECK(r->find("FPM") == r->unitsOf(speed)[2]);
        const auto c = r->converter(*r->find("ft/min"), *r->find("m/s"));
        REQUIRE(c.ok());
        CHECK((*c)(100.0) == Catch::Approx(0.508).epsilon(1e-15));
        CHECK(r->quantityByName("flow rate").has_value());
        CHECK(r->find("L/min").has_value());
    }
    SECTION("a user file may not reuse a shipped spelling") {
        const std::string user =
            userDoc(R"({"name": "Velocity", "family": "Velocity", "dim": [1,0,-1,0,0,0,0],
                                             "base": "mps", "units": [{"symbol": "mps"}, {"symbol": "KPH"}]})");
        requireInvalid(UnitRegistry::load(shipped, user), "/quantities/0/units/1/symbol", "shipped spelling");
    }
    SECTION("an extension repeats the shipped quantity's definition unchanged") {
        const std::string user = userDoc(R"({"name": "Speed", "family": "Speed", "dim": [1,0,-2,0,0,0,0],
                                             "base": "m/s", "units": [{"symbol": "ft/min", "toBase": 0.00508}]})");
        requireInvalid(UnitRegistry::load(shipped, user), "/quantities/0", "unchanged");
        const std::string delta = userDoc(R"({"name": "Temperature", "family": "Temperature",
                                              "dim": [0,0,0,0,1,0,0], "base": "K",
                                              "units": [{"symbol": "deg R", "scalar": 1.8}]})");
        requireInvalid(UnitRegistry::load(shipped, delta), "/quantities/0", "deltaFamily unchanged");
        const std::string fine = userDoc(R"({"name": "Temperature", "family": "Temperature",
                                             "dim": [0,0,0,0,1,0,0], "base": "K", "deltaFamily": "Temperature Delta",
                                             "units": [{"symbol": "deg R", "scalar": 1.8}]})");
        auto r = UnitRegistry::load(shipped, fine);
        REQUIRE(r.ok());
        const auto c = r->converter(*r->find("deg C"), *r->find("deg R"));
        REQUIRE(c.ok());
        CHECK((*c)(0.0) == Catch::Approx(491.67).epsilon(1e-15));
    }
    SECTION("the same name twice in one user file, or a case variant of a shipped name, is a duplicate") {
        const std::string q = R"({"name": "Flow", "family": "Flow", "dim": [3,0,-1,0,0,0,0], "base": "m3/s",
                                  "units": [{"symbol": "m3/s"}]})";
        requireInvalid(UnitRegistry::load(shipped, userDoc(q + "," + q)), "/quantities/1/name",
                       "already used");
        requireInvalid(UnitRegistry::load(shipped, userDoc(R"({"name": "speed", "family": "Speed",
                           "dim": [1,0,-1,0,0,0,0], "base": "m/s", "units": [{"symbol": "fpm"}]})")),
                       "/quantities/0/name", "already used");
    }
    SECTION("a user file cannot define the empty symbol") {
        requireInvalid(
            UnitRegistry::load(shipped, userDoc(R"({"name": "N", "family": "N", "dim": [0,0,0,0,0,0,0],
                           "base": "", "units": [{"symbol": ""}]})")),
            "/quantities/0/units/0/symbol", "reserved");
    }
    SECTION("errors name the user table") {
        auto r = UnitRegistry::load(shipped, "{");
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Format);
    }
}

TEST_CASE("registry load: loadFiles reads files and reports their paths", "[unit][registry]") {
    const std::filesystem::path dir = scratchDir();
    const std::filesystem::path units = dir / "units.json";
    const std::filesystem::path user = dir / "units.user.json";
    writeFile(units, table(kSpeed));
    std::filesystem::remove(user);

    SECTION("a missing user file is skipped") {
        auto r = UnitRegistry::loadFiles(units, user);
        REQUIRE(r.ok());
        CHECK(r->find("kph").has_value());
        CHECK(UnitRegistry::loadFiles(units).ok());
    }
    SECTION("a missing table is an Io error naming it") {
        auto r = UnitRegistry::loadFiles(dir / "missing.json");
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Io);
        CHECK(r.error().context.path == dir / "missing.json");
    }
    SECTION("a directory where the table belongs is an Io error naming it, never an exception") {
        const std::filesystem::path folder = dir / "folder.json";
        std::filesystem::create_directories(folder);
        Result<UnitRegistry> r = fail(ErrorCode::Internal, "not called");
        REQUIRE_NOTHROW(r = UnitRegistry::loadFiles(folder)); // libstdc++ throws reading a directory
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Io);
        CHECK(r.error().context.path == folder);
    }
    SECTION("an invalid user file fails with its path") {
        writeFile(user, userDoc(R"({"name": "X", "family": "X", "dim": [1,0,0,0,0,0,0], "base": "m",
                                    "units": [{"symbol": "km/h"}]})"));
        auto r = UnitRegistry::loadFiles(units, user);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Invalid);
        CHECK(r.error().context.path == user);
        writeFile(user, userDoc(R"({"name": "X", "family": "X", "dim": [1,0,0,0,0,0,0], "base": "m",
                                    "units": [{"symbol": "m"}]})"));
        auto fine = UnitRegistry::loadFiles(units, user);
        REQUIRE(fine.ok());
        CHECK(fine->quantityByName("X").has_value());
    }
    SECTION("a syntax error in the table names the table") {
        writeFile(units, "{\n  \"quantities\": [ nope ]\n}");
        auto r = UnitRegistry::loadFiles(units);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Format);
        CHECK(r.error().context.path == units);
        CHECK(r.error().context.offset.has_value());
    }
}

// The schema examples of schemas/examples/units-1 (written against the schema) agree with the loader: every
// invalid example is refused at (or under) the JSON pointer expect.json names, the extension example loads as
// a shipped table and the user example as a user file.
TEST_CASE("registry load: agrees with the units-1 schema examples", "[unit][registry]") {
    const std::filesystem::path dir = test::repoRoot() / "schemas" / "examples" / "units-1";
    if (!std::filesystem::exists(dir / "expect.json")) {
        SKIP("schemas/examples/units-1 is not present");
    }
    const auto expect = nlohmann::json::parse(test::readFile(dir / "expect.json"));
    std::size_t checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir / "invalid")) {
        const std::string name = pathToUtf8(entry.path().filename());
        INFO("invalid example " << name);
        REQUIRE(expect.contains(name));
        const std::string pointer = expect[name]["pointer"].get<std::string>();
        auto r = UnitRegistry::load(test::kMinimalTable, test::readFile(entry.path()));
        REQUIRE_FALSE(r.ok());
        INFO(r.error().toString());
        CHECK(r.error().code == ErrorCode::Invalid);
        REQUIRE(r.error().context.detail.has_value());
        CHECK(r.error().context.detail->starts_with(pointer));
        ++checked;
    }
    CHECK(checked >= 10);
    if (std::filesystem::exists(dir / "valid" / "registry-extensions.json")) {
        auto r = UnitRegistry::load(test::readFile(dir / "valid" / "registry-extensions.json"));
        INFO((r.ok() ? std::string() : r.error().toString()));
        CHECK(r.ok());
    }
    if (std::filesystem::exists(dir / "valid" / "user-file.json")) {
        auto r = UnitRegistry::load(test::kMinimalTable, test::readFile(dir / "valid" / "user-file.json"));
        INFO((r.ok() ? std::string() : r.error().toString()));
        CHECK(r.ok());
    }
}
