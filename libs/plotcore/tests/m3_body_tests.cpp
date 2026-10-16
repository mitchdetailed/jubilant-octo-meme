// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The M3 component bodies of esses_plotcore (04 sections 9, 10.2, 11.2, 14 item 6): parse(x).toJson() == x
// for the valid examples of schemas/examples/components (the AC-M3-08 round trip at the body level), errors
// with JSON pointers, defaults written only when they change, unknown members kept, and the channel lists the
// builders receive (componentChannelUses order). Bodies with a BandAxis (histogram bins, colour axes) need
// analysis::BandAxis::fromJson; those cases skip with a warning while it is an M3 stub.

#include "m3_golden_support.hpp"

#include <esses/plotcore/histogram_geometry.hpp>
#include <esses/plotcore/outing_geometry.hpp>
#include <esses/plotcore/scatter_geometry.hpp>
#include <esses/plotcore/track_geometry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using namespace esses;
using namespace esses::plot;

namespace {

JsonDoc doc(const std::string& text) {
    return JsonDoc::parse(text).value();
}

JsonDoc example(const std::string& type, const std::string& file) {
    const std::filesystem::path p = std::filesystem::path(ESSES_TEST_REPO_ROOT) / "schemas" / "examples" /
                                    "components" / (type + "-1") / "valid" / file;
    Result<JsonDoc> d = JsonDoc::load(p);
    REQUIRE(d.ok());
    return std::move(*d);
}

project::ComponentSpec specOf(const std::string& type, const JsonDoc& body) {
    project::ComponentSpec spec;
    spec.type = type;
    spec.body = body;
    return spec;
}

} // namespace

TEST_CASE("OutingBody: examples round trip, enableOverlays is typed, the graph members are shared",
          "[unit]") {
    for (const char* file : {"empty.json", "full.json"}) {
        const JsonDoc body = example("outing", file);
        const Result<OutingBody> parsed = OutingBody::parse(body);
        REQUIRE(parsed.ok());
        CHECK(parsed->toJson() == body);
    }
    Result<OutingBody> b = OutingBody::parse(doc(R"({"enableOverlays":true,"groups":[{"name":"Speed",)"
                                                 R"("traces":[{"channel":"Corr Speed"}]}],"x-extra":1})"));
    REQUIRE(b.ok());
    CHECK(b->enableOverlays);
    CHECK(b->channelNames() == std::vector<std::string>{"Corr Speed"});
    b->enableOverlays = false;
    CHECK(b->toJson() == doc(R"({"enableOverlays":false,"groups":[{"name":"Speed",)"
                             R"("traces":[{"channel":"Corr Speed"}]}],"x-extra":1})"));
    OutingBody fresh;
    CHECK(fresh.toJson() == doc("{}"));
    fresh.enableOverlays = true;
    CHECK(fresh.toJson() == doc(R"({"enableOverlays":true})"));
    const Result<OutingBody> bad = OutingBody::parse(doc(R"({"enableOverlays":1})"));
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == ErrorCode::Invalid);
    CHECK(bad.error().context.detail == "/enableOverlays");
    const auto uses = componentChannelUses(
        specOf("outing", doc(R"({"groups":[{"traces":[{"channel":"Corr Speed","displayUnit":"km/h"}]}]})")));
    REQUIRE(uses.size() == 1);
    CHECK(uses[0] == ChannelUse{"Corr Speed", "km/h", std::nullopt});
}

