// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The M3 layout pieces of esses_plotcore (07 section 6.1): HitTester point sets against a brute-force search
// (04 section 10.1 item 7, D-196 tags), BandLayout (04 section 8.11), the KeyLayout datum and measurement
// columns (04 section 8.4; the default columns keep the M1 layout exactly), the shared body JSON helpers
// (ScaleSpec, ColourBands) and the DrawList JSON goldens of the band and key layouts
// (tests/golden/drawlist/band_layout, key_layout).

#include "m3_golden_support.hpp"

#include <esses/plotcore/band_layout.hpp>
#include <esses/plotcore/component_bodies.hpp>
#include <esses/plotcore/hit_tester.hpp>
#include <esses/plotcore/key_layout.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::plot;

namespace {

// [0, 1) from the top 53 bits (portable, unlike the standard distributions).
double unit(std::mt19937_64& rng) {
    return static_cast<double>(rng() >> 11U) * (1.0 / 9007199254740992.0);
}

struct BruteHit {
    int set = -1;
    int point = -1;
};

BruteHit bruteNearest(const std::vector<std::vector<PointPx>>& sets, float x, float y, float maxD) {
    BruteHit best;
    double bestD2 = std::numeric_limits<double>::infinity();
    const double maxD2 = static_cast<double>(maxD) * static_cast<double>(maxD);
    for (std::size_t s = 0; s < sets.size(); ++s) {
        double setD2 = std::numeric_limits<double>::infinity();
        int setIndex = -1;
        for (std::size_t i = 0; i < sets[s].size(); ++i) {
            const PointPx& p = sets[s][i];
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
                continue;
            }
            const double dx = static_cast<double>(p.x) - static_cast<double>(x);
            const double dy = static_cast<double>(p.y) - static_cast<double>(y);
            const double d2 = dx * dx + dy * dy;
            if (d2 <= maxD2 && d2 < setD2) {
                setD2 = d2;
                setIndex = static_cast<int>(i);
            }
        }
        if (setIndex >= 0 && setD2 <= bestD2) {
            bestD2 = setD2;
            best = BruteHit{static_cast<int>(s), setIndex};
        }
    }
    return best;
}

HitResult setBase(int trace) {
    HitResult r;
    r.trace = trace;
    r.slot = 0;
    return r;
}

const FixedTextMetrics& metrics() {
    static const FixedTextMetrics m;
    return m;
}

} // namespace

// ---- HitTester point sets

TEST_CASE("HitTester: nearestPoint equals a brute-force search over several point sets", "[unit][property]") {
    std::mt19937_64 rng(test::testSeed());
    for (int round = 0; round < 20; ++round) {
        HitTester hits;
        std::vector<std::vector<PointPx>> sets;
        const RectPx bounds{10.0F, 20.0F, 300.0F, 200.0F};
        const int setCount = 1 + static_cast<int>(rng() % 3U);
        for (int s = 0; s < setCount; ++s) {
            std::vector<PointPx> pts;
            const auto n = static_cast<std::size_t>(rng() % 400U);
            for (std::size_t i = 0; i < n; ++i) {
                // Some points outside the bounds, some on a coarse lattice (exact ties), a few NaN.
                PointPx p{static_cast<float>(-20.0 + 360.0 * unit(rng)),
                          static_cast<float>(0.0 + 240.0 * unit(rng))};
                if (i % 7 == 0) {
                    p = PointPx{std::round(p.x / 10.0F) * 10.0F, std::round(p.y / 10.0F) * 10.0F};
                }
                if (i % 53 == 0) {
                    p.x = std::numeric_limits<float>::quiet_NaN();
                }
                pts.push_back(p);
            }
            hits.addPoints(bounds, pts, setBase(s));
            sets.push_back(std::move(pts));
        }
        for (int q = 0; q < 200; ++q) {
            const auto x = static_cast<float>(-30.0 + 380.0 * unit(rng));
            const auto y = static_cast<float>(-10.0 + 260.0 * unit(rng));
            const float maxD = q % 5 == 0 ? 0.0F : static_cast<float>(1.0 + 30.0 * unit(rng));
            const BruteHit expected = bruteNearest(sets, x, y, maxD);
            const HitResult got = hits.nearestPoint(x, y, maxD);
            if (expected.set < 0) {
                CHECK(got.kind == HitKind::None);
                continue;
            }
            REQUIRE(got.kind == HitKind::Point);
            CHECK(got.trace == expected.set);
            CHECK(got.point == expected.point);
        }
    }
}

