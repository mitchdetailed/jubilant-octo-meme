// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The round-trip rule of 02 section 6.5 rule 3 for every esses_project document kind, over the shipped
// documents and every schema example: save(load(f)) == f for the shipped (canonical, current-version)
// documents; save(load(save(load(f)))) == save(load(f)) and a schema-valid output for every valid example;
// the loaders reject what the schemas reject (the members the M1 types keep opaque, options, dataWindow and
// print, through SchemaRegistry since M2).
#include "test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/project/project.hpp>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <set>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::project;
namespace pt = esses::project::test;

namespace {

// parse(text) -> canonical text, or the error message.
using Rewrite = std::function<Result<std::string>(const std::string&)>;

template <class T> Result<std::string> rewriteWith(Result<T> parsed) {
    if (!parsed) {
        return Failure{parsed.error()};
    }
    return parsed->toJson();
}

struct Kind {
    std::string schemaId;
    Rewrite rewrite;
};

std::vector<Kind> kinds() {
    return {
        {"workbook-1",
         [](const std::string& t) {
             std::vector<std::string> log;
             return rewriteWith(WorkbookFile::parse(t, "example.workbook.json", log));
         }},
        {"project-1",
         [](const std::string& t) {
             std::vector<std::string> log;
             return rewriteWith(ProjectFile::parse(t, log));
         }},
        {"scheme-1",
         [](const std::string& t) {
             std::vector<std::string> log;
             return rewriteWith(Scheme::parse(t, log));
         }},
        {"keymap-1",
         [](const std::string& t) {
             std::vector<std::string> log;
             return rewriteWith(Keymap::parse(t, log));
         }},
        {"profile-1",
         [](const std::string& t) {
             std::vector<std::string> log;
             return rewriteWith(Profile::parse(t, log));
         }},
    };
}

const Kind& kind(const std::string& schemaId) {
    static const std::vector<Kind> all = kinds();
    const Kind* found = nullptr;
    for (const Kind& k : all) {
        if (k.schemaId == schemaId) {
            found = &k;
        }
    }
    REQUIRE(found != nullptr);
    return *found;
}

} // namespace

TEST_CASE("round trip: the shipped documents are canonical and byte-identical after load and save",
          "[unit]") {
    const std::vector<std::pair<std::string_view, std::string>> shipped = {
        {kCircuitDefaultWorkbook, "workbook-1"},
        {kLightScheme, "scheme-1"},
        {kDarkScheme, "scheme-1"},
        {kDefaultKeymap, "keymap-1"},
        {kCircuitProfile, "profile-1"},
    };
    for (const auto& [rel, schemaId] : shipped) {
        INFO(rel);
        const std::string bytes = pt::readAll(pt::dataFile(rel));
        CHECK(bytes.find('\r') == std::string::npos);
        CHECK_FALSE(bytes.starts_with("\xEF\xBB\xBF"));
        CHECK(bytes.ends_with("}\n"));
        CHECK(JsonDoc::parse(bytes)->canonical() == bytes); // canonical form of 02 section 6.5 rule 1
        const auto out = kind(schemaId).rewrite(bytes);
        REQUIRE(out.ok());
        CHECK(*out == bytes);
        CHECK(pt::schemaErrors(bytes, schemaId).empty());
    }
}

TEST_CASE("round trip: every valid schema example loads, saves idempotently and validates", "[unit]") {
    std::size_t checked = 0;
    for (const char* schemaId : {"workbook-1", "project-1", "scheme-1", "keymap-1", "profile-1"}) {
        for (const auto& path : pt::jsonFiles(pt::repoRoot() / "schemas/examples" / schemaId / "valid")) {
            INFO(path.string());
            const auto once = kind(schemaId).rewrite(pt::readAll(path));
            REQUIRE(once.ok());
            const auto twice = kind(schemaId).rewrite(*once);
            REQUIRE(twice.ok());
            CHECK(*twice == *once);
            CHECK(pt::schemaErrors(*once, schemaId).empty());
            ++checked;
        }
    }
    CHECK(checked >= 10);
}

TEST_CASE("round trip: the loaders reject the invalid schema examples", "[unit]") {
    // M2: the members M1 keeps as opaque JSON (options, dataWindow, print) are checked by SchemaRegistry on
    // load, so every invalid example is refused, those three with the schema's JSON pointer.
    const std::set<std::string> opaque{"project-1/bad-filter-kind.json", "project-1/bad-time-format.json",
                                       "scheme-1/print-unknown-member.json"};
    std::size_t checked = 0;
    for (const char* schemaId : {"workbook-1", "project-1", "scheme-1", "keymap-1", "profile-1"}) {
        const auto expect = nlohmann::json::parse(
            pt::readAll(pt::repoRoot() / "schemas/examples" / schemaId / "expect.json"));
        for (const auto& path : pt::jsonFiles(pt::repoRoot() / "schemas/examples" / schemaId / "invalid")) {
            const std::string key = std::string(schemaId) + "/" + path.filename().string();
            INFO(key);
            const auto r = kind(schemaId).rewrite(pt::readAll(path));
            REQUIRE_FALSE(r.ok());
            if (opaque.count(key) != 0) {
                CHECK(r.error().code == ErrorCode::Invalid);
                CHECK(r.error().context.detail ==
                      expect.at(path.filename().string()).at("pointer").get<std::string>());
            }
            ++checked;
        }
    }
    CHECK(checked >= 40);
}

TEST_CASE("shipped data: shippedFile resolves under the data directory", "[unit]") {
    PathOverrides overrides;
    overrides.dataDir = pt::repoRoot() / "data";
    overrides.configDir = pt::repoRoot() / "data";
    overrides.cacheDir = pt::repoRoot() / "data";
    const auto paths = Paths::resolve(overrides);
    REQUIRE(paths.ok());
    CHECK(shippedFile(*paths, kDefaultKeymap) == pt::repoRoot() / "data" / "keymaps" / "default.json");
    std::vector<std::string> log;
    CHECK(Keymap::load(shippedFile(*paths, kDefaultKeymap), log).ok());
    CHECK(Profile::load(shippedFile(*paths, kCircuitProfile), log).ok());
    CHECK(WorkbookFile::load(shippedFile(*paths, kCircuitDefaultWorkbook), log).ok());
    CHECK(Scheme::load(shippedFile(*paths, kLightScheme), log).ok());
    CHECK(Scheme::load(shippedFile(*paths, kDarkScheme), log).ok());
}
