// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Unit tests of the plot core without data: ChangeMask, SelectionSnapshot and the D-017 / D-100 colour rules,
// the DrawList helpers and golden JSON form, vertex emission (04 section 4.7), AxisLayout, KeyLayout and
// HitTester against FixedTextMetrics.

#include <esses/plotcore/plotcore.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::plot;

namespace {

project::Scheme testScheme() {
    project::Scheme s;
    s.roles["mainSingleColour"] = 0x202124FFU;
    s.roles["overlay0"] = 0x8A8F96FFU;
    s.channelPalette = {0xD32F2FFFU, 0x1565C0FFU, 0x2E7D32FFU};
    s.overlayPalette = {0x4FC3F7FFU, 0xAED581FFU};
    return s;
}

std::vector<float> vertsOf(const DrawList& list, const DrawCmd& cmd) {
    const auto begin = list.verts.begin() + static_cast<std::ptrdiff_t>(cmd.vertexOffset) * 2;
    return {begin, begin + static_cast<std::ptrdiff_t>(cmd.vertexCount) * 2};
}

M4Column col(float first, float lo, float hi, float last, std::uint8_t flags = 0, float xFrac = 0.0F) {
    return M4Column{first, last, lo, hi, flags, xFrac};
}

} // namespace

TEST_CASE("ChangeMask: bits and the layer groups", "[unit]") {
    const ChangeMask m = Change::Cursor | Change::Zoom;
    CHECK(has(m, Change::Cursor));
    CHECK(has(m, Change::Zoom));
    CHECK_FALSE(has(m, Change::Domain));
    CHECK(has(kFullRebuild, Change::Scheme));
    CHECK(has(kDynamicOnly, Change::Datum));
    CHECK_FALSE(has(kStaticRebuild, Change::Cursor));
    CHECK(mask(Change::Size) == 256);

    ViewState a;
    ViewState b = a;
    b.version = 9;
    CHECK(viewChanges(a, b) == 0);
    b.cursorT = 5;
    b.datumT = 3;
    CHECK(viewChanges(a, b) == (Change::Cursor | Change::Datum));
    CHECK(staticReusable(viewChanges(a, b)));
    b.zoom = XSpan{0.0, 10.0};
    b.domain = Domain::Distance;
    b.offsetAxis = true;
    const ChangeMask all = viewChanges(a, b);
    CHECK(has(all, Change::Zoom));
    CHECK(has(all, Change::Domain));
    CHECK(has(all, Change::Offsets));
    CHECK_FALSE(staticReusable(all));
    CHECK_FALSE(staticReusable(mask(Change::Scheme)));
}

TEST_CASE("SelectionSnapshot: slots and the reference overlay", "[unit]") {
    SelectionSnapshot s;
    model::RangeRef a;
    a.groupPath = "Outings:Laps";
    a.name = "Lap 3";
    model::RangeRef b = a;
    b.name = "Lap 5";
    s.overlays = {a, b};
    auto slots = s.visibleSlots();
    REQUIRE(slots.size() == 3);
    CHECK(slots[0].kind == SlotKind::Main);
    CHECK(slots[2].kind == SlotKind::Overlay);
    CHECK(slots[2].number == 2);
    CHECK(s.referenceOverlay()->name == "Lap 3");
    s.overlay0 = b;
    CHECK(s.referenceOverlay()->name == "Lap 5");
    CHECK(s.visibleSlots()[1].kind == SlotKind::Overlay0);
    s.overlaysHidden = true;
    CHECK(s.visibleSlots().size() == 1);
    CHECK_FALSE(s.referenceOverlay().has_value());
}

