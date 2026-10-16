// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackMap (track-1, 02 section 6.9; D-027): reading the schema examples, the canonical document and its
// round trip, the checks of validate() with their JSON pointers, and pointAt along open and closed lines.
#include "analysis_test_support.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <fstream>
#include <iterator>
#include <string>

using namespace esses;
using namespace esses::analysis;
using namespace esses::analysis::test;

namespace {

JsonDoc example(const std::string& kind, const std::string& name) {
    const std::filesystem::path path = repoRoot() / "schemas" / "examples" / "track-1" / kind / name;
    auto doc = JsonDoc::load(path);
    REQUIRE(doc);
    return std::move(doc).value();
}

JsonDoc parse(std::string_view text) {
    auto doc = JsonDoc::parse(text);
    REQUIRE(doc);
    return std::move(doc).value();
}

std::string detailOf(std::string_view text) {
    auto map = TrackMap::fromJson(parse(text));
    REQUIRE_FALSE(map);
    CHECK(map.error().code == ErrorCode::Invalid);
    return map.error().context.detail.value_or("?");
}

std::string validateDetail(const TrackMap& map) {
    auto v = map.validate();
    REQUIRE_FALSE(v);
    CHECK(v.error().code == ErrorCode::Invalid);
    return v.error().context.detail.value_or("?");
}

// A 40 m square loop (the schema example's centreline).
TrackMap square() {
    TrackMap m;
    m.name = "Square";
    m.venue = "Square";
    m.lengthM = 40.0;
    m.centre = {{0, 0, 0}, {10, 0, 10}, {10, 10, 20}, {0, 10, 30}, {0, 0, 40}};
    m.sections = {TrackSection{"s1", "Default", "Straight 1", 0.0, 20.0, "straight", false},
                  TrackSection{"t1", "Default", "Turn 1", 20.0, 40.0, "corner", false}};
    return m;
}

} // namespace

TEST_CASE("track map: the full schema example reads in base units", "[unit]") {
    auto map = TrackMap::fromJson(example("valid", "full.json"));
    REQUIRE(map);
    CHECK(map->name == "Test Circuit");
    CHECK(map->venue == "Test Circuit");
    CHECK(map->origin == TrackOrigin{-33.4479, 149.552, 862.0});
    CHECK(map->direction == TrackDirection::Clockwise);
    CHECK(map->closed);
    CHECK(map->lengthM == 40.0);
    CHECK(map->generatedFrom.method == TrackMethod::Gps);
    CHECK(map->generatedFrom.source == "xxh3:00000000000000000000000000000000");
    REQUIRE(map->centre.size() == 5);
    CHECK(map->centre[2] == TrackPoint{10.0, 10.0, 20.0});
    CHECK(map->inner.empty());
    REQUIRE(map->sections.size() == 3);
    CHECK(map->sections[2].set == "Driver Notes");
    CHECK(map->sections[2].category == "Fast");
    CHECK(map->sections[2].userDefined);
    CHECK(map->sections[2].startM == Catch::Approx(20.0)); // 0.02 km
    CHECK(map->sections[2].endM == Catch::Approx(40.0));
    CHECK(map->rest.isNull());
    CHECK(map->validate());
    CHECK(map->sectionSets() == std::vector<std::string>{"Default", "Driver Notes"});

    auto minimal = TrackMap::fromJson(example("valid", "minimal.json"));
    REQUIRE(minimal);
    CHECK(minimal->centre.empty());
    CHECK(minimal->lengthM == 0.0);
    CHECK(minimal->closed);
    CHECK(minimal->direction == TrackDirection::Clockwise);
    CHECK(minimal->generatedFrom.method == TrackMethod::None);
    CHECK(minimal->validate());
}