TEST_CASE("HitTester: point sets carry tags and the base result, and clear() drops them", "[unit]") {
    HitTester hits;
    CHECK(hits.empty());
    const std::vector<PointPx> pts{{10.0F, 10.0F}, {20.0F, 10.0F}, {20.0F, 10.0F}};
    const std::vector<std::int64_t> tags{100, 200, 300};
    HitResult base;
    base.kind = HitKind::Point;
    base.group = 2;
    base.trace = 4;
    base.slot = 0;
    hits.addPoints(RectPx{0.0F, 0.0F, 100.0F, 100.0F}, pts, base, tags);
    CHECK_FALSE(hits.empty());
    std::int64_t tag = -1;
    HitResult r = hits.nearestPoint(19.0F, 11.0F, 5.0F, &tag);
    CHECK(r.kind == HitKind::Point);
    CHECK(r.group == 2);
    CHECK(r.trace == 4);
    CHECK(r.point == 1); // the lower index of two equal points
    CHECK(tag == 200);
    r = hits.nearestPoint(50.0F, 50.0F, 5.0F, &tag);
    CHECK(r.kind == HitKind::None);
    CHECK(tag == 0);
    // A later set wins a tie; a set without tags reports 0; a point outside the bounds is found.
    hits.addPoints(RectPx{0.0F, 0.0F, 100.0F, 100.0F}, std::vector<PointPx>{{20.0F, 10.0F}, {-50.0F, 300.0F}},
                   setBase(9));
    r = hits.nearestPoint(19.0F, 11.0F, 5.0F, &tag);
    CHECK(r.trace == 9);
    CHECK(r.point == 0);
    CHECK(tag == 0);
    r = hits.nearestPoint(-49.0F, 299.0F, 3.0F);
    CHECK(r.trace == 9);
    CHECK(r.point == 1);
    // Degenerate bounds still find the points; a non-finite query finds nothing.
    HitTester flat;
    flat.addPoints(RectPx{5.0F, 5.0F, 0.0F, 0.0F}, std::vector<PointPx>{{5.0F, 5.0F}, {8.0F, 5.0F}},
                   setBase(1));
    CHECK(flat.nearestPoint(7.0F, 5.0F, 2.0F).point == 1);
    CHECK(flat.nearestPoint(std::numeric_limits<float>::quiet_NaN(), 5.0F, 2.0F).kind == HitKind::None);
    hits.clear();
    CHECK(hits.empty());
    CHECK(hits.nearestPoint(19.0F, 11.0F, 5.0F).kind == HitKind::None);
}

// ---- BandLayout

namespace {

BandLayoutInput bandInput() {
    BandLayoutInput in;
    BandCell main;
    main.x0 = -40.0F;
    main.x1 = 150.0F;
    main.label = "Lap 2";
    main.colour = 0xFFE0E0FFU;
    main.slot = 0;
    main.index = 1;
    BandCell overlay;
    overlay.x0 = 150.0F;
    overlay.x1 = 230.0F;
    overlay.label = "Lap 3 synthetic_b.ld";
    overlay.colour = 0xE0E8FFFFU;
    overlay.slot = 1;
    overlay.index = 2;
    in.ranges = {main, overlay};
    BandCell corner;
    corner.x0 = 20.0F;
    corner.x1 = 90.0F;
    corner.label = "Turn 1";
    corner.category = "corner";
    corner.index = 0;
    BandCell straight;
    straight.x0 = 90.0F;
    straight.x1 = 100.0F;
    straight.label = "Back Straight";
    straight.category = "straight";
    straight.index = 1;
    BandCell user;
    user.x0 = 100.0F;
    user.x1 = 220.0F;
    user.label = "T1 entry";
    user.category = "braking";
    user.index = 2;
    in.sections = {corner, straight, user};
    BandMarker beacon;
    beacon.x = 60.0F;
    beacon.kind = BandMarkerKind::Beacon;
    beacon.text = "BCN";
    beacon.id = model::MarkerId{Uuid::parse("00000000-0000-4000-8000-000000000001")};
    BandMarker note;
    note.x = 120.0F;
    note.kind = BandMarkerKind::Annotation;
    note.text = "lift here";
    note.id = model::MarkerId{Uuid::parse("00000000-0000-4000-8000-000000000002")};
    BandMarker event;
    event.x = 250.0F;
    event.kind = BandMarkerKind::DeviceEvent;
    event.text = "Logger restart";
    event.id = model::MarkerId{Uuid::parse("00000000-0000-4000-8000-000000000003")};
    in.markers = {beacon, note, event};
    return in;
}

} // namespace

