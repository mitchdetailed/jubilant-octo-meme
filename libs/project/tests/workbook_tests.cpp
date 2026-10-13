// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// WorkbookFile (workbook-1; 02 section 6.4, 04 section 7.3, D-019) and the shipped circuit-default workbook.
#include "test_support.hpp"

#include <esses/base/hash.hpp>
#include <esses/project/shipped_data.hpp>
#include <esses/project/workbook_file.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::project;
namespace pt = esses::project::test;

TEST_CASE("workbook: the shipped circuit-default workbook loads and round-trips byte for byte", "[unit]") {
    const auto path = pt::dataFile(kCircuitDefaultWorkbook);
    std::vector<std::string> log;
    const auto wb = WorkbookFile::load(path, log);
    REQUIRE(wb.ok());
    CHECK(log.empty());
    CHECK_FALSE(wb->dirty);
    CHECK(wb->toJson() == pt::readAll(path));
    CHECK(pt::schemaErrors(wb->toJson(), "workbook-1").empty());

    // Worksheet 0 (esses-render --worksheet 0, AC-M1-05) is a time_distance graph with the default channels
    // next to a values_list.
    REQUIRE(wb->worksheets.size() >= 1);
    const WorksheetDef& sheet = wb->worksheets[0];
    CHECK(wb->activeWorksheet == sheet.name);
    CHECK(sheet.domain == "time");
    CHECK(sheet.linkTo == "Laps");
    REQUIRE(sheet.components.size() == 2);
    const ComponentSpec& graph = sheet.components[0];
    CHECK(graph.type == "time_distance");
    CHECK(graph.known);
    CHECK(graph.activeChannel == "Corr Speed");
    const auto body = nlohmann::json::parse(graph.body.compact());
    REQUIRE(body.at("groups").is_array());
    std::vector<std::string> channels;
    for (const auto& g : body.at("groups")) {
        const auto& edges = g.at("edges");
        CHECK(edges.at(0).get<double>() < edges.at(1).get<double>());
        for (const auto& t : g.at("traces")) {
            channels.push_back(t.at("channel").get<std::string>());
        }
    }
    CHECK(channels ==
          std::vector<std::string>{"Corr Speed", "SPEED", "THROTTLE", "BRAKE", "STEERANGLE", "RPMS", "GEAR"});
    const ComponentSpec& values = sheet.components[1];
    CHECK(values.type == "values_list");
    CHECK(values.known);
    CHECK(values.z > graph.z);
    CHECK(graph.rect.r == values.rect.l); // side by side, no overlap
    CHECK(nlohmann::json::parse(values.body.compact()).at("channels").size() == 9);
    // Every workbook, worksheet and component id is distinct.
    std::vector<std::string> ids{wb->id.str()};
    for (const WorksheetDef& w : wb->worksheets) {
        ids.push_back(w.id.str());
        for (const ComponentSpec& c : w.components) {
            ids.push_back(c.id.str());
        }
    }
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
    REQUIRE(wb->worksheet("Driver") != nullptr);
    CHECK(wb->worksheet("Driver")->domain == "distance");
    CHECK(wb->worksheet("Nope") == nullptr);
}

TEST_CASE("workbook: missing ids are derived from the file name and the indices, and mark it dirty",
          "[unit]") {
    const std::string text = R"({"$schema": "urn:esses:workbook-1", "schemaVersion": 1, "name": "Hand",
        "worksheets": [{"name": "A", "components": [{"type": "time_distance"}, {"type": "values_list"}]}]})";
    std::vector<std::string> log;
    const auto wb = WorkbookFile::parse(text, "Hand.workbook.json", log);
    REQUIRE(wb.ok());
    CHECK(wb->dirty);
    CHECK(wb->id == uuidFromHash(hash128({"workbook", "Hand.workbook.json"})));
    const WorksheetDef& w = wb->worksheets.at(0);
    CHECK(w.id == uuidFromHash(hash128({"worksheet", wb->id.str(), "0"})));
    CHECK(w.components.at(1).id == uuidFromHash(hash128({"component", w.id.str(), "1"})));
    CHECK(log.size() == 4);
    // Stable: the same file name derives the same ids; once saved, nothing is derived any more.
    std::vector<std::string> log2;
    const auto again = WorkbookFile::parse(text, "Hand.workbook.json", log2);
    REQUIRE(again.ok());
    CHECK(again->worksheets == wb->worksheets);
    std::vector<std::string> log3;
    const auto saved = WorkbookFile::parse(wb->toJson(), "Renamed.workbook.json", log3);
    REQUIRE(saved.ok());
    CHECK_FALSE(saved->dirty);
    CHECK(log3.empty());
    CHECK(saved->id == wb->id);
    CHECK(saved->toJson() == wb->toJson());
}

TEST_CASE("workbook: a missing $schema or name is read leniently and logged", "[unit]") {
    std::vector<std::string> log;
    const auto wb = WorkbookFile::parse(R"({"worksheets": [{"name": "A"}]})", "Old Book.workbook.json", log);
    REQUIRE(wb.ok());
    CHECK(wb->name == "Old Book");
    CHECK(wb->dirty);
    CHECK(log.size() == 5); // $schema, schemaVersion, workbook id, name, worksheet id
    // The written form is complete and canonical.
    CHECK(pt::schemaErrors(wb->toJson(), "workbook-1").empty());
    CHECK(wb->toJson().starts_with("{\n  \"$schema\": \"urn:esses:workbook-1\",\n  \"schemaVersion\": 1,\n"));
}

