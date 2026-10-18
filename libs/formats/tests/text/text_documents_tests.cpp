// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The three text-importer documents: importer-profile-1 (TextProfile), dictionary-1 (Dictionary) and
// unitmap-1 (UnitMappingProfile): reading, canonical writing (unitmap-1 byte-identical, AC-M2-07), errors
// with JSON pointers, matching and rule resolution; the shipped profiles and dictionaries parse and validate.
#include "text_test_support.hpp"

#include <esses/base/json_nlohmann.hpp>

#include <nlohmann/json-schema.hpp>

#include <sstream>

using namespace esses;
using namespace esses::formats;
using namespace esses::formats::texttest;

namespace {

std::string schemaErrors(std::string_view schemaId, const std::string& text) {
    class Collect : public nlohmann::json_schema::error_handler {
    public:
        void error(const nlohmann::json::json_pointer& ptr, const nlohmann::json&,
                   const std::string& msg) override {
            out << "  at '" << ptr.to_string() << "': " << msg << "\n";
        }
        std::ostringstream out;
    };
    nlohmann::json_schema::json_validator validator;
    validator.set_root_schema(
        nlohmann::json::parse(readText(repoRoot() / "schemas" / (std::string(schemaId) + ".schema.json"))));
    Collect errors;
    validator.validate(nlohmann::json::parse(text), errors);
    return errors.out.str();
}

constexpr std::string_view kUnitMap = R"({
  "$schema": "urn:esses:unitmap-1",
  "schemaVersion": 1,
  "importer": "text.profile:generic.csv",
  "rules": [
    {
      "analysisName": "Eng Oil Pres",
      "match": "nameAndUnit",
      "name": "Oil P",
      "quantity": "Pressure",
      "unit": "psi",
      "vendorUnit": "PSIG"
    },
    {
      "match": "unitOnly",
      "quantity": "Speed",
      "unit": "km/h",
      "vendorUnit": "KMH"
    },
    {
      "match": "nameOnly",
      "name": "Oil P",
      "quantity": "Pressure",
      "unit": "bar"
    },
    {
      "match": "nameOnly",
      "name": "Mystery",
      "quantity": "No Such Quantity",
      "unit": "x"
    }
  ],
  "x-note": "kept"
}
)";

} // namespace

TEST_CASE("text documents: the shipped profiles and dictionaries parse and validate", "[unit]") {
    for (const char* stem : {"motec.csv", "generic.csv"}) {
        INFO(stem);
        std::vector<std::string> log;
        const std::filesystem::path pf =
            repoRoot() / "data" / "profiles-text" / (std::string(stem) + ".json");
        auto p = TextProfile::load(pf, log);
        REQUIRE(p.ok());
        CHECK(log.empty());
        CHECK(p->id == stem);
        CHECK(schemaErrors("importer-profile-1", readText(pf)).empty());
        const std::filesystem::path df = repoRoot() / "data" / "dictionaries" / (std::string(stem) + ".json");
        auto d = Dictionary::load(df, log);
        REQUIRE(d.ok());
        CHECK(d->importer == p->effectiveImporterId());
        CHECK(schemaErrors("dictionary-1", readText(df)).empty());
        // toJson is canonical and parses back to the same profile text.
        auto text = p->toJson();
        REQUIRE(text.ok());
        CHECK(schemaErrors("importer-profile-1", *text).empty());
        auto again = TextProfile::parse(*text, log);
        REQUIRE(again.ok());
        CHECK(again->toJson().value() == *text);
        CHECK(again->contentHash() == p->contentHash());
    }
    std::vector<std::string> log;
    auto legacy = TextProfile::load(repoRoot() / "data" / "profiles-text" / "motec.csv.json", log);
    REQUIRE(legacy.ok());
    CHECK(legacy->effectiveImporterId() == kLegacyCsvImporterId);
    CHECK(legacy->sparse == TextProfile::Sparse::Hold);
    CHECK(legacy->preamble.keyValueColumns.size() == 2);
    CHECK(legacy->time.rate == "fromDetail:@rate");
    REQUIRE(legacy->lapSource.fallback);
    CHECK(legacy->lapSource.fallback->kind == "lap_counter_edge");
    auto generic = TextProfile::load(repoRoot() / "data" / "profiles-text" / "generic.csv.json", log);
    REQUIRE(generic.ok());
    CHECK(generic->effectiveImporterId() == kGenericCsvImporterId);
    CHECK(generic->header.unitsRow == "@auto");
    CHECK(generic->sniff.confidence <= 30); // 05 section 1.3 rule 1
}

