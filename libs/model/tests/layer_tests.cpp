// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DataLayer indexes and lookups (02 section 4.2).
#include "model_test_support.hpp"

#include <esses/model/layer.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace esses;
using namespace esses::model;
using esses::model::test::makeChannel;

TEST_CASE("layer: channels are indexed by case-folded name, logged name and id", "[unit]") {
    DataLayer layer;
    CHECK(layer.name == "Normal");
    CHECK(layer.addChannel(makeChannel("Engine RPM")) == 0);
    CHECK(layer.addChannel(makeChannel("Wheel Speed FL", "WheelSP_FL")) == 1);
    CHECK(layer.addChannel(makeChannel("Stra\u00dfe")) == 2); // U+00DF folds to "ss"

    REQUIRE(layer.find("engine rpm") != nullptr);
    CHECK(layer.find("ENGINE RPM")->desc.name == "Engine RPM");
    CHECK(layer.find("STRASSE") == &layer.channels[2]);
    CHECK(layer.find("WheelSP_FL") == nullptr); // canonical name only
    CHECK(layer.findLogged("wheelsp_fl") == &layer.channels[1]);
    CHECK(layer.findById(layer.channels[1].desc.id) == &layer.channels[1]);
    CHECK(layer.findById(ChannelId{}) == nullptr);
    CHECK(layer.findMutable("engine rpm") == &layer.channels[0]);
}

TEST_CASE("layer: on a folded-name collision the earlier channel keeps the index entry", "[unit]") {
    DataLayer layer;
    layer.addChannel(makeChannel("Speed", "Speed A"));
    layer.addChannel(makeChannel("SPEED", "Speed B"));
    CHECK(layer.find("speed")->desc.loggedName == "Speed A");
    CHECK(layer.findLogged("speed b")->desc.name == "SPEED");
    CHECK(layer.channels.size() == 2);
}

TEST_CASE("layer: rebuildIndex follows renames", "[unit]") {
    DataLayer layer;
    layer.addChannel(makeChannel("A"));
    layer.addChannel(makeChannel("B"));
    layer.channels[0].desc.name = "Alpha";
    CHECK(layer.find("Alpha") == nullptr); // stale until rebuilt
    layer.rebuildIndex();
    CHECK(layer.find("alpha") == &layer.channels[0]);
    CHECK(layer.find("A") == nullptr);
    CHECK(layer.findLogged("A") == &layer.channels[0]);
    CHECK(layer.byId.size() == 2);
}

TEST_CASE("layer: enumeration tables and markers by id", "[unit]") {
    DataLayer layer;
    layer.enumTables.push_back(EnumTable{EnumTableId{4}, "Gear", {{0, "N", 0}}});
    CHECK(layer.enumTable(EnumTableId{4})->name == "Gear");
    CHECK(layer.enumTable(EnumTableId{5}) == nullptr);
    layer.markers.push_back(makeLoggedMarker(MarkerClass::SOL, 0, "", ""));
    CHECK(layer.findMarker(layer.markers[0].id) == &layer.markers[0]);
    CHECK(layer.findMarker(MarkerId{}) == nullptr);
}