TEST_CASE("BandLayout: bands stack from the top and hidden bands take no height", "[unit]") {
    BandLayoutInput in = bandInput();
    const RectPx area{40.0F, 18.0F, 300.0F, 200.0F};
    BandGeometry g = BandLayout::layout(in, area, 1.0F);
    CHECK(g.range == RectPx{40.0F, 18.0F, 300.0F, 14.0F});
    CHECK(g.section == RectPx{40.0F, 32.0F, 300.0F, 14.0F});
    CHECK(g.marker == RectPx{40.0F, 46.0F, 300.0F, 14.0F});
    CHECK(g.heightPx == 42.0F);
    in.showSections = false;
    g = BandLayout::layout(in, area, 2.0F);
    CHECK(g.range.h == 28.0F);
    CHECK(g.section.h == 0.0F);
    CHECK(g.marker.y == 18.0F + 28.0F);
    CHECK(g.heightPx == 56.0F);
    in.showRanges = in.showMarkers = false;
    CHECK(BandLayout::layout(in, area, 1.0F).heightPx == 0.0F);
}

TEST_CASE("BandLayout: cells clip to the band, labels elide or drop, hits carry indices and marker ids",
          "[unit]") {
    const BandLayoutInput in = bandInput();
    const RectPx area{40.0F, 18.0F, 300.0F, 200.0F};
    const BandGeometry g = BandLayout::layout(in, area, 1.0F);
    DrawList list;
    HitTester hits;
    BandLayout::emit(list, in, g, metrics(), BandColours{}, 1.0F, &hits);
    // The Main range cell starts before the band: clipped to its left edge.
    HitResult h = hits.hit(41.0F, 20.0F);
    CHECK(h.kind == HitKind::Band);
    CHECK(h.band == 1);
    CHECK(h.slot == 0);
    h = hits.hit(40.0F + 200.0F, 20.0F);
    CHECK(h.band == 2);
    CHECK(h.slot == 1);
    CHECK(hits.hit(39.0F, 20.0F).kind == HitKind::None);
    // Sections: index carried in `section`; the 10 px straight is too narrow for its label (dropped).
    h = hits.hit(40.0F + 50.0F, 34.0F);
    CHECK(h.kind == HitKind::Section);
    CHECK(h.section == 0);
    bool straightLabel = false;
    bool elided = false;
    for (const TextRun& run : list.texts) {
        straightLabel = straightLabel || list.textOf(run) == "Back Straight";
        elided = elided || (list.textOf(run) == "Lap 3 synthetic_b.ld" && run.maxWidthPx > 0.0F);
    }
    CHECK_FALSE(straightLabel);
    CHECK(elided); // the overlay's long label is elided to its cell
    // Markers: beacon tick, annotation rectangle, device-event triangle (a Quads command).
    h = hits.hit(40.0F + 60.0F, 50.0F);
    CHECK(h.kind == HitKind::Marker);
    CHECK(h.markerId == in.markers[0].id);
    h = hits.hit(40.0F + 120.0F, 52.0F);
    CHECK(h.kind == HitKind::Annotation);
    CHECK(h.markerId == in.markers[1].id);
    h = hits.hit(40.0F + 250.0F, 52.0F);
    CHECK(h.kind == HitKind::Marker);
    CHECK(h.markerId == in.markers[2].id);
    std::size_t quads = 0;
    for (const DrawCmd& c : list.cmds) {
        quads += c.kind == CmdKind::Quads ? 1 : 0;
    }
    CHECK(quads == 1);
    // Every band restores the scissor it set.
    CHECK(list.cmds.back().kind == CmdKind::Lines);
}

TEST_CASE("BandLayout: DrawList JSON goldens at DPR 1 and 2", "[unit]") {
    for (const float dpr : {1.0F, 2.0F}) {
        const BandLayoutInput base = bandInput();
        BandLayoutInput in = base;
        for (BandCell& c : in.ranges) {
            c.x0 *= dpr;
            c.x1 *= dpr;
        }
        for (BandCell& c : in.sections) {
            c.x0 *= dpr;
            c.x1 *= dpr;
        }
        for (BandMarker& m : in.markers) {
            m.x *= dpr;
        }
        const RectPx area{40.0F * dpr, 18.0F * dpr, 300.0F * dpr, 200.0F * dpr};
        DrawList list;
        list.widthPx = static_cast<int>(360.0F * dpr);
        list.heightPx = static_cast<int>(80.0F * dpr);
        list.dpr = dpr;
        BandColours colours;
        colours.background = 0xF7F7F8FFU;
        BandLayout::emit(list, in, BandLayout::layout(in, area, dpr), metrics(), colours, dpr, nullptr);
        test::checkGolden("band_layout", "three_bands", dpr,
                          R"({"case":"three_bands","dpr":)" + std::to_string(static_cast<int>(dpr)) + ",\n" +
                              R"("static":)" + list.toJson() + "}\n");
    }
}

