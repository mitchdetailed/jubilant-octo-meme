// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ProjectFile (project-1; 02 sections 6.1-6.2, 06 section 5.5) and the explicit-Save backups (D-070).
#include "test_support.hpp"

#include <esses/project/project_file.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace esses;
using namespace esses::project;
namespace pt = esses::project::test;

namespace {

std::filesystem::path example(const char* name) {
    return pt::repoRoot() / "schemas/examples/project-1/valid" / name;
}

} // namespace

TEST_CASE("project file: every member of the 02 section 6.2 example is read and written back", "[unit]") {
    const std::string text = pt::readAll(example("full.json"));
    std::vector<std::string> log;
    const auto p = ProjectFile::parse(text, log);
    REQUIRE(p.ok());
    CHECK(log.empty());
    CHECK(p->name == "Bathurst 2026");
    CHECK(p->profile == "circuit");
    CHECK_FALSE(p->layoutLocked);
    CHECK(p->mathsOrder == std::vector<std::string>{"esses-circuit", "user"});
    REQUIRE(p->builtinMaths.count("esses.corrected_speed_distance") == 1);
    const BuiltinMathsEntry& csd = p->builtinMaths.at("esses.corrected_speed_distance");
    CHECK(csd.version == 1);
    CHECK(csd.enabled);
    CHECK(nlohmann::json::parse(csd.settings.compact()).at("method") == "fastestWheel");
    CHECK(p->workbooks == std::vector<std::string>{"General", "Chassis"});
    REQUIRE(p->recentSources.size() == 1);
    CHECK(p->recentSources[0].importer == "motec.ld");
    CHECK(p->recentSources[0].openedAt == "2026-10-09T20:50:00+10:00");
    REQUIRE(p->lastSelection.main.has_value());
    CHECK(p->lastSelection.main->name == "Lap 7");
    CHECK(p->lastSelection.main->groupPath == "Outings:Laps");
    CHECK(p->lastSelection.main->source.hash == "xxh3:9f3c0a1b2c3d4e5f60718293a4b5c6d7");
    REQUIRE(p->lastSelection.overlay0.has_value());
    CHECK(p->lastSelection.overlay0->startMarker == "a1b2c3d4-e5f6-5a7b-8c9d-0e1f2a3b4c5d");
    CHECK(p->lastSelection.overlays.empty());
    CHECK(nlohmann::json::parse(p->options.compact()).at("general").at("autosaveMinutes") == 2);
    CHECK(nlohmann::json::parse(p->dataWindow.compact()).at("applyToAllSources") == false);
    CHECK(p->unknown.compact() == R"({"futureMember":[1,2]})");
    // The example is canonical and in the written form: byte-identical (02 section 6.5 rule 3).
    CHECK(p->toJson() == text);
    CHECK(pt::schemaErrors(p->toJson(), "project-1").empty());
}

TEST_CASE("project file: a minimal file fills the defaults and writes the full form idempotently", "[unit]") {
    std::vector<std::string> log;
    const auto p = ProjectFile::parse(pt::readAll(example("minimal.json")), log);
    REQUIRE(p.ok());
    CHECK(p->profile == "circuit");
    CHECK(p->options.isNull());
    CHECK(p->builtinMaths.empty());
    // The example is a version-1 document: migrated on load (D-162) and so dirty, written as version 2.
    CHECK(p->dirty);
    CHECK(p->schemaVersion == ProjectFile::kSchemaVersion);
    const std::string once = p->toJson();
    CHECK(pt::schemaErrors(once, "project-1").empty());
    const auto again = ProjectFile::parse(once, log);
    REQUIRE(again.ok());
    CHECK_FALSE(again->dirty);
    ProjectFile migrated = *p;
    migrated.dirty = false;
    CHECK(*again == migrated);
    CHECK(again->toJson() == once);
    CHECK(once.find("\"options\"") == std::string::npos); // absent stays absent
}

