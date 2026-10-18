// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// GaugeNumericBody, GaugeBarBody and GaugeGeometry (04 sections 16-17, D-198): the body round trips of the
// schema examples, errors with JSON pointers, the channel each gauge lists, the values at the cursor for Main
// and the reference overlay (expected values read from the synthetic series here and formatted with
// std::to_chars, not through the builder), colour bands (text, background, bar, lines, fill; ALT+n
// visibility), the dynamic font scale, the bar geometry for every origin style and both orientations, and the
// DrawList JSON goldens of tests/golden/drawlist/gauge_numeric and gauge_bar at DPR 1 and 2.

#include "m3_golden_support.hpp"
#include "plots_golden_cases.hpp"

#include <esses/plotcore/component_bodies.hpp>
#include <esses/plotcore/gauge_geometry.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::plot;

namespace {

constexpr Micros kSec = test::kSec;
constexpr Micros kPeriod = 50'000; // the 20 Hz channels of the synthetic sources

JsonDoc doc(const std::string& text) {
    return JsonDoc::parse(text).value();
}

JsonDoc example(const std::string& type, const std::string& kind, const std::string& file) {
    const std::filesystem::path p = std::filesystem::path(ESSES_TEST_REPO_ROOT) / "schemas" / "examples" /
                                    "components" / (type + "-1") / kind / file;
    Result<JsonDoc> d = JsonDoc::load(p);
    REQUIRE(d.ok());
    return std::move(*d);
}

std::string fixed(double v, int dps) {
    std::array<char, 64> buf{};
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::fixed, dps);
    return {buf.data(), r.ptr};
}

struct GaugeCase {
    std::string name;
    std::string type; // gauge_numeric | gauge_bar
    std::string body;
    bool dark = false;
    bool caption = true;
    bool focused = false;
    bool pending = false;
    int width = 240;
    int height = 140;
    Micros cursorT = 20 * kSec;
    std::vector<test::GoldenCase::SlotSpec> slots{{'a', 0, SlotKind::Main, 0}};
};

std::string channelOfBody(const GaugeCase& c) {
    if (c.type == "gauge_bar") {
        return GaugeBarBody::parse(doc(c.body)).value().channel;
    }
    return GaugeNumericBody::parse(doc(c.body)).value().channel;
}

ComponentBuildInputs gaugeInputs(const test::Fixture& fx, const GaugeCase& c, float dpr) {
    ComponentBuildInputs in;
    in.spec.type = c.type;
    in.spec.caption.show = c.caption;
    in.spec.caption.autoText = true;
    in.spec.body = doc(c.body);
    in.view.domain = Domain::Time;
    in.view.zoom = {0.0, 60.0e6};
    in.view.cursorT = c.cursorT;
    in.view.version = 1;
    const std::vector<std::string> names{channelOfBody(c)};
    for (const test::GoldenCase::SlotSpec& s : c.slots) {
        in.slots.push_back(test::slotOf(test::sourceOf(fx, s.source), s.lap, s.kind, s.number, names));
        const model::RangeRef& ref = in.slots.back().slot.range;
        if (s.kind == SlotKind::Main) {
            in.selection.main = ref;
        } else if (s.kind == SlotKind::Overlay0) {
            in.selection.overlay0 = ref;
        } else {
            in.selection.overlays.push_back(ref);
        }
    }
    in.scheme = std::make_shared<const project::Scheme>(c.dark ? test::darkScheme() : test::lightScheme());
    in.metrics = std::make_shared<const FixedTextMetrics>();
    in.layout.widthPx = static_cast<int>(std::lround(static_cast<float>(c.width) * dpr));
    in.layout.heightPx = static_cast<int>(std::lround(static_cast<float>(c.height) * dpr));
    in.layout.dpr = dpr;
    in.focused = c.focused;
    return in;
}