TEST_CASE("traceColour: D-017 roles and the D-100 overlayStyle", "[unit]") {
    project::Scheme scheme = testScheme();
    SelectionSnapshot sel;
    const Slot main{SlotKind::Main, 0, {}};
    const Slot o0{SlotKind::Overlay0, 0, {}};
    const Slot o2{SlotKind::Overlay, 2, {}};
    CHECK(traceColour(scheme, sel, main, 1) == 0x1565C0FFU);
    CHECK(traceColour(scheme, sel, main, 4) == 0x1565C0FFU); // palette wraps
    sel.singleColourMain = true;
    CHECK(traceColour(scheme, sel, main, 1) == 0x202124FFU);
    CHECK(traceColour(scheme, sel, o0, 1) == 0x8A8F96FFU);
    CHECK(traceColour(scheme, sel, o2, 2) == 0x2E7D32FFU); // default: plain channel colours
    CHECK(traceDash(scheme, o2) == DashId::Solid);
    scheme.overlayStyle.colourSource = "overlayPalette";
    scheme.overlayStyle.dash = "dash";
    scheme.overlayStyle.lightness = 1.0;
    scheme.overlayStyle.widthScale = 2.0;
    CHECK(traceColour(scheme, sel, o2, 0) == 0xFFFFFFFFU); // overlayPalette[1] lightened fully
    CHECK(traceDash(scheme, o2) == DashId::Dash);
    CHECK(traceDash(scheme, main) == DashId::Solid);
    CHECK(traceWidthScale(scheme, o2) == 2.0F);
    CHECK(traceWidthScale(scheme, o0) == 1.0F);
    CHECK(adjustLightness(0x804020FFU, -0.5) == 0x402010FFU);
    CHECK(adjustLightness(0x00000080U, 0.5) == 0x80808080U);
}

TEST_CASE("rangeKeyColour: the key colour of a range (06 section 5.3 item 3)", "[unit]") {
    project::Scheme scheme = testScheme();
    scheme.roles["cursorMain"] = 0xE3261CFFU;
    const Slot main{SlotKind::Main, 0, {}};
    const Slot o0{SlotKind::Overlay0, 0, {}};
    const Slot o1{SlotKind::Overlay, 1, {}};
    const Slot o2{SlotKind::Overlay, 2, {}};
    const Slot o3{SlotKind::Overlay, 3, {}};
    CHECK(rangeKeyColour(scheme, main) == 0xE3261CFFU);
    CHECK(rangeKeyColour(scheme, o0) == 0x8A8F96FFU);
    CHECK(rangeKeyColour(scheme, o1) == 0x4FC3F7FFU);
    CHECK(rangeKeyColour(scheme, o2) == 0xAED581FFU);
    CHECK(rangeKeyColour(scheme, o3) == 0x4FC3F7FFU); // the overlay palette cycles
    // Unlike the trace colours, two overlays never share a key colour under the default overlayStyle.
    SelectionSnapshot sel;
    CHECK(traceColour(scheme, sel, o1, 0) == traceColour(scheme, sel, o2, 0));
    CHECK(rangeKeyColour(scheme, o1) != rangeKeyColour(scheme, o2));
}

