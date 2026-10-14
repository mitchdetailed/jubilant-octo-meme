// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// BandAxis (04 section 7.3): the persisted definition reads and writes every member of the schema's
// "bandAxis" (unknown members survive), and the generated bands equal hand-computed edges for width, count,
// centred and arbitrary axes with the open bands, converted through the unit registry.
#include "analysis_test_support.hpp"

#include <esses/units/unit_registry.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::analysis;
using namespace esses::analysis::test;

namespace {

const units::UnitRegistry& registry() {
    static const units::UnitRegistry r = [] {
        auto loaded = units::UnitRegistry::loadFiles(repoRoot() / "data" / "units.json");
        REQUIRE(loaded.ok());
        return std::move(loaded).value();
    }();
    return r;
}

JsonDoc json(const std::string& text) {
    auto j = JsonDoc::parse(text);
    REQUIRE(j.ok());
    return std::move(j).value();
}

BandAxis axisOf(const std::string& text) {
    auto a = BandAxis::fromJson(json(text));
    REQUIRE(a.ok());
    return std::move(a).value();
}

std::vector<Band> bandsOf(const BandAxis& axis, double autoMin = 0.0, double autoMax = 1.0,
                          const units::UnitRegistry* units = nullptr, std::string_view unit = "") {
    auto b = generateBands(axis, autoMin, autoMax, units, unit);
    REQUIRE(b.ok());
    return std::move(b).value();
}

void checkEdges(const std::vector<Band>& bands, const std::vector<double>& edges) {
    REQUIRE(bands.size() + 1 == edges.size());
    for (std::size_t k = 0; k < bands.size(); ++k) {
        INFO("band " << k);
        CHECK(bands[k].lo == Catch::Approx(edges[k]).margin(1e-12));
        CHECK(bands[k].hi == Catch::Approx(edges[k + 1]).margin(1e-12));
        CHECK_FALSE(bands[k].lessMin);
        CHECK_FALSE(bands[k].greaterMax);
    }
}

constexpr double kInf = std::numeric_limits<double>::infinity();

} // namespace

TEST_CASE("band_axis: reads and writes every member", "[unit]") {
    const BandAxis defaults = axisOf("{}");
    CHECK(defaults == BandAxis{});
    CHECK(BandAxis::fromJson(defaults.toJson()).value() == defaults);

    const BandAxis full = axisOf(R"({"mode": "arbitrary", "bandMode": "count",
        "dataMin": {"v": 0, "unit": "km/h"}, "dataMax": {"v": 300, "unit": "km/h"},
        "bandWidth": {"v": 20, "unit": "km/h"}, "bandCount": 15, "palette": "twoColour",
        "colour1": "#2040FF", "colour2": "#ff202080", "reverse": true, "centred": true, "lessMinBand": false,
        "greaterMaxBand": false, "continuous": true, "x-future": [1, 2],
        "bands": [{"min": {"v": 0, "unit": "km/h"}, "max": {"v": 50, "unit": "km/h"}, "colour": "#2040ff"},
                  {"min": {"v": 50, "unit": "km/h"}, "max": {"v": 90.5, "unit": "km/h"}}]})");
    CHECK(full.mode == BandAxisMode::Arbitrary);
    CHECK(full.bandMode == BandMode::Count);
    CHECK(full.dataMin == UnitValue{0.0, "km/h"});
    CHECK(full.dataMax == UnitValue{300.0, "km/h"});
    CHECK(full.bandWidth == UnitValue{20.0, "km/h"});
    CHECK(full.bandCount == 15);
    CHECK(full.palette == "twoColour");
    CHECK(full.colour1 == 0x2040FFFFu);
    CHECK(full.colour2 == 0xFF202080u);
    CHECK((full.reverse && full.centred && !full.lessMinBand && !full.greaterMaxBand && full.continuous));
    REQUIRE(full.bands.size() == 2);
    CHECK(full.bands[0].colour == 0x2040FFFFu);
    CHECK(full.bands[1].colour == BandDef{}.colour); // the default colour
    CHECK(full.bands[1].max == UnitValue{90.5, "km/h"});
    CHECK(full.rest == json(R"({"x-future": [1, 2]})"));

    // Canonical output: colours lower case, opaque ones without alpha; the unknown member kept.
    const JsonDoc written = full.toJson();
    CHECK(written == json(R"({"mode": "arbitrary", "bandMode": "count",
        "dataMin": {"v": 0, "unit": "km/h"}, "dataMax": {"v": 300, "unit": "km/h"},
        "bandWidth": {"v": 20, "unit": "km/h"}, "bandCount": 15, "palette": "twoColour",
        "colour1": "#2040ff", "colour2": "#ff202080", "reverse": true, "centred": true, "lessMinBand": false,
        "greaterMaxBand": false, "continuous": true, "x-future": [1, 2],
        "bands": [{"min": {"v": 0, "unit": "km/h"}, "max": {"v": 50, "unit": "km/h"}, "colour": "#2040ff"},
                  {"min": {"v": 50, "unit": "km/h"}, "max": {"v": 90.5, "unit": "km/h"},
                   "colour": "#2040ff"}]})"));
    CHECK(BandAxis::fromJson(written).value() == full);
}