TEST_CASE("TrackPositionBody: examples round trip, members typed, defaults omitted", "[unit]") {
    for (const char* file : {"empty.json", "full.json"}) {
        const JsonDoc body = example("track_position", file);
        const Result<TrackPositionBody> parsed = TrackPositionBody::parse(body);
        REQUIRE(parsed.ok());
        CHECK(parsed->toJson() == body);
    }
    Result<TrackPositionBody> b = TrackPositionBody::parse(doc(
        R"({"sizes":{"cursor":9},"scale":{"mode":"manual","zoom":2.5,"panX":-10,"rotation":90},"keep":[1]})"));
    REQUIRE(b.ok());
    CHECK(b->sizeCursor == 9.0F);
    CHECK(b->sizeDetail == 2.0F);
    CHECK(b->scaleMode == "manual");
    CHECK(b->zoom == 2.5);
    CHECK(b->panX == -10.0);
    CHECK(b->rotationDeg == 90.0);
    b->showSectionLabels = true;
    b->sizeDetail = 3.0F;
    CHECK(b->toJson() == doc(R"({"keep":[1],"scale":{"mode":"manual","panX":-10,"rotation":90,"zoom":2.5},)"
                             R"("showSectionLabels":true,"sizes":{"cursor":9,"detail":3}})"));
    CHECK(TrackPositionBody{}.toJson() == doc("{}"));
    Result<TrackPositionBody> bad = TrackPositionBody::parse(doc(R"({"scale":{"mode":"stretch"}})"));
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().context.detail == "/scale/mode");
    bad = TrackPositionBody::parse(doc(R"({"sizes":{"cursor":"big"}})"));
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().context.detail == "/sizes/cursor");
    CHECK_FALSE(TrackPositionBody::parse(doc("[1]")).ok());
    CHECK(componentChannelUses(specOf("track_position", doc(R"({"showZoomSpan":false})"))).empty());
}

TEST_CASE("ScatterBody: members, round trip without a colour axis, channel order", "[unit]") {
    const JsonDoc body = doc(
        R"({"cursorStyle":"ring","density":{"auto":false,"bins":[128,64],"threshold":5000},)"
        R"("gating":{"channel":"Corner Gate"},"intervalAverage":{"binWidth":{"unit":"km/h","v":5},)"
        R"("enabled":true},"layout":"tiled","pointSize":3,"scales":{"x":{"mode":"channel"},)"
        R"("y":{"max":{"unit":"G","v":3},"min":{"unit":"G","v":-3},"mode":"manual"}},"showGrid":false,)"
        R"("style":"pointsTrend","tileCols":2,"traces":[{"visible":true,"yChannel":"G Force Lat",)"
        R"("yDelayUs":-20000},{"visible":false,"yChannel":"G Force Long"}],)"
        R"("trend":{"order":3,"showEquation":false,"significantDigits":6,"throughZero":true,"type":"poly"},)"
        R"("xChannel":"Corr Speed","colourChannel":{"channel":"Throttle Pos"}})");
    const Result<ScatterBody> parsed = ScatterBody::parse(body);
    REQUIRE(parsed.ok());
    const ScatterBody& s = *parsed;
    CHECK(s.xChannel == "Corr Speed");
    REQUIRE(s.traces.size() == 2);
    CHECK(s.traces[0].yDelayUs == -20000);
    CHECK_FALSE(s.traces[1].visible);
    CHECK(s.colourChannel == "Throttle Pos");
    CHECK_FALSE(s.colourAxis.has_value());
    CHECK(s.gatingChannel == "Corner Gate");
    CHECK(s.layout == "tiled");
    CHECK(s.tileCols == 2);
    CHECK(s.style == "pointsTrend");
    CHECK(s.pointSize == 3.0F);
    CHECK(s.trend.type == analysis::TrendType::Polynomial);
    CHECK(s.trend.order == 3);
    CHECK(s.trend.throughZero);
    CHECK_FALSE(s.showEquation);
    CHECK(s.significantDigits == 6);
    CHECK(s.intervalAverage);
    CHECK(s.intervalBinWidth == analysis::UnitValue{5.0, "km/h"});
    CHECK(s.yScale.mode == ScaleMode::Manual);
    CHECK(s.cursorStyle == "ring");
    CHECK_FALSE(s.densityAuto);
    CHECK(s.densityThreshold == 5000U);
    CHECK(s.densityBinsX == 128);
    CHECK(s.densityBinsY == 64);
    CHECK_FALSE(s.showGrid);
    CHECK(s.toJson() == body);
    CHECK(ScatterBody{}.toJson() == doc("{}"));
    const std::vector<ChannelUse> uses = s.channels();
    REQUIRE(uses.size() == 5);
    CHECK(uses[0].name == "Corr Speed");
    CHECK(uses[1].name == "G Force Lat");
    CHECK(uses[2].name == "G Force Long");
    CHECK(uses[3].name == "Throttle Pos");
    CHECK(uses[4].name == "Corner Gate");
    CHECK(componentChannelUses(specOf("scatter", body)).size() == 5);
    Result<ScatterBody> bad = ScatterBody::parse(doc(R"({"style":"dots"})"));
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().context.detail == "/style");
    bad = ScatterBody::parse(doc(R"({"traces":[{"visible":true}]})"));
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().context.detail == "/traces/0"); // a missing required member is reported at its object
    bad = ScatterBody::parse(doc(R"({"density":{"bins":[1]}})"));
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().context.detail == "/density/bins");
}

