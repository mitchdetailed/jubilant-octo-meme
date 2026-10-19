// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// M4 followups in esses_plotcore (D-211; docs/milestones/M3.md section 6 items 19, 21, 22): the default
// display unit of a channel and the Corr Dist display unit (display_units.hpp; expectations from the
// quantities and units of data/units.json as read by hand: Ratio's base "ratio" with "%" at scalar 100, Speed
// m/s with km/h and mph, Distance m with km and mi), the per-channel display units of the scatter body and of
// the channel report's channels in componentChannelUses, and the GroupHeading hit regions of a time_distance
// Static build (04 section 8.2 item 4: each group's part of the key column, beneath its labels).

#include "td_m3_cases.hpp"

#include <esses/plotcore/component_bodies.hpp>
#include <esses/plotcore/display_units.hpp>
#include <esses/plotcore/scatter_geometry.hpp>
#include <esses/plotcore/time_distance_builder.hpp>
#include <esses/units/unit_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using namespace esses;
using namespace esses::plot;

namespace {

const units::UnitRegistry& shippedUnits() {
    static const units::UnitRegistry registry = [] {
        Result<units::UnitRegistry> r = units::UnitRegistry::loadFiles(
            std::filesystem::path(ESSES_TEST_REPO_ROOT) / "data" / "units.json");
        REQUIRE(r.ok());
        return std::move(r).value();
    }();
    return registry;
}

units::UnitId unitOf(std::string_view symbol) {
    const std::optional<units::UnitId> id = shippedUnits().find(symbol);
    REQUIRE(id.has_value());
    return id.value_or(units::kNoUnit);
}

std::string symbolOf(units::UnitId id) {
    return shippedUnits().unit(id).symbol;
}

project::ComponentSpec specOf(const char* type, const char* body) {
    project::ComponentSpec spec;
    spec.type = type;
    spec.body = JsonDoc::parse(body).value();
    return spec;
}

} // namespace

TEST_CASE("followups D-211: a fraction stored in ratio shows in percent, every other stored unit as stored",
          "[unit]") {
    const units::UnitRegistry& reg = shippedUnits();
    CHECK(symbolOf(defaultDisplayUnit(reg, unitOf("ratio"))) == "%");
    CHECK(symbolOf(defaultDisplayUnit(reg, unitOf("%"))) == "%");     // already a reading unit
    CHECK(symbolOf(defaultDisplayUnit(reg, unitOf("m/s"))) == "m/s"); // other quantities: the stored unit
    CHECK(symbolOf(defaultDisplayUnit(reg, unitOf("K"))) == "K");
    CHECK(symbolOf(defaultDisplayUnit(reg, unitOf("rad"))) == "rad");
    CHECK(defaultDisplayUnit(reg, units::kNoUnit) == units::kNoUnit); // unitless stays unitless
    const auto unknown = static_cast<units::UnitId>(reg.unitCount() + 5);
    CHECK(defaultDisplayUnit(reg, unknown) == unknown); // an id the registry does not hold is kept
    // The % shown converts the stored fraction by its scalar 100 (data/units.json).
    const Result<units::Converter> c =
        reg.converter(unitOf("ratio"), defaultDisplayUnit(reg, unitOf("ratio")));
    REQUIRE(c.ok());
    CHECK((*c)(0.5) == 50.0);
}

TEST_CASE("followups D-211: a named display unit of the channel's quantity wins, any other falls back",
          "[unit]") {
    const units::UnitRegistry& reg = shippedUnits();
    CHECK(symbolOf(displayUnitFor(reg, unitOf("m/s"), "km/h")) == "km/h");
    CHECK(symbolOf(displayUnitFor(reg, unitOf("m/s"), "kph")) == "km/h");      // an alias
    CHECK(symbolOf(displayUnitFor(reg, unitOf("ratio"), "ratio")) == "ratio"); // asked for explicitly
    CHECK(symbolOf(displayUnitFor(reg, unitOf("ratio"), "")) == "%");
    CHECK(symbolOf(displayUnitFor(reg, unitOf("ratio"), "km/h")) == "%"); // not of the quantity: default
    CHECK(symbolOf(displayUnitFor(reg, unitOf("m/s"), "G")) == "m/s");
}

TEST_CASE("followups D-211: the Corr Dist display unit follows its override, else the speed unit, else m",
          "[unit]") {
    const units::UnitRegistry& reg = shippedUnits();
    CHECK(symbolOf(corrDistDisplayUnit(reg, "", "")) == "m");
    CHECK(symbolOf(corrDistDisplayUnit(reg, "km", "")) == "km");
    CHECK(symbolOf(corrDistDisplayUnit(reg, "mi", "km/h")) == "mi"); // the override wins
    CHECK(symbolOf(corrDistDisplayUnit(reg, "", "km/h")) == "km");   // 03 section 9.1 item 6
    CHECK(symbolOf(corrDistDisplayUnit(reg, "", "mph")) == "mi");
    CHECK(symbolOf(corrDistDisplayUnit(reg, "", "m/s")) == "m");
    CHECK(symbolOf(corrDistDisplayUnit(reg, "km/h", "mph")) == "mi"); // not a length: the speed rule
    CHECK(symbolOf(corrDistDisplayUnit(reg, "nonsense", "")) == "m");
}