TEST_CASE("DrawList: helpers, append and the golden JSON form", "[unit]") {
    DrawList list;
    list.widthPx = 100;
    list.heightPx = 50;
    list.dpr = 2.0F;
    list.clear(0xFFFFFFFFU);
    const std::array<float, 4> xy{0.0F, 0.0F, 10.004F, -0.001F};
    list.polyline(xy.data(), 2, 0xFF0000FFU, 1.5F, DashId::Dot);
    list.lines(xy.data(), 2, 0x00FF0080U, 1.0F);
    list.points(xy.data(), 2, 0x0000FFFFU, 3.0F);
    list.scissor(RectPx{1.0F, 2.0F, 3.0F, 4.0F});
    list.filledRect(RectPx{0.125F, 0.0F, 5.0F, 5.0F}, 0x112233FFU);
    list.textRun("Lap \"7\"", 1.0F, 2.0F, TextStyleId::Key, TextAnchor::MiddleRight, 0x000000FFU, 40.0F, 0,
                 0xEEEEEEFFU);
    list.rectOutline(RectPx{0.0F, 0.0F, 10.0F, 10.0F}, 0x000000FFU, 2.0F);
    CHECK(list.textOf(list.texts[0]) == "Lap \"7\"");
    const std::string json = list.toJson();
    const std::string expected =
        "{\"w\":100,\"h\":50,\"dpr\":2,\"cmds\":[\n"
        "{\"k\":\"clear\",\"c\":\"#ffffff\"},\n"
        "{\"k\":\"polyline\",\"c\":\"#ff0000\",\"w\":1.5,\"d\":\"dot\",\"v\":[0,0,10,0]},\n"
        "{\"k\":\"lines\",\"c\":\"#00ff0080\",\"w\":1,\"d\":\"solid\",\"v\":[0,0,10,0]},\n"
        "{\"k\":\"points\",\"c\":\"#0000ff\",\"s\":3,\"v\":[0,0,10,0]},\n"
        "{\"k\":\"scissor\",\"r\":[1,2,3,4]},\n"
        "{\"k\":\"rect\",\"c\":\"#112233\",\"r\":[0.13,0,5,5]},\n"
        "{\"k\":\"text\",\"t\":\"Lap \\\"7\\\"\",\"x\":1,\"y\":2,\"style\":\"key\","
        "\"anchor\":\"middleRight\",\"c\":\"#000000\",\"maxw\":40,\"bg\":\"#eeeeee\"},\n"
        "{\"k\":\"polyline\",\"c\":\"#000000\",\"w\":2,\"d\":\"solid\","
        "\"v\":[1,1,9,1,9,9,1,9,1,1]}\n"
        "]}\n";
    CHECK(json == expected);

    DrawList outer;
    outer.append(list, 100.0F, 50.0F);
    REQUIRE(outer.cmds.size() == list.cmds.size());
    CHECK(outer.verts[0] == 100.0F);
    CHECK(outer.texts[0].x == 101.0F);
    CHECK(outer.cmds[4].rect.x == 101.0F);
    CHECK(outer.textOf(outer.texts[0]) == "Lap \"7\"");
}

