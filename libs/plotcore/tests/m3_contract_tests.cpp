// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The M3 contract of esses_plotcore (07 section 6.1): what is final in the new and extended headers. HitKind
// keeps the M1 values, hitAll() lists stacked regions topmost first, componentChannelUses lists the channels
// of the M1 types in builder order and of unknown types none, KeyLayout's column-aware layout keeps the M1
// layout until the time_distance work adds the datum and measurement columns. The M3-STUB builders are tested
// by their owners (src/geometry, src/band_layout.cpp, src/key_layout.cpp).
#include <esses/plotcore/plotcore.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace esses;
using namespace esses::plot;

namespace {

project::ComponentSpec specOf(const std::string& type, const std::string& body) {
    project::ComponentSpec spec;
    spec.type = type;
    spec.body = JsonDoc::parse(body).value();
    return spec;
}

HitResult hitOf(HitKind kind, int band = -1) {
    HitResult r;
    r.kind = kind;
    r.band = band;
    return r;
}

} // namespace

TEST_CASE("m3 contract: HitKind keeps the M1 values", "[unit]") {
    CHECK(static_cast<int>(HitKind::None) == 0);
    CHECK(static_cast<int>(HitKind::Panel) == 1);
    CHECK(static_cast<int>(HitKind::Handle) == 8);
    CHECK(static_cast<int>(HitKind::GroupHeading) == 9);
    const HitResult r;
    CHECK(r.handleKind == HandleKind::None);
    CHECK(r.point == -1);
    CHECK(r.bin == -1);
    CHECK(r.section == -1);
    CHECK(r.cell == -1);
}

TEST_CASE("m3 contract: hitAll lists every region under a pixel, topmost first", "[unit]") {
    HitTester hits;
    hits.addRect(RectPx{0, 0, 100, 100}, hitOf(HitKind::Panel));
    hits.addRect(RectPx{0, 0, 100, 14}, hitOf(HitKind::Band, 0));
    hits.addRect(RectPx{10, 0, 5, 14}, hitOf(HitKind::Marker));
    const std::vector<HitResult> all = hits.hitAll(12.0F, 5.0F);
    REQUIRE(all.size() == 3);
    CHECK(all[0].kind == HitKind::Marker);
    CHECK(all[1].kind == HitKind::Band);
    CHECK(all[2].kind == HitKind::Panel);
    CHECK(hits.hit(12.0F, 5.0F).kind == HitKind::Marker);
    CHECK(hits.hitAll(50.0F, 50.0F).size() == 1);
    CHECK(hits.hitAll(150.0F, 50.0F).empty());
    hits.clear();
    CHECK(hits.empty());
}

TEST_CASE("m3 contract: componentChannelUses for the M1 types and unknown types", "[unit]") {
    const auto td = componentChannelUses(
        specOf("time_distance",
               R"({"groups": [{"name": "Speed", "traces": [{"channel": "Corr Speed", "displayUnit": "km/h",
            "dps": 1}, {"channel": "GPS Speed"}]}, {"name": "Pedals", "traces": [{"channel": "Throttle Pos"}]}]})"));
    REQUIRE(td.size() == 3);
    CHECK(td[0] == ChannelUse{"Corr Speed", "km/h", 1});
    CHECK(td[1].name == "GPS Speed");
    CHECK(td[2].name == "Throttle Pos");

    const auto values = componentChannelUses(specOf(
        "values_list",
        R"({"channels": [{"channel": "Engine RPM", "displayUnit": "rpm", "dps": 0}, {"channel": ""}]})"));
    REQUIRE(values.size() == 1); // rows without a name are dropped
    CHECK(values[0] == ChannelUse{"Engine RPM", "rpm", 0});

    CHECK(componentChannelUses(specOf("time_report", R"({"rowGroup": "Outings:Laps"})")).empty());
    CHECK(componentChannelUses(specOf("gauge_dial", R"({"channel": "Engine RPM"})")).empty());
    project::ComponentSpec unknown = specOf("time_distance", R"({"groups": []})");
    unknown.known = false;
    CHECK(componentChannelUses(unknown).empty());
}

TEST_CASE("m3 contract: KeyLayout with columns keeps the M1 layout until the columns exist", "[unit]") {
    const FixedTextMetrics metrics;
    std::vector<KeyRow> rows(2);
    rows[0].label = "Corr Speed [km/h]";
    rows[0].valueText = "123.4";
    rows[1].label = "Throttle Pos [%]";
    rows[1].valueText = "99.9";
    const KeyGeometry m1 = KeyLayout::layout(rows, metrics, 800.0F, 16.0F, 1.0F);
    KeyColumns columns;
    columns.datum = true;
    const KeyGeometry m3 = KeyLayout::layout(rows, metrics, 800.0F, 16.0F, 1.0F, columns);
    CHECK(m3.columns.datum);
    CHECK(m3.widthPx >= m1.widthPx);
    CHECK(m3.rowRects.size() == m1.rowRects.size());
    CHECK(m3.cellRect(0, 0, KeyCell::Value) == m3.valueRect(0, 0));
}