TEST_CASE(
    "followups D-211: the scatter's x, y and colour display units parse, write only when set, and are used",
    "[unit]") {
    const project::ComponentSpec spec =
        specOf("scatter", R"({"xChannel":"Corr Speed","xDisplayUnit":"km/h",)"
                          R"("traces":[{"yChannel":"G Force Lat","displayUnit":"G"},{"yChannel":"Steer"}],)"
                          R"("colourChannel":{"channel":"Throttle Pos","displayUnit":"%"}})");
    const Result<ScatterBody> body = ScatterBody::parse(spec.body);
    REQUIRE(body.ok());
    CHECK(body->xDisplayUnit == "km/h");
    REQUIRE(body->traces.size() == 2);
    CHECK(body->traces[0].displayUnit == "G");
    CHECK(body->traces[1].displayUnit.empty());
    CHECK(body->colourDisplayUnit == "%");
    CHECK(body->toJson() == spec.body); // a canonical body round-trips
    const std::vector<ChannelUse> uses = componentChannelUses(spec);
    REQUIRE(uses.size() == 4);
    CHECK(uses[0] == ChannelUse{"Corr Speed", "km/h", std::nullopt});
    CHECK(uses[1] == ChannelUse{"G Force Lat", "G", std::nullopt});
    CHECK(uses[2] == ChannelUse{"Steer", "", std::nullopt});
    CHECK(uses[3] == ChannelUse{"Throttle Pos", "%", std::nullopt});
    // Unset members are not written.
    ScatterBody plain;
    plain.xChannel = "Corr Speed";
    plain.traces.push_back(ScatterTrace{"G Force Lat", true, 0, ""});
    const std::string json = plain.toJson().compact();
    CHECK(json.find("xDisplayUnit") == std::string::npos);
    CHECK(json.find("displayUnit") == std::string::npos);
    // A display unit that is not text is Invalid at its pointer.
    const Result<ScatterBody> bad =
        ScatterBody::parse(JsonDoc::parse(R"({"traces":[{"yChannel":"Y","displayUnit":3}]})").value());
    REQUIRE(!bad.ok());
    CHECK(bad.error().context.detail == "/traces/0/displayUnit");
}

TEST_CASE("followups D-211: a channel report's channels carry their display unit to the channel uses",
          "[unit]") {
    const project::ComponentSpec spec =
        specOf("channel_report",
               R"({"report":{"channels":[{"name":"Corr Speed","stats":["max"],"displayUnit":"km/h"},)"
               R"({"name":"Throttle Pos","stats":["avg"]}]}})");
    const std::vector<ChannelUse> uses = componentChannelUses(spec);
    REQUIRE(uses.size() == 2);
    CHECK(uses[0] == ChannelUse{"Corr Speed", "km/h", std::nullopt});
    CHECK(uses[1] == ChannelUse{"Throttle Pos", "", std::nullopt});
}

TEST_CASE("followups D-211: every group's key column is its GroupHeading, beneath its labels", "[unit]") {
    const test::M3Fixture m;
    const auto cases = test::m3Cases(m);
    for (const char* name : {"datum_key", "overlapped"}) {
        INFO(name);
        const test::M3Case* c = nullptr;
        for (const test::M3Case& mc : cases) {
            if (mc.name == name) {
                c = &mc;
            }
        }
        REQUIRE(c != nullptr);
        const ComponentBuildInputs in = test::m3InputsOf(m, *c, 1.0F);
        const TdLayout lay = TimeDistanceBuilder::layoutOf(in);
        REQUIRE(lay.groups.size() >= 2);
        const Result<ComponentBuild> build = TimeDistanceBuilder::buildStatic(in, CancelToken{});
        REQUIRE(build.ok());
        int headings = 0;
        for (const TdGroupLayout& gl : lay.groups) {
            REQUIRE(gl.key.h > 0.0F);
            REQUIRE(!gl.keyGeometry.rowRects.empty());
            // Below the group's last label row, inside its key: the heading.
            const RectPx last = gl.keyGeometry.rowRects.back();
            const float yBelow = gl.key.y + last.y + last.h + 0.5F * (gl.key.h - (last.y + last.h));
            if (gl.key.h - (last.y + last.h) >= 2.0F) {
                const HitResult h = build->hits.hit(gl.key.x + 2.0F, yBelow);
                CHECK(h.kind == HitKind::GroupHeading);
                CHECK(h.group == gl.group);
                ++headings;
            }
            // On a label the label wins.
            const RectPx first = gl.keyGeometry.rowRects.front();
            const HitResult l =
                build->hits.hit(gl.key.x + first.x + 1.0F, gl.key.y + first.y + 0.5F * first.h);
            CHECK(l.kind == HitKind::KeyLabel);
            CHECK(l.group == gl.group);
        }
        CHECK(headings > 0); // the cases leave room below the labels
    }
}