TEST_CASE("project file: makeDefault carries the documented defaults and validates", "[unit]") {
    const ProjectFile p = ProjectFile::makeDefault("Bathurst 2026");
    CHECK(p.name == "Bathurst 2026");
    CHECK(p.profile == "circuit");
    CHECK(p.mathsOrder == std::vector<std::string>{"esses-circuit"});
    CHECK(p.workbooks.empty());
    REQUIRE(p.builtinMaths.count("esses.corrected_speed_distance") == 1);
    CHECK(p.builtinMaths.at("esses.corrected_speed_distance").version == 1);
    CHECK(p.builtinMaths.at("esses.corrected_speed_distance").enabled);
    const auto options = nlohmann::json::parse(p.options.compact());
    CHECK(options.at("general").at("saveOnExit") == true);
    CHECK(options.at("general").at("autosaveMinutes") == 2);
    CHECK(options.at("timeFormat") == "m:ss.fff");
    CHECK(options.at("fastLapTolerancePct") == 1);
    CHECK(options.at("lapStretch").at("tolerancePct") == 5);
    CHECK(options.at("maths").at("precision") == "single");
    CHECK(nlohmann::json::parse(p.dataWindow.compact()).at("filters").at("default").at("kind") == "all");
    CHECK(pt::schemaErrors(p.toJson(), "project-1").empty());
    std::vector<std::string> log;
    const auto back = ProjectFile::parse(p.toJson(), log);
    REQUIRE(back.ok());
    CHECK(*back == p);
}

TEST_CASE("project file: closed records reject unknown members; hashes and markers are checked", "[unit]") {
    struct Case {
        std::string json;
        std::string pointer;
    };
    const std::string head = R"({"$schema": "urn:esses:project-1", "schemaVersion": 1, "name": "P", )";
    const std::string key = R"({"source": {"path": "a.ld", "hash": "xxh3:0123456789abcdef0123456789abcdef"},
                                "groupPath": "Outings:Laps", "startMarker": "7c0e9a2b-1d3f-5a6c-8e9f-0a1b2c3d4e5f"})";
    const std::vector<Case> cases = {
        {head + R"("ui": {"theme": "dark"}})", "/ui/theme"},
        {head + R"("recentSources": [{"path": "a.ld", "hash": "9f3c"}]})", "/recentSources/0/hash"},
        {head + R"("recentSources": [{"path": "a.ld", "hash": "xxh3:0123456789ABCDEF0123456789abcdef"}]})",
         "/recentSources/0/hash"},
        {head + R"("lastSelection": {"main": )" + key + R"(, "datum": 0}})", "/lastSelection/datum"},
        {head +
             R"("lastSelection": {"main": {"source": {"path": "a.ld", "hash": "xxh3:0123456789abcdef0123456789abcdef"},
                   "groupPath": "Outings:Laps", "startMarker": "7"}}})",
         "/lastSelection/main/startMarker"},
        {head +
             R"("lastSelection": {"overlays": [{"groupPath": "x", "startMarker": "7c0e9a2b-1d3f-5a6c-8e9f-0a1b2c3d4e5f"}]}})",
         "/lastSelection/overlays/0"},
        {head + R"("builtinMaths": {"m.x": {"version": 0}}})", "/builtinMaths/m.x/version"},
        {head + R"("builtinMaths": {"m.x": {"settings": "fast"}}})", "/builtinMaths/m.x/settings"},
        {head + R"("options": []})", "/options"},
        {head + R"("workbooks": ["a", 2]})", "/workbooks/1"},
        {R"({"$schema": "urn:esses:project-1", "schemaVersion": 1})", ""},
    };
    for (const Case& c : cases) {
        INFO(c.json);
        std::vector<std::string> log;
        const auto r = ProjectFile::parse(c.json, log);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Invalid);
        CHECK(r.error().context.detail == c.pointer);
    }
}

TEST_CASE("project file: save writes atomically and keeps the last five backups per file", "[unit]") {
    pt::TempDir dir("project");
    const auto target = dir.path() / "project.json";
    ProjectFile p = ProjectFile::makeDefault("P");
    REQUIRE(p.save(target).ok());
    CHECK(pt::readAll(target) == p.toJson());
    CHECK_FALSE(std::filesystem::exists(dir.path() / "backup")); // nothing overwritten yet

    // Saving identical bytes makes no backup.
    REQUIRE(p.save(target).ok());
    CHECK_FALSE(std::filesystem::exists(dir.path() / "backup"));

    const auto t0 = std::chrono::sys_days{std::chrono::year{2026} / 10 / 11} + std::chrono::hours{9};
    std::vector<std::string> previous;
    for (int i = 0; i < 7; ++i) {
        previous.push_back(pt::readAll(target));
        p.workbooks = {"W" + std::to_string(i)};
        // Two saves in the same second for i = 5, 6: the second gets a "-2" suffix.
        const auto now = t0 + std::chrono::seconds(i < 6 ? i : 5);
        REQUIRE(saveWithBackup(dir.path(), target, p.toJson(), now).ok());
    }
    CHECK(pt::readAll(target) == p.toJson());
    const auto backups = pt::jsonFiles(dir.path() / "backup");
    REQUIRE(backups.size() == 5);
    std::vector<std::string> names;
    for (const auto& b : backups) {
        names.push_back(b.filename().string());
    }
    CHECK(names == std::vector<std::string>{"project.20261011-090002.json", "project.20261011-090003.json",
                                            "project.20261011-090004.json", "project.20261011-090005-2.json",
                                            "project.20261011-090005.json"});
    // The newest backup holds the bytes the last save replaced.
    CHECK(pt::readAll(dir.path() / "backup" / "project.20261011-090005-2.json") == previous.back());
    // Other files' backups are not pruned with this one.
    pt::writeAll(dir.path() / "backup" / "Other.workbook.20200101-000000.json", "{}\n");
    REQUIRE(saveWithBackup(dir.path(), target, std::string("{}\n"), t0 + std::chrono::hours{1}).ok());
    CHECK(std::filesystem::exists(dir.path() / "backup" / "Other.workbook.20200101-000000.json"));
    CHECK(pt::jsonFiles(dir.path() / "backup").size() == 6);
}

