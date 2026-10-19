// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SectionDetector::defaults (06 section 13.5 item 3, D-027, AC-M3-04): Turns and Straights from the
// centreline curvature on exact geometry built here (straights and circular arcs), the 10 m minimum, tiling
// of [0, length], names and ids in track order, user sections kept, S-bends, kinks and runs across the start
// line.
#include "analysis_test_support.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::analysis;
using namespace esses::analysis::test;

namespace {

// A centreline from straights and arcs (signed angle in degrees, radius), sampled every metre from the
// start, heading east. Closed when the pieces return to the start.
struct Builder {
    double x = 0.0;
    double y = 0.0;
    double heading = 0.0;
    double dist = 0.0;
    std::vector<TrackPoint> pts{TrackPoint{0.0, 0.0, 0.0}};

    Builder& straight(double length) {
        const auto n = static_cast<int>(std::round(length));
        for (int i = 0; i < n; ++i) {
            x += std::cos(heading);
            y += std::sin(heading);
            dist += 1.0;
            pts.push_back(TrackPoint{x, y, dist});
        }
        return *this;
    }
    Builder& arc(double angleDeg, double radius) {
        const double angle = angleDeg * std::numbers::pi / 180.0;
        const double length = std::abs(angle) * radius;
        const auto n = static_cast<int>(std::ceil(length));
        const double ds = length / n;
        const double k = (angle > 0 ? 1.0 : -1.0) / radius;
        for (int i = 0; i < n; ++i) {
            const double mid = heading + 0.5 * k * ds;
            x += ds * std::cos(mid);
            y += ds * std::sin(mid);
            heading += k * ds;
            dist += ds;
            pts.push_back(TrackPoint{x, y, dist});
        }
        return *this;
    }
    TrackMap map(bool closed = true) const {
        TrackMap m;
        m.centre = pts;
        m.closed = closed;
        if (closed) {
            m.centre.back().x = 0.0; // close exactly
            m.centre.back().y = 0.0;
        }
        m.lengthM = dist;
        return m;
    }
};

// A stadium: straights of 200 m and two left semicircles of radius 50 m, starting mid-straight.
TrackMap stadium() {
    return Builder{}.straight(100).arc(180, 50).straight(200).arc(180, 50).straight(100).map();
}

std::vector<std::string> names(const TrackMap& m) {
    std::vector<std::string> out;
    out.reserve(m.sections.size());
    for (const TrackSection& s : m.sections) {
        out.push_back(s.name);
    }
    return out;
}

void checkTiling(const TrackMap& m) {
    std::vector<const TrackSection*> generated;
    for (const TrackSection& s : m.sections) {
        if (s.set == "Default" && !s.userDefined) {
            generated.push_back(&s);
        }
    }
    REQUIRE(!generated.empty());
    CHECK(generated.front()->startM == 0.0);
    CHECK(generated.back()->endM == m.lengthM);
    for (std::size_t i = 0; i < generated.size(); ++i) {
        CHECK(generated[i]->lengthM() >= TrackMap::kMinSectionM);
        if (i > 0) {
            CHECK(generated[i]->startM == generated[i - 1]->endM);
        }
    }
    CHECK(m.validate());
}

} // namespace

TEST_CASE("section detector: a stadium has two turns and three straight pieces", "[unit][AC-M3-04]") {
    TrackMap m = stadium();
    REQUIRE(SectionDetector::defaults(m));
    checkTiling(m);
    CHECK(names(m) == std::vector<std::string>{"Straight 1", "Turn 1", "Straight 2", "Turn 2", "Straight 3"});
    REQUIRE(m.sections.size() == 5);
    CHECK(m.sections[1].id == "t1");
    CHECK(m.sections[2].id == "s2");
    CHECK(m.sections[1].category == "corner");
    CHECK(m.sections[0].category == "straight");
    CHECK(m.sections[1].set == "Default");
    CHECK_FALSE(m.sections[1].userDefined);
    // The detected turn covers the arc (100 m to 257 m) and reaches about smoothingM into the straights.
    CHECK(m.sections[1].startM == Catch::Approx(100.0).margin(25.0));
    CHECK(m.sections[1].endM == Catch::Approx(100.0 + 50.0 * std::numbers::pi).margin(25.0));
    CHECK(m.sections[1].startM < 100.0);
}

TEST_CASE("section detector: user sections stay, generated ones are replaced", "[unit][AC-M3-04]") {
    TrackMap m = stadium();
    m.sections.push_back(TrackSection{"u1", "Driver Notes", "Back straight", 300.0, 450.0, "Fast", true, ""});
    m.sections.push_back(TrackSection{"t1", "Default", "Hairpin entry", 80.0, 110.0, "corner", true, ""});
    m.sections.push_back(TrackSection{"s9", "Default", "Old generated", 0.0, 50.0, "straight", false, ""});
    m.sections.push_back(TrackSection{"x1", "Imported", "Kept", 0.0, 50.0, "straight", false, ""});
    REQUIRE(SectionDetector::defaults(m));
    checkTiling(m);
    CHECK(names(m) == std::vector<std::string>{"Straight 1", "Turn 1", "Straight 2", "Turn 2", "Straight 3",
                                               "Back straight", "Hairpin entry", "Kept"});
    CHECK(m.sections[1].id == "t1-2"); // "t1" belongs to a user section
    CHECK(m.sectionSets() == std::vector<std::string>{"Default", "Driver Notes", "Imported"});
    // Run twice: the same result.
    TrackMap again = m;
    REQUIRE(SectionDetector::defaults(again));
    CHECK(again == m);
}

