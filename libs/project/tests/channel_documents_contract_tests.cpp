// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The in-memory parts of the M2 channel documents that are final before the readers and writers land
// (02 section 6.3, D-013): case-insensitive override lookup and editing, and the Mappings ->
// model::MappingTable conversion the resolver consumes. The M2-STUB functions are tested by their owner.

#include <esses/project/channel_overrides.hpp>
#include <esses/project/mappings.hpp>
#include <esses/units/unit_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::project;

TEST_CASE("channel overrides: case-insensitive find, set and clear", "[unit]") {
    ChannelOverrides doc;
    ChannelOverrides::Entry rpm;
    rpm.displayUnit = "rpm";
    rpm.dps = 0;
    doc.set("Engine RPM", rpm);
    REQUIRE(doc.find("engine rpm") != nullptr);
    CHECK(doc.find("ENGINE RPM")->dps == 0);

    ChannelOverrides::Entry changed = rpm;
    changed.colourIndex = 2;
    doc.set("engine rpm", changed); // keeps the existing spelling
    REQUIRE(doc.channels.size() == 1);
    CHECK(doc.channels.begin()->first == "Engine RPM");
    CHECK(doc.channels.begin()->second.colourIndex == 2);

    doc.set("Gear", ChannelOverrides::Entry{}); // an empty entry adds nothing
    CHECK(doc.channels.size() == 1);
    CHECK(doc.clear("ENGINE rpm"));
    CHECK_FALSE(doc.clear("Engine RPM"));
    CHECK(doc.channels.empty());
    CHECK(ChannelOverrides::Entry{}.empty());
}

TEST_CASE("mappings: the resolver table stamps known units only", "[unit]") {
    const auto loaded =
        units::UnitRegistry::loadFiles(std::filesystem::path(ESSES_TEST_REPO_ROOT) / "data" / "units.json");
    REQUIRE(loaded.ok());
    Mappings doc;
    doc.rows.push_back({"WheelSP_FL", "Wheel Speed FL", std::string("km/h"), true});
    doc.rows.push_back({"GPS Latitudine", "GPS Latitude", std::nullopt, false});
    doc.rows.push_back({"Odd", "Odd Name", std::string("not-a-unit"), false});
    std::vector<std::string> log;
    const auto table = doc.toTable(*loaded, log);
    REQUIRE(table.rows.size() == 3);
    CHECK(table.rows[0].from == "WheelSP_FL");
    CHECK(table.rows[0].unit == loaded->find("km/h"));
    CHECK(table.rows[0].unitlessOnly);
    CHECK_FALSE(table.rows[1].unit.has_value());
    CHECK_FALSE(table.rows[2].unit.has_value());
    CHECK(log.size() == 1);
}
