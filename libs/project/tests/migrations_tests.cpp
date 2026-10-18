// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Migrations (02 section 6.6) and the one shipped step, project-1 1 -> 2 (D-162: Data-window filter keys),
// through Migrations::upgrade and through the loader (migrate -> parse -> validate; dirty; backup on the
// first save); and R-38: a document of a newer minor keeps its version on save, and one this build cannot
// read in full is refused as Unsupported instead of being overwritten with a loss.

#include "test_support.hpp"

#include <esses/project/migrations.hpp>
#include <esses/project/project_file.hpp>
#include <esses/project/scheme.hpp>
#include <esses/project/workbook_file.hpp>

#include <catch2/catch_test_macros.hpp>

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

constexpr std::string_view kHexA = "9F3C0A1B2C3D4E5F60718293A4B5C6D7";
constexpr std::string_view kHexB = "0123456789abcdef0123456789abcdef";

std::string projectV1(std::string_view filters, std::string_view extra = {}) {
    return std::string(R"({"$schema": "urn:esses:project-1", "schemaVersion": 1, "name": "P",)") +
           std::string(extra) + R"( "dataWindow": {"applyToAllSources": false, "filters": )" +
           std::string(filters) + "}}";
}

} // namespace

TEST_CASE("migrations: upgrade runs the steps in sequence and stamps each version", "[unit]") {
    Migrations m;
    m.add("thing-1", 1, [](JsonDoc d) -> Result<JsonDoc> {
        auto j = nlohmann::json::parse(d.compact());
        j["renamed"] = j["old"];
        j["x-esses-legacy"] = {{"old", j["old"]}};
        j.erase("old");
        return JsonDoc::parse(j.dump());
    });
    m.add("thing-1", 2, [](JsonDoc d) -> Result<JsonDoc> { return d; });
    std::vector<std::string> log;
    const auto up = m.upgrade(doc(R"({"$schema": "urn:esses:thing-1", "schemaVersion": 1, "old": 5})"),
                              "thing-1", 3, log);
    REQUIRE(up.ok());
    CHECK(up->compact() ==
          R"({"$schema":"urn:esses:thing-1","schemaVersion":3,"renamed":5,"x-esses-legacy":{"old":5}})");
    CHECK(log == std::vector<std::string>{"thing-1: migrated schemaVersion 1 -> 2 (02 section 6.6)",
                                          "thing-1: migrated schemaVersion 2 -> 3 (02 section 6.6)"});

    log.clear();
    const auto same = m.upgrade(doc(R"({"schemaVersion": 3})"), "thing-1", 3, log);
    REQUIRE(same.ok());
    CHECK(log.empty());
    const auto absent = m.upgrade(doc(R"({"old": 1})"), "thing-1", 2, log); // no schemaVersion: version 1
    REQUIRE(absent.ok());
    CHECK(absent->schemaVersion() == 2);

    const auto newer = m.upgrade(doc(R"({"schemaVersion": 4})"), "thing-1", 3, log);
    REQUIRE_FALSE(newer.ok());
    CHECK(newer.error().code == ErrorCode::Unsupported);
    CHECK(newer.error().context.schemaId == "urn:esses:thing-1");
    const auto gap = m.upgrade(doc(R"({"schemaVersion": 1})"), "thing-1", 5, log);
    REQUIRE_FALSE(gap.ok());
    CHECK(gap.error().code == ErrorCode::Unsupported);
    CHECK(gap.error().message == "no migration step thing-1 3 -> 4");
    const auto notObject = m.upgrade(doc("[1]"), "thing-1", 2, log);
    REQUIRE_FALSE(notObject.ok());
    CHECK(notObject.error().code == ErrorCode::Invalid);

    m.add("bad-1", 1, [](JsonDoc) -> Result<JsonDoc> { return fail(ErrorCode::Invalid, "cannot"); });
    const auto failed = m.upgrade(doc(R"({"schemaVersion": 1})"), "bad-1", 2, log);
    REQUIRE_FALSE(failed.ok());
    CHECK(failed.error().message == "bad-1 1 -> 2: cannot");
    m.add("bad-1", 1, [](JsonDoc) -> Result<JsonDoc> { return JsonDoc::parse("[]"); }); // replaces the step
    const auto array = m.upgrade(doc(R"({"schemaVersion": 1})"), "bad-1", 2, log);
    REQUIRE_FALSE(array.ok());
    CHECK(array.error().code == ErrorCode::Internal);
}

