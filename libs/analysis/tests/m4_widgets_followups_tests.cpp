// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// M4 followups in esses_analysis (D-211; docs/milestones/M3.md section 6 items 19 and 22): a track section's
// speed class (06 section 13.5 item 4; track-1 "speedClass", slow or fast, absent = none) and a channel
// report channel's display unit (channel_report-1 "displayUnit", absent = the channel's default). Both are
// read, written only when set (so documents without them are unchanged) and checked.
#include "analysis_test_support.hpp"

#include <esses/analysis/report_spec.hpp>
#include <esses/analysis/track_map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace esses;
using namespace esses::analysis;

namespace {

JsonDoc parse(std::string_view text) {
    auto doc = JsonDoc::parse(text);
    REQUIRE(doc);
    return std::move(doc).value();
}

TrackMap square() {
    TrackMap m;
    m.name = "Square";
    m.venue = "Square";
    m.lengthM = 40.0;
    m.centre = {{0, 0, 0}, {10, 0, 10}, {10, 10, 20}, {0, 10, 30}, {0, 0, 40}};
    m.sections = {TrackSection{"s1", "Default", "Straight 1", 0.0, 20.0, "straight", false, ""},
                  TrackSection{"t1", "Default", "Turn 1", 20.0, 40.0, "corner", true, ""}};
    return m;
}

} // namespace

TEST_CASE("followups D-211: a track section's speed class reads, writes only when set and is checked",
          "[unit]") {
    TrackMap m = square();
    // Without a class nothing is written: maps written before M4 stay byte-identical.
    CHECK(m.toJson().compact().find("speedClass") == std::string::npos);
    m.sections[1].speedClass = "slow";
    REQUIRE(m.validate());
    const JsonDoc doc = m.toJson();
    const std::string text = doc.compact();
    CHECK(text.find(R"("speedClass":"slow")") != std::string::npos);
    CHECK(text.find("speedClass") == text.rfind("speedClass")); // only the classed section carries it
    const Result<TrackMap> back = TrackMap::fromJson(doc);
    REQUIRE(back);
    CHECK(back->sections[0].speedClass.empty());
    CHECK(back->sections[1].speedClass == "slow");
    CHECK(*back == m);
    // fast is the other class; anything else is refused at its pointer.
    m.sections[0].speedClass = "fast";
    CHECK(m.validate());
    m.sections[0].speedClass = "medium";
    const Result<void> bad = m.validate();
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == ErrorCode::Invalid);
    CHECK(bad.error().context.detail.value_or("") == "/sections/0/speedClass");
    // A class that is not text does not read.
    const Result<TrackMap> wrongType = TrackMap::fromJson(parse(
        R"({"$schema":"urn:esses:track-1","schemaVersion":1,"lines":{"centre":[[0,0,0],[0,40,40]]},)"
        R"("sections":[{"id":"t1","name":"T1","startDist":{"v":0,"unit":"m"},"endDist":{"v":20,"unit":"m"},)"
        R"("speedClass":1}]})"));
    REQUIRE_FALSE(wrongType);
    CHECK(wrongType.error().context.detail.value_or("") == "/sections/0/speedClass");
}

TEST_CASE("followups D-211: a report channel's display unit reads, writes only when set and round-trips",
          "[unit]") {
    const Result<ReportSpec> spec = ReportSpec::fromJson(
        parse(R"({"channels":[{"name":"Corr Speed","stats":["max"],"displayUnit":"km/h"},)"
              R"({"name":"Throttle Pos","stats":["avg"]}]})"));
    REQUIRE(spec);
    REQUIRE(spec->channels.size() == 2);
    CHECK(spec->channels[0].displayUnit == "km/h");
    CHECK(spec->channels[1].displayUnit.empty());
    const std::string text = spec->toJson().compact();
    CHECK(text.find(R"("displayUnit":"km/h")") != std::string::npos);
    CHECK(text.find("displayUnit") == text.rfind("displayUnit")); // the unset one is not written
    const Result<ReportSpec> back = ReportSpec::fromJson(spec->toJson());
    REQUIRE(back);
    CHECK(back->channels == spec->channels);
    const Result<ReportSpec> bad =
        ReportSpec::fromJson(parse(R"({"channels":[{"name":"X","displayUnit":2}]})"));
    REQUIRE_FALSE(bad);
    CHECK(bad.error().context.detail.value_or("") == "/channels/0/displayUnit");
}
