// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackMapFile (trackmaps/<venue>.track.json, track-1; 02 sections 6.5, 6.6 and 6.9; AC-M3-04 "user sections
// persist in trackmaps/<venue>.track.json and reload"): the schema examples, the round-trip rule, the load
// pipeline's checks, atomic save into a new trackmaps/ folder, and the file name of a venue: distinct venues
// never share a file, a file is read only for the venue it holds, and the files of the builds before D-205
// are still found for their own venue (review finding: venue file names collided).
#include "test_support.hpp"

#include <esses/analysis/section_detector.hpp>
#include <esses/base/utf8.hpp>
#include <esses/project/track_map_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <set>
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

// `name` is "<slug>-<16 lower-case hex digits>.track.json" (TrackMapFile::fileNameFor).
bool hashedName(const std::string& name, const std::string& slug) {
    constexpr std::string_view kExt = ".track.json";
    if (name.size() != slug.size() + 1 + 16 + kExt.size() || !name.starts_with(slug + "-") ||
        !name.ends_with(kExt)) {
        return false;
    }
    for (std::size_t i = slug.size() + 1; i < slug.size() + 17; ++i) {
        const char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

// The loop map of `venue`, named "<venue> map", saved where TrackMapService::setMap saves it (claimFor).
std::filesystem::path saveVenueMap(const std::filesystem::path& folder, const std::string& venue) {
    TrackMapFile f;
    f.map = loop();
    f.map.name = venue + " map";
    f.map.venue = venue;
    auto path = TrackMapFile::claimFor(folder, venue);
    REQUIRE(path);
    REQUIRE(f.save(*path));
    return *path;
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
    const std::filesystem::path path = project.path() / std::string(TrackMapFile::kDirectory) /
                                       pathFromUtf8(TrackMapFile::fileNameFor(f.map.venue));
    CHECK(pathToUtf8(path.filename()).starts_with("test-ring-"));
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
    // The readable slug, then the first 16 hex digits of xxh3-128 of the exact venue (D-205). The published
    // xxHash vectors (as libs/base/tests/hash_tests.cpp) pin the hash, so no build renames another's files.
    CHECK(TrackMapFile::fileNameFor("") == "track-99aa06d3014798d8.track.json");
    CHECK(TrackMapFile::fileNameFor("a") == "a-a96faf705af16834.track.json");
    CHECK(TrackMapFile::fileNameFor("abc") == "abc-06b05ab6733a6185.track.json");
    CHECK(hashedName(TrackMapFile::fileNameFor("Mount Panorama"), "mount-panorama"));
    CHECK(hashedName(TrackMapFile::fileNameFor("  Spa / Francorchamps (B) "), "spa-francorchamps-b"));
    CHECK(hashedName(TrackMapFile::fileNameFor("***"), "track"));
    CHECK(hashedName(TrackMapFile::fileNameFor("N\xC3\xBCrburgring"), "n\xC3\xBCrburgring"));
    // A Windows device name only ever starts a longer stem ("con-<hash>").
    CHECK(hashedName(TrackMapFile::fileNameFor("CON"), "con"));
    CHECK(hashedName(TrackMapFile::fileNameFor("Lpt1"), "lpt1"));
    // Unicode spaces, format characters and invalid UTF-8 separate like ASCII punctuation.
    CHECK(hashedName(TrackMapFile::fileNameFor("Mount\xC2\xA0Panorama"), "mount-panorama")); // no-break space
    CHECK(hashedName(TrackMapFile::fileNameFor("Spa\xE2\x80\x8B"), "spa")); // zero-width space
    CHECK(hashedName(TrackMapFile::fileNameFor("\xE2\x80\x8FMonza\xE2\x81\xA9"), "monza")); // bidi controls
    CHECK(hashedName(TrackMapFile::fileNameFor("Caf\xC3x"), "caf-x"));                      // a cut sequence
    CHECK(hashedName(TrackMapFile::fileNameFor("\xFF\xFE"), "track"));
    // A long venue: the slug stops at kMaxSlugBytes, between characters and never on a '-'.
    std::string euros = "a";
    std::string fifteen = "a";
    for (int i = 0; i < 20; ++i) {
        euros += "\xE2\x82\xAC"; // U+20AC, three bytes
        if (i < 15) {
            fifteen += "\xE2\x82\xAC";
        }
    }
    CHECK(hashedName(TrackMapFile::fileNameFor(std::string(300, 'x')), std::string(48, 'x')));
    CHECK(hashedName(TrackMapFile::fileNameFor(euros), fifteen));
    CHECK(hashedName(TrackMapFile::fileNameFor(std::string(47, 'y') + " z"), std::string(47, 'y')));
    for (const std::string& venue :
         {std::string(300, 'x'), euros, std::string("Caf\xC3x"), std::string("\xFF")}) {
        const std::string name = TrackMapFile::fileNameFor(venue);
        CHECK(name.size() <= 76);
        CHECK(utf8::isValid(name));
    }

    // The names of the builds before D-205, read for their own venue only (legacyFileNameFor).
    CHECK(TrackMapFile::legacyFileNameFor("Mount Panorama") == "mount-panorama.track.json");
    CHECK(TrackMapFile::legacyFileNameFor("  Spa / Francorchamps (B) ") == "spa-francorchamps-b.track.json");
    CHECK(TrackMapFile::legacyFileNameFor("") == "track.track.json");
    CHECK(TrackMapFile::legacyFileNameFor("***") == "track.track.json");
    CHECK(TrackMapFile::legacyFileNameFor("N\xC3\xBCrburgring") == "n\xC3\xBCrburgring.track.json");
    CHECK(TrackMapFile::legacyFileNameFor("CON") == "con-track.track.json");
    CHECK(TrackMapFile::legacyFileNameFor("Lpt1") == "lpt1-track.track.json");
    CHECK(TrackMapFile::legacyFileNameFor("COM10") == "com10.track.json");
}

TEST_CASE("track map file: distinct venues never share a file name", "[unit]") {
    // The review's examples: venues of one slug, and the names without a letter or digit, shared one legacy
    // file ("spa-francorchamps.track.json", "track.track.json").
    CHECK(TrackMapFile::legacyFileNameFor("Spa / Francorchamps") ==
          TrackMapFile::legacyFileNameFor("Spa-Francorchamps"));
    CHECK(TrackMapFile::legacyFileNameFor("***") == TrackMapFile::legacyFileNameFor("!!!"));
    const std::string noBreakSpace = "\xC2\xA0";
    const std::vector<std::string> venues{"Spa / Francorchamps",
                                          "Spa-Francorchamps",
                                          "spa-francorchamps",
                                          "SPA FRANCORCHAMPS",
                                          "Spa-Francorchamps ",
                                          "Spa" + noBreakSpace + "Francorchamps",
                                          "",
                                          " ",
                                          "***",
                                          "!!!",
                                          "-",
                                          "/",
                                          "?",
                                          "...",
                                          "\xE2\x80\x8B"};
    std::set<std::string> names;
    for (const std::string& venue : venues) {
        INFO(venue);
        const std::string name = TrackMapFile::fileNameFor(venue);
        CHECK(names.insert(name).second);
        CHECK(TrackMapFile::fileNameFor(venue) == name); // stable
    }
    CHECK(names.size() == venues.size());
}

TEST_CASE("track map file: a map saved for one venue is never loaded for another", "[unit]") {
    const TempDir project("trackmaps-venues");
    const std::filesystem::path folder = project.path() / std::string(TrackMapFile::kDirectory);
    const std::vector<std::string> venues{"Spa / Francorchamps", "Spa-Francorchamps", "***", "!!!", ""};
    std::set<std::filesystem::path> files;
    for (const std::string& venue : venues) {
        files.insert(saveVenueMap(folder, venue));
    }
    CHECK(files.size() == venues.size()); // one file each: none overwrote another
    std::vector<std::string> log;
    for (const std::string& venue : venues) {
        INFO(venue);
        auto loaded = TrackMapFile::loadFor(folder, venue, log);
        REQUIRE(loaded);
        CHECK(loaded->map.venue == venue);
        CHECK(loaded->map.name == venue + " map");
    }
    // A venue of the same slug without a map of its own has none (not a neighbour's).
    auto none = TrackMapFile::loadFor(folder, "Spa Francorchamps", log);
    REQUIRE_FALSE(none);
    CHECK(none.error().code == ErrorCode::NotFound);

    // A file under a venue's name that holds another venue's map is not that venue's map, and a write for
    // that venue is refused rather than overwriting it.
    const std::filesystem::path spa = folder / pathFromUtf8(TrackMapFile::fileNameFor("Spa / Francorchamps"));
    const std::filesystem::path squatted = folder / pathFromUtf8(TrackMapFile::fileNameFor("Elsewhere"));
    std::filesystem::copy_file(spa, squatted);
    auto foreign = TrackMapFile::loadFor(folder, "Elsewhere", log);
    REQUIRE_FALSE(foreign);
    CHECK(foreign.error().code == ErrorCode::NotFound);
    auto refused = TrackMapFile::claimFor(folder, "Elsewhere");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::Io);
    CHECK(refused.error().context.path == squatted);
    CHECK(readAll(squatted) == readAll(spa));
    CHECK(log.empty());
}

TEST_CASE("track map file: a file of an earlier build is read for its own venue and moves on its next write",
          "[unit]") {
    const TempDir project("trackmaps-legacy");
    const std::filesystem::path folder = project.path() / std::string(TrackMapFile::kDirectory);
    // An earlier build wrote "Spa / Francorchamps" under the slug the two venues share.
    TrackMapFile old;
    old.map = loop();
    old.map.name = "old";
    old.map.venue = "Spa / Francorchamps";
    const std::filesystem::path legacy =
        folder / pathFromUtf8(TrackMapFile::legacyFileNameFor(old.map.venue));
    CHECK(pathToUtf8(legacy.filename()) == "spa-francorchamps.track.json");
    REQUIRE(old.save(legacy));
    std::vector<std::string> log;
    auto found = TrackMapFile::loadFor(folder, "Spa / Francorchamps", log);
    REQUIRE(found);
    CHECK(found->map == old.map);
    auto notMine = TrackMapFile::loadFor(folder, "Spa-Francorchamps", log);
    REQUIRE_FALSE(notMine);
    CHECK(notMine.error().code == ErrorCode::NotFound);

    // The other venue's write leaves it alone; the venue's own write moves it to the hashed name.
    auto other = TrackMapFile::claimFor(folder, "Spa-Francorchamps");
    REQUIRE(other);
    CHECK(*other == folder / pathFromUtf8(TrackMapFile::fileNameFor("Spa-Francorchamps")));
    CHECK(std::filesystem::exists(legacy));
    auto mine = TrackMapFile::claimFor(folder, "Spa / Francorchamps");
    REQUIRE(mine);
    CHECK(*mine == folder / pathFromUtf8(TrackMapFile::fileNameFor("Spa / Francorchamps")));
    CHECK_FALSE(std::filesystem::exists(legacy));
    CHECK(readAll(*mine) == old.toJson());
    auto moved = TrackMapFile::loadFor(folder, "Spa / Francorchamps", log);
    REQUIRE(moved);
    CHECK(moved->map == old.map);

    // The hashed file wins over a legacy file of the same venue (one whose rename did not happen).
    TrackMapFile stale = old;
    stale.map.name = "stale";
    REQUIRE(stale.save(legacy));
    auto current = TrackMapFile::loadFor(folder, "Spa / Francorchamps", log);
    REQUIRE(current);
    CHECK(current->map.name == "old");

    // A legacy file that cannot be read is reported for the venues of its name, and never moved.
    const std::filesystem::path broken = folder / pathFromUtf8(TrackMapFile::legacyFileNameFor("!!!"));
    writeAll(broken, "{ not json");
    auto unreadable = TrackMapFile::loadFor(folder, "!!!", log);
    REQUIRE_FALSE(unreadable);
    CHECK(unreadable.error().code != ErrorCode::NotFound);
    CHECK(unreadable.error().context.path == broken);
    auto claimed = TrackMapFile::claimFor(folder, "!!!");
    REQUIRE(claimed);
    CHECK(std::filesystem::exists(broken));
    CHECK(log.empty());
}