TEST_CASE("text documents: profile errors carry the JSON pointer", "[unit]") {
    std::vector<std::string> log;
    const auto errorAt = [&](std::string_view json) {
        auto p = TextProfile::parse(json, log);
        REQUIRE_FALSE(p.ok());
        return p.error().context.detail.value_or("?");
    };
    const std::string head = R"({"$schema": "urn:esses:importer-profile-1", "schemaVersion": 1, "id": "x", )"
                             R"("name": "X", "version": "1")";
    CHECK(errorAt(head + R"(, "delimiter": ":"})") == "/delimiter");
    CHECK(errorAt(head + R"(, "sniff": {"preambleRegex": "(open"}})") == "/sniff/preambleRegex");
    CHECK(errorAt(head + R"(, "header": {"unitRow": 1}})") == "/header/unitRow");
    CHECK(errorAt(head + R"(, "lapSource": {"kind": "gps"}})") == "/lapSource/kind");
    CHECK(errorAt(head + R"(, "time": {"rate": {"num": 0, "den": 1}}})") == "/time/rate");
    CHECK(errorAt(head + R"(, "quote": "ab"})") == "/quote");
    CHECK(
        errorAt(
            R"({"$schema": "urn:esses:importer-profile-1", "schemaVersion": 1, "name": "X", "version": "1"})")
            .empty()); // the document root
    auto newer =
        TextProfile::parse(R"({"$schema": "urn:esses:importer-profile-2", "schemaVersion": 1})", log);
    REQUIRE_FALSE(newer.ok());
    CHECK(newer.error().code == ErrorCode::Unsupported);
    // Unknown top-level members are kept (and logged).
    log.clear();
    auto kept = TextProfile::parse(head + R"(, "x-extra": {"a": 1}, "quote": null})", log);
    REQUIRE(kept.ok());
    CHECK_FALSE(kept->quote.has_value());
    CHECK(log.size() == 1);
    CHECK(kept->toJson().value().find("\"x-extra\"") != std::string::npos);
    CHECK(kept->effectiveImporterId() == "text.profile:x");
    TextProfile other = *kept;
    other.version = "2";
    CHECK(other.contentHash() != kept->contentHash());
}

TEST_CASE("text documents: unitmap-1 round trip is byte-identical and rules resolve most specific first",
          "[unit]") {
    std::vector<std::string> log;
    auto map = UnitMappingProfile::parse(kUnitMap, log);
    REQUIRE(map.ok());
    CHECK(log.size() == 1); // x-note
    CHECK(map->toJson().value() == kUnitMap);
    CHECK(schemaErrors("unitmap-1", std::string(kUnitMap)).empty());
    const units::UnitRegistry& u = shippedUnits();
    // nameAndUnit beats nameOnly; names and units compare case-folded.
    auto a = map->resolve("oil p", "psig", u);
    REQUIRE(a.has_value());
    CHECK(u.unit(a->unit).symbol == "psi a"); // "psi" is an alias of the absolute unit
    CHECK(a->analysisName == "Eng Oil Pres");
    REQUIRE(a->rule != nullptr);
    CHECK(a->rule->match == UnitMappingProfile::Match::NameAndUnit);
    auto b = map->resolve("Oil P", "kPa", u);
    REQUIRE(b.has_value());
    CHECK(u.unit(b->unit).symbol == "bar a");
    CHECK_FALSE(b->analysisName.has_value());
    auto c = map->resolve("Wheel Speed", "KMH", u);
    REQUIRE(c.has_value());
    CHECK(u.quantity(c->quantity).name == "Speed");
    CHECK_FALSE(map->resolve("Wheel Speed", "", u).has_value()); // unitOnly needs a unit
    CHECK_FALSE(map->resolve("Mystery", "", u).has_value());     // an unknown quantity is skipped
    CHECK_FALSE(map->resolve("Other", "m", u).has_value());

    TempDir dir("unitmap");
    const auto file = dir.path() / "channels" / "unitmaps" / "generic.csv.json";
    REQUIRE(map->save(file).ok());
    CHECK(readText(file) == kUnitMap);
    auto loaded = UnitMappingProfile::load(file, log);
    REQUIRE(loaded.ok());
    CHECK(loaded->rules == map->rules);

    auto bad = UnitMappingProfile::parse(
        R"({"$schema": "urn:esses:unitmap-1", "schemaVersion": 1, "importer": "x", "rules": [{"match": "nameOnly", "quantity": "Speed", "unit": "km/h"}]})",
        log);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().context.detail == "/rules/0");
}