// D-190: the previous version is copied to backup/ before the target is replaced, so a failed backup either
// leaves the target as it was (the caller asked for no report) or is reported beside a written target.
TEST_CASE("project file: saveWithBackup backs up before it writes", "[unit]") {
    pt::TempDir dir("project-backup-first");
    const auto target = dir.path() / "project.json";
    pt::writeAll(target, "{\"v\": 1}\n");
    pt::writeAll(dir.path() / "backup", "a file where the backup folder belongs");
    const auto now = std::chrono::sys_days{std::chrono::year{2026} / 10 / 13} + std::chrono::hours{9};
    const auto refused = saveWithBackup(dir.path(), target, std::string("{\"v\": 2}\n"), now);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().code == ErrorCode::Io);
    CHECK(pt::readAll(target) == "{\"v\": 1}\n"); // not written: an error means the target is untouched

    std::optional<Error> backupError;
    REQUIRE(saveWithBackup(dir.path(), target, std::string("{\"v\": 2}\n"), now, 5, &backupError).ok());
    CHECK(pt::readAll(target) == "{\"v\": 2}\n"); // written, the backup's failure reported beside it
    REQUIRE(backupError.has_value());
    CHECK(backupError->code == ErrorCode::Io);

    std::filesystem::remove(dir.path() / "backup");
    backupError.reset();
    REQUIRE(saveWithBackup(dir.path(), target, std::string("{\"v\": 3}\n"), now, 5, &backupError).ok());
    CHECK_FALSE(backupError.has_value());
    CHECK(pt::readAll(dir.path() / "backup" / "project.20261013-090000.json") == "{\"v\": 2}\n");
}

TEST_CASE("project file: load reports Io and Format with the path", "[unit]") {
    pt::TempDir dir("project-load");
    std::vector<std::string> log;
    const auto missing = ProjectFile::load(dir.path() / "project.json", log);
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().code == ErrorCode::Io);
    pt::writeAll(dir.path() / "project.json", "{\"name\": ");
    const auto broken = ProjectFile::load(dir.path() / "project.json", log);
    REQUIRE_FALSE(broken.ok());
    CHECK(broken.error().code == ErrorCode::Format);
    CHECK(broken.error().context.path == dir.path() / "project.json");
}

TEST_CASE("project file: the autosave interval and the lap-stretch settings come from the options",
          "[unit]") {
    ProjectFile p = ProjectFile::makeDefault("P");
    CHECK(p.autosaveMinutes() == 2);
    CHECK(p.lapStretchSettings() == analysis::LapStretchSettings{true, 0.05});
    const auto with = [](std::string_view options) {
        ProjectFile q;
        q.options = *JsonDoc::parse(options);
        return q;
    };
    CHECK(with(R"({"general": {"autosaveMinutes": 0}})").autosaveMinutes() == 0); // off
    CHECK(with(R"({"general": {"autosaveMinutes": 5}})").autosaveMinutes() == 5);
    CHECK(with(R"({"general": {"autosaveMinutes": -1}})").autosaveMinutes() == 2);
    CHECK(with(R"({"general": {"autosaveMinutes": "2"}})").autosaveMinutes() == 2);
    CHECK(ProjectFile{}.autosaveMinutes() == 2);
    const auto off = with(R"({"lapStretch": {"enabled": false, "tolerancePct": 2.5}})").lapStretchSettings();
    CHECK_FALSE(off.enabled);
    CHECK(off.tolerance == 0.025);
    CHECK(with(R"({"lapStretch": {"tolerancePct": -3}})").lapStretchSettings().tolerance == 0.05);
}
