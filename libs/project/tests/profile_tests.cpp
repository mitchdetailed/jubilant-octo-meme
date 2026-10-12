// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Profile (profile-1; 02 section 6.9, D-036) and the shipped Circuit profile.
#include "test_support.hpp"

#include <esses/project/profile.hpp>
#include <esses/project/shipped_data.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace esses;
using namespace esses::project;
namespace pt = esses::project::test;

TEST_CASE("profile: the shipped Circuit profile equals circuitDefault and round-trips", "[unit]") {
    const auto path = pt::dataFile(kCircuitProfile);
    std::vector<std::string> log;
    const auto p = Profile::load(path, log);
    REQUIRE(p.ok());
    CHECK(log.empty());
    CHECK(*p == Profile::circuitDefault());
    CHECK(p->toJson() == pt::readAll(path));
    CHECK(Profile::circuitDefault().toJson() == pt::readAll(path));
    CHECK(pt::schemaErrors(p->toJson(), "profile-1").empty());

    CHECK(p->id == "circuit");
    CHECK(p->rules == std::vector<std::string>{"circuit.detail-groups", "circuit.whole-outing",
                                               "circuit.beacon-pair", "circuit.track-sections"});
    REQUIRE(p->defaultView() != nullptr);
    CHECK(p->defaultView()->title == "Laps");
    CHECK(p->defaultView()->tree.back() == "Outings:Laps");
    CHECK(p->defaultWorkbook == "circuit-default");
    REQUIRE(p->findDetail("Stoich Ratio") != nullptr);
    CHECK(p->findDetail("Stoich Ratio")->defaultValue == 14.7);
    CHECK(p->findDetail("Start Lap")->dps == 0);
    CHECK(p->findDetail("Start Lap")->unit.empty());
    CHECK(p->findDetail("Driver")->type == "string");
    CHECK(p->findDetail("Nope") == nullptr);
    CHECK(p->terms.compact() == R"({"editor":"Lap Editor","range":"Lap","ranges":"Laps"})");
    // The shipped workbook the profile names exists.
    CHECK(std::filesystem::exists(pt::dataFile("workbooks/" + p->defaultWorkbook + ".workbook.json")));
}

TEST_CASE("profile: optional members are written only when set; unknown members survive", "[unit]") {
    std::vector<std::string> log;
    const auto minimal =
        Profile::parse(pt::readAll(pt::repoRoot() / "schemas/examples/profile-1/valid/minimal.json"), log);
    REQUIRE(minimal.ok());
    CHECK(minimal->primaryRanges == "Outings:Laps");
    CHECK(minimal->defaultView() == nullptr);
    const std::string out = minimal->toJson();
    CHECK(pt::schemaErrors(out, "profile-1").empty());
    const auto again = Profile::parse(out, log);
    REQUIRE(again.ok());
    CHECK(*again == *minimal);
    CHECK(out.find("\"terms\"") == std::string::npos);

    const auto withUnknown = Profile::parse(
        pt::readAll(pt::repoRoot() / "schemas/examples/profile-1/valid/circuit-with-unknown-member.json"),
        log);
    REQUIRE(withUnknown.ok());
    CHECK(withUnknown->unknown.compact() == R"({"futureMember":{"x":1}})");
    CHECK(withUnknown->toJson() ==
          pt::readAll(pt::repoRoot() / "schemas/examples/profile-1/valid/circuit-with-unknown-member.json"));
    // The Outings view writes neither "default" nor "outingDetail".
    CHECK(withUnknown->toJson().find("\"default\": false") == std::string::npos);
}

TEST_CASE("profile: malformed profiles are Invalid at a JSON pointer", "[unit]") {
    struct Case {
        std::string json;
        std::string pointer;
    };
    const std::string head =
        R"({"$schema": "urn:esses:profile-1", "schemaVersion": 1, "id": "p", "name": "P", )";
    const std::vector<Case> cases = {
        {R"({"$schema": "urn:esses:profile-1", "schemaVersion": 1, "name": "P"})", ""},
        {head + R"("details": [{"id": "Driver", "type": "string", "unit": "km"}]})", "/details/0"},
        {head + R"("details": [{"id": "X", "type": "date"}]})", "/details/0/type"},
        {head + R"("details": [{"id": "X", "type": "number", "dps": 20}]})", "/details/0/dps"},
        {head + R"("details": [{"id": "X", "type": "number"}, {"id": "X", "type": "number"}]})",
         "/details/1"},
        {head + R"("dataViews": [{"title": "Laps"}]})", "/dataViews/0"},
        {head + R"("dataViews": [{"title": "A", "tree": [], "default": true}, {"title": "B", "tree": [],
                   "default": true}]})",
         "/dataViews"},
        {head + R"("dataViews": [{"title": "A", "tree": [], "sort": "time"}]})", "/dataViews/0/sort"},
        {head + R"("rules": ["a", 1]})", "/rules/1"},
        {head + R"("terms": "Lap"})", "/terms"},
    };
    for (const Case& c : cases) {
        INFO(c.json);
        std::vector<std::string> log;
        const auto r = Profile::parse(c.json, log);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Invalid);
        CHECK(r.error().context.detail == c.pointer);
    }
    pt::TempDir dir("profile");
    REQUIRE(Profile::circuitDefault().save(dir.path() / "circuit.profile.json").ok());
    std::vector<std::string> log;
    const auto back = Profile::load(dir.path() / "circuit.profile.json", log);
    REQUIRE(back.ok());
    CHECK(*back == Profile::circuitDefault());
}