// The display value of a 20 Hz channel of a synthetic source at sample i.
double sampleOf(const test::SyntheticSource& src, const std::string& channel, SampleIdx i) {
    const test::SyntheticChannel& ch = src.channels.at(channel);
    return ch.toDisplay(store::seriesValues(ch.series).at(i));
}

struct Built {
    DrawList staticList;
    HitTester hits;
    DrawList dynamicList;
    DrawList chromeList;
};

Built build(const ComponentBuildInputs& in, bool pending = false) {
    Built b;
    const bool numeric = in.spec.type == "gauge_numeric";
    Result<ComponentBuild> s = numeric ? GaugeGeometry::buildNumericStatic(in, CancelToken{})
                                       : GaugeGeometry::buildBarStatic(in, CancelToken{});
    REQUIRE(s.ok());
    b.staticList = std::move(s->list);
    b.hits = std::move(s->hits);
    b.dynamicList = numeric ? GaugeGeometry::buildNumericDynamic(in) : GaugeGeometry::buildBarDynamic(in);
    b.chromeList =
        numeric ? GaugeGeometry::buildNumericChrome(in, pending) : GaugeGeometry::buildBarChrome(in, pending);
    return b;
}

std::vector<const TextRun*> runsOf(const DrawList& list, TextStyleId style) {
    std::vector<const TextRun*> out;
    for (const TextRun& r : list.texts) {
        if (r.style == style) {
            out.push_back(&r);
        }
    }
    return out;
}

std::vector<RectPx> rectsOf(const DrawList& list, Rgba colour) {
    std::vector<RectPx> out;
    for (const DrawCmd& c : list.cmds) {
        if (c.kind == CmdKind::FilledRect && c.colour == colour) {
            out.push_back(c.rect);
        }
    }
    return out;
}

// The track of a bar gauge: the Static FilledRect in the keyBackground role.
RectPx trackOf(const DrawList& staticList, const project::Scheme& scheme) {
    const std::vector<RectPx> r = rectsOf(staticList, scheme.role("keyBackground", 0));
    REQUIRE(r.size() == 1);
    return r.front();
}

// The gauge cases of the shared golden cases (plots_golden_cases.hpp), by name.
test::PlotGoldenCase caseNamed(const std::vector<test::PlotGoldenCase>& cases, const std::string& name) {
    const auto it = std::find_if(cases.begin(), cases.end(),
                                 [&](const test::PlotGoldenCase& c) { return c.name == name; });
    REQUIRE(it != cases.end());
    return *it;
}

} // namespace

TEST_CASE("GaugeNumericBody: examples round trip, members typed, defaults omitted, errors with pointers",
          "[unit]") {
    for (const char* file : {"empty.json", "full.json"}) {
        const JsonDoc body = example("gauge_numeric", "valid", file);
        const Result<GaugeNumericBody> parsed = GaugeNumericBody::parse(body);
        REQUIRE(parsed.ok());
        CHECK(parsed->toJson() == body);
    }
    const Result<GaugeNumericBody> full =
        GaugeNumericBody::parse(example("gauge_numeric", "valid", "full.json"));
    REQUIRE(full.ok());
    CHECK(full->channel == "Engine RPM");
    CHECK(full->displayDps == 0);
    REQUIRE(full->colourBands.size() == 1);
    CHECK(full->colourBands[0].open == BandOpen::Above);
    CHECK(full->colourBands[0].colour == 0xFF3030FFU);

    GaugeNumericBody fresh;
    CHECK(fresh.toJson() == doc("{}"));
    fresh.channel = "Engine RPM";
    fresh.displayDps = 1;
    fresh.bandMode = "background";
    CHECK(fresh.toJson() == doc(R"({"bandMode":"background","channel":"Engine RPM","displayDps":1})"));
    fresh.displayDps.reset();
    CHECK(fresh.toJson() == doc(R"({"bandMode":"background","channel":"Engine RPM"})"));

    Result<GaugeNumericBody> withFont =
        GaugeNumericBody::parse(doc(R"({"font":{"family":"Noto Sans","pointSize":24},"x-extra":true})"));
    REQUIRE(withFont.ok());
    CHECK(withFont->font.isObject());
    withFont->font = JsonDoc{};
    CHECK(withFont->toJson() == doc(R"({"font":null,"x-extra":true})"));

    for (const auto& [file, pointer] : std::vector<std::pair<std::string, std::string>>{
             {"bad-band-mode.json", "/bandMode"}, {"band-open-sideways.json", "/colourBands/0/open"}}) {
        const Result<GaugeNumericBody> bad =
            GaugeNumericBody::parse(example("gauge_numeric", "invalid", file));
        REQUIRE_FALSE(bad.ok());
        CHECK(bad.error().code == ErrorCode::Invalid);
        CHECK(bad.error().context.detail == pointer);
    }
    const Result<GaugeNumericBody> badFont =
        GaugeNumericBody::parse(doc(R"({"font":{"family":"A","pointSize":2}})"));
    REQUIRE_FALSE(badFont.ok());
    CHECK(badFont.error().context.detail == "/font/pointSize");
    const Result<GaugeNumericBody> badDps = GaugeNumericBody::parse(doc(R"({"displayDps":16})"));
    REQUIRE_FALSE(badDps.ok());
    CHECK(badDps.error().context.detail == "/displayDps");
}