TEST_CASE("text documents: dictionary matching order and canonical writing", "[unit]") {
    constexpr std::string_view kDict = R"({
  "$schema": "urn:esses:dictionary-1", "schemaVersion": 1, "importer": "motec.ld", "version": "0.2.0",
  "entries": [
    {"match": {"nameRegex": "^Wheel"}, "quantity": "Speed"},
    {"match": {"name": "Wheel FL"}, "name": "Wheel Speed FL", "unit": "km/h"},
    {"match": {"vendorId": "12"}, "name": "By Id", "kind": "Integer", "interp": "Previous"},
    {"match": {"vendorId": "13", "name": "Both"}, "dtype": "F64"}
  ],
  "bitfields": [
    {"match": {"name": "Status"}, "device": {"type": "LOGGER-2", "minVersion": 3}, "bits": [{"mask": "0x0004", "name": "Bit 2"}]}
  ]
})";
    std::vector<std::string> log;
    auto d = Dictionary::parse(kDict, log);
    REQUIRE(d.ok());
    // vendorId first, then exact name, then nameRegex; every stated member must hold.
    REQUIRE(d->match("12", "Wheel FL") != nullptr);
    CHECK(d->match("12", "Wheel FL")->name == "By Id");
    CHECK(d->match("", "Wheel FL")->name == "Wheel Speed FL");
    CHECK(d->match("", "Wheel RR")->quantity == "Speed");
    CHECK(d->match("13", "Other") == nullptr);
    CHECK(d->match("13", "Both")->dtype == "F64");
    CHECK(d->match("", "Engine") == nullptr);
    REQUIRE(d->bitfields.size() == 1);
    CHECK(d->bitfields[0].bits[0].mask == 4u);
    auto text = d->toJson();
    REQUIRE(text.ok());
    CHECK(text->find("\"mask\": \"0x0004\"") != std::string::npos);
    CHECK(schemaErrors("dictionary-1", *text).empty());
    auto again = Dictionary::parse(*text, log);
    REQUIRE(again.ok());
    CHECK(again->toJson().value() == *text);

    auto bad = Dictionary::parse(
        R"({"$schema": "urn:esses:dictionary-1", "schemaVersion": 1, "importer": "x", "version": "1", "bitfields": [{"match": {"name": "G"}, "bits": [{"mask": "0x0", "name": "B"}]}]})",
        log);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().context.detail == "/bitfields/0/bits/0/mask");
    auto noMatch = Dictionary::parse(
        R"({"$schema": "urn:esses:dictionary-1", "schemaVersion": 1, "importer": "x", "version": "1", "entries": [{"match": {}}]})",
        log);
    REQUIRE_FALSE(noMatch.ok());
    CHECK(noMatch.error().context.detail == "/entries/0/match");
}

TEST_CASE("text documents: loadProfiles reads a directory in file-name order and skips bad files", "[unit]") {
    TempDir dir("profiles");
    const std::string head =
        R"({"$schema": "urn:esses:importer-profile-1", "schemaVersion": 1, "name": "N", "version": "1", )";
    writeText(dir.path() / "b.json", head + R"("id": "b"})");
    writeText(dir.path() / "a.json", head + R"("id": "a"})");
    writeText(dir.path() / "broken.json", "{");
    writeText(dir.path() / "notes.txt", "not a profile");
    std::vector<std::string> log;
    const std::vector<TextProfile> ps = TextEngine::loadProfiles(dir.path(), log);
    REQUIRE(ps.size() == 2);
    CHECK(ps[0].id == "a");
    CHECK(ps[1].id == "b");
    REQUIRE(log.size() == 1);
    CHECK(log[0].find("broken.json") != std::string::npos);
    CHECK(TextEngine::loadProfiles(dir.path() / "missing", log).empty());
}
