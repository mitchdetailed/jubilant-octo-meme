// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// OutingGeometry (04 section 9) and TrackGeometry (04 section 14): behaviour and the DrawList JSON goldens of
// tests/golden/drawlist/outing and tests/golden/drawlist/track_position at DPR 1 and 2. The outing cases
// reuse the synthetic sources of td_golden_cases.hpp (columns made resident first, so the summary path is
// deterministic); the track cases use an octagonal circuit with rational corners (lengths by sqrt, which IEEE
// rounds correctly everywhere) and constant-speed laps.

#include "m3_golden_support.hpp"

#include <esses/model/layer.hpp>
#include <esses/model/marker.hpp>
#include <esses/plotcore/outing_geometry.hpp>
#include <esses/plotcore/track_geometry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::plot;

namespace {

constexpr Micros kSec = test::kSec;

// ---- outing

const std::string kOutingBody =
    R"({"display":{"traceStyle":"line","lineWidth":1,"showGrid":true},"groups":[{"name":"Speed",)"
    R"("scaleMode":"channel","traces":[{"channel":"Corr Speed","displayUnit":"km/h","dps":1}]},)"
    R"({"name":"Throttle","scaleMode":"manual","scaleMin":{"v":0,"unit":"%"},"scaleMax":{"v":100,"unit":"%"},)"
    R"("traces":[{"channel":"Throttle Pos"}]}]})";

struct OutingCase {
    std::string name;
    test::GoldenCase gc;
    bool enableOverlays = false;
};

std::vector<OutingCase> outingCases(const test::Fixture& fx) {
    std::vector<OutingCase> cases;
    {
        OutingCase c;
        c.name = "outing_time";
        c.gc.heightLogical = 200;
        c.gc.zoom = {0.0, 61.5e6};
        c.gc.cursorT = 20 * kSec;
        c.gc.body = kOutingBody;
        c.gc.activeChannel = "Corr Speed";
        c.gc.slots = {{'a', 1, SlotKind::Main, 0}};
        cases.push_back(c);
    }
    {
        OutingCase c;
        c.name = "outing_overlays";
        c.enableOverlays = true;
        c.gc.dark = true;
        c.gc.heightLogical = 200;
        c.gc.zoom = {-10.0e6, 30.0e6};
        c.gc.cursorT = 25 * kSec;
        c.gc.body = kOutingBody.substr(0, kOutingBody.size() - 1) + R"(,"enableOverlays":true})";
        c.gc.activeChannel = "Throttle Pos";
        c.gc.slots = {{'a', 1, SlotKind::Main, 0}, {'b', 1, SlotKind::Overlay0, 0}};
        cases.push_back(c);
    }
    {
        OutingCase c;
        c.name = "outing_distance";
        c.gc.domain = Domain::Distance;
        c.gc.heightLogical = 200;
        c.gc.pending = true;
        c.gc.focused = true;
        const auto chain = fx.a.chainFor(fx.a.laps[1].start);
        c.gc.zoom = {0.0, chain->distAt(fx.a.laps[1].end)};
        c.gc.cursorT = 30 * kSec;
        c.gc.body = R"({"display":{"key":{"pos":"right"}},"groups":[{"name":"Speed","traces":[)"
                    R"({"channel":"Corr Speed","dps":1}]}],"enableOverlays":true})";
        c.gc.activeChannel = "Corr Speed";
        c.gc.slots = {{'a', 1, SlotKind::Main, 0}, {'b', 1, SlotKind::Overlay, 1}};
        cases.push_back(c);
    }
    return cases;
}

ComponentBuildInputs outingInputs(const test::Fixture& fx, const OutingCase& c, float dpr) {
    ComponentBuildInputs in = test::inputsOf(fx, c.gc, dpr);
    in.spec.type = "outing";
    return in;
}

// ---- track

double segLength(double ax, double ay, double bx, double by) {
    const double dx = bx - ax;
    const double dy = by - ay;
    return std::sqrt(dx * dx + dy * dy);
}

std::shared_ptr<const analysis::TrackMap> octagon() {
    auto map = std::make_shared<analysis::TrackMap>();
    map->name = "Test Octagon";
    map->venue = "Test Octagon";
    const std::vector<std::pair<double, double>> corners{{0, 0},     {300, 0}, {400, 50},   {400, 150},
                                                         {300, 200}, {0, 200}, {-100, 150}, {-100, 50}};
    double d = 0.0;
    for (std::size_t i = 0; i < corners.size(); ++i) {
        if (i > 0) {
            d += segLength(corners[i - 1].first, corners[i - 1].second, corners[i].first, corners[i].second);
        }
        map->centre.push_back(analysis::TrackPoint{corners[i].first, corners[i].second, d});
    }
    d +=
        segLength(corners.back().first, corners.back().second, corners.front().first, corners.front().second);
    map->lengthM = d;
    for (const auto& [x, y] : corners) {
        map->inner.push_back(analysis::TrackPoint{x * 0.9 + 15.0, y * 0.9 + 10.0, 0.0});
    }
    map->sections = {
        analysis::TrackSection{"t1", "Default", "Turn 1", 280.0, 420.0, "corner", false},
        analysis::TrackSection{"s1", "Default", "Back Straight", 420.0, 700.0, "straight", false},
        analysis::TrackSection{"t2", "Default", "Turn 2", 700.0, 980.0, "corner", false},
        analysis::TrackSection{"u1", "Mine", "Braking", 250.0, 300.0, "braking", true},
        analysis::TrackSection{"s2", "Default", "Start Straight", 1180.0, 1240.0, "straight", false},
    };
    return map;
}

// Three constant-speed laps of 20 s over the octagon; BCN at the starts of laps 1 and 2 only when
// `outLap` (lap 0 is then an out lap, measured backwards from its end beacon).
struct TrackSource {
    std::vector<LapPiece> pieces;
    std::shared_ptr<model::DataSource> source;
};

TrackSource trackSource(double lengthM, bool outLap, double speedScale) {
    TrackSource ts;
    for (int lap = 0; lap < 3; ++lap) {
        LapPiece p;
        p.start = static_cast<Micros>(lap) * 20 * kSec;
        p.end = static_cast<Micros>(lap + 1) * 20 * kSec;
        p.lookup = test::FakeLapDistance::constant(p.start, p.end, speedScale * lengthM / 20.0);
        ts.pieces.push_back(std::move(p));
    }
    ts.source = std::make_shared<model::DataSource>();
    model::DataLayer layer;
    if (outLap) {
        for (int lap = 1; lap < 3; ++lap) {
            model::Marker m;
            m.cls = model::MarkerClass::BCN;
            m.group = "Beacons";
            m.t = static_cast<Micros>(lap) * 20 * kSec;
            layer.markers.push_back(m);
        }
    }
    ts.source->layers.push_back(std::move(layer));
    return ts;
}

SlotData trackSlot(const TrackSource& ts, std::size_t lap, SlotKind kind, int number) {
    SlotData s;
    s.slot.kind = kind;
    s.slot.number = number;
    s.slot.range.name = "Lap " + std::to_string(lap + 1);
    s.source = ts.source;
    s.range = RangeGeom{ts.pieces[lap].start, ts.pieces[lap].end};
    s.outing = OutingGeom{ts.pieces.front().start, ts.pieces.back().end};
    s.chain = std::make_shared<const DistanceChain>(ts.pieces, ts.pieces[lap].start);
    return s;
}

struct TrackCase {
    std::string name;
    std::string body;
    bool dark = false;
    bool withMap = true;
    bool focused = false;
    Domain domain = Domain::Time;
    XSpan zoom;
    Micros cursorT = 0;
    int width = 320;
    int height = 240;
};

std::vector<TrackCase> trackCases() {
    std::vector<TrackCase> cases;
    {
        TrackCase c;
        c.name = "track_fit";
        c.body = R"({"showSectionLabels":true})";
        c.zoom = {2.0e6, 9.0e6};
        c.cursorT = 6 * kSec;
        cases.push_back(c);
    }
    {
        TrackCase c;
        c.name = "track_manual";
        c.body = R"({"showSectionMarks":false,"showSectionColours":true,"sizes":{"cursor":9,"detail":3,)"
                 R"("material":1},"scale":{"mode":"manual","zoom":1.5,"panX":40,"panY":-20}})";
        c.dark = true;
        c.domain = Domain::Distance;
        c.zoom = {-150.0, 400.0};
        c.cursorT = 15 * kSec;
        c.focused = true;
        cases.push_back(c);
    }
    {
        TrackCase c;
        c.name = "track_no_map";
        c.body = "{}";
        c.withMap = false;
        c.zoom = {0.0, 20.0e6};
        c.cursorT = 5 * kSec;
        cases.push_back(c);
    }
    return cases;
}

ComponentBuildInputs trackInputs(const TrackCase& c, const TrackSource& main, const TrackSource& other,
                                 float dpr) {
    ComponentBuildInputs in;
    in.spec.type = "track_position";
    in.spec.caption.show = true;
    in.spec.caption.autoText = true;
    in.spec.body = JsonDoc::parse(c.body).value();
    in.view.domain = c.domain;
    in.view.zoom = c.zoom;
    in.view.cursorT = c.cursorT;
    in.view.version = 1;
    in.slots = {trackSlot(main, 1, SlotKind::Main, 0), trackSlot(other, 2, SlotKind::Overlay, 1)};
    in.selection.main = in.slots[0].slot.range;
    in.selection.overlays = {in.slots[1].slot.range};
    in.scheme = std::make_shared<const project::Scheme>(c.dark ? test::darkScheme() : test::lightScheme());
    in.metrics = std::make_shared<const FixedTextMetrics>();
    in.layout.widthPx = static_cast<int>(std::lround(static_cast<float>(c.width) * dpr));
    in.layout.heightPx = static_cast<int>(std::lround(static_cast<float>(c.height) * dpr));
    in.layout.dpr = dpr;
    in.focused = c.focused;
    if (c.withMap) {
        in.trackMap = octagon();
    }
    return in;
}

std::size_t countKind(const DrawList& list, CmdKind kind) {
    std::size_t n = 0;
    for (const DrawCmd& c : list.cmds) {
        n += c.kind == kind ? 1 : 0;
    }
    return n;
}

std::size_t countColour(const DrawList& list, Rgba colour) {
    std::size_t n = 0;
    for (const DrawCmd& c : list.cmds) {
        n += (c.kind == CmdKind::Polyline || c.kind == CmdKind::Lines) && c.colour == colour ? 1 : 0;
    }
    return n;
}

bool hasText(const DrawList& list, std::string_view needle) {
    for (const TextRun& r : list.texts) {
        if (list.textOf(r).find(needle) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

// ---- OutingGeometry

TEST_CASE("OutingGeometry: the whole outing, the lap band, the zoom rectangle and the cursor", "[unit]") {
    const test::Fixture fx;
    test::warmAll(fx.a);
    test::warmAll(fx.b);
    const OutingCase oc = outingCases(fx).front();
    const ComponentBuildInputs in = outingInputs(fx, oc, 1.0F);
    const Result<ComponentBuild> build = OutingGeometry::buildStatic(in, CancelToken{});
    REQUIRE(build.ok());
    // The zoom (the Main lap, [60 s, 121.5 s) of a [0, 180 s) outing) maps to that share of the trace area.
    const RectPx zoom = OutingGeometry::zoomRect(in);
    REQUIRE(zoom.w > 0.0F);
    const HitResult panel = build->hits.hit(zoom.x + 1.0F, zoom.y + 1.0F);
    CHECK(panel.kind == HitKind::Panel);
    // Lap cells: the band above the trace area holds one cell per lap; the middle one is the Main lap.
    const HitResult lap = build->hits.hit(zoom.x + 0.5F * zoom.w, zoom.y - 10.0F);
    CHECK(lap.kind == HitKind::Band);
    CHECK(lap.band == 1);
    // Trace area x share: (60 / 180, 121.5 / 180) of the width.
    float traceX = 0.0F;
    float traceW = 0.0F;
    for (const DrawCmd& c : build->list.cmds) {
        if (c.kind == CmdKind::Scissor && c.rect.w > 0.0F) {
            traceX = c.rect.x;
            traceW = c.rect.w;
        }
    }
    REQUIRE(traceW > 0.0F);
    CHECK(std::abs(zoom.x - (traceX + traceW * 60.0F / 180.0F)) < 1.0F);
    CHECK(std::abs(zoom.x + zoom.w - (traceX + traceW * 121.5F / 180.0F)) < 1.0F);
    // A zoom wholly outside the outing gives no rectangle.
    ComponentBuildInputs outside = in;
    outside.view.zoom = {200.0e6, 260.0e6};
    CHECK(OutingGeometry::zoomRect(outside).w == 0.0F);
    // Dynamic: the translucent rectangle and the cursor line.
    const DrawList dyn = OutingGeometry::buildDynamic(in);
    REQUIRE_FALSE(dyn.cmds.empty());
    CHECK(dyn.cmds.front().kind == CmdKind::FilledRect);
    CHECK((dyn.cmds.front().colour & 0xFFU) == 0x40U);
    CHECK(hasText(dyn, "."));
    // Without overlays enabled only Main is drawn; with them the overlay's outing too (time mode only).
    const Rgba overlay0 = test::darkScheme().role("overlay0");
    ComponentBuildInputs withOverlay = outingInputs(fx, outingCases(fx)[1], 1.0F);
    const Result<ComponentBuild> both = OutingGeometry::buildStatic(withOverlay, CancelToken{});
    REQUIRE(both.ok());
    CHECK(countColour(both->list, overlay0) > 0);
    withOverlay.view.domain = Domain::Distance;
    withOverlay.view.zoom = {0.0, 3000.0};
    const Result<ComponentBuild> distance = OutingGeometry::buildStatic(withOverlay, CancelToken{});
    REQUIRE(distance.ok());
    CHECK(countColour(distance->list, overlay0) == 0);
    CHECK(countKind(distance->list, CmdKind::Polyline) > 0);
    // A cancelled build fails with Cancelled.
    CancelSource cancel;
    cancel.cancel();
    const Result<ComponentBuild> cancelled = OutingGeometry::buildStatic(in, cancel.token());
    REQUIRE_FALSE(cancelled.ok());
    CHECK(cancelled.error().code == ErrorCode::Cancelled);
}

TEST_CASE("OutingGeometry: an empty selection and an unparsable body draw an empty graph", "[unit]") {
    ComponentBuildInputs in;
    in.layout = LayoutContext{200, 100, 1.0F};
    in.spec.body = JsonDoc::parse("[1]").value();
    const Result<ComponentBuild> build = OutingGeometry::buildStatic(in, CancelToken{});
    REQUIRE(build.ok());
    CHECK(OutingGeometry::zoomRect(in).w == 0.0F);
    CHECK(OutingGeometry::buildDynamic(in).cmds.empty());
}

TEST_CASE("OutingGeometry: DrawList JSON goldens at DPR 1 and 2", "[unit]") {
    const test::Fixture fx;
    test::warmAll(fx.a);
    test::warmAll(fx.b);
    for (const OutingCase& oc : outingCases(fx)) {
        for (const float dpr : {1.0F, 2.0F}) {
            const ComponentBuildInputs in = outingInputs(fx, oc, dpr);
            const Result<ComponentBuild> build = OutingGeometry::buildStatic(in, CancelToken{});
            REQUIRE(build.ok());
            test::checkGolden("outing", oc.name, dpr,
                              test::layersJson(oc.name, dpr, build->list, OutingGeometry::buildDynamic(in),
                                               OutingGeometry::buildChrome(in, oc.gc.pending)));
        }
    }
}

// ---- TrackGeometry

TEST_CASE("TrackGeometry: fit centres the map, manual scale zooms and pans, rotation turns it", "[unit]") {
    const auto map = octagon();
    const RectPx area{10.0F, 30.0F, 300.0F, 200.0F};
    TrackPositionBody body;
    const TrackFit fit = TrackGeometry::fit(*map, area, body);
    // Bounding box [-100, 400] x [0, 200]: 500 x 200 m into 300 x 200 px -> 0.6 px/m, centred.
    CHECK(fit.scale == 0.6);
    const PointPx c = fit.toPx(150.0, 100.0);
    CHECK(c.x == 160.0F);
    CHECK(c.y == 130.0F);
    const PointPx tl = fit.toPx(-100.0, 200.0); // north-west corner: top left
    CHECK(tl.x == 10.0F);
    CHECK(tl.y == 70.0F);
    body.scaleMode = "manual";
    body.zoom = 2.0;
    body.panX = 10.0;
    body.panY = 5.0;
    const TrackFit manual = TrackGeometry::fit(*map, area, body);
    CHECK(manual.scale == 1.2);
    const PointPx m = manual.toPx(150.0, 100.0);
    CHECK(std::abs(m.x - (160.0F + 12.0F)) < 1e-4F);
    CHECK(std::abs(m.y - (130.0F - 6.0F)) < 1e-4F);
    body = TrackPositionBody{};
    body.rotationDeg = 90.0;
    const TrackFit turned = TrackGeometry::fit(*map, area, body);
    CHECK(std::abs(turned.scale - 0.4) < 1e-9);     // 200 x 500 m into 300 x 200 px
    const PointPx east = turned.toPx(400.0, 100.0); // east end points up after a quarter turn
    CHECK(east.y < turned.toPx(-100.0, 100.0).y);
}

TEST_CASE("TrackGeometry: trackDistanceAt measures forward from a beacon and backwards on an out lap",
          "[unit]") {
    const auto map = octagon();
    const double len = map->lengthM;
    const TrackSource fwd = trackSource(len, false, 1.0);
    const SlotData s = trackSlot(fwd, 1, SlotKind::Main, 0);
    auto d = TrackGeometry::trackDistanceAt(s, *map, 25 * kSec);
    REQUIRE(d.has_value());
    CHECK(std::abs(*d - len / 4.0) < 1e-6);
    // Another lap through the same chain, and the outing end clamps into the last lap.
    d = TrackGeometry::trackDistanceAt(s, *map, 50 * kSec);
    REQUIRE(d.has_value());
    CHECK(std::abs(*d - len / 2.0) < 1e-6);
    // A short out lap (0.8 x the track) without a beacon at its start is measured back from its end beacon.
    const TrackSource out = trackSource(len, true, 0.8);
    const SlotData o = trackSlot(out, 0, SlotKind::Main, 0);
    d = TrackGeometry::trackDistanceAt(o, *map, 10 * kSec); // half the lap: 0.4 L travelled of 0.8 L
    REQUIRE(d.has_value());
    CHECK(std::abs(*d - (len - 0.4 * len)) < 1e-6);
    d = TrackGeometry::trackDistanceAt(o, *map, 30 * kSec); // lap 1 starts at a beacon: forward
    REQUIRE(d.has_value());
    CHECK(std::abs(*d - 0.4 * len) < 1e-6);
    // No chain, or a lap without an index: nullopt.
    SlotData none = s;
    none.chain = nullptr;
    CHECK_FALSE(TrackGeometry::trackDistanceAt(none, *map, 25 * kSec).has_value());
    std::vector<LapPiece> pieces = fwd.pieces;
    pieces[1].lookup = nullptr;
    none.chain = std::make_shared<const DistanceChain>(pieces, pieces[0].start);
    CHECK_FALSE(TrackGeometry::trackDistanceAt(none, *map, 25 * kSec).has_value());
}

TEST_CASE("TrackGeometry: sections, dots, the zoom span and the banners", "[unit]") {
    const auto map = octagon();
    const TrackSource a = trackSource(map->lengthM, false, 1.0);
    const TrackSource b = trackSource(map->lengthM, false, 0.9);
    const TrackCase tc = trackCases().front();
    ComponentBuildInputs in = trackInputs(tc, a, b, 1.0F);
    const Result<ComponentBuild> build = TrackGeometry::buildStatic(in, CancelToken{});
    REQUIRE(build.ok());
    // Turn 1 starts at 280 m on the first straight (300 m long): (280, 0) in map metres.
    const TrackFit fit = TrackGeometry::fit(*map, RectPx{6.0F, 24.0F, 308.0F, 210.0F}, TrackPositionBody{});
    const PointPx t1 = fit.toPx(290.0, 0.0);
    const HitResult hit = build->hits.hit(t1.x, t1.y);
    CHECK(hit.kind == HitKind::Section);
    CHECK(hit.section == 0);
    CHECK(hasText(build->list, "Turn 1"));
    CHECK_FALSE(hasText(build->list, "Braking")); // another section set is not drawn
    // Dynamic: the zoom span (one wide polyline) and two dots (overlay beneath, Main last in red).
    const DrawList dyn = TrackGeometry::buildDynamic(in);
    REQUIRE(countKind(dyn, CmdKind::Points) == 2);
    CHECK(countKind(dyn, CmdKind::Polyline) == 1);
    CHECK(dyn.cmds.back().colour == test::lightScheme().role("cursorMain"));
    // Main at the cursor (6 s into lap 2 of 20 s): 0.3 of the track along the centreline.
    const DrawCmd& mainDot = dyn.cmds.back();
    const float mx = dyn.verts[static_cast<std::size_t>(mainDot.vertexOffset) * 2];
    const float my = dyn.verts[static_cast<std::size_t>(mainDot.vertexOffset) * 2 + 1];
    const auto d = TrackGeometry::trackDistanceAt(in.slots[0], *map, in.slots[0].range.start + tc.cursorT);
    REQUIRE(d.has_value());
    CHECK(std::abs(*d - 0.3 * map->lengthM) < 1e-6);
    CHECK(std::isfinite(mx));
    CHECK(std::isfinite(my));
    // Without a map: no dots, the banner.
    in.trackMap = nullptr;
    CHECK(TrackGeometry::buildDynamic(in).cmds.empty());
    CHECK(hasText(TrackGeometry::buildChrome(in, false), "No track map"));
    // Without distance data on Main: the banner and no dots.
    in.trackMap = map;
    in.slots[0].chain = nullptr;
    CHECK(hasText(TrackGeometry::buildChrome(in, false), "No distance data"));
    CHECK(countKind(TrackGeometry::buildDynamic(in), CmdKind::Points) == 1);
}

TEST_CASE("TrackGeometry: DrawList JSON goldens at DPR 1 and 2", "[unit]") {
    const auto map = octagon();
    const TrackSource a = trackSource(map->lengthM, false, 1.0);
    const TrackSource b = trackSource(map->lengthM, false, 0.9);
    for (const TrackCase& tc : trackCases()) {
        for (const float dpr : {1.0F, 2.0F}) {
            const ComponentBuildInputs in = trackInputs(tc, a, b, dpr);
            const Result<ComponentBuild> build = TrackGeometry::buildStatic(in, CancelToken{});
            REQUIRE(build.ok());
            test::checkGolden("track_position", tc.name, dpr,
                              test::layersJson(tc.name, dpr, build->list, TrackGeometry::buildDynamic(in),
                                               TrackGeometry::buildChrome(in, false)));
        }
    }
}