TEST_CASE("GaugeBarBody: examples round trip, members typed, defaults omitted, errors with pointers",
          "[unit]") {
    for (const char* file : {"empty.json", "full.json"}) {
        const JsonDoc body = example("gauge_bar", "valid", file);
        const Result<GaugeBarBody> parsed = GaugeBarBody::parse(body);
        REQUIRE(parsed.ok());
        CHECK(parsed->toJson() == body);
    }
    const Result<GaugeBarBody> full = GaugeBarBody::parse(example("gauge_bar", "valid", "full.json"));
    REQUIRE(full.ok());
    CHECK(full->scale.mode == ScaleMode::Manual);
    const std::optional<analysis::UnitValue>& max = full->scale.max;
    REQUIRE(max.has_value());
    CHECK(max.value_or(analysis::UnitValue{}).v == 100.0);
    CHECK_FALSE(full->barColour.has_value());
    REQUIRE(full->customOrigin);

    GaugeBarBody b;
    CHECK(b.toJson() == doc("{}"));
    b.orientation = "horizontal";
    b.barColour = 0x7E57C2FFU;
    b.customOrigin = analysis::UnitValue{5.0, "bar"};
    CHECK(b.toJson() ==
          doc(R"({"barColour":"#7e57c2","customOrigin":{"unit":"bar","v":5},"orientation":"horizontal"})"));
    Result<GaugeBarBody> again = GaugeBarBody::parse(b.toJson());
    REQUIRE(again.ok());
    again->barColour.reset();
    again->customOrigin.reset();
    CHECK(again->toJson() == doc(R"({"barColour":null,"orientation":"horizontal"})"));

    for (const auto& [file, pointer] : std::vector<std::pair<std::string, std::string>>{
             {"bad-origin.json", "/originStyle"}, {"bar-colour-name.json", "/barColour"}}) {
        const Result<GaugeBarBody> bad = GaugeBarBody::parse(example("gauge_bar", "invalid", file));
        REQUIRE_FALSE(bad.ok());
        CHECK(bad.error().code == ErrorCode::Invalid);
        CHECK(bad.error().context.detail == pointer);
    }
    const Result<GaugeBarBody> badScale = GaugeBarBody::parse(doc(R"({"scale":{"mode":"log"}})"));
    REQUIRE_FALSE(badScale.ok());
    CHECK(badScale.error().context.detail == "/scale/mode");
}

TEST_CASE("GaugeGeometry: each gauge lists its one channel and fails its Static build on a bad body",
          "[unit]") {
    project::ComponentSpec n;
    n.type = "gauge_numeric";
    n.body = doc(R"({"channel":"Engine RPM","displayUnit":"rpm","displayDps":0})");
    CHECK(componentChannelUses(n) == std::vector<ChannelUse>{ChannelUse{"Engine RPM", "rpm", 0}});
    project::ComponentSpec b;
    b.type = "gauge_bar";
    b.body = doc(R"({"channel":"Brake Pres","displayUnit":"bar"})");
    CHECK(componentChannelUses(b) == std::vector<ChannelUse>{ChannelUse{"Brake Pres", "bar", std::nullopt}});

    const test::Fixture fx;
    GaugeCase c;
    c.type = "gauge_bar";
    c.body = R"({"channel":"Brake Pres"})";
    ComponentBuildInputs in = gaugeInputs(fx, c, 1.0F);
    in.spec.body = doc(R"({"orientation":"diagonal"})");
    const Result<ComponentBuild> bad = GaugeGeometry::buildBarStatic(in, CancelToken{});
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == ErrorCode::Invalid);
    in.spec.type = "gauge_numeric";
    in.spec.body = doc(R"({"bandMode":"bar"})");
    CHECK_FALSE(GaugeGeometry::buildNumericStatic(in, CancelToken{}).ok());
    in.spec.body = doc(R"({"channel":"Brake Pres"})");
    CancelSource cancel;
    cancel.cancel();
    const Result<ComponentBuild> c1 = GaugeGeometry::buildNumericStatic(in, cancel.token());
    REQUIRE_FALSE(c1.ok());
    CHECK(c1.error().code == ErrorCode::Cancelled);
}

TEST_CASE("GaugeGeometry numeric: the value at the cursor, its unit, the reference overlay in its key colour",
          "[unit]") {
    const test::Fixture fx;
    test::warmAll(fx.a);
    test::warmAll(fx.b);
    GaugeCase c;
    c.type = "gauge_numeric";
    c.body = R"({"channel":"Corr Speed","displayUnit":"km/h","displayDps":2})";
    c.cursorT = 20 * kSec;
    c.slots = {{'a', 0, SlotKind::Main, 0}, {'b', 0, SlotKind::Overlay0, 0}};
    const ComponentBuildInputs in = gaugeInputs(fx, c, 1.0F);
    const Built b = build(in);
    const std::vector<const TextRun*> values = runsOf(b.dynamicList, TextStyleId::Gauge);
    REQUIRE(values.size() == 2);
    const double mainV = sampleOf(fx.a, "Corr Speed", 20 * kSec / kPeriod);
    const double refV = sampleOf(fx.b, "Corr Speed", 20 * kSec / kPeriod); // aligned at lap start
    CHECK(b.dynamicList.textOf(*values[0]) == fixed(mainV, 2));
    CHECK(b.dynamicList.textOf(*values[1]) == fixed(refV, 2));
    CHECK(values[0]->colour == in.scheme->role("cptForeground", 0));
    CHECK(values[1]->colour == in.scheme->role("overlay0", 0)); // the reference overlay's key colour
    CHECK(values[0]->y < values[1]->y);                         // the reference beneath
    CHECK(values[1]->scale <= 0.5F * values[0]->scale + 1e-6F);
    const std::vector<const TextRun*> units = runsOf(b.dynamicList, TextStyleId::GaugeUnits);
    REQUIRE(units.size() == 2);
    CHECK(b.dynamicList.textOf(*units[0]) == "km/h");
    // The Static layer holds the frame and the Panel hit; the caption is Chrome (auto text = the channel).
    CHECK(runsOf(b.staticList, TextStyleId::Gauge).empty());
    CHECK(
        b.hits
            .hit(static_cast<float>(in.layout.widthPx) / 2.0F, static_cast<float>(in.layout.heightPx) / 2.0F)
            .kind == HitKind::Panel);
    const std::vector<const TextRun*> caption = runsOf(b.chromeList, TextStyleId::Caption);
    REQUIRE(caption.size() == 1);
    CHECK(b.chromeList.textOf(*caption[0]) == "Corr Speed");

    // displayDps absent: the channel's decimals; showUnits off: no unit.
    GaugeCase d = c;
    d.body = R"({"channel":"Corr Speed","showUnits":false})";
    d.slots = {{'a', 0, SlotKind::Main, 0}};
    const Built e = build(gaugeInputs(fx, d, 1.0F));
    const std::vector<const TextRun*> one = runsOf(e.dynamicList, TextStyleId::Gauge);
    REQUIRE(one.size() == 1);
    CHECK(e.dynamicList.textOf(*one[0]) == fixed(mainV, 1));
    CHECK(runsOf(e.dynamicList, TextStyleId::GaugeUnits).empty());
}

TEST_CASE("GaugeGeometry numeric: missing, pending and invalid values", "[unit]") {
    const test::Fixture fx;
    test::warmAll(fx.a);
    GaugeCase c;
    c.type = "gauge_numeric";
    c.body = R"({"channel":"No Such Channel"})";
    ComponentBuildInputs in = gaugeInputs(fx, c, 1.0F);
    DrawList d = GaugeGeometry::buildNumericDynamic(in);
    std::vector<const TextRun*> v = runsOf(d, TextStyleId::Gauge);
    REQUIRE(v.size() == 1);
    CHECK(d.textOf(*v[0]) == "(missing)");

    c.body = R"({"channel":"Corr Speed"})";
    in = gaugeInputs(fx, c, 1.0F);
    in.slots[0].channels[0].status = model::ChannelStatus::Pending;
    in.slots[0].channels[0].series.reset();
    d = GaugeGeometry::buildNumericDynamic(in);
    v = runsOf(d, TextStyleId::Gauge);
    REQUIRE(v.size() == 1);
    CHECK(d.textOf(*v[0]) == "\xE2\x80\xA6");
    CHECK(runsOf(d, TextStyleId::GaugeUnits).empty());
    // The pending stripe in Chrome.
    const DrawList chrome = GaugeGeometry::buildNumericChrome(in, false);
    CHECK_FALSE(rectsOf(chrome, in.scheme->role("pendingHatch", 0)).empty());

    // Before the first sample of the outing the value is invalid.
    in = gaugeInputs(fx, c, 1.0F);
    in.view.cursorT = -5 * kSec;
    d = GaugeGeometry::buildNumericDynamic(in);
    v = runsOf(d, TextStyleId::Gauge);
    REQUIRE(v.size() == 1);
    CHECK(d.textOf(*v[0]) == "-");
}

TEST_CASE("GaugeGeometry numeric: colour bands colour the text or the background; hidden bands do not count",
          "[unit]") {
    const test::Fixture fx;
    test::warmAll(fx.a);
    GaugeCase c;
    c.type = "gauge_numeric";
    // Two bands in m/s over a km/h display: the converted band [10, 20) m/s = [36, 72) km/h.
    const double v = sampleOf(fx.a, "Corr Speed", 20 * kSec / kPeriod);
    const bool inLow = v >= 36.0 && v < 72.0;
    c.body =
        R"({"channel":"Corr Speed","colourBands":[{"min":{"v":10,"unit":"m/s"},"max":{"v":20,"unit":"m/s"},)"
        R"("colour":"#112233"},{"min":{"v":0,"unit":"km/h"},"max":{"v":0,"unit":"km/h"},"open":"above",)"
        R"("colour":"#445566"}]})";
    ComponentBuildInputs in = gaugeInputs(fx, c, 1.0F);
    Result<units::UnitRegistry> loaded =
        units::UnitRegistry::loadFiles(std::filesystem::path(ESSES_TEST_REPO_ROOT) / "data" / "units.json");
    REQUIRE(loaded.ok());
    in.units = std::make_shared<const units::UnitRegistry>(std::move(*loaded));
    DrawList d = GaugeGeometry::buildNumericDynamic(in);
    std::vector<const TextRun*> runs = runsOf(d, TextStyleId::Gauge);
    REQUIRE(runs.size() == 1);
    CHECK(runs[0]->colour == (inLow ? 0x112233FFU : 0x445566FFU));

    // Band 0 hidden (ALT+0): the open band above takes every value.
    Result<GaugeNumericBody> body = GaugeNumericBody::parse(in.spec.body);
    REQUIRE(body.ok());
    body->colourBands[0].visible = false;
    in.spec.body = body->toJson();
    d = GaugeGeometry::buildNumericDynamic(in);
    runs = runsOf(d, TextStyleId::Gauge);
    REQUIRE(runs.size() == 1);
    CHECK(runs[0]->colour == 0x445566FFU);

    // bandMode background: the content is filled, the text keeps the foreground.
    body->bandMode = "background";
    in.spec.body = body->toJson();
    d = GaugeGeometry::buildNumericDynamic(in);
    CHECK(rectsOf(d, 0x445566FFU).size() == 1);
    runs = runsOf(d, TextStyleId::Gauge);
    REQUIRE(runs.size() == 1);
    CHECK(runs[0]->colour == in.scheme->role("cptForeground", 0));
}

TEST_CASE("GaugeGeometry numeric: dynamicFont fits the widest value, never below 6 pt; a fixed font scales",
          "[unit]") {
    const test::Fixture fx;
    test::warmAll(fx.a);
    const FixedTextMetrics metrics;
    GaugeCase c;
    c.type = "gauge_numeric";
    c.body = R"({"channel":"Corr Speed","showUnits":false})";
    for (const auto& [w, h] : std::vector<std::pair<int, int>>{{400, 200}, {120, 300}, {60, 40}}) {
        c.width = w;
        c.height = h;
        const ComponentBuildInputs in = gaugeInputs(fx, c, 1.0F);
        const DrawList d = GaugeGeometry::buildNumericDynamic(in);
        const std::vector<const TextRun*> runs = runsOf(d, TextStyleId::Gauge);
        REQUIRE(runs.size() == 1);
        const float scale = runs[0]->scale;
        const double basePt = in.scheme->font("gauge").pointSize;
        CHECK(static_cast<double>(scale) >= 6.0 / basePt - 1e-6);
        // The widest value ("888.8": the summary maximum of Corr Speed is three digits in km/h) fits the
        // content width or height unless the 6 pt floor wins.
        const float contentW = static_cast<float>(w) - 8.0F - 4.0F;
        const float contentH = static_cast<float>(h) - 18.0F - 8.0F - 4.0F;
        const float valueW = metrics.advancePx(TextStyleId::Gauge, "888.8", 1.0F);
        const float lineH = metrics.lineHeightPx(TextStyleId::Gauge, 1.0F);
        const float fit = std::min(contentW / valueW, contentH / lineH);
        CHECK(scale == Catch::Approx(std::max(fit, static_cast<float>(6.0 / basePt))).margin(1e-4));
    }
    c.width = 240;
    c.height = 140;
    c.body = R"({"channel":"Corr Speed","dynamicFont":false})";
    ComponentBuildInputs in = gaugeInputs(fx, c, 2.0F);
    DrawList d = GaugeGeometry::buildNumericDynamic(in);
    std::vector<const TextRun*> runs = runsOf(d, TextStyleId::Gauge);
    REQUIRE(runs.size() == 1);
    CHECK(runs[0]->scale == 1.0F);
    c.body = R"({"channel":"Corr Speed","dynamicFont":false,"font":{"family":"Noto Sans","pointSize":18}})";
    in = gaugeInputs(fx, c, 2.0F);
    d = GaugeGeometry::buildNumericDynamic(in);
    runs = runsOf(d, TextStyleId::Gauge);
    REQUIRE(runs.size() == 1);
    CHECK(runs[0]->scale == Catch::Approx(18.0 / in.scheme->font("gauge").pointSize).epsilon(1e-6));
    CHECK(d.toJson().find("\"scale\":") != std::string::npos);
}

TEST_CASE("GaugeGeometry bar: the bar spans the origin to the value along the track", "[unit]") {
    const test::Fixture fx;
    test::warmAll(fx.a);
    const double v = sampleOf(fx.a, "Brake Pres", 9 * kSec / kPeriod);
    REQUIRE(v > 0.0);
    REQUIRE(v < 60.0);
    struct Origin {
        const char* style;
        double origin;
    };
    for (const bool vertical : {true, false}) {
        for (const Origin o :
             {Origin{"min", 0.0}, Origin{"max", 60.0}, Origin{"zero", 0.0}, Origin{"custom", 25.0}}) {
            GaugeCase c;
            c.type = "gauge_bar";
            c.width = vertical ? 140 : 300;
            c.height = vertical ? 260 : 100;
            c.cursorT = 9 * kSec;
            c.body = std::string(R"({"channel":"Brake Pres","orientation":")") +
                     (vertical ? "vertical" : "horizontal") + R"(","originStyle":")" + o.style +
                     R"(","customOrigin":{"v":25,"unit":"bar"},"barColour":"#123456",)"
                     R"("scale":{"mode":"manual","min":{"v":0,"unit":"bar"},"max":{"v":60,"unit":"bar"}}})";
            const ComponentBuildInputs in = gaugeInputs(fx, c, 1.0F);
            const Built b = build(in);
            const RectPx track = trackOf(b.staticList, *in.scheme);
            const std::vector<RectPx> bars = rectsOf(b.dynamicList, 0x123456FFU);
            REQUIRE(bars.size() == 1);
            const RectPx& bar = bars.front();
            const auto at = [&](double value) {
                const double f = value / 60.0;
                return vertical ? track.y + static_cast<float>((1.0 - f) * static_cast<double>(track.h))
                                : track.x + static_cast<float>(f * static_cast<double>(track.w));
            };
            const float p0 = at(o.origin);
            const float p1 = at(v);
            if (vertical) {
                CHECK(bar.x == track.x);
                CHECK(bar.w == track.w);
                CHECK(bar.y == Catch::Approx(std::min(p0, p1)).margin(1e-3));
                CHECK(bar.h == Catch::Approx(std::abs(p1 - p0)).margin(1e-3));
            } else {
                CHECK(bar.y == track.y);
                CHECK(bar.h == track.h);
                CHECK(bar.x == Catch::Approx(std::min(p0, p1)).margin(1e-3));
                CHECK(bar.w == Catch::Approx(std::abs(p1 - p0)).margin(1e-3));
            }
            // The value text at the bar end.
            const std::vector<const TextRun*> text = runsOf(b.dynamicList, TextStyleId::KeyValue);
            REQUIRE(text.size() == 1);
            CHECK(b.dynamicList.textOf(*text[0]) == fixed(v, 1));
        }
    }
}

