// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// markers-1 (expected.markers.json): the marker serializer shared with sidecar-1 (02 section 5.3 rule 8).
#include "model_test_support.hpp"

#include <esses/model/markers_document.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace esses;
using namespace esses::model;

TEST_CASE("markers-1: valid examples load and round-trip canonically", "[unit][schema]") {
    const auto files = test::exampleFiles("markers-1", "valid");
    REQUIRE(files.size() >= 2);
    for (const auto& path : files) {
        INFO(path.filename().string());
        auto doc = MarkersDocument::parse(test::readFile(path));
        INFO((doc.ok() ? std::string() : doc.error().toString()));
        REQUIRE(doc.ok());
        const auto text = doc->toJson();
        REQUIRE(text.ok());
        CHECK(test::schemaErrors("markers-1", *text).empty());
        auto again = MarkersDocument::parse(*text);
        REQUIRE(again.ok());
        CHECK(*again == *doc);
        CHECK(*again->toJson() == *text);
    }
}

TEST_CASE("markers-1: the companion-merge example", "[unit]") {
    auto doc = MarkersDocument::parse(
        test::readFile(test::schemasDir() / "examples" / "markers-1" / "valid" / "companion-merge.json"));
    REQUIRE(doc.ok());
    REQUIRE(doc->layers.size() == 2);
    const auto& normal = doc->layers.at("Normal");
    REQUIRE(normal.size() == 6);
    CHECK(normal[0].cls == MarkerClass::SOL);
    CHECK(normal[0].prov == Provenance::Derived);
    CHECK(normal[1].cls == MarkerClass::DeviceEvent);
    CHECK(normal[2].flags == 77);
    CHECK(normal[4].comment == "pit entry");
    CHECK(doc->layers.at("Fastest").empty());
    const std::string text = *doc->toJson();
    // flags 0 is omitted on write ("an absent flags means 0").
    CHECK(text.find("\"flags\": 0") == std::string::npos);
    CHECK(text.find("\"flags\": 77") != std::string::npos);
}

TEST_CASE("markers-1: every invalid example is rejected", "[unit][schema]") {
    const auto expect =
        nlohmann::json::parse(test::readFile(test::schemasDir() / "examples" / "markers-1" / "expect.json"));
    for (const auto& path : test::exampleFiles("markers-1", "invalid")) {
        const std::string name = path.filename().string();
        INFO(name);
        const auto r = MarkersDocument::parse(test::readFile(path));
        REQUIRE_FALSE(r.ok());
        INFO(r.error().toString());
        REQUIRE(r.error().context.detail.has_value());
        CHECK(r.error().context.detail->starts_with(expect.at(name).at("pointer").get<std::string>()));
        if (name == "wrong-schema-id.json") {
            CHECK(r.error().code == ErrorCode::Unsupported); // markers-2: a newer major
        } else {
            CHECK(r.error().code == ErrorCode::Invalid);
        }
    }
}

TEST_CASE("markers-1: fromSource lists the base markers of every layer", "[unit]") {
    DataSource src;
    src.layers.resize(2);
    src.layers[1].name = "Fastest";
    const Marker sol = makeLoggedMarker(MarkerClass::SOL, 0, "", "", "", 0, 0, Provenance::Derived);
    const Marker b1 =
        makeLoggedMarker(MarkerClass::BCN, 1'000, "Beacons", "B1", "", 77, 0, Provenance::Companion);
    src.layers[0].baseMarkers = {sol, b1};
    Marker moved = b1;
    moved.originalT = 1'000;
    moved.t = 5'000;
    Marker user = b1;
    user.id = MarkerId{uuidRandom()};
    user.prov = Provenance::User;
    src.layers[0].markers = {sol, user, moved};
    src.layers[1].markers = {moved, user}; // no base captured: edits undone, user markers dropped

    const MarkersDocument doc = MarkersDocument::fromSource(src);
    REQUIRE(doc.layers.at("Normal").size() == 2);
    CHECK(doc.layers.at("Normal")[1] == b1);
    REQUIRE(doc.layers.at("Fastest").size() == 1);
    CHECK(doc.layers.at("Fastest")[0] == b1);
    const auto text = doc.toJson();
    REQUIRE(text.ok());
    CHECK(test::schemaErrors("markers-1", *text).empty());
}