TEST_CASE("section detector: short pieces are absorbed", "[unit][AC-M3-04]") {
    SECTION("a short straight between two left turns joins them") {
        TrackMap m = Builder{}
                         .straight(150)
                         .arc(90, 40)
                         .straight(4)
                         .arc(90, 40)
                         .straight(300)
                         .arc(180, 50)
                         .straight(150)
                         .map(false);
        REQUIRE(SectionDetector::defaults(m));
        checkTiling(m);
        CHECK(names(m) ==
              std::vector<std::string>{"Straight 1", "Turn 1", "Straight 2", "Turn 2", "Straight 3"});
    }
    SECTION("an S-bend stays two turns") {
        TrackMap m = Builder{}.straight(150).arc(90, 40).arc(-90, 40).straight(150).map(false);
        REQUIRE(SectionDetector::defaults(m));
        checkTiling(m);
        CHECK(names(m) == std::vector<std::string>{"Straight 1", "Turn 1", "Turn 2", "Straight 2"});
    }
    SECTION("a kink below the turn angle is part of the straight") {
        TrackMap m = Builder{}.straight(150).arc(10, 60).straight(150).map(false);
        REQUIRE(SectionDetector::defaults(m));
        CHECK(names(m) == std::vector<std::string>{"Straight 1"});
        SectionDetectorOptions o;
        o.minTurnDeg = 5.0;
        REQUIRE(SectionDetector::defaults(m, o));
        checkTiling(m);
        CHECK(names(m) == std::vector<std::string>{"Straight 1", "Turn 1", "Straight 2"});
    }
    SECTION("a gentle bend above the corner radius is part of the straight") {
        TrackMap m = Builder{}.straight(150).arc(30, 400).straight(150).map(false);
        REQUIRE(SectionDetector::defaults(m));
        CHECK(names(m) == std::vector<std::string>{"Straight 1"});
    }
}

TEST_CASE("section detector: a turn across the start line is cut there", "[unit][AC-M3-04]") {
    // The stadium started 20 m before the end of a turn: the first turn piece is about 20 m plus the reach of
    // the curvature window, the last turn piece takes the rest.
    TrackMap m = Builder{}
                     .arc(180.0 * 20.0 / (50.0 * std::numbers::pi), 50)
                     .straight(200)
                     .arc(180, 50)
                     .straight(200)
                     .arc(180.0 - 180.0 * 20.0 / (50.0 * std::numbers::pi), 50)
                     .map();
    REQUIRE(SectionDetector::defaults(m));
    checkTiling(m);
    REQUIRE(m.sections.size() == 5);
    CHECK(m.sections.front().category == "corner");
    CHECK(m.sections.back().category == "corner");
    CHECK(names(m) == std::vector<std::string>{"Turn 1", "Straight 1", "Turn 2", "Straight 2", "Turn 3"});

    // Started 3 m before the end of the turn: the 3 m piece cannot stand alone and joins the straight after
    // it.
    TrackMap tight = Builder{}
                         .arc(180.0 * 3.0 / (50.0 * std::numbers::pi), 50)
                         .straight(200)
                         .arc(180, 50)
                         .straight(200)
                         .arc(180.0 - 180.0 * 3.0 / (50.0 * std::numbers::pi), 50)
                         .map();
    SectionDetectorOptions o;
    o.smoothingM = 2.0; // a sharp window, so the cut piece stays short
    REQUIRE(SectionDetector::defaults(tight, o));
    checkTiling(tight);
    CHECK(tight.sections.front().category == "straight");
    CHECK(tight.sections.back().category == "corner");
}

TEST_CASE("section detector: invalid maps and options", "[unit]") {
    TrackMap empty;
    CHECK(SectionDetector::defaults(empty).error().code == ErrorCode::Invalid);
    TrackMap m = stadium();
    m.lengthM = 0.0;
    CHECK(SectionDetector::defaults(m).error().code == ErrorCode::Invalid);
    m = stadium();
    SectionDetectorOptions o;
    o.cornerRadiusM = 0.0;
    CHECK(SectionDetector::defaults(m, o).error().code == ErrorCode::Invalid);
    // A minimum below 10 m is raised to it.
    o = SectionDetectorOptions{};
    o.minSectionM = 1.0;
    REQUIRE(SectionDetector::defaults(m, o));
    checkTiling(m);
    // An oval of constant curvature is one turn.
    TrackMap circle = Builder{}.arc(360, 60).map();
    REQUIRE(SectionDetector::defaults(circle));
    CHECK(names(circle) == std::vector<std::string>{"Turn 1"});
    checkTiling(circle);
}