// ---- KeyLayout

namespace {

std::vector<KeyRow> keyRows(bool datum, bool measurements) {
    std::vector<KeyRow> rows(2);
    rows[0].label = "Corr Speed [km/h]";
    rows[0].colour = 0xD32F2FFFU;
    rows[0].group = 0;
    rows[0].trace = 0;
    rows[0].slotValues = {KeyValue{"183.4", 0xD32F2FFFU}, KeyValue{"179.9", 0x0288D1FFU}};
    rows[1].label = "Throttle Pos [%]";
    rows[1].colour = 0x1565C0FFU;
    rows[1].group = 1;
    rows[1].trace = 1;
    rows[1].slotValues = {KeyValue{"100", 0x1565C0FFU}, KeyValue{"(not in Lap 9)", 0x0288D1FFU}};
    if (datum) {
        rows[0].datumValues = {KeyValue{"92.0", 0xD32F2FFFU}, KeyValue{"95.5", 0x0288D1FFU}};
        rows[0].deltaValues = {KeyValue{"91.4", 0xD32F2FFFU}, KeyValue{"84.4", 0x0288D1FFU}};
        rows[1].datumValues = {KeyValue{"12", 0x1565C0FFU}};
        rows[1].deltaValues = {KeyValue{"88", 0x1565C0FFU}};
    }
    if (measurements) {
        KeyMeasurement m;
        m.min = KeyValue{"61.2", 0};
        m.max = KeyValue{"241.7", 0};
        m.avg = KeyValue{"163.0", 0};
        rows[0].measurements = {m, m};
        m.min = KeyValue{"0", 0x1565C0FFU};
        m.max = KeyValue{"100", 0x1565C0FFU};
        m.avg = KeyValue{"64", 0x1565C0FFU};
        rows[1].measurements = {m};
    }
    return rows;
}

} // namespace

TEST_CASE("KeyLayout: the default columns give the M1 layout and DrawLists exactly", "[unit]") {
    const std::vector<KeyRow> rows = keyRows(false, false);
    for (const float dpr : {1.0F, 2.0F}) {
        const KeyGeometry m1 = KeyLayout::layout(rows, metrics(), 640.0F * dpr, 12.8F * dpr, dpr);
        const KeyGeometry m3 =
            KeyLayout::layout(rows, metrics(), 640.0F * dpr, 12.8F * dpr, dpr, KeyColumns{});
        CHECK(m3.widthPx == m1.widthPx);
        CHECK(m3.labelWidthPx == m1.labelWidthPx);
        CHECK(m3.valueWidthPx == m1.valueWidthPx);
        CHECK(m3.rowRects == m1.rowRects);
        CHECK(m3.datumWidthPx == 0.0F);
        CHECK(m3.measureWidthPx == 0.0F);
        CHECK(m3.cellRect(1, 1, KeyCell::Value) == m1.valueRect(1, 1));
        CHECK(m3.cellRect(1, 0, KeyCell::Datum).w == 0.0F);
        CHECK(m3.cellRect(1, 0, KeyCell::Min).w == 0.0F);
        DrawList a;
        DrawList b;
        KeyLayout::emitValues(a, rows, m1, 3.0F, 20.0F, 0x202020FFU, 0xF7F7F8FFU);
        KeyLayout::emitValues(b, rows, m3, 3.0F, 20.0F, 0x202020FFU, 0xF7F7F8FFU);
        CHECK(a == b);
    }
}

