// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The M3 contract of esses_analysis (07 section 6.1): what is final in the new headers. The value types'
// defaults follow the design (04 sections 10-13, 02 section 6.9), the statistic and scope names are the body
// spellings, TrackMap::sectionSets orders "Default" first, the track-sections rule is phase 2 and stays
// skipped by RuleRunner::fromRuleIds until its parser exists, ReportTable counts pending cells. The M3-STUB
// functions are tested by their owners (src/track, src/reports, src/binning, src/variance, src/speed).
#include "analysis_test_support.hpp"

#include <esses/analysis/analysis.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::analysis;
using esses::analysis::test::repoRoot;

TEST_CASE("m3 contract: report statistic and scope names", "[unit]") {
    const std::vector<std::string> names{"avg", "min",    "max",    "absMax", "start",
                                         "end", "change", "stdDev", "delta"};
    for (const std::string& name : names) {
        INFO(name);
        const std::optional<ReportStat> stat = parseReportStat(name);
        REQUIRE(stat.has_value());
        if (stat) {
            CHECK(reportStatName(*stat) == name);
        }
    }
    CHECK_FALSE(parseReportStat("median").has_value());
    CHECK_FALSE(parseReportStat("Avg").has_value());
    for (const char* scope : {"selectedRanges", "selectedOutings", "allOutings"}) {
        INFO(scope);
        const std::optional<ReportScope> s = parseReportScope(scope);
        REQUIRE(s.has_value());
        if (s) {
            CHECK(reportScopeName(*s) == scope);
        }
    }
    CHECK_FALSE(parseReportScope("all").has_value());
}

TEST_CASE("m3 contract: value-type defaults follow the design", "[unit]") {
    const ReportSpec report;
    CHECK(report.scope == ReportScope::SelectedRanges);
    CHECK(report.headers == std::vector<std::string>{"Outings:Laps"});
    CHECK(report.options.probeSeconds == 5.0);
    CHECK_FALSE(report.options.includeUntrusted);
    CHECK_FALSE(report.deltaVsOverlay0);

    const TimeReportSpec time;
    CHECK(time.scope == ReportScope::SelectedOutings);
    CHECK(time.rowGroup == "Outings:Laps:Track Sections:Default");
    CHECK_FALSE(time.includeUntrusted);
    CHECK((time.summaries.eclectic && time.summaries.rollingMin && time.summaries.total));

    CHECK(TrackMap::kMinSectionM == 10.0);
    CHECK(TrackMap::kDefaultSet == "Default");
    CHECK(SectionDetectorOptions{}.minSectionM == TrackMap::kMinSectionM);
    CHECK(TrendFit::kMaxOrder == 6);
    CHECK(ScatterBinner::kDensityThreshold == 1000000);
    CHECK(StatCache::kDefaultBudgetBytes == std::size_t{64} * 1024 * 1024);

    const TrackSectionsSettings sections;
    CHECK(sections.parent == "Outings:Laps");
    CHECK(sections.categoryGroups.at("corner") == "Corners");
    CHECK(sections.categoryGroups.at("straight") == "Straights");
}

TEST_CASE("m3 contract: TrackMap::sectionSets lists Default first, then first appearance", "[unit]") {
    TrackMap map;
    CHECK(map.sectionSets().empty());
    map.sections.push_back(TrackSection{"u1", "Driver Notes", "Back", 0.0, 20.0, "Fast", true});
    map.sections.push_back(TrackSection{"t1", "Default", "Turn 1", 20.0, 40.0, "corner", false});
    map.sections.push_back(TrackSection{"u2", "Braking", "T1 entry", 15.0, 30.0, "corner", true});
    map.sections.push_back(TrackSection{"s1", "Default", "Straight 1", 0.0, 20.0, "straight", false});
    CHECK(map.sectionSets() == std::vector<std::string>{"Default", "Driver Notes", "Braking"});
    CHECK(map.sections[1].lengthM() == 20.0);
}

TEST_CASE("m3 contract: the track-sections rule is phase 2 and skipped until its parser exists", "[unit]") {
    const TrackSectionsRule rule;
    CHECK(rule.kind() == "track-sections");
    CHECK(rule.phase() == 2);
    CHECK(rule.id() == "circuit.track-sections");
    CHECK(rule.appliesTo("Normal"));
    const auto doc = RuleDocument::load(repoRoot() / "data" / "rules" / "circuit.track-sections.rule.json");
    REQUIRE(doc);
    const auto made = makeRangeRule(*doc);
    if (!made) {
        CHECK(made.error().code == ErrorCode::Unsupported); // M3-STUB: fromRuleIds skips it
    } else {
        CHECK((*made)->kind() == "track-sections");
    }
}

TEST_CASE("m3 contract: ReportTable counts its pending cells", "[unit]") {
    ReportTable table;
    table.columns.resize(2);
    table.rows.resize(2);
    table.cells.resize(4);
    table.cells[1].state = ReportCellState::Pending;
    table.cells[3].state = ReportCellState::Pending;
    table.cells[2].state = ReportCellState::Ok;
    CHECK(table.pending() == 2);
    CHECK(table.at(1, 1).state == ReportCellState::Pending);
    CHECK(table.at(1, 0).state == ReportCellState::Ok);
}

TEST_CASE("m3 contract: StatCache keeps its budget", "[unit]") {
    const StatCache cache(1024);
    CHECK(cache.budgetBytes() == 1024);
    CHECK(cache.bytes() == cache.size() * StatCache::kBytesPerCell);
}
