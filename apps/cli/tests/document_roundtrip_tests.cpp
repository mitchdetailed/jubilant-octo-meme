// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M2-07 for unitmap-1, the one document kind of the project folder whose type lives outside esses_project
// (formats::UnitMappingProfile, 05 section 8.2): the canonical text comes back byte-identical after load and
// save, every schema example saves idempotently, and the output validates through esses_project's
// SchemaRegistry. The other kinds are in libs/project/tests/roundtrip_m2_tests.cpp.

#include <esses/base/json_doc.hpp>
#include <esses/formats/unit_mapping_profile.hpp>
#include <esses/project/schema_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using esses::JsonDoc;
using esses::formats::UnitMappingProfile;
using esses::project::SchemaRegistry;

std::string readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// parse -> toJson; the error message when either fails.
esses::Result<std::string> rewrite(const std::string& text) {
    std::vector<std::string> log;
    auto profile = UnitMappingProfile::parse(text, log);
    if (!profile) {
        return esses::Failure{profile.error()};
    }
    return profile->toJson();
}

void checkSchemaValid(const std::string& text) {
    const auto valid = SchemaRegistry::instance().validate(JsonDoc::parse(text).value(), "unitmap-1");
    INFO((valid.ok() ? std::string() : valid.error().message));
    CHECK(valid.ok());
}

} // namespace

TEST_CASE("roundtrip: unitmap-1 documents come back byte-identical", "[unit]") {
    // 02 section 6.3's example (05 section 8.2 owns the format).
    const std::string example =
        R"({"$schema": "urn:esses:unitmap-1", "schemaVersion": 1, "importer": "text.profile:generic.csv",)"
        R"( "rules": [ { "match": "nameAndUnit", "name": "Speed", "vendorUnit": "MPH", "quantity": "Speed",)"
        R"( "unit": "mph", "analysisName": "GPS Speed" },)"
        R"( { "match": "unitOnly", "vendorUnit": "kpa", "quantity": "Pressure", "unit": "kPa a" },)"
        R"( { "match": "nameOnly", "name": "Lambda1", "quantity": "Air Fuel Ratio", "unit": "lambda",)"
        R"( "analysisName": "Lambda 1" } ] })";
    const auto once = rewrite(example);
    REQUIRE(once.ok()); // UnitMappingProfile is implemented: a failure is a failure, never a skip
    CHECK(JsonDoc::parse(*once).value().canonical() == *once); // the written form is canonical
    const auto twice = rewrite(*once);
    REQUIRE(twice.ok());
    CHECK(*twice == *once); // save(load(f)) == f for the canonical document
    checkSchemaValid(*once);

    const auto dir = std::filesystem::path(ESSES_TEST_REPO_ROOT) / "schemas/examples/unitmap-1/valid";
    std::error_code ec;
    REQUIRE(std::filesystem::is_directory(dir, ec));
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& path : files) {
        INFO(path.string());
        const auto first = rewrite(readAll(path));
        REQUIRE(first.ok());
        const auto second = rewrite(*first);
        REQUIRE(second.ok());
        CHECK(*second == *first);
        checkSchemaValid(*first);
    }
}