TEST_CASE("track map: the canonical document round-trips and keeps unknown members", "[unit]") {
    TrackMap m = square();
    m.origin = TrackOrigin{45.0, 7.5, 250.0};
    m.direction = TrackDirection::CounterClockwise;
    m.generatedFrom = TrackGeneratedFrom{TrackMethod::SpeedLatG, "xxh3:ab", "lap-1"};
    m.inner = {{1, 1, 0}, {9, 1, 10}};
    m.sections.push_back(TrackSection{"u1", "Driver Notes", "Back half", 20.0, 40.0, "Fast", true});
    const JsonDoc doc = m.toJson();
    REQUIRE(doc.isObject());
    CHECK(doc.schemaId() == "urn:esses:track-1");
    CHECK(doc.schemaVersion() == 1);
    const std::string text = doc.canonical();
    CHECK(text.find("\"length\": {\n    \"unit\": \"m\",\n    \"v\": 40\n  }") != std::string::npos);
    CHECK(text.find("\"method\": \"speedLatG\"") != std::string::npos);
    CHECK(text.find("\"direction\": \"ccw\"") != std::string::npos);
    auto back = TrackMap::fromJson(doc);
    REQUIRE(back);
    CHECK(*back == m);
    CHECK(back->toJson() == doc);

    // Unknown top-level members ("x-i2" and others) survive; generatedFrom is written only when set.
    auto withRest = TrackMap::fromJson(parse(R"({"$schema": "urn:esses:track-1", "schemaVersion": 1,
        "lines": {"centre": [[0, 0, 0], [10, 0, 10]]}, "x-i2": {"Rotation": "12"}, "x-later": [1, 2]})"));
    REQUIRE(withRest);
    CHECK(withRest->lengthM == 10.0); // the last centreline distance when "length" is absent
    CHECK(withRest->rest.compact() == R"({"x-i2":{"Rotation":"12"},"x-later":[1,2]})");
    const std::string written = withRest->toJson().compact();
    CHECK(written.find(R"("x-i2":{"Rotation":"12"})") != std::string::npos);
    CHECK(written.find(R"("x-later":[1,2])") != std::string::npos);
    CHECK(written.find("generatedFrom") == std::string::npos);
    auto again = TrackMap::fromJson(withRest->toJson());
    REQUIRE(again);
    CHECK(*again == *withRest);

    // A non-finite number cannot be written.
    TrackMap broken = square();
    broken.centre[1].x = std::numeric_limits<double>::quiet_NaN();
    CHECK(broken.toJson().isNull());
}

TEST_CASE("track map: malformed documents are Invalid at a JSON pointer", "[unit]") {
    const std::string head = R"({"$schema": "urn:esses:track-1", "schemaVersion": 1, )";
    CHECK(detailOf(head + R"("direction": "left", "lines": {"centre": []}})") == "/direction");
    CHECK(detailOf(head + R"("lines": {"centre": [[0, 0]]}})") == "/lines/centre/0");
    CHECK(detailOf(head + R"("lines": {"centre": [[0, "a", 0]]}})") == "/lines/centre/0/1");
    CHECK(detailOf(head + R"("lines": {"centre": [], "middle": []}})") == "/lines/middle");
    CHECK(detailOf(R"({"$schema": "urn:esses:track-1", "schemaVersion": 1})") == ""); // no lines
    CHECK(detailOf(head + R"("lines": {"centre": []}, "length": {"v": 3, "unit": "furlong"}})") ==
          "/length/unit");
    CHECK(detailOf(head + R"("lines": {"centre": []}, "length": 3})") == "/length");
    CHECK(detailOf(head + R"("lines": {"centre": []}, "generatedFrom": {"method": "laser"}})") ==
          "/generatedFrom/method");
    CHECK(detailOf(head + R"("lines": {"centre": []}, "origin": {"lat": 1}})") == "/origin");
    CHECK(detailOf(head + R"("lines": {"centre": []}, "sections": [{"id": "a", "name": "A",
          "startDist": {"v": 0, "unit": "m"}}]})") == "/sections/0");
    CHECK(detailOf(head + R"("lines": {"centre": []}, "sections": [{"id": "a", "name": "A", "colour": "red",
          "startDist": {"v": 0, "unit": "m"}, "endDist": {"v": 20, "unit": "m"}}]})") ==
          "/sections/0/colour");
    auto other = TrackMap::fromJson(parse(R"({"$schema": "urn:esses:rule-1", "lines": {"centre": []}})"));
    REQUIRE_FALSE(other);
    CHECK(other.error().code == ErrorCode::Invalid);
    auto newer = TrackMap::fromJson(parse(R"({"$schema": "urn:esses:track-2", "lines": {"centre": []}})"));
    REQUIRE_FALSE(newer);
    CHECK(newer.error().code == ErrorCode::Unsupported);
    // The unit table: ft, mi, km, cm, mm, yd, in.
    auto feet =
        TrackMap::fromJson(parse(head + R"("lines": {"centre": []}, "length": {"v": 1000, "unit": "ft"}})"));
    REQUIRE(feet);
    CHECK(feet->lengthM == Catch::Approx(304.8));
    auto miles =
        TrackMap::fromJson(parse(head + R"("lines": {"centre": []}, "length": {"v": 2, "unit": "mi"}})"));
    REQUIRE(miles);
    CHECK(miles->lengthM == Catch::Approx(3218.688));
}

