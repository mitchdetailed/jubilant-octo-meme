// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathModuleIo (03 section 4.1, 02 sections 6.5-6.6): maths-1 <-> MathModule. The schema examples load, the
// written form is stable (save(load(save(load(f)))) == save(load(f))) and validates against the maths-1
// schema, unknown top-level members and x-i2 bags survive, numbers keep their units, and maths-1 rules
// beyond the schema (unknown units, duplicate enumeration values) are refused with their JSON pointer.

#include "lang_test_support.hpp"

#include <esses/maths/math_module_io.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::maths;
using namespace esses::maths::test;

namespace {

std::string readText(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}

std::filesystem::path examples() {
    return repoRoot() / "schemas" / "examples" / "maths-1";
}

// Validates against schemas/maths-1.schema.json; the message of the first violation, "" when valid.
std::string schemaViolation(const std::string& text) {
    static const nlohmann::json schema =
        nlohmann::json::parse(readText(repoRoot() / "schemas" / "maths-1.schema.json"));
    nlohmann::json_schema::json_validator validator;
    validator.set_root_schema(schema);
    try {
        validator.validate(nlohmann::json::parse(text));
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

MathModule parseModule(const std::string& text, std::vector<std::string>* logOut = nullptr) {
    std::vector<std::string> log;
    Result<MathModule> m = MathModuleIo::parse(text, *shippedUnits(), log);
    if (!m) {
        UNSCOPED_INFO(m.error().message);
    }
    REQUIRE(m.ok());
    if (logOut != nullptr) {
        *logOut = log;
    }
    return std::move(m).value();
}

std::string written(const MathModule& m) {
    Result<std::string> t = MathModuleIo::toJson(m, *shippedUnits());
    REQUIRE(t.ok());
    return std::move(t).value();
}

Error parseError(const std::string& text) {
    std::vector<std::string> log;
    Result<MathModule> m = MathModuleIo::parse(text, *shippedUnits(), log);
    REQUIRE_FALSE(m.ok());
    return m.error();
}

} // namespace

TEST_CASE("maths-1: the full example loads with every member", "[unit]") {
    const MathModule m = parseModule(readText(examples() / "valid" / "full.json"));
    CHECK(m.id == "Chassis");
    CHECK(m.enabled);
    CHECK(m.condition == "'Vehicle Id' == \"GT3-07\"");
    REQUIRE(m.constants.size() == 2);
    CHECK(m.constants[1].value.v == 2.7);
    CHECK(m.constants[1].value.unit == unitOf("m"));
    CHECK_FALSE(m.constants[1].enabled);
    CHECK(m.constants[1].comment == "spare");
    REQUIRE(m.enumerations.size() == 1);
    CHECK(m.enumerations[0].entries.size() == 3);
    REQUIRE(m.items.size() == 9);
    const MathItem& yaw = m.items[0];
    CHECK(yaw.kind == ItemKind::Expression);
    CHECK(yaw.resultUnit == unitOf("rad/s"));
    CHECK(yaw.display.unit == unitOf("deg/s"));
    CHECK(yaw.display.colourIndex == 3);
    CHECK(yaw.rate.automatic);
    CHECK(shippedUnits()->quantity(yaw.quantity).name == "Angular Speed"); // "Angular Velocity" is an alias
    CHECK(m.items[1].rate.rate == Rate::fromHz(20));
    CHECK(m.items[1].enumeration == "Balance");
    CHECK(m.items[1].interp == store::Interp::Previous);
    CHECK(m.items[2].rate.rate == Rate::fromHz(50)); // a plain Hz number
    CHECK(m.items[2].precision == "double");
    CHECK(m.items[3].kind == ItemKind::Filter);
    CHECK(m.items[3].window.points == 6);
    CHECK(m.items[4].offset.v == -0.3);
    CHECK(m.items[4].offset.unit == shippedUnits()->find("bar"));
    CHECK(m.items[5].table.x.breakpoints.size() == 5);
    CHECK(m.items[5].table.values.size() == 5);
    CHECK(m.items[6].kind == ItemKind::Table3D);
    CHECK(m.items[6].table.values.size() == 12);
    CHECK(m.items[6].table.outOfRange == TableSpec::OutOfRange::Invalid);
    CHECK(m.items[7].kind == ItemKind::Builtin);
    CHECK(m.items[7].moduleId == "esses.corrected_speed_distance");
    CHECK_FALSE(m.items[7].settings.isNull());
    CHECK(m.items[8].kind == ItemKind::Regression);
    CHECK_FALSE(m.items[8].xI2.isNull());
}

TEST_CASE("maths-1: the written form is stable, canonical and schema-valid for every valid example",
          "[unit]") {
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(examples() / "valid")) {
        INFO(entry.path().filename().string());
        const MathModule m = parseModule(readText(entry.path()));
        const std::string once = written(m);
        CHECK(written(parseModule(once)) == once);
        CHECK(parseModule(once) == m);
        CHECK(schemaViolation(once).empty());
        CHECK(JsonDoc::parse(once)->canonical() == once);
        CHECK(once.rfind("{\n  \"$schema\": \"urn:esses:maths-1\",\n  \"schemaVersion\": 1,", 0) == 0);
        ++files;
    }
    CHECK(files >= 3);
}

TEST_CASE("maths-1: unknown top-level members, x-i2 bags and settings survive", "[unit]") {
    const MathModule local = parseModule(readText(examples() / "valid" / "local.json"));
    CHECK(local.id == "local");
    const std::string out = written(local);
    CHECK(nlohmann::json::parse(out).at("futureMember") == true);

    const MathModule full = parseModule(readText(examples() / "valid" / "full.json"));
    const nlohmann::json back = nlohmann::json::parse(written(full));
    CHECK(back.at("items").at(8).at("x-i2") == nlohmann::json{{"Flags", "3"}});
    CHECK(back.at("items").at(7).at("settings") == nlohmann::json{{"method", "fastestWheel"}});
    // table3d values come back as rows of len(y).
    CHECK(back.at("items").at(6).at("values").at("data").at(0) == nlohmann::json{41, 43, 45, 47});
}

TEST_CASE("maths-1: a module built in code round-trips byte for byte", "[unit]") {
    MathModule m;
    m.id = "user";
    m.precision = "double";
    m.constants.push_back(MathConstant{"Wheelbase", UnitValue{102.0, unitOf("in")}, true, ""});
    MathItem e;
    e.name = "Speed x2";
    e.expr = "'Speed'[km/h] * 2";
    e.rate.automatic = false;
    e.rate.rate = Rate{25, 2};
    e.resultUnit = unitOf("km/h");
    e.quantity = *shippedUnits()->quantityByName("Speed");
    e.display.unit = unitOf("km/h");
    e.display.dps = 1;
    e.comment = "doubled";
    m.items.push_back(e);
    MathItem f;
    f.kind = ItemKind::Filter;
    f.name = "Lat G";
    f.window.seconds = 0.25;
    m.items.push_back(f);
    MathItem so;
    so.kind = ItemKind::ScaleOffset;
    so.name = "Brake";
    so.scale = 1.02;
    so.offset = UnitValue{-0.3, unitOf("bar g")};
    m.items.push_back(so);

    const std::string text = written(m);
    const MathModule back = parseModule(text);
    CHECK(back == m);
    CHECK(written(back) == text);
    CHECK(schemaViolation(text).empty());
    const nlohmann::json j = nlohmann::json::parse(text);
    CHECK(j.at("constants").at(0).at("value") == nlohmann::json{{"unit", "in"}, {"v", 102}});
    CHECK(j.at("items").at(0).at("rate") == nlohmann::json{{"den", 2}, {"num", 25}});
}

TEST_CASE("maths-1: rate forms, lenient notes and refusals with their JSON pointer", "[unit]") {
    const std::string head = R"({"$schema": "urn:esses:maths-1", "schemaVersion": 1, "id": "m", "items": [)";
    const auto item = [&](const std::string& body) { return head + body + "]}"; };
    CHECK(parseModule(item(R"({"kind": "expression", "name": "a", "expr": "1", "rate": 12.5})"))
              .items[0]
              .rate.rate == (Rate{25, 2}));
    CHECK(parseModule(
              item(R"({"kind": "expression", "name": "a", "expr": "1", "rate": {"num": 100, "den": 3}})"))
              .items[0]
              .rate.rate == (Rate{100, 3}));

    std::vector<std::string> log;
    (void)parseModule(R"({"id": "m"})", &log);
    REQUIRE(log.size() == 1);
    CHECK(log[0].find("no $schema") != std::string::npos);
    (void)parseModule(R"({"$schema": "urn:esses:maths-1", "schemaVersion": 3, "id": "m"})", &log);
    REQUIRE(log.size() == 1);
    CHECK(log[0].find("newer") != std::string::npos);

    Error e = parseError(
        item(R"({"kind": "expression", "name": "a", "expr": "1", "resultUnit": "furlong/fortnight"})"));
    CHECK(e.code == ErrorCode::Invalid);
    CHECK(e.context.detail == "/items/0/resultUnit");
    e = parseError(item(R"({"kind": "frobnicate", "name": "a"})"));
    CHECK(e.context.detail == "/items/0/kind");
    e = parseError(item(R"({"kind": "expression", "name": "a'b", "expr": "1"})"));
    CHECK(e.context.detail == "/items/0/name");
    e = parseError(item(R"({"kind": "expression", "name": "a", "expr": "1", "colour": 3})"));
    CHECK(e.code == ErrorCode::Invalid);
    e = parseError(head + R"(], "enumerations": [{"name": "E", "entries": [{"name": "a", "value": 1},
                   {"name": "b", "value": 1}]}]})");
    CHECK(e.context.detail == "/enumerations/0/entries/1/value");
    e = parseError(R"({"$schema": "urn:esses:maths-2", "schemaVersion": 1, "id": "m"})");
    CHECK(e.code == ErrorCode::Unsupported);
    e = parseError(R"({"$schema": "urn:esses:workbook-1", "schemaVersion": 1, "id": "m"})");
    CHECK(e.code == ErrorCode::Invalid);
    e = parseError(
        R"({"$schema": "urn:esses:maths-1", "id": "m", "constants": [{"name": "c", "value": 3}]})");
    CHECK(e.context.detail == "/constants/0/value");
    CHECK(parseError("{ not json").code == ErrorCode::Format);
}

TEST_CASE("maths-1: save writes atomically and refuses a transient module", "[unit]") {
    const TempDir tmp("io"); // a directory of its own: processes running this test at once never share it
    const std::filesystem::path& dir = tmp.path();
    MathModule m;
    m.id = "user";
    MathItem e;
    e.name = "x";
    e.expr = "1";
    m.items.push_back(e);
    const std::filesystem::path file = dir / "user.maths.json";
    REQUIRE(MathModuleIo::save(m, file, *shippedUnits()).ok());
    std::vector<std::string> log;
    Result<MathModule> back = MathModuleIo::load(file, *shippedUnits(), log);
    REQUIRE(back.ok());
    CHECK(*back == m);
    MathModule transient = m;
    transient.id = std::string(MathModule::kTransientId);
    CHECK_FALSE(MathModuleIo::save(transient, dir / "t.maths.json", *shippedUnits()).ok());
    Result<MathModule> missing = MathModuleIo::load(dir / "absent.maths.json", *shippedUnits(), log);
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().code == ErrorCode::Io);
}