TEST_CASE("band_axis: malformed members are refused with their JSON pointer", "[unit]") {
    const auto detail = [](const std::string& text) {
        auto a = BandAxis::fromJson(json(text));
        REQUIRE_FALSE(a.ok());
        CHECK(a.error().code == ErrorCode::Invalid);
        return a.error().context.detail.value_or("?");
    };
    CHECK(detail(R"({"mode": "odd"})") == "/mode");
    CHECK(detail(R"({"bandMode": "range"})") == "/bandMode");
    CHECK(detail(R"({"bandCount": 0})") == "/bandCount");
    CHECK(detail(R"({"bandCount": 1001})") == "/bandCount");
    CHECK(detail(R"({"dataMin": 5})") == "/dataMin");
    CHECK(detail(R"({"dataMin": {"v": 5}})") == "/dataMin");
    CHECK(detail(R"({"dataMax": {"v": 5, "unit": "m", "x": 1}})") == "/dataMax/x");
    CHECK(detail(R"({"colour1": "red"})") == "/colour1");
    CHECK(detail(R"({"colour2": "#12345g"})") == "/colour2");
    CHECK(detail(R"({"reverse": 1})") == "/reverse");
    CHECK(detail(R"({"bands": {}})") == "/bands");
    CHECK(detail(R"({"bands": [{"min": {"v": 0, "unit": ""}}]})") == "/bands/0");
    CHECK(detail(R"([1])").empty());
    CHECK(BandAxis::fromJson(JsonDoc{}).error().code == ErrorCode::Invalid);
}

TEST_CASE("band_axis: even bands by width, count and centred, with the open bands", "[unit]") {
    // Width 20 over 0..300: 15 bands, plus the two open bands of the default axis.
    BandAxis width = axisOf(R"({"dataMin": {"v": 0, "unit": ""}, "dataMax": {"v": 300, "unit": ""},
                                "bandWidth": {"v": 20, "unit": ""}})");
    const std::vector<Band> open = bandsOf(width);
    REQUIRE(open.size() == 17);
    CHECK(open.front().lessMin);
    CHECK(open.front().lo == -kInf);
    CHECK(open.front().hi == 0.0);
    CHECK(open.back().greaterMax);
    CHECK(open.back().lo == 300.0);
    CHECK(open.back().hi == kInf);
    width.lessMinBand = width.greaterMaxBand = false;
    std::vector<double> edges;
    for (int k = 0; k <= 15; ++k) {
        edges.push_back(20.0 * k);
    }
    checkEdges(bandsOf(width), edges);
    CHECK(bandsOf(width).back().hi == 300.0); // exactly dataMax

    // A width that does not divide the range: full-width bands, the last past dataMax.
    BandAxis odd = axisOf(R"({"dataMin": {"v": 0, "unit": ""}, "dataMax": {"v": 100, "unit": ""},
                              "bandWidth": {"v": 30, "unit": ""}, "lessMinBand": false,
                              "greaterMaxBand": false})");
    checkEdges(bandsOf(odd), {0, 30, 60, 90, 120});
    // 0.1 steps that do not divide exactly in binary still give 11 bands over 0..1.1.
    BandAxis tenths = axisOf(R"({"dataMin": {"v": 0, "unit": ""}, "dataMax": {"v": 1.1, "unit": ""},
                                 "bandWidth": {"v": 0.1, "unit": ""}, "lessMinBand": false,
                                 "greaterMaxBand": false})");
    CHECK(bandsOf(tenths).size() == 11);

    // Count: equal bands; auto limits replace the absent dataMin/dataMax.
    BandAxis count =
        axisOf(R"({"bandMode": "count", "bandCount": 4, "lessMinBand": false, "greaterMaxBand": false})");
    checkEdges(bandsOf(count, -1.0, 1.0), {-1.0, -0.5, 0.0, 0.5, 1.0});
    // Width mode without a width falls back to bandCount.
    BandAxis noWidth = count;
    noWidth.bandMode = BandMode::Width;
    checkEdges(bandsOf(noWidth, -1.0, 1.0), {-1.0, -0.5, 0.0, 0.5, 1.0});

    // Centred: band k centred on min + k * w.
    BandAxis centred = odd;
    centred.centred = true;
    centred.bandWidth = UnitValue{25.0, ""};
    checkEdges(bandsOf(centred), {-12.5, 12.5, 37.5, 62.5, 87.5, 112.5});
    BandAxis centredCount = count;
    centredCount.centred = true;
    centredCount.bandCount = 5;
    checkEdges(bandsOf(centredCount, 0.0, 100.0), {-12.5, 12.5, 37.5, 62.5, 87.5, 112.5});
    centredCount.bandCount = 1; // ignored for one band
    checkEdges(bandsOf(centredCount, 0.0, 100.0), {0.0, 100.0});
}

