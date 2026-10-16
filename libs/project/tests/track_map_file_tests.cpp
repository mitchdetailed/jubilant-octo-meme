// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackMapFile (trackmaps/<venue>.track.json, track-1; 02 sections 6.5, 6.6 and 6.9; AC-M3-04 "user sections
// persist in trackmaps/<venue>.track.json and reload"): the schema examples, the round-trip rule, the load
// pipeline's checks, atomic save into a new trackmaps/ folder, and the file name of a venue.
#include "test_support.hpp"

#include <esses/analysis/section_detector.hpp>
#include <esses/project/track_map_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::project;
using namespace esses::project::test;

namespace {

std::filesystem::path examples(const char* kind) {
    return repoRoot() / "schemas" / "examples" / "track-1" / kind;
}

// A closed 1 km loop: two 300 m straights joined by semicircles of radius 63.66 m, every metre.
analysis::TrackMap loop() {
    analysis::TrackMap m;
    m.name = "Test Ring";
    m.venue = "Test Ring";
    m.origin = analysis::TrackOrigin{45.0, 7.5, 250.0};
    m.direction = analysis::TrackDirection::CounterClockwise;
    m.generatedFrom = analysis::TrackGeneratedFrom{analysis::TrackMethod::Gps, "xxh3:1", "lap-1"};
    const double r = 200.0 / std::numbers::pi;
    for (int i = 0; i <= 1000; ++i) {
        const double d = i;
        double x = 0.0;
        double y = 0.0;
        if (d <= 300.0) {
            x = d;
        } else if (d <= 500.0) {
            const double a = (d - 300.0) / r;
            x = 300.0 + r * std::sin(a);
            y = r - r * std::cos(a);
        } else if (d <= 800.0) {
            x = 300.0 - (d - 500.0);
            y = 2.0 * r;
        } else {
            const double a = (d - 800.0) / r;
            x = -r * std::sin(a);
            y = r + r * std::cos(a);
        }
        m.centre.push_back(
            analysis::TrackPoint{std::round(x * 1000.0) / 1000.0, std::round(y * 1000.0) / 1000.0, d});
    }
    m.centre.back().x = 0.0;
    m.centre.back().y = 0.0;
    m.lengthM = 1000.0;
    return m;
}

} // namespace

TEST_CASE("track map file: the valid schema examples load, the invalid ones do not", "[unit]") {
    for (const auto& path : jsonFiles(examples("valid"))) {
        INFO(path.filename().string());
        std::vector<std::string> log;
        auto f = TrackMapFile::load(path, log);
        REQUIRE(f);
        CHECK(f->schemaVersion == 1);
        CHECK(log.empty());
    }
    for (const auto& path : jsonFiles(examples("invalid"))) {
        INFO(path.filename().string());
        std::vector<std::string> log;
        auto f = TrackMapFile::load(path, log);
        REQUIRE_FALSE(f);
        CHECK(f.error().context.path == path);
    }
}

TEST_CASE("track map file: canonical files round-trip byte for byte", "[unit]") {
    std::vector<std::string> log;
    auto full = TrackMapFile::load(examples("valid") / "full.json", log);
    REQUIRE(full);
    const std::string once = full->toJson();
    REQUIRE(!once.empty());
    auto again = TrackMapFile::parse(once, log);
    REQUIRE(again);
    CHECK(again->toJson() == once); // save(load(f)) == f for a canonical, current file
    CHECK(again->map == full->map);
    CHECK(once.find("\"unit\": \"km\"") == std::string::npos); // lengths are written in metres

    TrackMapFile f;
    f.map = loop();
    REQUIRE(analysis::SectionDetector::defaults(f.map));
    const std::string text = f.toJson();
    auto parsed = TrackMapFile::parse(text, log);
    REQUIRE(parsed);
    CHECK(parsed->map == f.map);
    CHECK(parsed->toJson() == text);
    CHECK(log.empty());
}