TEST_CASE("emit: M4 vertex patterns, gaps, steps and clamping (04 section 4.7)", "[unit]") {
    TraceSpec spec;
    spec.colour = 0xFF0000FFU;
    spec.widthLogicalPx = 1.0F;
    YMap y;
    y.yTopPx = 0.0F;
    y.yBottomPx = 100.0F;
    y.vMin = 0.0;
    y.vMax = 100.0; // y = 100 - v

    SECTION("B/C columns: first, lo/hi in first-last order, last; joined") {
        const std::vector<M4Column> cols{col(10, 5, 20, 15), col(15, 0, 30, 2)};
        DrawList list;
        TraceBuilder::emit(list, cols, 0.0, spec, y, 10.0F, 1.0F);
        REQUIRE(list.cmds.size() == 1);
        CHECK(list.cmds[0].kind == CmdKind::Polyline);
        const std::vector<float> expected{10.0F, 90.0F, 10.5F, 95.0F, 10.5F, 80.0F,  11.0F, 85.0F,
                                          11.0F, 85.0F, 11.5F, 70.0F, 11.5F, 100.0F, 12.0F, 98.0F};
        CHECK(vertsOf(list, list.cmds[0]) == expected);
    }
    SECTION("an Empty|HasInvalid column breaks, a bare Empty column joins") {
        const std::vector<M4Column> cols{col(1, 1, 1, 1), col(0, 0, 0, 0, M4Flag::Empty),
                                         col(2, 2, 2, 2), col(0, 0, 0, 0, M4Flag::Empty | M4Flag::HasInvalid),
                                         col(3, 3, 3, 3), col(4, 4, 4, 4)};
        DrawList list;
        TraceBuilder::emit(list, cols, 0.0, spec, y, 0.0F, 1.0F);
        REQUIRE(list.cmds.size() == 2);
        CHECK(list.cmds[0].vertexCount == 8);
        CHECK(list.cmds[1].vertexCount == 8);
    }
    SECTION("break flags of regime A (excluded samples around a raw sample) split the line") {
        const std::vector<M4Column> cols{col(1, 1, 1, 1, M4Flag::RawRegime, 0.5F),
                                         col(2, 2, 2, 2, M4Flag::RawRegime | M4Flag::BreakAfter, 0.5F),
                                         col(3, 3, 3, 3, M4Flag::RawRegime, 0.5F),
                                         col(4, 4, 4, 4, M4Flag::RawRegime | M4Flag::BreakBefore, 0.5F),
                                         col(5, 5, 5, 5, M4Flag::RawRegime, 0.5F)};
        DrawList list;
        TraceBuilder::emit(list, cols, 0.0, spec, y, 0.0F, 1.0F);
        // [1, 2] | [3] | [4, 5]: two polylines and the isolated sample 3 as a dot.
        REQUIRE(list.cmds.size() == 3);
        CHECK(list.cmds[0].kind == CmdKind::Polyline);
        CHECK(list.cmds[0].vertexCount == 2);
        CHECK(list.cmds[1].kind == CmdKind::Polyline);
        CHECK(list.cmds[1].vertexCount == 2);
        CHECK(vertsOf(list, list.cmds[1]) == std::vector<float>{3.5F, 96.0F, 4.5F, 95.0F});
        CHECK(list.cmds[2].kind == CmdKind::Points);
        CHECK(vertsOf(list, list.cmds[2]) == std::vector<float>{2.5F, 97.0F});
    }
    SECTION("break flags of regime B and an isolated sample as a dot") {
        const std::vector<M4Column> cols{col(1, 1, 1, 1, M4Flag::BreakAfter | M4Flag::HasInvalid),
                                         col(2, 2, 2, 2, M4Flag::BreakBefore | M4Flag::HasInvalid),
                                         col(0, 0, 0, 0, M4Flag::Empty | M4Flag::HasInvalid),
                                         col(5, 5, 5, 5, M4Flag::RawRegime, 0.5F),
                                         col(0, 0, 0, 0, M4Flag::Empty | M4Flag::HasInvalid)};
        DrawList list;
        TraceBuilder::emit(list, cols, 0.0, spec, y, 0.0F, 1.0F);
        REQUIRE(list.cmds.size() == 3);
        CHECK(list.cmds[0].kind == CmdKind::Polyline);
        CHECK(list.cmds[1].kind == CmdKind::Polyline);
        CHECK(list.cmds[2].kind == CmdKind::Points);
        CHECK(vertsOf(list, list.cmds[2]) == std::vector<float>{3.5F, 95.0F});
    }
    SECTION("regime A vertices at xFrac, Previous steps, point markers when sparse") {
        spec.interp = store::Interp::Previous;
        std::vector<M4Column> cols(10, col(0, 0, 0, 0, M4Flag::Empty));
        cols[1] = col(10, 10, 10, 10, M4Flag::RawRegime, 0.25F);
        cols[6] = col(20, 20, 20, 20, M4Flag::RawRegime, 0.5F);
        DrawList list;
        TraceBuilder::emit(list, cols, 0.0, spec, y, 0.0F, 2.0F);
        REQUIRE(list.cmds.size() == 2);
        CHECK(list.cmds[0].widthPx == 2.0F);
        CHECK(vertsOf(list, list.cmds[0]) == std::vector<float>{1.25F, 90.0F, 6.5F, 90.0F, 6.5F, 80.0F});
        CHECK(list.cmds[1].kind == CmdKind::Points); // 2 samples over 10 px: below 0.25 spp
        CHECK(list.cmds[1].pointSizePx == 6.0F);
    }
    SECTION("Y is clamped to the panel +- 2 line widths, origin offsets are applied") {
        const std::vector<M4Column> cols{col(-500, -500, 500, 500)};
        DrawList list;
        TraceBuilder::emit(list, cols, 50.0, spec, y, 0.0F, 1.0F);
        const auto v = vertsOf(list, list.cmds[0]);
        CHECK(v[1] == 102.0F);
        CHECK(v[7] == -2.0F);
    }
    SECTION("points style: points for raw columns, vertical segments otherwise") {
        spec.style = TraceStyle::Points;
        const std::vector<M4Column> cols{col(1, 0, 3, 2), col(5, 5, 5, 5, M4Flag::RawRegime, 0.0F)};
        DrawList list;
        TraceBuilder::emit(list, cols, 0.0, spec, y, 0.0F, 1.0F);
        REQUIRE(list.cmds.size() == 2);
        CHECK(list.cmds[0].kind == CmdKind::Lines);
        CHECK(vertsOf(list, list.cmds[0]) == std::vector<float>{0.5F, 100.0F, 0.5F, 97.0F});
        CHECK(list.cmds[1].kind == CmdKind::Points);
    }
}

