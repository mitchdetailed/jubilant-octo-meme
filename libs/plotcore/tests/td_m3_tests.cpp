// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The time_distance completion (04 section 8, parity-first section 5.4; 07 section 6.1): the M3 body members,
// the layout of the bands, the status strip, the variance panel, the offset axis and the scrollbars,
// overlapped mode, the key's datum and measurement columns against values computed here from the samples, the
// variance tables (one per visible overlay, none without one: AC-M3-05), scale lines and the crosshair, the
// hit regions and what a Static build reports to the widget; plus the DrawList JSON goldens of the 13 M3
// worksheet cases of td_m3_cases.hpp at DPR 1 and 2 (AC-M3-02; tests/golden/drawlist/time_distance/, exact on
// every OS).

#include "m3_golden_support.hpp"
#include "td_m3_cases.hpp"

#include <esses/analysis/variance_table.hpp>
#include <esses/plotcore/time_distance_builder.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::plot;

namespace {

bool hasText(const DrawList& list, std::string_view needle) {
    return std::any_of(list.texts.begin(), list.texts.end(), [&](const TextRun& r) {
        return list.textOf(r).find(needle) != std::string_view::npos;
    });
}

const TextRun* findText(const DrawList& list, std::string_view text) {
    for (const TextRun& r : list.texts) {
        if (list.textOf(r) == text) {
            return &r;
        }
    }
    return nullptr;
}

std::size_t countCmd(const DrawList& list, CmdKind kind, Rgba colour) {
    return static_cast<std::size_t>(std::count_if(list.cmds.begin(), list.cmds.end(), [&](const DrawCmd& c) {
        return c.kind == kind && c.colour == colour;
    }));
}

// A copy (GCC 13's -Wdangling-reference flags a reference returned from a call with a temporary argument).
test::M3Case caseNamed(const std::vector<test::M3Case>& cases, const std::string& name) {
    for (const test::M3Case& c : cases) {
        if (c.name == name) {
            return c;
        }
    }
    FAIL("no case " << name);
    return cases.front();
}

std::string formatFixed(double v, int dps) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", dps, v);
    std::string s(buf);
    if (s.front() == '-' && s.find_first_not_of("-0.") == std::string::npos) {
        s.erase(0, 1);
    }
    return s;
}

struct Brute {
    double min = std::numeric_limits<double>::infinity();
    double max = -std::numeric_limits<double>::infinity();
    double sum = 0.0;
    int count = 0;
    Micros tMin = 0;
    Micros tMax = 0;
};

// The valid samples of a uniform series in [t0, t1), read one by one.
Brute bruteForce(const store::Series& series, Micros t0, Micros t1) {
    Brute b;
    const auto& u = std::get<store::UniformSeries>(series);
    const store::Column& c = u.values;
    REQUIRE(c.require(0, c.size() - 1, Priority::Visible, CancelToken{}).ok());
    for (SampleIdx i = 0; i < c.size(); ++i) {
        const Micros t = u.rate.timeAt(i, u.t0);
        const double v = c.at(i);
        if (t < t0 || t >= t1 || !std::isfinite(v)) {
            continue;
        }
        if (v < b.min) {
            b.min = v;
            b.tMin = t;
        }
        if (v > b.max) {
            b.max = v;
            b.tMax = t;
        }
        b.sum += v;
        ++b.count;
    }
    return b;
}

} // namespace