TEST_CASE("migrations: project-1 1 -> 2 rewrites bare hex filter keys and keeps every other key", "[unit]") {
    const std::string filters =
        std::string(R"({"default": {"kind": "all"}, ")") + std::string(kHexA) +
        R"(": {"kind": "fastest"}, "xxh3:)" + std::string(kHexB) + R"(": {"kind": "selected"}, ")" +
        std::string(kHexB) + R"(": {"kind": "fastest3"}, "Lap view": {"kind": "lapNumbers", "laps": "3"}})";
    std::vector<std::string> log;
    const auto up = Migrations::instance().upgrade(
        doc(projectV1(filters, R"( "x-esses-legacy": {"older": true},)")), "project-1", 2, log);
    REQUIRE(up.ok());
    const auto j = nlohmann::json::parse(up->compact());
    CHECK(j.at("schemaVersion") == 2);
    const auto& f = j.at("dataWindow").at("filters");
    CHECK(f.size() == 3);
    CHECK(f.at("default").at("kind") == "all");
    CHECK(f.at("xxh3:9f3c0a1b2c3d4e5f60718293a4b5c6d7").at("kind") == "fastest"); // upper case folded
    CHECK(f.at("xxh3:" + std::string(kHexB)).at("kind") == "selected");           // the prefixed twin wins
    const auto& legacy = j.at("x-esses-legacy");
    CHECK(legacy.at("older") == true); // an existing bag is extended, not replaced
    CHECK(legacy.at("dataWindowFilters").at(std::string(kHexB)).at("kind") == "fastest3");
    CHECK(legacy.at("dataWindowFilters").at("Lap view").at("laps") == "3");
    CHECK(log.size() == 1);

    // Nothing to rewrite: only the version changes.
    const auto plain =
        Migrations::instance().upgrade(doc(R"({"schemaVersion": 1, "name": "P"})"), "project-1", 2, log);
    REQUIRE(plain.ok());
    CHECK(plain->compact() == R"({"schemaVersion":2,"name":"P"})");
    const auto badBag = Migrations::instance().upgrade(
        doc(projectV1(R"({"x": {"kind": "all"}})", R"( "x-esses-legacy": [1],)")), "project-1", 2, log);
    REQUIRE_FALSE(badBag.ok());
    CHECK(badBag.error().context.detail == "/x-esses-legacy");
}

TEST_CASE("migrations: a version-1 project.json is migrated on load, dirty, and backed up on save",
          "[unit]") {
    const pt::TempDir dir("migrate-project");
    const auto file = dir.path() / "project.json";
    const std::string original =
        projectV1(std::string(R"({")") + std::string(kHexB) + R"(": {"kind": "all"}})");
    pt::writeAll(file, original);
    std::vector<std::string> log;
    auto p = ProjectFile::load(file, log);
    REQUIRE(p.ok());
    CHECK(p->dirty);
    CHECK(p->schemaVersion == 2);
    CHECK(std::find(log.begin(), log.end(), "project-1: migrated schemaVersion 1 -> 2 (02 section 6.6)") !=
          log.end());
    CHECK(
        nlohmann::json::parse(p->dataWindow.compact()).at("filters").contains("xxh3:" + std::string(kHexB)));
    CHECK(pt::readAll(file) == original); // loading never writes
    REQUIRE(p->save(file).ok());
    CHECK(pt::schemaErrors(pt::readAll(file), "project-1").empty());
    // The explicit save kept the version-1 bytes in backup/ (02 section 6.6 rule 2, D-070).
    const auto backups = pt::jsonFiles(dir.path() / "backup");
    REQUIRE(backups.size() == 1);
    CHECK(pt::readAll(backups[0]) == original);
    const auto again = ProjectFile::load(file, log);
    REQUIRE(again.ok());
    CHECK_FALSE(again->dirty);
}

TEST_CASE("migrations: a newer minor keeps its version and is refused when it cannot be read in full (R-38)",
          "[unit]") {
    std::vector<std::string> log;
    // Readable in full: only a new top-level member, which is preserved.
    const auto scheme = Scheme::parse(pt::readAll(pt::repoRoot() / "data/schemes/light.scheme.json")
                                          .replace(0, 1, R"({"laterTopLevel": 1,)"),
                                      log);
    REQUIRE(scheme.ok());
    std::string newer = scheme->toJson();
    newer.replace(newer.find("\"schemaVersion\": 1"), 18, "\"schemaVersion\": 3");
    log.clear();
    const auto read3 = Scheme::parse(newer, log);
    REQUIRE(read3.ok());
    CHECK(read3->schemaVersion == 3);
    CHECK(read3->toJson() == newer); // never written back as 1
    CHECK(log.size() == 1);

    // Not readable in full: a member of a closed record this build does not know.
    const std::string wb = R"({"$schema": "urn:esses:workbook-1", "schemaVersion": 2, "id": )"
                           R"("b7c1d2e3-4f5a-4b6c-8d7e-9f0a1b2c3d4e", "name": "W", "worksheets": [{"id": )"
                           R"("6e1f0a1b-2c3d-4e5f-8a7b-9c0d1e2f3a4b", "name": "S", "newSheetMember": 1}]})";
    const auto refused = WorkbookFile::parse(wb, "W.workbook.json", log);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().code == ErrorCode::Unsupported);
    CHECK(refused.error().context.schemaId == "urn:esses:workbook-1");
    CHECK(refused.error().message.starts_with("workbook-1 schemaVersion 2 was written by a newer build"));
    // The same content at the current minor is simply invalid.
    std::string current = wb;
    current.replace(current.find("\"schemaVersion\": 2"), 18, "\"schemaVersion\": 1");
    const auto invalid = WorkbookFile::parse(current, "W.workbook.json", log);
    REQUIRE_FALSE(invalid.ok());
    CHECK(invalid.error().code == ErrorCode::Invalid);
    // Another major is refused before anything is read.
    const auto major =
        ProjectFile::parse(R"({"$schema": "urn:esses:project-2", "schemaVersion": 1, "name": "P"})", log);
    REQUIRE_FALSE(major.ok());
    CHECK(major.error().code == ErrorCode::Unsupported);
}