TEST_CASE("workbook: unknown members, unknown component types and x-i2 bags survive", "[unit]") {
    const std::string text = pt::readAll(pt::repoRoot() / "schemas/examples/workbook-1/valid/full.json");
    std::vector<std::string> log;
    const auto wb = WorkbookFile::parse(text, "General.workbook.json", log);
    REQUIRE(wb.ok());
    CHECK(wb->unknown.compact() == R"({"futureMember":{"kept":true}})");
    CHECK(wb->xI2.compact() == R"({"Version":"200"})");
    const auto& comps = wb->worksheets.at(0).components;
    REQUIRE(comps.size() == 3);
    CHECK_FALSE(comps[2].known);
    CHECK(comps[2].body.compact() == R"({"lambdaDelayUs":150000,"x":"Engine RPM","y":"Lambda 1"})");
    CHECK(wb->worksheets.at(0).hotkey == "Ctrl+Shift+1");
    const std::string saved = wb->toJson();
    CHECK(saved.find("\"futureMember\": {\n    \"kept\": true\n  }") != std::string::npos);
    CHECK(pt::schemaErrors(saved, "workbook-1").empty());
    std::vector<std::string> log2;
    const auto reread = WorkbookFile::parse(saved, "General.workbook.json", log2);
    REQUIRE(reread.ok());
    CHECK(*reread == *wb);
    CHECK(reread->toJson() == saved);
}

TEST_CASE("workbook: errors name the JSON pointer; another major is Unsupported", "[unit]") {
    struct Case {
        std::string json;
        ErrorCode code;
        std::string pointer;
    };
    const std::string head = R"({"$schema": "urn:esses:workbook-1", "schemaVersion": 1, "name": "W", )";
    const std::string id = "c1a0b2c3-d4e5-4f60-8172-839405a6b7c8";
    const std::vector<Case> cases = {
        {R"({"$schema": "urn:esses:workbook-2", "schemaVersion": 1, "name": "W", "worksheets": [{"name": "A"}]})",
         ErrorCode::Unsupported, "/$schema"},
        {R"({"$schema": "urn:esses:project-1", "name": "W", "worksheets": [{"name": "A"}]})",
         ErrorCode::Invalid, "/$schema"},
        {head + R"("worksheets": []})", ErrorCode::Invalid, "/worksheets"},
        {head + R"("worksheets": [{"name": "A"}, {"name": "A"}]})", ErrorCode::Invalid, "/worksheets/1"},
        {head + R"("worksheets": [{"name": "A", "domain": "space"}]})", ErrorCode::Invalid,
         "/worksheets/0/domain"},
        {head + R"("worksheets": [{"name": "A", "grid": [10, 100]}]})", ErrorCode::Invalid,
         "/worksheets/0/grid/0"},
        {head + R"("worksheets": [{"name": "A", "tabColour": 1}]})", ErrorCode::Invalid,
         "/worksheets/0/tabColour"},
        {head + R"("worksheets": [{"name": "A", "components": [{"id": ")" + id +
             R"(", "type": "x"}, {"id": ")" + id + R"(", "type": "y"}]}]})",
         ErrorCode::Invalid, "/worksheets/0/components/1"},
        {head + R"("worksheets": [{"name": "A", "components": [{"type": "x", "rect": [0, 0, 2, 1]}]}]})",
         ErrorCode::Invalid, "/worksheets/0/components/0/rect/2"},
        {head + R"("worksheets": [{"name": "A"}], "x-i2": 5})", ErrorCode::Invalid, "/x-i2"},
        {R"({"$schema": "urn:esses:workbook-1", "schemaVersion": 0, "name": "W", "worksheets": [{"name": "A"}]})",
         ErrorCode::Invalid, "/schemaVersion"},
    };
    for (const Case& c : cases) {
        INFO(c.json);
        std::vector<std::string> log;
        const auto r = WorkbookFile::parse(c.json, "w.workbook.json", log);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == c.code);
        CHECK(r.error().context.detail == c.pointer);
    }
}

TEST_CASE("workbook: save writes canonical bytes atomically; load reports errors with the path", "[unit]") {
    pt::TempDir dir("workbook");
    std::vector<std::string> log;
    auto wb = WorkbookFile::load(pt::dataFile(kCircuitDefaultWorkbook), log);
    REQUIRE(wb.ok());
    wb->name = "Copy";
    wb->worksheets[0].components[0].rect.r = 0.7;
    wb->worksheets[0].components[1].rect.l = 0.7;
    const auto target = dir.path() / "Copy.workbook.json";
    REQUIRE(wb->save(target).ok());
    CHECK(pt::readAll(target) == wb->toJson());
    const auto back = WorkbookFile::load(target, log);
    REQUIRE(back.ok());
    CHECK(*back == *wb);

    const auto missing = WorkbookFile::load(dir.path() / "none.workbook.json", log);
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().code == ErrorCode::Io);
    pt::writeAll(dir.path() / "bad.workbook.json", R"({"name": "W", "worksheets": [{"name": 3}]})");
    const auto bad = WorkbookFile::load(dir.path() / "bad.workbook.json", log);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == ErrorCode::Invalid);
    CHECK(bad.error().context.path == dir.path() / "bad.workbook.json");
    CHECK(bad.error().context.detail == "/worksheets/0/name");
}