TEST_CASE("band_axis: units, arbitrary bands, colours and refusals", "[unit]") {
    // Persisted in km/h, generated in m/s; a width converts by scale only (10 C wide is 18 F wide).
    const BandAxis kmh =
        axisOf(R"({"dataMin": {"v": 36, "unit": "km/h"}, "dataMax": {"v": 108, "unit": "km/h"},
        "bandWidth": {"v": 36, "unit": "km/h"}, "lessMinBand": false, "greaterMaxBand": false})");
    checkEdges(bandsOf(kmh, 0, 1, &registry(), "m/s"), {10.0, 20.0, 30.0});
    checkEdges(bandsOf(kmh, 0, 1, nullptr, "m/s"), {36.0, 72.0, 108.0}); // no registry: as written
    const BandAxis celsius = axisOf(R"({"dataMin": {"v": 0, "unit": "C"}, "dataMax": {"v": 20, "unit": "C"},
        "bandWidth": {"v": 10, "unit": "C"}, "lessMinBand": false, "greaterMaxBand": false})");
    checkEdges(bandsOf(celsius, 0, 1, &registry(), "F"), {32.0, 50.0, 68.0});
    auto mismatch = generateBands(kmh, 0, 1, &registry(), "m");
    REQUIRE_FALSE(mismatch.ok());
    CHECK(mismatch.error().context.detail == "/dataMin");

    // Arbitrary bands as written, with their colours and the open bands; gaps are allowed.
    const BandAxis arb = axisOf(R"({"mode": "arbitrary", "bands": [
        {"min": {"v": 0, "unit": ""}, "max": {"v": 10, "unit": ""}, "colour": "#ff0000"},
        {"min": {"v": 20, "unit": ""}, "max": {"v": 30, "unit": ""}, "colour": "#00ff0080"}]})");
    const std::vector<Band> bands = bandsOf(arb);
    REQUIRE(bands.size() == 4);
    CHECK(bands[0] == Band{-kInf, 0.0, 0xFF0000FFu, true, false});
    CHECK(bands[1] == Band{0.0, 10.0, 0xFF0000FFu, false, false});
    CHECK(bands[2] == Band{20.0, 30.0, 0x00FF0080u, false, false});
    CHECK(bands[3] == Band{30.0, kInf, 0x00FF0080u, false, true});
    const auto refusal = [](const BandAxis& a, double lo = 0.0, double hi = 1.0) {
        auto r = generateBands(a, lo, hi, nullptr, "");
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Invalid);
        return r.error().context.detail.value_or("?");
    };
    BandAxis overlap = arb;
    overlap.bands[1].min.v = 5.0;
    CHECK(refusal(overlap) == "/bands/1");
    BandAxis emptyArb = arb;
    emptyArb.bands.clear();
    CHECK(refusal(emptyArb) == "/bands");
    CHECK(refusal(BandAxis{}, 1.0, 1.0) == "/dataMax"); // min >= max
    BandAxis zeroWidth = kmh;
    zeroWidth.bandWidth = UnitValue{0.0, "km/h"};
    CHECK(refusal(zeroWidth) == "/bandWidth");
    BandAxis tooMany = kmh;
    tooMany.bandWidth = UnitValue{0.001, "km/h"};
    CHECK(refusal(tooMany) == "/bandWidth");

    // Palettes: rainbow runs blue to red, reverse flips it, twoColour blends colour1 into colour2.
    BandAxis rainbow =
        axisOf(R"({"bandMode": "count", "bandCount": 3, "lessMinBand": false, "greaterMaxBand": false})");
    std::vector<Band> r = bandsOf(rainbow);
    CHECK(r[0].colour == 0x0000FFFFu);
    CHECK(r[1].colour == 0x00FF00FFu);
    CHECK(r[2].colour == 0xFF0000FFu);
    rainbow.reverse = true;
    r = bandsOf(rainbow);
    CHECK(r[0].colour == 0xFF0000FFu);
    CHECK(r[2].colour == 0x0000FFFFu);
    rainbow.reverse = false;
    rainbow.palette = "twoColour";
    rainbow.colour1 = 0x000000FFu;
    rainbow.colour2 = 0xFFFFFFFFu;
    r = bandsOf(rainbow);
    CHECK(r[0].colour == 0x000000FFu);
    CHECK(r[1].colour == 0x808080FFu);
    CHECK(r[2].colour == 0xFFFFFFFFu);
}