TEST_CASE("track map: validate() names the offending member", "[unit]") {
    CHECK(square().validate());

    TrackMap m = square();
    m.sections[0].endM = 9.5;
    m.sections[1].startM = 9.5;
    CHECK(validateDetail(m) == "/sections/0"); // shorter than 10 m
    m = square();
    m.sections[0].endM = 10.0 - 1e-9; // within the tolerance
    CHECK(m.validate());
    m = square();
    m.sections[1].endM = 40.5;
    CHECK(validateDetail(m) == "/sections/1/endDist");
    m = square();
    m.sections[0].startM = -1.0;
    CHECK(validateDetail(m) == "/sections/0/startDist");
    m = square();
    m.sections[1].id = "s1";
    CHECK(validateDetail(m) == "/sections/1/id");
    m = square();
    m.sections[1].id.clear();
    CHECK(validateDetail(m) == "/sections/1/id");
    m = square();
    m.sections[0].name.clear();
    CHECK(validateDetail(m) == "/sections/0/name");
    m = square();
    m.sections[0].set.clear();
    CHECK(validateDetail(m) == "/sections/0/set");
    m = square();
    m.sections[0].category.clear();
    CHECK(validateDetail(m) == "/sections/0/category");
    m = square();
    std::swap(m.centre[1], m.centre[2]);
    CHECK(validateDetail(m) == "/lines/centre/2");
    m = square();
    m.inner = {{0, std::numeric_limits<double>::infinity(), 0}};
    CHECK(validateDetail(m) == "/lines/inner/0");
    m = square();
    m.lengthM = -1.0;
    CHECK(validateDetail(m) == "/length");
    m = square();
    m.origin.lat = 91.0;
    CHECK(validateDetail(m) == "/origin");
    m = square();
    m.rotationDeg = 400.0;
    CHECK(validateDetail(m) == "/rotationDeg");
    // Overlapping sections of a user set are allowed.
    m = square();
    m.sections.push_back(TrackSection{"u1", "Braking", "A", 5.0, 30.0, "corner", true});
    m.sections.push_back(TrackSection{"u2", "Braking", "B", 15.0, 40.0, "corner", true});
    CHECK(m.validate());
}

TEST_CASE("track map: pointAt interpolates and wraps a closed line", "[unit]") {
    TrackMap m = square();
    CHECK(m.pointAt(5.0) == TrackPoint{5.0, 0.0, 5.0});
    CHECK(m.pointAt(25.0) == TrackPoint{5.0, 10.0, 25.0});
    CHECK(m.pointAt(40.0) == TrackPoint{0.0, 0.0, 0.0});  // the start again
    CHECK(m.pointAt(45.0) == TrackPoint{5.0, 0.0, 5.0});  // the next lap
    CHECK(m.pointAt(-5.0) == TrackPoint{0.0, 5.0, 35.0}); // backwards over the start line
    // Without the closing row the last segment runs back to the start.
    m.centre.pop_back();
    CHECK(m.pointAt(35.0) == TrackPoint{0.0, 5.0, 35.0});
    // An open line clamps.
    m = square();
    m.closed = false;
    CHECK(m.pointAt(-5.0) == TrackPoint{0.0, 0.0, -5.0 + 5.0});
    CHECK(m.pointAt(55.0) == TrackPoint{0.0, 0.0, 40.0});
    // No centreline: the origin.
    CHECK(TrackMap{}.pointAt(12.0) == TrackPoint{0.0, 0.0, 12.0});
}