TEST_CASE("ScatterBody and HistogramBody: the full examples with a BandAxis round trip", "[unit]") {
    if (test::bandAxisIsStub()) {
        WARN("analysis::BandAxis::fromJson is an M3 stub: the BandAxis round trips are skipped");
        return;
    }
    for (const std::string type : {"scatter", "histogram"}) {
        for (const char* file : {"empty.json", "full.json"}) {
            const JsonDoc body = example(type, file);
            if (type == "scatter") {
                const Result<ScatterBody> parsed = ScatterBody::parse(body);
                REQUIRE(parsed.ok());
                CHECK(parsed->toJson() == body);
            } else {
                const Result<HistogramBody> parsed = HistogramBody::parse(body);
                REQUIRE(parsed.ok());
                CHECK(parsed->toJson() == body);
            }
        }
    }
    const Result<HistogramBody> h = HistogramBody::parse(example("histogram", "full.json"));
    REQUIRE(h.ok());
    CHECK(h->bins.bandMode == analysis::BandMode::Width);
    CHECK(h->style == "line");
    CHECK(h->binLabelStep == 2);
    CHECK_FALSE(h->colourAxis.has_value());
}

TEST_CASE("HistogramBody: members without bins, channel order, errors", "[unit]") {
    const JsonDoc body =
        doc(R"({"binLabelStep":3,"colourChannel":{"axis":null,"channel":"Throttle Pos"},)"
            R"("condensedAxes":true,"gating":{"channel":"Gate"},"layout":"tiled",)"
            R"("stackLines":true,"style":"bar","tileCols":1,"traces":[{"channel":"Damper FL"},)"
            R"({"channel":"Damper FR","visible":false}],"yMode":"time",)"
            R"("yScale":{"max":{"unit":"s","v":10},"min":{"unit":"s","v":0},"mode":"manual"}})");
    const Result<HistogramBody> parsed = HistogramBody::parse(body);
    REQUIRE(parsed.ok());
    const HistogramBody& h = *parsed;
    REQUIRE(h.traces.size() == 2);
    CHECK_FALSE(h.traces[1].visible);
    CHECK(h.yMode == "time");
    CHECK(h.yScale.mode == ScaleMode::Manual);
    CHECK(h.stackLines);
    CHECK(h.style == "bar");
    CHECK(h.layout == "tiled");
    CHECK(h.tileCols == 1);
    CHECK(h.binLabelStep == 3);
    CHECK(h.condensedAxes);
    CHECK(h.colourChannel == "Throttle Pos");
    CHECK_FALSE(h.colourAxis.has_value());
    CHECK(h.gatingChannel == "Gate");
    CHECK(h.toJson() == body); // no "bins": BandAxis is not read or written
    const std::vector<ChannelUse> uses = h.channels();
    REQUIRE(uses.size() == 4);
    CHECK(uses[0].name == "Damper FL");
    CHECK(uses[1].name == "Damper FR");
    CHECK(uses[2].name == "Throttle Pos");
    CHECK(uses[3].name == "Gate");
    Result<HistogramBody> bad = HistogramBody::parse(doc(R"({"yMode":"seconds"})"));
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().context.detail == "/yMode");
    bad = HistogramBody::parse(doc(R"({"traces":[{"channel":3}]})"));
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().context.detail == "/traces/0/channel");
}
