// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SchemaRegistry (02 sections 6.1 and 6.6, D-072): every schemas/*.schema.json loads; validate() reports the
// JSON pointer of the first error and lists every error; schemaOf() reads the three "$schema" forms; every
// schema example agrees with the registry (the same examples tools/check_schemas.py checks).

#include "test_support.hpp"

#include <esses/project/schema_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::project;
namespace pt = esses::project::test;

namespace {

JsonDoc doc(std::string_view text) {
    auto d = JsonDoc::parse(text);
    REQUIRE(d.ok());
    return std::move(d).value();
}

} // namespace

TEST_CASE("schema registry: the process registry holds every shipped schema", "[unit]") {
    const SchemaRegistry& registry = SchemaRegistry::instance();
    const std::vector<std::string> ids = registry.ids();
    // Every schema of the source tree (the registry may read the build tree's copy of the directory).
    for (const auto& path : pt::jsonFiles(pt::repoRoot() / "schemas")) {
        const std::string name = path.filename().string();
        const std::string id = name.substr(0, name.size() - std::string(".schema.json").size());
        INFO(id);
        CHECK(std::find(ids.begin(), ids.end(), id) != ids.end());
    }
    for (const char* id : {"project-1", "workbook-1", "overrides-1", "aliases-1", "mappings-1", "maths-1",
                           "journal-1", "sidecar-1", "scheme-1", "keymap-1", "profile-1"}) {
        INFO(id);
        CHECK(std::find(ids.begin(), ids.end(), id) != ids.end());
    }
    CHECK_FALSE(SchemaRegistry::findSchemasDir().empty());
    CHECK(SchemaRegistry::findSchemasDir(pt::repoRoot() / "data-that-does-not-exist") ==
          SchemaRegistry::findSchemasDir());
}

TEST_CASE("schema registry: every valid example validates and every invalid one fails at its pointer",
          "[unit]") {
    const SchemaRegistry& registry = SchemaRegistry::instance();
    std::size_t valid = 0;
    std::size_t invalid = 0;
    for (const std::string& id : registry.ids()) {
        const auto dir = pt::repoRoot() / "schemas/examples" / id;
        if (!std::filesystem::exists(dir / "valid") || !std::filesystem::exists(dir / "expect.json")) {
            continue; // tools/check_schemas.py requires both for every schema of the source tree
        }
        for (const auto& path : pt::jsonFiles(dir / "valid")) {
            INFO(path.string());
            CHECK(registry.validate(doc(pt::readAll(path)), id).ok());
            ++valid;
        }
        const auto expect = nlohmann::json::parse(pt::readAll(dir / "expect.json"));
        for (const auto& path : pt::jsonFiles(dir / "invalid")) {
            INFO(path.string());
            const auto r = registry.validate(doc(pt::readAll(path)), "urn:esses:" + id);
            REQUIRE_FALSE(r.ok());
            CHECK(r.error().code == ErrorCode::Invalid);
            CHECK(r.error().context.detail ==
                  expect.at(path.filename().string()).at("pointer").get<std::string>());
            CHECK(r.error().context.schemaId == "urn:esses:" + id);
            ++invalid;
        }
    }
    CHECK(valid >= 50);
    CHECK(invalid >= 180);
}

TEST_CASE("schema registry: schema id forms, unknown ids and the error message", "[unit]") {
    const SchemaRegistry& registry = SchemaRegistry::instance();
    const JsonDoc mappings =
        doc(R"({"$schema": "urn:esses:mappings-1", "schemaVersion": 1, "mappings": []})");
    for (const char* id :
         {"mappings-1", "urn:esses:mappings-1", "esses:mappings-1",
          "https://example.org/schemas/mappings-1.schema.json", "https://example.org/mappings-1.json"}) {
        INFO(id);
        CHECK(registry.validate(mappings, id).ok());
    }
    for (const char* id :
         {"mappings-2", "nothing-1", "urn:esses:Mappings-1", "", "mappings", "ftp://x/mappings-1.json"}) {
        INFO(id);
        const auto r = registry.validate(mappings, id);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::NotFound);
        CHECK(r.error().context.schemaId == std::string(id));
    }
    const JsonDoc twoErrors = doc(R"({"$schema": "urn:esses:mappings-1", "schemaVersion": 0,)"
                                  R"( "mappings": [{"from": "", "to": "B"}]})");
    const auto r = registry.validate(twoErrors, "mappings-1");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.starts_with("2 errors against mappings-1: at '"));
    CHECK(r.error().message.find("'/mappings/0/from'") != std::string::npos);
    CHECK(r.error().message.find("'/schemaVersion'") != std::string::npos);
}

TEST_CASE("schema registry: schemaOf reads the id, the major and the minor", "[unit]") {
    const SchemaRegistry& registry = SchemaRegistry::instance();
    auto ref = registry.schemaOf(doc(R"({"$schema": "urn:esses:sidecar-1", "schemaVersion": 3})"));
    REQUIRE(ref.ok());
    CHECK(*ref == SchemaRef{"sidecar", 1, 3});
    ref = registry.schemaOf(doc(R"({"$schema": "esses:builtin-esses.corrected_speed_distance-1"})"));
    REQUIRE(ref.ok());
    CHECK(*ref == SchemaRef{"builtin-esses.corrected_speed_distance", 1, 0});
    ref = registry.schemaOf(doc(R"({"$schema": "https://esses.example/s/project-12.schema.json"})"));
    REQUIRE(ref.ok());
    CHECK(*ref == SchemaRef{"project", 12, 0});
    for (const char* bad : {R"({"schemaVersion": 1})", R"({"$schema": 1})", R"([1])",
                            R"({"$schema": "http://json-schema.org/draft-07/schema#"})",
                            R"({"$schema": "urn:esses:project"})", R"({"$schema": "urn:esses:project-0"})"}) {
        INFO(bad);
        const auto r = registry.schemaOf(doc(bad));
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Invalid);
    }
}

TEST_CASE("schema registry: loading a directory reports unusable schemas and keeps the rest", "[unit]") {
    const pt::TempDir dir("schemas");
    pt::writeAll(dir.path() / "good-1.schema.json",
                 R"({"$schema": "http://json-schema.org/draft-07/schema#", "$id": "urn:esses:good-1",)"
                 R"( "type": "object", "required": ["a"]})");
    pt::writeAll(dir.path() / "wrong-id-1.schema.json",
                 R"({"$schema": "http://json-schema.org/draft-07/schema#", "$id": "urn:esses:other-1"})");
    pt::writeAll(dir.path() / "broken-1.schema.json", "{");
    pt::writeAll(dir.path() / "notes.json", "{}"); // not a schema file name: ignored
    SchemaRegistry registry;
    const auto loaded = registry.loadDirectory(dir.path());
    REQUIRE_FALSE(loaded.ok());
    CHECK(loaded.error().code == ErrorCode::Format);
    CHECK(loaded.error().message.find("wrong-id-1.schema.json") != std::string::npos);
    CHECK(loaded.error().message.find("broken-1.schema.json") != std::string::npos);
    CHECK(registry.ids() == std::vector<std::string>{"good-1"});
    CHECK(registry.validate(doc(R"({"a": 1})"), "good-1").ok());
    const auto missing = registry.validate(doc(R"({})"), "good-1");
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().context.detail == "");

    SchemaRegistry empty;
    const auto io = empty.loadDirectory(dir.path() / "absent");
    REQUIRE_FALSE(io.ok());
    CHECK(io.error().code == ErrorCode::Io);
    CHECK(empty.ids().empty());
}