TEST_CASE("TimeDistanceBody: the M3 members parse, validate and round-trip", "[unit]") {
    const std::filesystem::path dir = std::filesystem::path(ESSES_TEST_REPO_ROOT) / "schemas" / "examples" /
                                      "components" / "time_distance-1";
    const Result<JsonDoc> full = JsonDoc::load(dir / "valid" / "full.json");
    REQUIRE(full.ok());
    const Result<TimeDistanceBody> body = TimeDistanceBody::parse(*full);
    REQUIRE(body.ok());
    CHECK(body->pointSize == 2.0F);
    CHECK(body->showMarkers);
    CHECK(body->showBands);
    CHECK(body->bandMode == "sections");
    CHECK(body->bandGroup == "Outings:Laps:Track Sections:Default");
    CHECK(body->overlapMode == "panels");
    CHECK(body->cursorStyle == "line");
    CHECK(body->cursorSnap);
    CHECK_FALSE(body->keyStats);
    CHECK_FALSE(body->showStatusStrip);
    CHECK(body->showDeviceEvents);
    CHECK_FALSE(body->variance.show);
    CHECK(body->variance.heightFrac == 0.15);
    CHECK(body->variance.min == analysis::UnitValue{-2.0, "s"});
    CHECK(body->showXScrollbar);
    REQUIRE(body->groups.size() == 2);
    CHECK(body->groups[0].hi == 0.33);
    CHECK(body->groups[0].zeroLine);
    CHECK_FALSE(body->groups[0].minLine.enabled);
    CHECK(TimeDistanceBody::parse(body->toJson())->toJson() == body->toJson());
    CHECK(body->toJson() == *full); // parse(x).toJson() == x for the shipped example

    // Every M3 member edited survives a round trip.
    TimeDistanceBody edited = *body;
    edited.overlapMode = "overlapped";
    edited.cursorStyle = "crosshair";
    edited.keyStats = true;
    edited.showStatusStrip = true;
    edited.statusChannels = {"Engine Status"};
    edited.showDeviceEvents = false;
    edited.variance.show = true;
    edited.variance.max = analysis::UnitValue{0.5, "s"};
    edited.showXScrollbar = false;
    edited.groups[0].lo = 0.1;
    edited.groups[0].hi = 0.8;
    edited.groups[0].zeroColour = 0x6A1B9AFFU;
    edited.groups[0].minLine = TdScaleLine{true, 0x2E7D32FFU};
    edited.groups[1].maxLine.enabled = true;
    const Result<TimeDistanceBody> again = TimeDistanceBody::parse(edited.toJson());
    REQUIRE(again.ok());
    CHECK(again->overlapped());
    CHECK(again->cursorStyle == "crosshair");
    CHECK(again->keyStats);
    CHECK(again->statusChannels == std::vector<std::string>{"Engine Status"});
    CHECK_FALSE(again->showDeviceEvents);
    CHECK(again->variance.show);
    CHECK(again->variance.max == analysis::UnitValue{0.5, "s"});
    CHECK_FALSE(again->showXScrollbar);
    CHECK(again->groups[0].lo == 0.1);
    CHECK(again->groups[0].hi == 0.8);
    CHECK(again->groups[0].zeroColour == 0x6A1B9AFFU);
    CHECK(again->groups[0].minLine == TdScaleLine{true, 0x2E7D32FFU});
    CHECK(again->groups[1].maxLine.enabled);
    CHECK(edited.toJson().compact().find(R"("colour":"#6a1b9a")") != std::string::npos);
    // A cleared colour leaves the body.
    TimeDistanceBody cleared = *again;
    cleared.groups[0].zeroColour.reset();
    CHECK(cleared.toJson().compact().find("#6a1b9a") == std::string::npos);

    // Enumerations and colours are checked, with the pointer of the member.
    const auto errorAt = [](const char* json) {
        const Result<TimeDistanceBody> r = TimeDistanceBody::parse(*model::OpaqueJson::parse(json));
        return r.ok() ? std::string("ok") : r.error().context.detail;
    };
    CHECK(errorAt(R"({"display":{"overlapMode":"stacked"}})") == "/display/overlapMode");
    CHECK(errorAt(R"({"display":{"cursorStyle":"dot"}})") == "/display/cursorStyle");
    CHECK(errorAt(R"({"display":{"bandMode":1}})") == "/display/bandMode");
    CHECK(errorAt(R"({"display":{"variance":{"heightFrac":"x"}}})") == "/display/variance/heightFrac");
    CHECK(errorAt(R"({"groups":[{"lines":{"min":{"colour":"red"}}}]})") == "/groups/0/lines/min/colour");
    CHECK(errorAt(R"({"groups":[{"lo":"x"}]})") == "/groups/0/lo");
    CHECK(errorAt(R"({"display":{"statusChannels":[1]}})") == "/display/statusChannels");
}

TEST_CASE(
    "TimeDistanceBuilder: the M3 layout stacks bands, status strip, variance, offset axis and scrollbar",
    "[unit]") {
    const test::M3Fixture m;
    const auto cases = test::m3Cases(m);
    const test::M3Case full = caseNamed(cases, "dark_full_anatomy");
    for (const float dpr : {1.0F, 2.0F}) {
        const ComponentBuildInputs in = test::m3InputsOf(m, full, dpr);
        const TdLayout lay = TimeDistanceBuilder::layoutOf(in);
        const float band = std::round(14.0F * dpr);
        CHECK(lay.bands.range.h == band);
        CHECK(lay.bands.section.h == band);
        CHECK(lay.bands.marker.h == band);
        CHECK(lay.bands.range.y == lay.caption.y + lay.caption.h);
        CHECK(lay.bands.range.x == lay.traceArea.x);
        CHECK(lay.bands.range.w == lay.traceArea.w);
        CHECK(lay.traceArea.y == lay.bands.marker.y + band + std::round(6.0F * dpr));
        REQUIRE(lay.statusChannels.size() == 1);
        CHECK(lay.statusChannels[0].name == "Engine Status");
        CHECK(lay.statusStrip.y == lay.traceArea.y + lay.traceArea.h);
        CHECK(lay.statusStrip.h == std::round(10.0F * dpr));
        CHECK(lay.variance.y == lay.statusStrip.y + lay.statusStrip.h);
        CHECK(lay.variance.h > 0.0F);
        CHECK(lay.xAxis.y == lay.variance.y + lay.variance.h);
        CHECK(lay.offsetMain.y == lay.xAxis.y + lay.xAxis.h);
        CHECK(lay.offsetMain.h == std::round(12.0F * dpr));
        CHECK(lay.offsetReference.y == lay.offsetMain.y + lay.offsetMain.h);
        CHECK(lay.scrollbar.y + lay.scrollbar.h == lay.bounds.h);
        CHECK(lay.scrollbar.y == lay.offsetReference.y + lay.offsetReference.h);
        CHECK(lay.referenceSlot == 1);
        CHECK(lay.keyColumns.datum);
        CHECK(lay.keyColumn.y + lay.keyColumn.h == lay.traceArea.y + lay.traceArea.h);
        // The active group's vertical scrollbar sits in its axis strip.
        REQUIRE(lay.activeGroup == 0);
        CHECK(lay.groups[0].vScroll.w == std::round(6.0F * dpr));
        CHECK(lay.groups[1].vScroll.w == 0.0F);
    }
    // Without an overlay the variance panel takes no room (and F3 builds nothing, below).
    test::M3Case mainOnly = full;
    mainOnly.slots.resize(1);
    const TdLayout single = TimeDistanceBuilder::layoutOf(test::m3InputsOf(m, mainOnly, 1.0F));
    CHECK(single.variance.h == 0.0F);
    CHECK(single.referenceSlot == -1);
}

TEST_CASE("TimeDistanceBuilder: the variance panel builds one table per visible overlay and none without one "
          "(AC-M3-05)",
          "[unit]") {
    const test::M3Fixture m;
    const auto cases = test::m3Cases(m);
    const test::M3Case vd = caseNamed(cases, "variance_distance");
    const ComponentBuildInputs in = test::m3InputsOf(m, vd, 1.0F);
    TdStaticInfo info;
    const Result<ComponentBuild> build =
        TimeDistanceBuilder::buildStatic(in, TdLocalState{}, CancelToken{}, &info);
    REQUIRE(build.ok());
    CHECK(info.varianceTables == 2);
    CHECK_FALSE(info.varianceNoData);
    const TdLayout lay = TimeDistanceBuilder::layoutOf(in);
    // Each overlay's dt(d) is drawn in its key colour (Overlay 0: the overlay0 role; Overlay 1: the
    // channel's).
    const project::Scheme scheme = test::m3LightScheme();
    bool overlay0 = false;
    bool overlay1 = false;
    bool inVariance = false;
    std::vector<std::pair<float, float>> overlay0Vertices;
    for (const DrawCmd& c : build->list.cmds) {
        if (c.kind == CmdKind::Scissor) {
            inVariance = c.rect == lay.variance;
            continue;
        }
        if (inVariance && c.kind == CmdKind::Polyline) {
            overlay0 = overlay0 || c.colour == scheme.role("overlay0");
            overlay1 = overlay1 || c.colour == scheme.overlayColour(1); // the range key colour (06 5.3 3)
            if (c.colour == scheme.role("overlay0")) {
                for (std::uint32_t v = 0; v < c.vertexCount; ++v) {
                    overlay0Vertices.emplace_back(
                        build->list.verts[static_cast<std::size_t>(c.vertexOffset + v) * 2],
                        build->list.verts[(c.vertexOffset + v) * 2 + 1]);
                }
            }
        }
    }
    CHECK(overlay0);
    CHECK(overlay1);
    // The vertex of a column lies at the table's value there: dt(d) = tAtD_overlay(d) - tAtD_main(d).
    REQUIRE(overlay0Vertices.size() > 100);
    const auto& [vx, vy] = overlay0Vertices[overlay0Vertices.size() / 2];
    const double column = static_cast<double>(vx - lay.variance.x) - 0.5;
    const double d = vd.zoom.a + vd.zoom.length() * (column + 0.5) / static_cast<double>(lay.traceWidthPx);
    const double expected = m.b.indexes[1]->timeAt(d) - m.a.indexes[1]->timeAt(d);
    CHECK(std::abs(expected - analysis::varianceTimeAt(*m.a.indexes[1], *m.b.indexes[1], d)) < 1e-9);
    // The panel's scale is autoscaled over the tables and 0: the vertex lies at `expected` on it.
    analysis::VarianceRequest request;
    request.x0 = vd.zoom.a;
    request.x1 = vd.zoom.b;
    request.columns = lay.traceWidthPx;
    const auto t0 = analysis::VarianceTable::build(*m.a.indexes[1], *m.b.indexes[1], request);
    const auto t1 = analysis::VarianceTable::build(*m.a.indexes[1], *m.a.indexes[2], request);
    REQUIRE(t0.ok());
    REQUIRE(t1.ok());
    const std::optional<std::pair<double, double>> r0opt = t0->valueRange();
    const std::optional<std::pair<double, double>> r1opt = t1->valueRange();
    REQUIRE(r0opt.has_value());
    REQUIRE(r1opt.has_value());
    const std::pair<double, double> r0 = r0opt.value_or(std::pair<double, double>{});
    const std::pair<double, double> r1 = r1opt.value_or(std::pair<double, double>{});
    YMap ymap;
    ymap.yTopPx = lay.variance.y + 2.0F;
    ymap.yBottomPx = lay.variance.y + lay.variance.h - 2.0F;
    const double lo = std::min({0.0, r0.first, r1.first});
    const double hi = std::max({0.0, r0.second, r1.second});
    ymap.vMin = lo - 0.1 * (hi - lo); // the autoscale's 10 % headroom
    ymap.vMax = hi + 0.1 * (hi - lo);
    CHECK(std::abs(vy - ymap.yOf(expected)) < 1e-3F);

    // F3 without an overlay: no table, no panel.
    test::M3Case mainOnly = vd;
    mainOnly.slots.resize(1);
    TdStaticInfo none;
    REQUIRE(TimeDistanceBuilder::buildStatic(test::m3InputsOf(m, mainOnly, 1.0F), TdLocalState{},
                                             CancelToken{}, &none)
                .ok());
    CHECK(none.varianceTables == 0);
    // An overlay without distance data: the banner, no trace for it.
    const ComponentBuildInputs banner = test::m3InputsOf(m, caseNamed(cases, "variance_banner"), 1.0F);
    TdStaticInfo noData;
    REQUIRE(TimeDistanceBuilder::buildStatic(banner, TdLocalState{}, CancelToken{}, &noData).ok());
    CHECK(noData.varianceNoData);
    CHECK(noData.varianceTables == 0);
    CHECK(hasText(TimeDistanceBuilder::buildChrome(banner, false), "variance needs distance data"));
    CHECK_FALSE(hasText(TimeDistanceBuilder::buildChrome(in, false), "variance needs distance data"));
    // Main spanning two laps (an outing, a run of laps): no comparison of its first lap, the banner (D-201).
    ComponentBuildInputs spanning = in;
    REQUIRE(spanning.slots[0].chain != nullptr);
    const std::vector<LapPiece>& pieces = spanning.slots[0].chain->pieces();
    std::size_t first = 0;
    while (first < pieces.size() && !(pieces[first].start <= spanning.slots[0].range.start &&
                                      spanning.slots[0].range.start < pieces[first].end)) {
        ++first;
    }
    REQUIRE(first + 1 < pieces.size());
    spanning.slots[0].range.end = pieces[first + 1].end;
    TdStaticInfo spans;
    REQUIRE(TimeDistanceBuilder::buildStatic(spanning, TdLocalState{}, CancelToken{}, &spans).ok());
    CHECK(spans.varianceSpansLaps);
    CHECK(spans.varianceTables == 0);
    CHECK(hasText(TimeDistanceBuilder::buildChrome(spanning, false), "variance compares single laps"));
    CHECK_FALSE(info.varianceSpansLaps);
}

TEST_CASE("TimeDistanceBuilder: measurements equal the samples' min, max and avg over the visible span",
          "[unit]") {
    const test::M3Fixture m;
    const auto cases = test::m3Cases(m);
    const test::M3Case mc = caseNamed(cases, "measurements");
    const ComponentBuildInputs in = test::m3InputsOf(m, mc, 1.0F);
    TdStaticInfo info;
    const Result<ComponentBuild> build =
        TimeDistanceBuilder::buildStatic(in, TdLocalState{}, CancelToken{}, &info);
    REQUIRE(build.ok());
    // Two traces x two slots.
    REQUIRE(info.measurements.size() == 4);
    for (const TdMeasurementEntry& e : info.measurements) {
        CHECK(e.valid);
        const SlotData& slot = in.slots[static_cast<std::size_t>(e.slot)];
        const ResolvedChannel& rc = slot.channels[static_cast<std::size_t>(e.trace)];
        // Time mode: the visible span in the slot's lap is [R.start + a, R.start + b).
        const Micros t0 = slot.range.start + static_cast<Micros>(mc.zoom.a);
        const Micros t1 = slot.range.start + static_cast<Micros>(mc.zoom.b);
        const Brute b = bruteForce(*rc.series, t0, t1);
        REQUIRE(b.count > 0);
        CHECK(e.minT == b.tMin);
        CHECK(e.maxT == b.tMax);
        CHECK(e.minX == static_cast<double>(b.tMin - slot.range.start));
        const int dps = rc.dps;
        CHECK(findText(build->list, formatFixed(rc.toDisplay(b.min), dps)) != nullptr);
        CHECK(findText(build->list, formatFixed(rc.toDisplay(b.max), dps)) != nullptr);
        CHECK(findText(build->list, formatFixed(rc.toDisplay(b.sum / b.count), dps)) != nullptr);
    }
    // The cells are hit regions (cell 0 min, 1 max, 2 avg) carrying the trace and the slot.
    const TdLayout lay = TimeDistanceBuilder::layoutOf(in);
    const TdGroupLayout& g0 = lay.groups[0];
    const RectPx maxCell = g0.keyGeometry.cellRect(0, 1, KeyCell::Max);
    const HitResult h = build->hits.hit(g0.key.x + maxCell.x + 1.0F, g0.key.y + maxCell.y + 1.0F);
    CHECK(h.kind == HitKind::KeyMeasurement);
    CHECK(h.trace == 0);
    CHECK(h.slot == 1);
    CHECK(h.cell == 1);
}

TEST_CASE("TimeDistanceBuilder: the datum key shows value@main, value@datum and delta", "[unit]") {
    const test::M3Fixture m;
    const auto cases = test::m3Cases(m);
    const test::M3Case dc = caseNamed(cases, "datum_key");
    const ComponentBuildInputs in = test::m3InputsOf(m, dc, 1.0F);
    REQUIRE(TimeDistanceBuilder::buildStatic(in, CancelToken{}).ok()); // the chunks become resident
    const DrawList dyn = TimeDistanceBuilder::buildDynamic(in);
    const ResolvedChannel& rc = in.slots[0].channels[0]; // Corr Speed in Main
    const double atCursor = rc.toDisplay(
        store::Resampler::valueAt(*rc.series, in.slots[0].range.start + dc.cursorT, store::Interp::Linear));
    const double atDatum = rc.toDisplay(
        store::Resampler::valueAt(*rc.series, in.slots[0].range.start + *dc.datumT, store::Interp::Linear));
    CHECK(findText(dyn, formatFixed(atCursor, 1)) != nullptr);
    CHECK(findText(dyn, formatFixed(atDatum, 1)) != nullptr);
    const std::string delta = formatFixed(atCursor - atDatum, 1);
    CHECK(findText(dyn, (atCursor - atDatum > 0.0 ? "+" : "") + delta) != nullptr);
    // Without a datum the key has no datum or delta cells.
    ComponentBuildInputs noDatum = in;
    noDatum.view.datumT.reset();
    CHECK_FALSE(TimeDistanceBuilder::layoutOf(noDatum).keyColumns.datum);
    CHECK(TimeDistanceBuilder::layoutOf(in).keyColumns.datum);
}

TEST_CASE("TimeDistanceBuilder: the offset axis writes each slot's offset and hits its sub-axis", "[unit]") {
    CHECK(TimeDistanceBuilder::offsetText(Domain::Time, RangeGeom{0, 0, 120'000, 0.0, 1.0}) == "+0.120 s");
    CHECK(TimeDistanceBuilder::offsetText(Domain::Time, RangeGeom{0, 0, -1'200'000, 0.0, 1.0}) == "-1.200 s");
    CHECK(TimeDistanceBuilder::offsetText(Domain::Time, RangeGeom{}) == "+0.000 s");
    CHECK(TimeDistanceBuilder::offsetText(Domain::Distance, RangeGeom{0, 0, 0, -3.5, 1.0}) == "-3.50 m");
    CHECK(TimeDistanceBuilder::offsetText(Domain::Distance, RangeGeom{0, 0, 0, 20.25, 1.0}) == "+20.25 m");
    const test::M3Fixture m;
    const auto cases = test::m3Cases(m);
    for (const char* name : {"offset_axis_time", "offset_axis_distance"}) {
        const ComponentBuildInputs in = test::m3InputsOf(m, caseNamed(cases, name), 1.0F);
        const Result<ComponentBuild> build = TimeDistanceBuilder::buildStatic(in, CancelToken{});
        REQUIRE(build.ok());
        const TdLayout lay = TimeDistanceBuilder::layoutOf(in);
        CHECK(hasText(build->list, TimeDistanceBuilder::offsetText(in.view.domain, in.slots[0].range)));
        CHECK(hasText(build->list, TimeDistanceBuilder::offsetText(in.view.domain, in.slots[1].range)));
        const HitResult main = build->hits.hit(lay.offsetMain.x + 10.0F, lay.offsetMain.y + 3.0F);
        CHECK(main.kind == HitKind::OffsetAxis);
        CHECK(main.handleKind == HandleKind::OffsetMain);
        CHECK(main.slot == 0);
        const HitResult ref = build->hits.hit(lay.offsetReference.x + 10.0F, lay.offsetReference.y + 3.0F);
        CHECK(ref.kind == HitKind::OffsetAxis);
        CHECK(ref.handleKind == HandleKind::OffsetReference);
        CHECK(ref.slot == 1);
    }
}

TEST_CASE("TimeDistanceBuilder: bands show ranges, sections by category, beacons, events and annotations",
          "[unit]") {
    const test::M3Fixture m;
    const auto cases = test::m3Cases(m);
    const ComponentBuildInputs in = test::m3InputsOf(m, caseNamed(cases, "bands_sections"), 1.0F);
    TdStaticInfo info;
    const Result<ComponentBuild> build =
        TimeDistanceBuilder::buildStatic(in, TdLocalState{}, CancelToken{}, &info);
    REQUIRE(build.ok());
    // The neighbouring laps of the Main source, then the slots (two sources: their short names).
    std::vector<std::string> labels;
    labels.reserve(info.ranges.size());
    for (const TdBandEntry& e : info.ranges) {
        labels.push_back(e.label);
    }
    CHECK(std::find(labels.begin(), labels.end(), "Lap 1") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "Lap 3") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "Lap 2 (synthetic_a)") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "Lap 2 (synthetic_b)") != labels.end());
    const auto mainCell = std::find_if(info.ranges.begin(), info.ranges.end(),
                                       [](const TdBandEntry& e) { return e.slot == 0; });
    REQUIRE(mainCell != info.ranges.end());
    CHECK(mainCell->x0 == 0.0);
    CHECK(mainCell->x1 == static_cast<double>(m.fx.a.laps[1].end - m.fx.a.laps[1].start));
    // Sections of the Main lap and its neighbours, in their category colours.
    REQUIRE(info.sections.size() >= 6);
    const TdLayout lay = TimeDistanceBuilder::layoutOf(in);
    const project::Scheme scheme = test::m3LightScheme();
    CHECK(countCmd(build->list, CmdKind::FilledRect, scheme.role("sectionCorner")) >= 3);
    CHECK(countCmd(build->list, CmdKind::FilledRect, scheme.role("sectionStraight")) >= 3);
    const auto t1 = std::find_if(info.sections.begin(), info.sections.end(),
                                 [](const TdBandEntry& e) { return e.label == "T1" && e.x0 > 0.0; });
    REQUIRE(t1 != info.sections.end());
    const auto t1Px = static_cast<float>(DomainMapper::pxOf(0.5 * (t1->x0 + t1->x1), in.view.zoom,
                                                            static_cast<double>(lay.traceArea.x),
                                                            static_cast<double>(lay.traceWidthPx)));
    const HitResult hs = build->hits.hit(t1Px, lay.bands.section.y + 3.0F);
    CHECK(hs.kind == HitKind::Section);
    REQUIRE(hs.section >= 0);
    CHECK(info.sections[static_cast<std::size_t>(hs.section)].label == "T1");
    // A range cell hit carries its index into the reported ranges.
    const HitResult hr = build->hits.hit(lay.traceArea.x + 2.0F, lay.bands.range.y + 3.0F);
    CHECK(hr.kind == HitKind::Band);
    REQUIRE(hr.band >= 0);
    CHECK(info.ranges[static_cast<std::size_t>(hr.band)].label == "Lap 1");
    // Beacons at the lap starts in view.
    std::size_t beacons = 0;
    for (const TdMarkerEntry& e : info.markers) {
        beacons += e.kind == BandMarkerKind::Beacon ? 1 : 0;
    }
    CHECK(beacons == 2); // BCN at 60 s (Main start) and 121.5 s (Main end)

    // The marker band: annotations (with dotted lines) and device events; SHIFT+E hides the events.
    const test::M3Case ae = caseNamed(cases, "annotations_events");
    const ComponentBuildInputs ain = test::m3InputsOf(m, ae, 1.0F);
    TdStaticInfo ainfo;
    const Result<ComponentBuild> ab =
        TimeDistanceBuilder::buildStatic(ain, TdLocalState{}, CancelToken{}, &ainfo);
    REQUIRE(ab.ok());
    std::vector<std::string> texts;
    texts.reserve(ainfo.markers.size());
    for (const TdMarkerEntry& e : ainfo.markers) {
        texts.push_back(e.text);
    }
    CHECK(std::find(texts.begin(), texts.end(), "Kerb strike") != texts.end()); // at 101 s = 41 s into Lap 2
    CHECK(std::find(texts.begin(), texts.end(), "Power cycle") != texts.end());
    CHECK(std::find(texts.begin(), texts.end(), "Brake marker early") == texts.end()); // 12 s: outside
    const auto kerb = std::find_if(ainfo.markers.begin(), ainfo.markers.end(),
                                   [](const TdMarkerEntry& e) { return e.text == "Kerb strike"; });
    REQUIRE(kerb != ainfo.markers.end());
    CHECK(kerb->kind == BandMarkerKind::Annotation);
    CHECK(kerb->x == static_cast<double>(41 * test::kSec));
    CHECK(kerb->id.v == uuidFromHash(hash128("annotation:a2")));
    CHECK(countCmd(ab->list, CmdKind::Lines, scheme.role("annotationMark")) == 1); // the dotted lines
    const TdLayout aLay = TimeDistanceBuilder::layoutOf(ain);
    CHECK(aLay.bands.range.h == 0.0F);
    CHECK(aLay.bands.marker.h > 0.0F);
    const auto kerbPx =
        static_cast<float>(DomainMapper::pxOf(kerb->x, ain.view.zoom, static_cast<double>(aLay.traceArea.x),
                                              static_cast<double>(aLay.traceWidthPx)));
    const HitResult ha = ab->hits.hit(kerbPx, aLay.bands.marker.y + 4.0F);
    CHECK(ha.kind == HitKind::Annotation);
    CHECK(ha.markerId == kerb->id);
    test::M3Case hidden = ae;
    hidden.body.insert(hidden.body.find(R"("showBands")"), R"("showDeviceEvents":false,)");
    TdStaticInfo hinfo;
    REQUIRE(TimeDistanceBuilder::buildStatic(test::m3InputsOf(m, hidden, 1.0F), TdLocalState{}, CancelToken{},
                                             &hinfo)
                .ok());
    CHECK(std::none_of(hinfo.markers.begin(), hinfo.markers.end(),
                       [](const TdMarkerEntry& e) { return e.kind == BandMarkerKind::DeviceEvent; }));
}

TEST_CASE("TimeDistanceBuilder: the status strip fills the non-zero spans of each status channel", "[unit]") {
    const test::M3Fixture m;
    const auto cases = test::m3Cases(m);
    const ComponentBuildInputs in = test::m3InputsOf(m, caseNamed(cases, "status_strip"), 1.0F);
    TdStaticInfo info;
    const Result<ComponentBuild> build =
        TimeDistanceBuilder::buildStatic(in, TdLocalState{}, CancelToken{}, &info);
    REQUIRE(build.ok());
    const TdLayout lay = TimeDistanceBuilder::layoutOf(in);
    REQUIRE(lay.statusChannels.size() == 2);
    CHECK(info.statusNames == std::vector<std::string>{"Engine Status", "Pit Limiter"});
    // Row 0 (Engine Status): bit 0 over [20 %, 30 %) of the lap and bit 2 over [70 %, 76 %): two filled
    // spans. Row 1 (Pit Limiter): the first 6 s of the outing.
    std::vector<RectPx> row0;
    std::vector<RectPx> row1;
    for (const DrawCmd& c : build->list.cmds) {
        if (c.kind != CmdKind::FilledRect || c.rect.y < lay.statusStrip.y ||
            c.rect.y >= lay.statusStrip.y + lay.statusStrip.h) {
            continue;
        }
        if (c.colour == lay.statusChannels[0].colour) {
            row0.push_back(c.rect);
        } else if (c.colour == lay.statusChannels[1].colour) {
            row1.push_back(c.rect);
        }
    }
    REQUIRE(row0.size() == 2);
    REQUIRE(row1.size() == 1);
    const auto pxOf = [&](double t) {
        return static_cast<float>(DomainMapper::pxOf(t, in.view.zoom, static_cast<double>(lay.traceArea.x),
                                                     static_cast<double>(lay.traceWidthPx)));
    };
    CHECK(std::abs(row0[0].x - pxOf(0.2 * 60e6)) <= 1.0F);
    CHECK(std::abs(row0[0].x + row0[0].w - pxOf(0.3 * 60e6)) <= 1.0F);
    CHECK(std::abs(row1[0].x - lay.traceArea.x) <= 1.0F);
    CHECK(std::abs(row1[0].x + row1[0].w - pxOf(6e6)) <= 1.0F);
    const HitResult h =
        build->hits.hit(lay.traceArea.x + 30.0F, lay.statusStrip.y + lay.statusRowHeight * 1.5F);
    CHECK(h.kind == HitKind::StatusRow);
    CHECK(h.trace == 1);
    // Without a list the Main layer's Bitfield and Bool channels show.
    test::M3Case byDefault = caseNamed(cases, "status_strip");
    byDefault.body.replace(byDefault.body.find(R"(,"statusChannels")"),
                           std::string(R"(,"statusChannels":["Engine Status","Pit Limiter"])").size(), "");
    CHECK(TimeDistanceBuilder::layoutOf(test::m3InputsOf(m, byDefault, 1.0F)).statusChannels.size() == 2);
}

TEST_CASE(
    "TimeDistanceBuilder: overlapped mode, Lo/Hi handles, splitters, scale lines, crosshair, scrollbars",
    "[unit]") {
    const test::M3Fixture m;
    const auto cases = test::m3Cases(m);
    {
        const ComponentBuildInputs in = test::m3InputsOf(m, caseNamed(cases, "overlapped"), 1.0F);
        const TdLayout lay = TimeDistanceBuilder::layoutOf(in);
        REQUIRE(lay.overlapped);
        REQUIRE(lay.groups.size() == 2);
        CHECK(lay.activeGroup == 1);
        CHECK(lay.groups[0].panel.y == lay.traceArea.y);
        CHECK(lay.groups[0].panel.h == std::round(0.7F * lay.traceArea.h));
        CHECK(lay.groups[1].panel.y == lay.traceArea.y + std::round(0.4F * lay.traceArea.h));
        CHECK(lay.groups[1].panel.y + lay.groups[1].panel.h == lay.traceArea.y + lay.traceArea.h);
        CHECK(lay.groups[1].key.y >=
              lay.groups[0].key.y + lay.groups[0].key.h); // the key lists the groups in turn
        const Result<ComponentBuild> build = TimeDistanceBuilder::buildStatic(in, CancelToken{});
        REQUIRE(build.ok());
        const RectPx& lo = lay.groups[0].loHandle;
        const RectPx& hi = lay.groups[1].hiHandle;
        const HitResult hl = build->hits.hit(lo.x + 1.0F, lo.y + 1.0F);
        CHECK(hl.kind == HitKind::Handle);
        CHECK(hl.handleKind == HandleKind::GroupLo);
        CHECK(hl.group == 0);
        const HitResult hh = build->hits.hit(hi.x + 1.0F, hi.y + hi.h - 1.0F);
        CHECK(hh.handleKind == HandleKind::GroupHi);
        CHECK(hh.group == 1);
        // Between the regions the area still places the cursor (a Panel of the active group).
        CHECK(build->hits.hit(lay.traceArea.x + 20.0F, lay.traceArea.y + 0.5F * lay.traceArea.h).kind ==
              HitKind::Panel);
        // The active group's vertical scrollbar.
        const HitResult vs = build->hits.hit(lay.groups[1].vScroll.x + 1.0F, lay.groups[1].panel.y + 4.0F);
        CHECK(vs.kind == HitKind::ScrollBar);
        CHECK(vs.handleKind == HandleKind::ScrollThumb);
        CHECK(vs.group == 1);
    }
    {
        // Panels mode: a splitter between the panels, grabbed in the axis strip; in the trace area the click
        // places the cursor.
        const ComponentBuildInputs in = test::m3InputsOf(m, caseNamed(cases, "datum_key"), 1.0F);
        const TdLayout lay = TimeDistanceBuilder::layoutOf(in);
        const Result<ComponentBuild> build = TimeDistanceBuilder::buildStatic(in, CancelToken{});
        REQUIRE(build.ok());
        const float y = lay.groups[0].panel.y + lay.groups[0].panel.h;
        const HitResult s = build->hits.hit(lay.groups[0].axisStrip.x + 10.0F, y);
        CHECK(s.kind == HitKind::Handle);
        CHECK(s.handleKind == HandleKind::PanelSplitter);
        CHECK(s.group == 0);
        CHECK(s.handle == 1);
        CHECK(build->hits.hit(lay.traceArea.x + 40.0F, y).kind == HitKind::Panel);
        // The horizontal scrollbar: the thumb over the zoom within the Main outing.
        TdStaticInfo info;
        REQUIRE(TimeDistanceBuilder::buildStatic(in, TdLocalState{}, CancelToken{}, &info).ok());
        const XSpan outing{static_cast<double>(m.fx.a.laps.front().start - m.fx.a.laps[1].start),
                           static_cast<double>(m.fx.a.laps.back().end - m.fx.a.laps[1].start)};
        CHECK(info.scrollExtent == outing);
        const float thumbX =
            lay.scrollbar.x +
            static_cast<float>((in.view.zoom.a - outing.a) / outing.length()) * lay.scrollbar.w;
        const HitResult t = build->hits.hit(thumbX + 2.0F, lay.scrollbar.y + 0.5F * lay.scrollbar.h);
        CHECK(t.kind == HitKind::ScrollBar);
        CHECK(t.handleKind == HandleKind::ScrollThumb);
        CHECK(build->hits.hit(lay.scrollbar.x + 1.0F, lay.scrollbar.y + 0.5F * lay.scrollbar.h).handleKind ==
              HandleKind::None);
    }
    {
        // Scale lines at the override limits (20 and 60 m/s = 72 and 216 km/h) and the coloured zero line;
        // the crosshair's horizontal line at the active channel's value.
        const test::M3Case sc = caseNamed(cases, "scale_lines_crosshair");
        const ComponentBuildInputs in = test::m3InputsOf(m, sc, 1.0F);
        TdStaticInfo info;
        const Result<ComponentBuild> build =
            TimeDistanceBuilder::buildStatic(in, TdLocalState{}, CancelToken{}, &info);
        REQUIRE(build.ok());
        CHECK(countCmd(build->list, CmdKind::Lines, 0x2E7D32FFU) == 1);
        CHECK(countCmd(build->list, CmdKind::Lines, 0xC62828FFU) == 1);
        CHECK(countCmd(build->list, CmdKind::Lines, 0x6A1B9AFFU) == 1);
        // An auto scale ignores the override limits; Channel scaling takes them.
        const std::pair<double, double> autoScale = info.scales[0].value_or(std::pair<double, double>{});
        REQUIRE(info.scales[0].has_value());
        CHECK(autoScale.first < 72.0);
        CHECK(autoScale.second > 216.0);
        test::M3Case channelScale = sc;
        channelScale.body.replace(channelScale.body.find(R"("scaleMode":"auto")"),
                                  std::string_view(R"("scaleMode":"auto")").size(),
                                  R"("scaleMode":"channel")");
        TdStaticInfo channelInfo;
        REQUIRE(TimeDistanceBuilder::buildStatic(test::m3InputsOf(m, channelScale, 1.0F), TdLocalState{},
                                                 CancelToken{}, &channelInfo)
                    .ok());
        REQUIRE(channelInfo.scales[0].has_value());
        const std::pair<double, double> limits = channelInfo.scales[0].value_or(std::pair<double, double>{});
        CHECK(std::abs(limits.first - 72.0) < 1e-9);
        CHECK(std::abs(limits.second - 216.0) < 1e-9);
        TdLocalState local;
        local.builtScales = info.scales;
        const DrawList dyn = TimeDistanceBuilder::buildDynamic(in, local);
        const project::Scheme scheme = test::m3LightScheme();
        std::size_t dotted = 0;
        for (const DrawCmd& c : dyn.cmds) {
            dotted +=
                c.kind == CmdKind::Lines && c.dash == DashId::Dot && c.colour == scheme.role("cursorMain")
                    ? 1
                    : 0;
        }
        CHECK(dotted == 1);
        // cursorStyle none: no main cursor line, values still shown.
        test::M3Case none = sc;
        none.body.replace(none.body.find("crosshair"), 9, "none");
        const ComponentBuildInputs nin = test::m3InputsOf(m, none, 1.0F);
        const DrawList ndyn = TimeDistanceBuilder::buildDynamic(nin);
        CHECK(countCmd(ndyn, CmdKind::Lines, scheme.role("cursorMain")) == 0);
        CHECK_FALSE(ndyn.texts.empty());
    }
}

TEST_CASE("TimeDistanceBuilder: the vertical zoom shows a fraction of the group's scale", "[unit]") {
    CHECK(TimeDistanceBuilder::zoomedScale(0.0, 100.0, TdGroupZoom{}) ==
          std::pair<double, double>{0.0, 100.0});
    CHECK(TimeDistanceBuilder::zoomedScale(0.0, 100.0, TdGroupZoom{0.25, 0.5}) ==
          std::pair<double, double>{25.0, 50.0});
    CHECK(TimeDistanceBuilder::zoomedScale(-10.0, 10.0, TdGroupZoom{0.75, 0.25}) ==
          std::pair<double, double>{-5.0, 5.0});
    CHECK(TimeDistanceBuilder::zoomedScale(0.0, 1.0, TdGroupZoom{0.5, 0.5}) ==
          std::pair<double, double>{0.0, 1.0});
    const test::M3Fixture m;
    const auto cases = test::m3Cases(m);
    const ComponentBuildInputs in = test::m3InputsOf(m, caseNamed(cases, "datum_key"), 1.0F);
    TdStaticInfo full;
    REQUIRE(TimeDistanceBuilder::buildStatic(in, TdLocalState{}, CancelToken{}, &full).ok());
    TdLocalState local;
    local.vZoom[1] = TdGroupZoom{0.5, 1.0}; // the throttle group's top half: 50..100 %
    TdStaticInfo zoomed;
    REQUIRE(TimeDistanceBuilder::buildStatic(in, local, CancelToken{}, &zoomed).ok());
    REQUIRE(zoomed.scales[1].has_value());
    const std::pair<double, double> shown = zoomed.scales[1].value_or(std::pair<double, double>{});
    CHECK(shown.first == 50.0);
    CHECK(shown.second == 100.0);
    CHECK(zoomed.scales[0] == full.scales[0]);
}

TEST_CASE("TimeDistanceBuilder: DrawList JSON goldens of the M3 worksheet cases (AC-M3-02)", "[unit]") {
    const test::M3Fixture m;
    const auto cases = test::m3Cases(m);
    REQUIRE(cases.size() >= 12);
    for (const test::M3Case& mc : cases) {
        for (const float dpr : {1.0F, 2.0F}) {
            const ComponentBuildInputs in = test::m3InputsOf(m, mc, dpr);
            const Result<ComponentBuild> build = TimeDistanceBuilder::buildStatic(in, CancelToken{});
            REQUIRE(build.ok());
            const DrawList dynamic = TimeDistanceBuilder::buildDynamic(in);
            const DrawList chrome = TimeDistanceBuilder::buildChrome(in, false);
            test::checkGolden("time_distance", mc.name, dpr,
                              test::layersJson(mc.name, dpr, build->list, dynamic, chrome));
        }
    }
}