TEST_CASE("KeyLayout: datum and measurement columns, cells and hit regions", "[unit]") {
    const std::vector<KeyRow> rows = keyRows(true, true);
    KeyColumns columns;
    columns.datum = true;
    columns.measurements = true;
    const float lineH = 12.8F;
    const KeyGeometry g = KeyLayout::layout(rows, metrics(), 1200.0F, lineH, 1.0F, columns);
    const KeyGeometry m1 = KeyLayout::layout(rows, metrics(), 1200.0F, lineH, 1.0F);
    CHECK(g.widthPx > m1.widthPx);
    CHECK(g.datumWidthPx > 0.0F);
    CHECK(g.deltaWidthPx > 0.0F);
    CHECK(g.measureWidthPx > 0.0F);
    // Rows: value lines (one per slot) then the measurement lines.
    REQUIRE(g.rowRects.size() == 2);
    CHECK(g.rowRects[0].h == lineH * 4.0F);
    CHECK(g.rowRects[1].h == lineH * 3.0F);
    CHECK(g.valueLines == std::vector<std::uint32_t>{2, 2});
    // value · datum · delta right-aligned in that order, no overlaps; the delta ends at the right pad.
    const RectPx v = g.cellRect(0, 1, KeyCell::Value);
    const RectPx d = g.cellRect(0, 1, KeyCell::Datum);
    const RectPx t = g.cellRect(0, 1, KeyCell::Delta);
    CHECK(v == g.valueRect(0, 1));
    CHECK(std::abs(v.x + v.w + g.padPx - d.x) < 1e-3F);
    CHECK(std::abs(d.x + d.w + g.padPx - t.x) < 1e-3F);
    CHECK(std::abs(t.x + t.w + g.padPx - g.widthPx) < 1e-3F);
    CHECK(v.x > g.labelWidthPx);
    // min · max · avg on the measurement lines under the value lines.
    const RectPx mn = g.cellRect(0, 1, KeyCell::Min);
    const RectPx mx = g.cellRect(0, 1, KeyCell::Max);
    const RectPx av = g.cellRect(0, 1, KeyCell::Avg);
    CHECK(mn.y == lineH * 3.0F);
    CHECK(std::abs(mn.x + mn.w + g.padPx - mx.x) < 1e-3F);
    CHECK(std::abs(mx.x + mx.w + g.padPx - av.x) < 1e-3F);
    CHECK(std::abs(av.x + av.w + g.padPx - g.widthPx) < 1e-3F);
    CHECK(g.cellRect(1, 0, KeyCell::Avg).y == g.rowRects[1].y + 2.0F * lineH);
    // Measurement hits: group/trace from the row, slot = measurement index, cell 0/1/2.
    DrawList list;
    HitTester hits;
    KeyLayout::emitMeasurements(list, rows, g, 10.0F, 100.0F, 0x202020FFU, &hits);
    const HitResult h = hits.hit(10.0F + mx.x + 1.0F, 100.0F + mx.y + 1.0F);
    CHECK(h.kind == HitKind::KeyMeasurement);
    CHECK(h.group == 0);
    CHECK(h.trace == 0);
    CHECK(h.slot == 1);
    CHECK(h.cell == 1);
    CHECK(list.texts.size() == 9);
    CHECK(list.texts[0].colour == 0x202020FFU); // colour 0 -> the text colour
    CHECK(list.texts[0].style == TextStyleId::Measurement);
    // Datum values: six runs (two lines datum + delta for row 0, one each for row 1).
    DrawList dyn;
    KeyLayout::emitDatumValues(dyn, rows, g, 10.0F, 100.0F, 0x202020FFU, 0xF7F7F8FFU);
    CHECK(dyn.texts.size() == 6);
    // A narrow component shrinks the label first, down to 30 % of the column, then the cells (the
    // time_distance completion keeps the channel names readable with a datum).
    const KeyGeometry narrow = KeyLayout::layout(rows, metrics(), 200.0F, lineH, 1.0F, columns);
    CHECK(narrow.widthPx == 0.40F * 200.0F);
    CHECK(narrow.labelWidthPx == 0.30F * narrow.widthPx);
    CHECK(narrow.cellRect(0, 0, KeyCell::Value).x >= 0.0F);
    CHECK(narrow.cellRect(0, 0, KeyCell::Value).x + 1e-3F >=
          2.0F * narrow.padPx + narrow.squarePx + narrow.labelWidthPx);
}