TEST_CASE("GaugeGeometry bar: reference marker, band lines and fill, bandMode bar, scale labels", "[unit]") {
    const test::Fixture fx;
    test::warmAll(fx.a);
    test::warmAll(fx.b);
    const std::vector<test::PlotGoldenCase> cases = test::plotGoldenCases(fx);
    {
        const test::PlotGoldenCase c = caseNamed(cases, "bar_vertical_manual");
        const ComponentBuildInputs in = test::plotGoldenInputs(fx, c, 1.0F);
        const Built b = build(in);
        const RectPx track = trackOf(b.staticList, *in.scheme);
        // The band [40, +inf) has one finite edge: a line across the track at 40 bar.
        const float y40 = std::round(track.y + track.h * (1.0F - 40.0F / 60.0F));
        bool line = false;
        for (const DrawCmd& cmd : b.staticList.cmds) {
            if (cmd.kind == CmdKind::Lines && cmd.colour == 0xE53935FFU) {
                REQUIRE(cmd.vertexCount == 2);
                CHECK(b.staticList.verts[2 * cmd.vertexOffset + 1] == Catch::Approx(y40));
                line = true;
            }
        }
        CHECK(line);
        // Tick labels with scaleDps (0) decimals; the extremes are labelled.
        std::vector<std::string> labels;
        for (const TextRun* r : runsOf(b.staticList, TextStyleId::Axis)) {
            labels.emplace_back(b.staticList.textOf(*r));
        }
        CHECK(std::find(labels.begin(), labels.end(), "0") != labels.end());
        CHECK(std::find(labels.begin(), labels.end(), "60") != labels.end());
        // The reference overlay's marker in the Overlay 0 colour at its value (lap 3 of source b, aligned at
        // its start).
        const double ref = sampleOf(fx.b, "Brake Pres", (fx.b.laps[2].start + 9 * kSec) / kPeriod);
        bool marker = false;
        for (const DrawCmd& cmd : b.dynamicList.cmds) {
            if (cmd.kind == CmdKind::Lines && cmd.colour == in.scheme->role("overlay0", 0)) {
                const float y = std::round(
                    track.y + track.h * static_cast<float>(1.0 - std::clamp(ref, 0.0, 60.0) / 60.0));
                CHECK(b.dynamicList.verts[2 * cmd.vertexOffset + 1] == Catch::Approx(y));
                marker = true;
            }
        }
        CHECK(marker);
    }
    {
        const test::PlotGoldenCase c = caseNamed(cases, "bar_horizontal_zero_fill");
        const ComponentBuildInputs in = test::plotGoldenInputs(fx, c, 1.0F);
        const Built b = build(in);
        const RectPx track = trackOf(b.staticList, *in.scheme);
        // Fill: band strips beside (under) the track over [0, 1) V and [1, 2] V.
        const std::vector<RectPx> blue = rectsOf(b.staticList, 0x1E88E5FFU);
        REQUIRE(blue.size() == 1);
        CHECK(blue[0].x == Catch::Approx(track.x + track.w / 3.0F).margin(1e-3));
        CHECK(blue[0].w == Catch::Approx(track.w / 3.0F).margin(1e-3));
        CHECK(blue[0].y > track.y + track.h);
        // bandMode bar: the bar takes the colour of the band holding the value.
        const double v = sampleOf(fx.a, "Ch 03", SampleIdx{130}); // 13 s of a 10 Hz channel
        const Rgba expected = v >= 1.0 ? 0xE53935FFU : 0x1E88E5FFU;
        CHECK(rectsOf(b.dynamicList, expected).size() == 1);
        // scaleDps 1.
        bool oneDecimal = false;
        for (const TextRun* r : runsOf(b.staticList, TextStyleId::Axis)) {
            oneDecimal = oneDecimal || b.staticList.textOf(*r) == "0.0";
        }
        CHECK(oneDecimal);
    }
    {
        const test::PlotGoldenCase c = caseNamed(cases, "bar_custom_origin_dark");
        const Built b = build(test::plotGoldenInputs(fx, c, 1.0F));
        CHECK(runsOf(b.staticList, TextStyleId::Axis).empty()); // showScale off
        CHECK(rectsOf(b.dynamicList, 0x7E57C2FFU).size() == 1);
    }
}

TEST_CASE("GaugeGeometry: DrawList JSON goldens at DPR 1 and 2", "[unit]") {
    const test::Fixture fx;
    test::warmAll(fx.a);
    test::warmAll(fx.b);
    int n = 0;
    for (const test::PlotGoldenCase& c : test::plotGoldenCases(fx)) {
        if (c.type != "gauge_numeric" && c.type != "gauge_bar") {
            continue;
        }
        ++n;
        for (const float dpr : {1.0F, 2.0F}) {
            const ComponentBuildInputs in = test::plotGoldenInputs(fx, c, dpr);
            const Built b = build(in, c.pending);
            test::checkGolden(c.type, c.name, dpr,
                              test::layersJson(c.name, dpr, b.staticList, b.dynamicList, b.chromeList));
        }
    }
    CHECK(n == 6);
}
