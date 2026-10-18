// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The channel documents of 02 section 6.3 (D-013): overrides-1, aliases-1 and mappings-1 read, written and
// validated, the alias cycle rule of 02 section 4.6 rule 17 and the typed values the editors and the resolver
// consume.

#include "test_support.hpp"

#include <esses/base/casefold.hpp>
#include <esses/project/aliases.hpp>
#include <esses/project/channel_overrides.hpp>
#include <esses/project/mappings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace esses;
using namespace esses::project;
namespace pt = esses::project::test;

namespace {

std::filesystem::path example(const char* schemaId, const char* kind, const char* file) {
    return pt::repoRoot() / "schemas/examples" / schemaId / kind / file;
}

} // namespace

TEST_CASE("overrides: the 02 section 6.3 example reads into typed entries and writes back", "[unit]") {
    const std::string text = pt::readAll(example("overrides-1", "valid", "full.json"));
    std::vector<std::string> log;
    const auto doc = ChannelOverrides::parse(text, log);
    REQUIRE(doc.ok());
    CHECK(log.empty());
    REQUIRE(doc->channels.size() == 4);
    const auto* rpm = doc->find("engine rpm");
    REQUIRE(rpm != nullptr);
    CHECK(rpm->displayUnit == "rpm");
    CHECK(rpm->dps == 0);
    CHECK(rpm->colourIndex == 2);
    CHECK(rpm->autoScale == false);
    REQUIRE(rpm->scaleMax.has_value());
    CHECK(rpm->scaleMax->v == 8000.0);
    CHECK(rpm->scaleMax->symbol == "rpm");
    CHECK(rpm->interp == "Linear");
    CHECK(rpm->warn.compact() == R"({"max":{"unit":"rpm","v":7800}})");
    const auto* gear = doc->find("Gear");
    REQUIRE(gear != nullptr);
    CHECK(gear->scaleMin->symbol.empty()); // unitless values keep the empty unit
    const auto* oil = doc->find("Oil Temp");
    REQUIRE(oil != nullptr);
    CHECK(oil->unknown.compact() == R"({"laterOption":{"kept":true}})"); // members of a later version survive
    CHECK(doc->toJson() == text); // canonical in, byte-identical out (AC-M2-07)
    CHECK(pt::schemaErrors(doc->toJson(), "overrides-1").empty());
}

TEST_CASE("overrides: an edited document saves canonically and loads equal", "[unit]") {
    ChannelOverrides doc;
    ChannelOverrides::Entry speed;
    speed.displayUnit = "km/h";
    speed.dps = 1;
    speed.scaleMin = model::Quantityd{0.0, units::kNoUnit, 2, "km/h"};
    speed.scaleMax = model::Quantityd{312.5, units::kNoUnit, 2, "km/h"};
    speed.interp = "Closest";
    doc.set("Corr Speed", speed);
    const pt::TempDir dir("overrides");
    const auto file = dir.path() / "overrides.json";
    REQUIRE(doc.save(file).ok());
    const std::string bytes = pt::readAll(file);
    CHECK(bytes == doc.toJson());
    CHECK(pt::schemaErrors(bytes, "overrides-1").empty());
    std::vector<std::string> log;
    const auto back = ChannelOverrides::load(file, log);
    REQUIRE(back.ok());
    CHECK(*back == doc);
    CHECK(back->toJson() == bytes);
}

TEST_CASE("overrides: invalid documents fail at the JSON pointer of the error", "[unit]") {
    const auto expect =
        nlohmann::json::parse(pt::readAll(pt::repoRoot() / "schemas/examples/overrides-1/expect.json"));
    std::size_t checked = 0;
    for (const auto& path : pt::jsonFiles(pt::repoRoot() / "schemas/examples/overrides-1/invalid")) {
        INFO(path.filename().string());
        std::vector<std::string> log;
        const auto r = ChannelOverrides::parse(pt::readAll(path), log);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Invalid);
        const std::string pointer = expect.at(path.filename().string()).at("pointer").get<std::string>();
        // The typed reader reports the member itself where the schema reports its parent object.
        CHECK(r.error().context.detail.value_or("?").starts_with(pointer));
        ++checked;
    }
    CHECK(checked >= 7);

    std::vector<std::string> log;
    const auto twice = ChannelOverrides::parse(
        R"({"$schema": "urn:esses:overrides-1", "schemaVersion": 1, "channels": {"Speed": {"dps": 1},)"
        R"( "SPEED": {"dps": 2}}})",
        log);
    REQUIRE(twice.ok()); // kept; lookups take the exact spelling first
    CHECK(twice->find("SPEED")->dps == 2);
    CHECK(twice->find("Speed")->dps == 1);
    CHECK(log.size() == 1);
}

TEST_CASE("aliases: the 02 section 6.3 example, the enable flags and the resolver table", "[unit]") {
    const std::string text = pt::readAll(example("aliases-1", "valid", "full.json"));
    std::vector<std::string> log;
    const auto doc = Aliases::parse(text, log);
    REQUIRE(doc.ok());
    REQUIRE(doc->table.rows.size() == 2);
    const model::AliasRow& wheel = doc->table.rows[0];
    CHECK(wheel.name == "Wheel Speed");
    REQUIRE(wheel.candidates.size() == 3);
    CHECK(wheel.candidates[2].name == "Ground Speed");
    CHECK_FALSE(wheel.candidates[2].enabled);
    CHECK(wheel.colourIndex == 4);
    CHECK(wheel.dps == 1);
    CHECK(doc->table.rows[1].colourIndex == -1); // absent
    CHECK_FALSE(doc->table.rows[1].dps.has_value());
    CHECK(doc->toJson() == text);
    // "Speed" lists itself (its own channel) and "Wheel Speed" (another alias without a way back): no cycle.
    CHECK_FALSE(doc->cycleThrough("Speed").has_value());
    CHECK(log.empty());

    const auto bare = Aliases::parse(pt::readAll(example("aliases-1", "valid", "without-flags.json")), log);
    REQUIRE(bare.ok());
    CHECK(bare->table.rows.at(0).candidates.at(0).enabled); // absent flags: enabled
    CHECK(bare->unknown.compact() == R"({"futureMember":{"a":1}})");
    CHECK(pt::schemaErrors(bare->toJson(), "aliases-1").empty());
}

