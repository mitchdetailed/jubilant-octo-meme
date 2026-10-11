// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// probe_json (05 section 1.2, schema probe-1):
//   * goldens: the probe of every synthetic family in synthetic_families.hpp equals, byte for byte, the
//     reviewed tests/data/probe/<family>.probe.json, validates against schemas/probe-1.schema.json with the
//     vendored json-schema-validator, is a canonical fixed point and reads back to the same text; probe() and
//     open() agree, and the DataSource carries the same text as prov.probe. ESSES_UPDATE_GOLDEN=1 rewrites
//     the goldens (review the diff before committing);
//   * the schema's valid examples read and re-serialise to schema-valid documents; every invalid example is
//     refused by ProbeResult::fromJson;
//   * the writer omits absent members and repeats == 1, rejects non-finite numbers, and keeps detail forms.
#include "formats_test_support.hpp"
#include "synthetic_families.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using namespace esses;
using namespace esses::formats;
using namespace esses::formats::test;
namespace fs = std::filesystem;

namespace {

fs::path goldenDir() {
    return pathFromUtf8(ESSES_TEST_SOURCE_DIR) / "tests" / "data" / "probe";
}

bool updateGoldens() {
    const auto v = getEnv(envName("UPDATE_GOLDEN"));
    return v && *v == "1";
}

std::vector<fs::path> examples(std::string_view kind) {
    std::vector<fs::path> out;
    for (const auto& e : fs::directory_iterator(repoRoot() / "schemas" / "examples" / "probe-1" / kind)) {
        if (e.path().extension() == ".json") {
            out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

TEST_CASE("probe_json: synthetic families match their reviewed goldens", "[unit][probe_json]") {
    ld::test::TempDir dir("probe-golden");
    const auto registry = ImporterRegistry::withBuiltins(&shippedUnits());
    std::size_t compared = 0;
    for (const GoldenFamily& f : goldenFamilies()) {
        INFO(f.name);
        const fs::path path = writeFamily(dir.path(), f);
        auto probed = registry->probe(path);
        REQUIRE(probed.ok());
        CHECK(probed->first->id() == kLdImporterId);
        auto text = probed->second.canonicalJson();
        REQUIRE(text.ok());
        CHECK(schemaErrors("probe-1", *text).empty());
        INFO(schemaErrors("probe-1", *text));

        // Canonical fixed point, and the reader gives the same text back.
        auto reparsed = model::OpaqueJson::parse(*text);
        REQUIRE(reparsed.ok());
        CHECK(reparsed->canonical() == *text);
        auto back = ProbeResult::fromJsonText(*text);
        REQUIRE(back.ok());
        CHECK(back->canonicalJson().value() == *text);

        // probe() and the reader of open() report the same; the source carries it as prov.probe.
        auto opened = registry->open(path, {}, context(&shippedUnits()));
        REQUIRE(opened.ok());
        CHECK(opened->probe.canonicalJson().value() == *text);
        CHECK(opened->source->prov.probe.canonical() == *text);

        const fs::path golden = goldenDir() / (f.name + ".probe.json");
        if (updateGoldens()) {
            fs::create_directories(goldenDir());
            writeText(golden, *text);
            WARN("golden rewritten: " << pathToUtf8(golden));
            continue;
        }
        REQUIRE(fs::exists(golden));
        CHECK(readText(golden) == *text);
        ++compared;
    }
    CHECK((updateGoldens() || compared == goldenFamilies().size()));
}

TEST_CASE("probe_json: the schema examples read back and re-serialise to valid documents",
          "[unit][probe_json]") {
    const auto valid = examples("valid");
    REQUIRE(!valid.empty());
    for (const fs::path& p : valid) {
        INFO(pathToUtf8(p.filename()));
        auto probe = ProbeResult::fromJsonText(readText(p));
        REQUIRE(probe.ok());
        auto text = probe->canonicalJson();
        REQUIRE(text.ok());
        CHECK(schemaErrors("probe-1", *text).empty());
        auto again = ProbeResult::fromJsonText(*text);
        REQUIRE(again.ok());
        CHECK(*again == *probe);
        CHECK(again->canonicalJson().value() == *text);
    }
    const auto invalid = examples("invalid");
    REQUIRE(!invalid.empty());
    for (const fs::path& p : invalid) {
        INFO(pathToUtf8(p.filename()));
        auto probe = ProbeResult::fromJsonText(readText(p));
        REQUIRE_FALSE(probe.ok());
        CHECK(probe.error().code == ErrorCode::Invalid);
        CHECK(probe.error().context.detail.has_value());
    }
}

TEST_CASE("probe_json: members, numbers and schema ids", "[unit][probe_json]") {
    ProbeResult p;
    p.importerId = "motec.ld";
    p.importerVersion = "0.1.0";
    p.layers.emplace_back();
    ChannelInfo c;
    c.loggedName = "Speed";
    c.vendorDtype = "f32";
    c.timing = Rate{100, 3};
    c.sampleCount = 7;
    p.layers[0].channels.push_back(c);
    model::Quantityd q;
    q.v = 6.25;
    q.symbol = "m";
    q.dps = 2;
    p.details.push_back({"Venue Length", q, model::Provenance::Logged, std::nullopt});
    p.details.push_back({"Log Date", std::string("10/10/2026"), model::Provenance::Logged, std::nullopt});
    p.details.push_back(
        {"When", model::DateTime{1760090564000000, 60}, model::Provenance::Logged, std::nullopt});
    ImportLogEntry once;
    once.code = "ld.example_one";
    once.message = "m";
    ImportLogEntry many = once;
    many.code = "ld.example_two";
    many.ctx.repeats = 4;
    many.ctx.channel = "Speed";
    p.warnings = {once, many};
    auto text = p.canonicalJson();
    REQUIRE(text.ok());
    CHECK(schemaErrors("probe-1", *text).empty());
    const auto j = nlohmann::json::parse(*text);
    CHECK(text->rfind("{\n  \"$schema\": \"urn:esses:probe-1\",\n  \"schemaVersion\": 1,\n", 0) == 0);
    CHECK(j["details"][0]["value"] == nlohmann::json::parse(R"({"t":"number","v":6.25,"unit":"m","dps":2})"));
    CHECK(j["details"][1]["value"] == "10/10/2026");
    CHECK(j["details"][2]["value"]["v"] == "2025-10-10T11:02:44+01:00");
    CHECK(j["layers"][0]["channels"][0]["timing"] ==
          nlohmann::json::parse(R"({"rate":{"num":100,"den":3}})"));
    CHECK_FALSE(j["layers"][0]["channels"][0].contains("vendorId"));
    CHECK_FALSE(j["layers"][0]["channels"][0].contains("enumTable"));
    CHECK_FALSE(j["layers"][0].contains("utc"));
    CHECK(j["warnings"][0]["ctx"] == nlohmann::json::object());
    CHECK(j["warnings"][1]["ctx"] == nlohmann::json::parse(R"({"channel":"Speed","repeats":4})"));

    // utc with assumedLocal; timestamped timing.
    p.layers[0].utc = UtcInstant{1760090564000000};
    p.layers[0].utcAssumedLocal = true;
    p.layers[0].channels[0].timing = Timestamped{};
    const auto j2 = nlohmann::json::parse(p.canonicalJson().value());
    CHECK(j2["layers"][0]["utc"] ==
          nlohmann::json::parse(R"({"unixUs":1760090564000000,"assumedLocal":true})"));
    CHECK(j2["layers"][0]["channels"][0]["timing"] == nlohmann::json::parse(R"({"timestamped":true})"));
    auto back = ProbeResult::fromJsonText(p.canonicalJson().value());
    REQUIRE(back.ok());
    CHECK(*back == p);

    // Non-finite numbers are refused with the JSON pointer.
    q.v = std::numeric_limits<double>::quiet_NaN();
    p.details[0].value = q;
    auto bad = p.toJson();
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == ErrorCode::Invalid);
    CHECK(bad.error().context.detail.value_or("") == "/details/0/v");

    // Schema ids: the three accepted forms, another major is Unsupported, another schema Invalid.
    std::string doc = back->canonicalJson().value();
    const auto withSchema = [&](const std::string& id) {
        std::string d = doc;
        const std::string from = "\"urn:esses:probe-1\"";
        d.replace(d.find(from), from.size(), "\"" + id + "\"");
        return ProbeResult::fromJsonText(d);
    };
    CHECK(withSchema("esses:probe-1").ok());
    CHECK(withSchema("https://esses.invalid/schemas/probe-1.schema.json").ok());
    CHECK(withSchema("urn:esses:probe-2").error().code == ErrorCode::Unsupported);
    CHECK(withSchema("urn:esses:sidecar-1").error().code == ErrorCode::Invalid);

    CHECK(parseDtype("I32") == store::Dtype::I32);
    CHECK_FALSE(parseDtype("i32").has_value());
    CHECK(parseSampleKind("Bitfield") == store::SampleKind::Bitfield);
    CHECK(parseImportLogLevel("Warning") == ImportLogEntry::Level::Warning);
}