TEST_CASE("KeyLayout: DrawList JSON goldens of the datum and measurement columns", "[unit]") {
    const std::vector<KeyRow> rows = keyRows(true, true);
    KeyColumns columns;
    columns.datum = true;
    columns.measurements = true;
    for (const float dpr : {1.0F, 2.0F}) {
        const float lineH = metrics().lineHeightPx(TextStyleId::Key, dpr);
        const KeyGeometry g = KeyLayout::layout(rows, metrics(), 900.0F * dpr, lineH, dpr, columns);
        DrawList staticList;
        staticList.widthPx = static_cast<int>(900.0F * dpr);
        staticList.heightPx = static_cast<int>(120.0F * dpr);
        staticList.dpr = dpr;
        DrawList dynamicList = staticList;
        KeyLayout::emitLabels(staticList, rows, g, 0.0F, 18.0F * dpr, 0x6B6F75FFU);
        KeyLayout::emitMeasurements(staticList, rows, g, 0.0F, 18.0F * dpr, 0x202124FFU, nullptr);
        KeyLayout::emitValues(dynamicList, rows, g, 0.0F, 18.0F * dpr, 0x202124FFU, 0xF7F7F8FFU);
        KeyLayout::emitDatumValues(dynamicList, rows, g, 0.0F, 18.0F * dpr, 0x202124FFU, 0xF7F7F8FFU);
        test::checkGolden("key_layout", "datum_measurements", dpr,
                          test::layersJson("datum_measurements", dpr, staticList, dynamicList, DrawList{}));
    }
}

// ---- body JSON helpers

namespace {

JsonDoc doc(const std::string& text) {
    return JsonDoc::parse(text).value();
}

} // namespace

TEST_CASE("ScaleSpec and ColourBands: JSON round trips and errors", "[unit]") {
    const JsonDoc manual = doc(R"({"mode":"manual","min":{"v":-3,"unit":"G"},"max":{"v":3,"unit":"G"}})");
    const Result<ScaleSpec> s = parseScaleSpec(manual);
    REQUIRE(s.ok());
    CHECK(s->mode == ScaleMode::Manual);
    CHECK(s->min == analysis::UnitValue{-3.0, "G"});
    CHECK(scaleSpecToJson(*s) == manual);
    CHECK(scaleSpecToJson(ScaleSpec{}) == doc(R"({"mode":"channel"})"));
    Result<ScaleSpec> bad = parseScaleSpec(doc(R"({"mode":"bogus"})"));
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == ErrorCode::Invalid);
    CHECK(bad.error().context.detail == "/mode");
    bad = parseScaleSpec(doc(R"({"mode":"manual","min":3})"));
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().context.detail == "/min");

    const JsonDoc bands =
        doc(R"([{"min":{"v":80,"unit":"C"},"max":{"v":100,"unit":"C"},"open":"none",)"
            R"("colour":"#ffb000","visible":true},{"min":{"v":100,"unit":"C"},"max":{"v":0,)"
            R"("unit":"C"},"open":"above","colour":"#ff3030c0","visible":false}])");
    const Result<std::vector<ColourBand>> b = parseColourBands(bands);
    REQUIRE(b.ok());
    REQUIRE(b->size() == 2);
    CHECK((*b)[0].colour == 0xFFB000FFU);
    CHECK((*b)[1].open == BandOpen::Above);
    CHECK((*b)[1].colour == 0xFF3030C0U);
    CHECK_FALSE((*b)[1].visible);
    CHECK(colourBandsToJson(*b) == bands);
    CHECK(parseColourBands(JsonDoc{}).value().empty());
    const Result<std::vector<ColourBand>> badColour = parseColourBands(doc(R"([{"colour":"red"}])"));
    REQUIRE_FALSE(badColour.ok());
    CHECK(badColour.error().context.detail == "/0/colour");
    CHECK_FALSE(parseColourBands(doc(R"({"min":1})")).ok());
}

TEST_CASE("colourBandOf: closed, open and hidden bands", "[unit]") {
    std::vector<ColourBand> bands(3);
    bands[0].min = {80.0, "C"};
    bands[0].max = {100.0, "C"};
    bands[1].max = {20.0, "C"};
    bands[1].open = BandOpen::Below;
    bands[2].min = {100.0, "C"};
    bands[2].open = BandOpen::Above;
    CHECK(colourBandOf(bands, 80.0) == 0U);
    CHECK(colourBandOf(bands, 99.9) == 0U);
    CHECK(colourBandOf(bands, 100.0) == 2U);
    CHECK(colourBandOf(bands, 1e9) == 2U);
    CHECK(colourBandOf(bands, -40.0) == 1U);
    CHECK_FALSE(colourBandOf(bands, 50.0).has_value());
    CHECK_FALSE(colourBandOf(bands, std::numeric_limits<double>::quiet_NaN()).has_value());
    bands[0].visible = false;
    CHECK_FALSE(colourBandOf(bands, 90.0).has_value());
}