TEST_CASE("track map file: user sections persist in trackmaps/<venue>.track.json and reload",
          "[unit][AC-M3-04]") {
    const TempDir project("trackmaps");
    TrackMapFile f;
    f.map = loop();
    REQUIRE(analysis::SectionDetector::defaults(f.map));
    f.map.sections.push_back(
        analysis::TrackSection{"u1", "Driver Notes", "T1 entry", 280.0, 330.0, "Braking", true});
    f.map.sections.push_back(
        analysis::TrackSection{"u2", "Driver Notes", "Back", 500.0, 800.0, "Fast", true});
    const std::filesystem::path path =
        project.path() / std::string(TrackMapFile::kDirectory) / TrackMapFile::fileNameFor(f.map.venue);
    CHECK(path.filename() == "test-ring.track.json");
    REQUIRE(f.save(path)); // creates trackmaps/
    std::vector<std::string> log;
    auto loaded = TrackMapFile::load(path, log);
    REQUIRE(loaded);
    CHECK(loaded->map == f.map);
    CHECK(loaded->map.sectionSets() == std::vector<std::string>{"Default", "Driver Notes"});
    CHECK(readAll(path) == f.toJson());
    // The saved file validates against the schema as esses-cli validate does.
    CHECK(schemaErrors(readAll(path), "track-1").empty());

    // Regenerating the default sections keeps the user sections.
    TrackMapFile edited = *loaded;
    REQUIRE(analysis::SectionDetector::defaults(edited.map));
    CHECK(edited.map == loaded->map);
}

TEST_CASE("track map file: the load pipeline's checks", "[unit]") {
    std::vector<std::string> log;
    const std::string head =
        R"({"$schema": "urn:esses:track-1", "schemaVersion": 1, "lines": {"centre": [[0, 0, 0],
        [40, 0, 40]]}, "length": {"v": 40, "unit": "m"}, )";
    // A section shorter than 10 m (the map's own check).
    auto shortSection = TrackMapFile::parse(head + R"("sections": [{"id": "a", "name": "A",
        "startDist": {"v": 0, "unit": "m"}, "endDist": {"v": 5, "unit": "m"}}]})",
                                            log);
    REQUIRE_FALSE(shortSection);
    CHECK(shortSection.error().code == ErrorCode::Invalid);
    CHECK(shortSection.error().context.detail == "/sections/0");
    // Duplicate ids.
    auto duplicate = TrackMapFile::parse(head + R"("sections": [
        {"id": "a", "name": "A", "startDist": {"v": 0, "unit": "m"}, "endDist": {"v": 20, "unit": "m"}},
        {"id": "a", "name": "B", "startDist": {"v": 20, "unit": "m"}, "endDist": {"v": 40, "unit": "m"}}]})",
                                         log);
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().context.detail == "/sections/1/id");
    // A missing header is read as track-1 and logged; a newer minor is kept on save.
    log.clear();
    auto bare = TrackMapFile::parse(R"({"lines": {"centre": []}})", log);
    REQUIRE(bare);
    CHECK(log.size() == 2);
    CHECK(bare->toJson().find("\"$schema\": \"urn:esses:track-1\"") != std::string::npos);
    log.clear();
    auto newer = TrackMapFile::parse(R"({"$schema": "urn:esses:track-1", "schemaVersion": 3,
        "lines": {"centre": []}})",
                                     log);
    REQUIRE(newer);
    CHECK(newer->schemaVersion == 3);
    CHECK(newer->toJson().find("\"schemaVersion\": 3") != std::string::npos);
    // Another major.
    auto major = TrackMapFile::parse(R"({"$schema": "urn:esses:track-2", "lines": {"centre": []}})", log);
    REQUIRE_FALSE(major);
    CHECK(major.error().code == ErrorCode::Unsupported);
    // A missing file.
    auto missing = TrackMapFile::load(repoRoot() / "no-such.track.json", log);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::Io);
}

TEST_CASE("track map file: the file name of a venue", "[unit]") {
    CHECK(TrackMapFile::fileNameFor("Mount Panorama") == "mount-panorama.track.json");
    CHECK(TrackMapFile::fileNameFor("  Spa / Francorchamps (B) ") == "spa-francorchamps-b.track.json");
    CHECK(TrackMapFile::fileNameFor("") == "track.track.json");
    CHECK(TrackMapFile::fileNameFor("***") == "track.track.json");
    CHECK(TrackMapFile::fileNameFor("N\xC3\xBCrburgring") == "n\xC3\xBCrburgring.track.json");
    CHECK(TrackMapFile::fileNameFor("CON") == "con-track.track.json");
    CHECK(TrackMapFile::fileNameFor("Lpt1") == "lpt1-track.track.json");
    CHECK(TrackMapFile::fileNameFor("COM10") == "com10.track.json");
}