TEST_CASE("AxisLayout: nice steps, time labels and tick spacing", "[unit]") {
    CHECK(AxisLayout::niceStep(0.7) == 1.0);
    CHECK(AxisLayout::niceStep(1.0) == 1.0);
    CHECK(AxisLayout::niceStep(1.2) == 2.0);
    CHECK(AxisLayout::niceStep(3.0) == 5.0);
    CHECK(AxisLayout::niceStep(7.0) == 10.0);
    CHECK(AxisLayout::niceStep(4200.0) == 5000.0);
    CHECK(AxisLayout::niceStep(0.013) == 0.02);

    CHECK(AxisLayout::formatTime(83'456'789, TimeFormat::MinutesSeconds, 3) == "1:23.457");
    CHECK(AxisLayout::formatTime(83'456'789, TimeFormat::Seconds, 1) == "83.5");
    CHECK(AxisLayout::formatTime(3'723'400'000, TimeFormat::Hours, 1) == "1:02:03.4");
    CHECK(AxisLayout::formatTime(-1'500'000, TimeFormat::MinutesSeconds, 1) == "-0:01.5");
    CHECK(AxisLayout::formatTime(-400, TimeFormat::Seconds, 0) == "0");
    CHECK(AxisLayout::formatTime(59'999'999, TimeFormat::MinutesSeconds, 2) == "1:00.00");

    const FixedTextMetrics metrics;
    AxisSpec time;
    time.kind = AxisKind::Time;
    time.lo = 0.0;
    time.hi = 60.0 * 1e6;
    time.lengthPx = 600.0F;
    const auto ticks = AxisLayout::ticks(time, metrics, 1.0F);
    REQUIRE(ticks.size() >= 3);
    CHECK(ticks.front().label == "0:00");
    for (std::size_t i = 1; i < ticks.size(); ++i) {
        const float gap = ticks[i].posPx - ticks[i - 1].posPx;
        const float need = metrics.advancePx(TextStyleId::Axis, ticks[i].label, 1.0F);
        CHECK(gap >= need);
        CHECK(gap >= 6.0F * metrics.advancePx(TextStyleId::Axis, "0", 1.0F));
    }

    AxisSpec value;
    value.kind = AxisKind::Value;
    value.vertical = true;
    value.lo = 300.0; // top
    value.hi = -20.0; // bottom
    value.lengthPx = 200.0F;
    const auto vt = AxisLayout::ticks(value, metrics, 2.0F);
    REQUIRE(vt.size() >= 2);
    CHECK(vt.front().value <= 0.0);
    CHECK(vt.front().posPx > vt.back().posPx); // values grow upward
    for (const AxisTick& t : vt) {
        CHECK(t.posPx >= 0.0F);
        CHECK(t.posPx <= 200.0F);
    }
    DrawList list;
    AxisLayout::emit(list, value, vt, RectPx{0.0F, 10.0F, 72.0F, 200.0F}, 0x000000FFU, 2.0F);
    REQUIRE(list.cmds.size() == vt.size() + 1);
    CHECK(list.cmds[0].kind == CmdKind::Lines);
    CHECK(list.texts[0].anchor == TextAnchor::MiddleRight);

    AxisSpec empty = value;
    empty.lo = empty.hi;
    CHECK(AxisLayout::ticks(empty, metrics, 1.0F).empty());
}

TEST_CASE("KeyLayout: widths, clamping, rows per slot and emission", "[unit]") {
    const FixedTextMetrics metrics;
    std::vector<KeyRow> rows(2);
    rows[0].label = "Corr Speed [km/h]";
    rows[0].colour = 0xFF0000FFU;
    rows[0].slotValues = {{"-88888.8", 0xFF0000FFU}, {"-88888.8", 0x00FF00FFU}};
    rows[1].label = "Throttle Pos [%]";
    rows[1].colour = 0x0000FFFFU;
    rows[1].missing = true;
    rows[1].slotValues = {{"-88888.8", 0x0000FFFFU}, {"", 0x00FF00FFU}};
    const float rowH = metrics.lineHeightPx(TextStyleId::Key, 1.0F);
    const KeyGeometry g = KeyLayout::layout(rows, metrics, 1000.0F, rowH, 1.0F);
    const float labelAdv = metrics.advancePx(TextStyleId::Key, rows[0].label, 1.0F);
    const float valueAdv = metrics.advancePx(TextStyleId::KeyValue, "-88888.8", 1.0F);
    CHECK(g.valueWidthPx == valueAdv);
    CHECK(g.widthPx >= 120.0F); // 12 % of the component
    CHECK(g.labelWidthPx >= labelAdv);
    REQUIRE(g.rowRects.size() == 2);
    CHECK(g.rowRects[0].h == 2.0F * rowH);
    CHECK(g.rowRects[1].y == 2.0F * rowH);

    const KeyGeometry narrow = KeyLayout::layout(rows, metrics, 300.0F, rowH, 1.0F);
    CHECK(narrow.widthPx == 120.0F); // 40 % of the component
    CHECK(narrow.labelWidthPx < labelAdv);

    DrawList labels;
    KeyLayout::emitLabels(labels, rows, g, 5.0F, 7.0F, 0x777777FFU);
    REQUIRE(labels.texts.size() == 2);
    CHECK(labels.texts[0].colour == 0xFF0000FFU);
    CHECK(labels.texts[1].colour == 0x777777FFU); // missing
    CHECK(labels.texts[0].anchor == TextAnchor::MiddleLeft);
    DrawList values;
    KeyLayout::emitValues(values, rows, g, 5.0F, 7.0F, 0x000000FFU, 0xEEEEEEFFU);
    REQUIRE(values.texts.size() == 3);
    CHECK(values.texts[1].colour == 0x00FF00FFU);
    CHECK(values.texts[1].y == 7.0F + 1.5F * rowH);
    CHECK(values.texts[0].x == 5.0F + g.widthPx - g.padPx);
    const RectPx sq = g.squareRect(1);
    CHECK(sq.w == g.squarePx);
    CHECK(sq.y > g.rowRects[1].y);
}

TEST_CASE("HitTester: later regions win, empty rectangles are ignored", "[unit]") {
    HitTester hits;
    HitResult panel;
    panel.kind = HitKind::Panel;
    panel.group = 1;
    HitResult key;
    key.kind = HitKind::KeyLabel;
    key.trace = 3;
    hits.addRect(RectPx{0.0F, 0.0F, 100.0F, 100.0F}, panel);
    hits.addRect(RectPx{10.0F, 10.0F, 10.0F, 10.0F}, key);
    hits.addRect(RectPx{50.0F, 50.0F, 0.0F, 10.0F}, key);
    CHECK(hits.hit(15.0F, 15.0F).kind == HitKind::KeyLabel);
    CHECK(hits.hit(15.0F, 15.0F).trace == 3);
    CHECK(hits.hit(50.0F, 55.0F).kind == HitKind::Panel);
    CHECK(hits.hit(100.0F, 5.0F).kind == HitKind::None);
    hits.clear();
    CHECK(hits.empty());
}
