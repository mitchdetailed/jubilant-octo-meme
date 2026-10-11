// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelResolver and ResolutionTrace (02 section 4.6 rules 15-18; D-013, D-074, D-132 (f)).
#include "model_test_support.hpp"

#include <esses/model/resolver.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace esses;
using namespace esses::model;
using esses::model::test::makeChannel;

namespace {

const std::string kArrow = "\xE2\x86\x92";

std::string arrows(std::string s) { // "->" in expectations reads better than the UTF-8 bytes
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s.compare(i, 2, "->") == 0) {
            out += kArrow;
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

DataLayer sampleLayer() {
    const auto& reg = test::shippedUnits();
    DataLayer layer;
    Channel fl = makeChannel("Wheel Speed FL");
    fl.desc.unit = reg.find("km/h").value();
    fl.desc.quantity = reg.quantityOf(fl.desc.unit);
    fl.desc.dps = 2;
    fl.desc.colourIndex = 3;
    layer.addChannel(fl);
    Channel fr = makeChannel("Wheel Speed FR", "WheelSP_FR"); // renamed by a dictionary
    fr.desc.unit = fl.desc.unit;
    fr.desc.quantity = fl.desc.quantity;
    layer.addChannel(fr);
    layer.addChannel(makeChannel("Wheel Speed RL", {}, ChannelStatus::Error));
    layer.addChannel(makeChannel("Engine RPM"));
    layer.addChannel(makeChannel("Speed"));
    Channel gps = makeChannel("GPS_Spd");
    gps.desc.unit = units::kNoUnit;
    layer.addChannel(gps);
    return layer;
}

AliasTable sampleAliases() {
    AliasTable t;
    t.rows.push_back(AliasRow{"Wheel Speed", {{"Wheel Speed RL", true}, {"Wheel Speed FL", true}}, 5, 1});
    t.rows.push_back(
        AliasRow{"Velocity", {{"Missing Channel", true}, {"Wheel Speed", true}}, 7, std::nullopt});
    t.rows.push_back(AliasRow{"Speed", {{"Speed", true}}, 2, std::nullopt}); // spelled like its own alias
    t.rows.push_back(AliasRow{"Revs", {{"Engine RPM", false}, {"Speed", true}}, -1, std::nullopt});
    t.rows.push_back(AliasRow{"Ghost", {{"X", true}, {"Y", true}}, -1, std::nullopt});
    t.rows.push_back(AliasRow{"A", {{"B", true}}, -1, std::nullopt});
    t.rows.push_back(AliasRow{"B", {{"A", true}}, -1, std::nullopt});
    return t;
}

MappingTable sampleMappings() {
    MappingTable m;
    m.rows.push_back(MappingRow{"Vehicle Speed", "Speed", std::nullopt, false});
    return m;
}

} // namespace

TEST_CASE("resolver: canonical name, exact and case-folded", "[unit]") {
    const DataLayer layer = sampleLayer();
    const ChannelResolver r({}, {}, &test::shippedUnits());
    auto res = r.resolve("Engine RPM", layer);
    REQUIRE(res.channel == layer.find("Engine RPM"));
    CHECK(res.trace.result == res.channel->desc.id);
    REQUIRE(res.trace.steps.size() == 1);
    CHECK(res.trace.steps[0].kind == ResolutionStep::Kind::CaseFold);
    CHECK(res.trace.steps[0].note == "exact");
    CHECK(res.trace.summary() == "'Engine RPM'");
    CHECK_FALSE(res.alias.has_value());

    res = r.resolve("ENGINE rpm", layer);
    CHECK(res.channel == layer.find("Engine RPM"));
    CHECK(res.trace.summary() == arrows("'ENGINE rpm' -> case-fold -> 'Engine RPM'"));
}

TEST_CASE("resolver: logged name and dictionary step (rule 15)", "[unit]") {
    const DataLayer layer = sampleLayer();
    const ChannelResolver r({}, {}, &test::shippedUnits());
    auto res = r.resolve("wheelsp_fr", layer);
    REQUIRE(res.channel == layer.find("Wheel Speed FR"));
    CHECK(res.trace.steps.back().kind == ResolutionStep::Kind::LoggedName);
    CHECK(res.trace.unitLabel == "km/h");
    CHECK(res.trace.summary() == arrows("'wheelsp_fr' -> logged name -> 'Wheel Speed FR' (km/h)"));

    res = r.resolve("Wheel Speed FR", layer);
    REQUIRE(res.trace.steps.size() == 2);
    CHECK(res.trace.steps[1].kind == ResolutionStep::Kind::Dictionary);
    CHECK(res.trace.steps[1].from == "WheelSP_FR");
    CHECK(res.trace.summary() == "'Wheel Speed FR' [dictionary: logged as 'WheelSP_FR'] (km/h)");
}

TEST_CASE("resolver: mappings come first (rule 16)", "[unit]") {
    const DataLayer layer = sampleLayer();
    const ChannelResolver r(sampleMappings(), {}, nullptr);
    const auto res = r.resolve("vehicle SPEED", layer);
    REQUIRE(res.channel == layer.find("Speed"));
    CHECK(res.trace.steps[0].kind == ResolutionStep::Kind::Mapping);
    CHECK(res.trace.steps[0].to == "Speed");
    CHECK(res.trace.summary() == arrows("'vehicle SPEED' -> mapping -> 'Speed'"));
    CHECK(res.trace.unitLabel.empty()); // no registry
}

TEST_CASE("resolver: alias candidates in order, Error and Missing fall through (rule 17)", "[unit]") {
    const DataLayer layer = sampleLayer();
    const ChannelResolver r({}, sampleAliases(), &test::shippedUnits());
    const auto res = r.resolve("wheel speed", layer, test::testSource());
    REQUIRE(res.channel == layer.find("Wheel Speed FL"));
    CHECK(res.trace.summary() == arrows("'wheel speed' -> alias -> 'Wheel Speed FL' (km/h)"));
    REQUIRE(res.trace.steps.size() >= 2);
    CHECK(res.trace.steps[0].kind == ResolutionStep::Kind::Alias);
    CHECK(res.trace.steps[0].to == "Wheel Speed RL");
    CHECK(res.trace.steps[0].note == "skipped: status Error (W-ALIAS-FALLBACK)");
    CHECK(res.trace.steps[1].to == "Wheel Speed FL");
    CHECK(res.trace.steps[1].note.empty());

    // The Alias descriptor carries the alias's colour and dps (D-013, D-074).
    REQUIRE(res.alias.has_value());
    CHECK(res.alias->name == "Wheel Speed");
    CHECK(res.alias->source == ChannelSource::Alias);
    CHECK(res.alias->colourIndex == 5);
    CHECK(res.alias->dps == 1);
    CHECK(res.alias->unit == res.channel->desc.unit);
    CHECK(res.alias->sourceEncoding == nullptr);
    CHECK(res.alias->id == channelIdOther(ChannelSource::Alias, test::testSource(), "Normal", "Wheel Speed"));
    CHECK(res.trace.result == res.channel->desc.id);
}

TEST_CASE("resolver: an alias may name another alias and its own spelling", "[unit]") {
    const DataLayer layer = sampleLayer();
    const ChannelResolver r({}, sampleAliases(), &test::shippedUnits());

    auto res = r.resolve("Velocity", layer);
    REQUIRE(res.channel == layer.find("Wheel Speed FL"));
    CHECK(res.trace.summary() ==
          arrows("'Velocity' -> alias -> 'Wheel Speed' -> alias -> 'Wheel Speed FL' (km/h)"));
    CHECK(res.trace.steps[0].note == "skipped: missing");
    REQUIRE(res.alias.has_value());
    CHECK(res.alias->name == "Velocity"); // the alias that was asked for
    CHECK(res.alias->colourIndex == 7);
    CHECK(res.alias->dps == 2); // no alias dps: the channel's

    res = r.resolve("speed", layer); // candidate 'Speed' reaches the logged channel, not the alias again
    REQUIRE(res.channel == layer.find("Speed"));
    CHECK(res.trace.summary() == arrows("'speed' -> alias -> 'Speed'"));
    CHECK(res.alias->colourIndex == 2);

    res = r.resolve("Revs", layer); // disabled candidates are skipped
    REQUIRE(res.channel == layer.find("Speed"));
    CHECK(res.trace.steps[0].note == "skipped: disabled");
}

TEST_CASE("resolver: alias cycles stop with an error, missing names end in Missing", "[unit]") {
    const DataLayer layer = sampleLayer();
    const ChannelResolver r({}, sampleAliases(), nullptr);

    auto res = r.resolve("A", layer);
    CHECK(res.channel == nullptr);
    CHECK_FALSE(res.trace.resolved());
    REQUIRE_FALSE(res.trace.steps.empty());
    CHECK(res.trace.steps.back().kind == ResolutionStep::Kind::Missing);
    CHECK(res.trace.steps.back().note == arrows("Error: alias cycle 'A' -> 'B' -> 'A'"));
    CHECK(res.trace.summary() == arrows("'A' -> missing (Error: alias cycle 'A' -> 'B' -> 'A')"));

    res = r.resolve("Ghost", layer);
    CHECK(res.channel == nullptr);
    CHECK(res.trace.summary() == arrows("'Ghost' -> missing (no candidate of alias 'Ghost' resolved)"));

    res = r.resolve("Nope", layer);
    CHECK(res.channel == nullptr);
    CHECK(res.trace.summary() == arrows("'Nope' -> missing"));
}

TEST_CASE("resolver: load-time mappings rename and stamp units (rule 16)", "[unit]") {
    const auto& reg = test::shippedUnits();
    DataLayer layer = sampleLayer();
    const ChannelId before = layer.find("GPS_Spd")->desc.id;
    MappingTable m;
    m.rows.push_back(MappingRow{"gps_spd", "Ground Speed", reg.find("km/h"), false});
    m.rows.push_back(MappingRow{"Engine RPM", "Engine Speed", std::nullopt, false});
    m.rows.push_back(MappingRow{"WheelSP_FR", "Front Right", reg.find("mph"), true}); // has a unit already
    const auto outcomes = applyMappings(layer, m, test::testSource(), &reg);
    REQUIRE(outcomes.size() == 3);

    const Channel* gps = layer.find("Ground Speed");
    REQUIRE(gps != nullptr);
    CHECK(gps->desc.loggedName == "GPS_Spd");
    CHECK(gps->desc.source == ChannelSource::Mapped);
    CHECK(gps->desc.unit == reg.find("km/h").value());
    CHECK(reg.quantity(gps->desc.quantity).name == "Speed");
    CHECK(gps->desc.id != before);
    CHECK(gps->desc.id ==
          channelIdOther(ChannelSource::Mapped, test::testSource(), "Normal", "Ground Speed"));

    const Channel* rpm = layer.find("Engine Speed");
    REQUIRE(rpm != nullptr);
    CHECK(rpm->desc.source == ChannelSource::Logged); // no unit stamped
    CHECK(layer.find("Engine RPM") == nullptr);

    const Channel* fr = layer.find("Front Right");
    REQUIRE(fr != nullptr);
    CHECK(fr->desc.unit == reg.find("km/h").value()); // unitlessOnly kept the existing unit
    CHECK(outcomes[0].to == "Front Right");           // outcomes follow channel order
    CHECK_FALSE(outcomes[0].unitStamped);
    CHECK(outcomes[2].to == "Ground Speed");
    CHECK(outcomes[2].unitStamped);

    // The old spelling resolves through the mapping; the new name shows the load-time rename quietly.
    const ChannelResolver r(m, {}, &reg);
    auto res = r.resolve("GPS_Spd", layer);
    REQUIRE(res.channel == gps);
    CHECK(res.trace.summary() == arrows("'GPS_Spd' -> mapping -> 'Ground Speed' (km/h)"));
    res = r.resolve("Ground Speed", layer);
    REQUIRE(res.channel == gps);
    CHECK(res.trace.steps.back().note == "renamed at load");
    CHECK(res.trace.summary() == "'Ground Speed' (km/h)");
}

TEST_CASE("resolver: step kind names", "[unit]") {
    CHECK(resolutionStepKindName(ResolutionStep::Kind::Dictionary) == "dictionary");
    CHECK(resolutionStepKindName(ResolutionStep::Kind::CaseFold) == "case-fold");
    CHECK(resolutionStepKindName(ResolutionStep::Kind::LoggedName) == "logged name");
    CHECK(resolutionStepKindName(ResolutionStep::Kind::Missing) == "missing");
}