TEST_CASE("aliases: the loader refuses what the schema cannot express", "[unit]") {
    const auto parse = [](const std::string& rows) {
        std::vector<std::string> log;
        return Aliases::parse(
            R"({"$schema": "urn:esses:aliases-1", "schemaVersion": 1, "aliases": )" + rows + "}", log);
    };
    const auto mismatch = parse(R"([{"name": "A", "candidates": ["x", "y"], "enabled": [true]}])");
    REQUIRE_FALSE(mismatch.ok());
    CHECK(mismatch.error().context.detail == "/aliases/0/enabled");
    const auto twice =
        parse(R"([{"name": "Speed", "candidates": ["x"]}, {"name": "SPEED", "candidates": ["y"]}])");
    REQUIRE_FALSE(twice.ok());
    CHECK(twice.error().context.detail == "/aliases/1/name");
    const auto expect =
        nlohmann::json::parse(pt::readAll(pt::repoRoot() / "schemas/examples/aliases-1/expect.json"));
    for (const auto& path : pt::jsonFiles(pt::repoRoot() / "schemas/examples/aliases-1/invalid")) {
        INFO(path.filename().string());
        std::vector<std::string> log;
        const auto r = Aliases::parse(pt::readAll(path), log);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().context.detail.value_or("?").starts_with(
            expect.at(path.filename().string()).at("pointer").get<std::string>()));
    }
}

TEST_CASE("aliases: cycles are found the way the resolver expands aliases", "[unit]") {
    Aliases doc;
    const auto row = [](std::string name, std::vector<std::string> candidates) {
        model::AliasRow r;
        r.name = std::move(name);
        for (std::string& c : candidates) {
            r.candidates.push_back({std::move(c), true});
        }
        return r;
    };
    doc.table.rows.push_back(row("A", {"A", "B"}));        // "A" is its own channel, not a cycle
    doc.table.rows.push_back(row("B", {"c"}));             // names compare case-insensitively
    doc.table.rows.push_back(row("C", {"Logged C", "a"})); // back to A: A -> B -> C -> A
    doc.table.rows.push_back(row("D", {"Logged D"}));
    doc.table.rows.push_back(row("E", {"B"})); // reaches the cycle without being on it
    CHECK(doc.cycleThrough("a") == "A -> B -> C -> A");
    CHECK(doc.cycleThrough("B") == "B -> C -> A -> B");
    CHECK(doc.cycleThrough("E") == "B -> C -> A -> B");
    CHECK_FALSE(doc.cycleThrough("D").has_value());
    CHECK_FALSE(doc.cycleThrough("not an alias").has_value());
    // A disabled candidate still counts: enabling it later must not create a cycle the editor allowed.
    doc.table.rows[2].candidates[1].enabled = false;
    CHECK(doc.cycleThrough("A") == "A -> B -> C -> A");

    std::vector<std::string> log;
    const auto loaded = Aliases::parse(doc.toJson(), log);
    REQUIRE(loaded.ok()); // a cycle is reported, not refused: the resolver stops it at run time
    CHECK(log.size() == 4);
}

TEST_CASE("mappings: the 02 section 6.3 example and the loader's uniqueness rule", "[unit]") {
    const std::string text = pt::readAll(example("mappings-1", "valid", "full.json"));
    std::vector<std::string> log;
    const auto doc = Mappings::parse(text, log);
    REQUIRE(doc.ok());
    REQUIRE(doc->rows.size() == 2);
    CHECK(doc->rows[0] == Mappings::Row{"WheelSP_FL", "Wheel Speed FL", std::string("km/h"), true});
    CHECK(doc->rows[1] == Mappings::Row{"GPS Latitudine", "GPS Latitude", std::nullopt, false});
    CHECK(doc->toJson() == text);
    const pt::TempDir dir("mappings");
    REQUIRE(doc->save(dir.path() / "mappings.json").ok());
    const auto back = Mappings::load(dir.path() / "mappings.json", log);
    REQUIRE(back.ok());
    CHECK(*back == *doc);

    const auto twice =
        Mappings::parse(R"({"$schema": "urn:esses:mappings-1", "schemaVersion": 1, "mappings": )"
                        R"([{"from": "rpm", "to": "A"}, {"from": "RPM", "to": "B"}]})",
                        log);
    REQUIRE_FALSE(twice.ok());
    CHECK(twice.error().context.detail == "/mappings/1/from");
    const auto expect =
        nlohmann::json::parse(pt::readAll(pt::repoRoot() / "schemas/examples/mappings-1/expect.json"));
    for (const auto& path : pt::jsonFiles(pt::repoRoot() / "schemas/examples/mappings-1/invalid")) {
        INFO(path.filename().string());
        const auto r = Mappings::parse(pt::readAll(path), log);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().context.detail.value_or("?").starts_with(
            expect.at(path.filename().string()).at("pointer").get<std::string>()));
    }
}

TEST_CASE("channel documents: a missing file is an Io error with the path", "[unit]") {
    const pt::TempDir dir("channels-missing");
    std::vector<std::string> log;
    const auto r = Aliases::load(dir.path() / "aliases.json", log);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Io);
    CHECK(r.error().context.path == dir.path() / "aliases.json");
}
