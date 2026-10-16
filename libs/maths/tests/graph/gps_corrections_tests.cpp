// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// esses.gps_corrections v1 (03 section 9.2, 07 section 5.1): the default settings and the shipped settings
// schema with its examples; the zero-fix step through MathGraph (both channels invalid where latitude and
// longitude are both exactly 0; consumers read the cleaned chain head); Disabled without GPS channels; the
// step runs whatever the builtin item's `enabled` says; prefixed outputs.

#include "graph_test_support.hpp"

#include <esses/maths/gps_corrections.hpp>
#include <esses/maths/math_graph.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::maths;
using test::expr;

namespace {

model::OpaqueJson readJson(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    Result<model::OpaqueJson> r = model::OpaqueJson::parse(text);
    REQUIRE(r);
    return std::move(r).value();
}

std::filesystem::path schemasDir() {
    return test::repoRoot() / "schemas";
}

// GPS Latitude / GPS Longitude at 10 Hz over 10 s; samples 0-4 are (0, 0) before the first fix, sample 50 has
// latitude 0 only (a real equator crossing, kept), samples 70 and 71 are (0, 0) again.
std::shared_ptr<model::DataSource> gpsSource(store::ChunkCache& cache, bool withLongitude = true) {
    std::vector<float> lat(100);
    std::vector<float> lon(100);
    for (std::size_t i = 0; i < lat.size(); ++i) {
        lat[i] = static_cast<float>(0.8 + 1e-4 * static_cast<double>(i));
        lon[i] = static_cast<float>(0.1 + 1e-4 * static_cast<double>(i));
    }
    for (const std::size_t i : {0u, 1u, 2u, 3u, 4u, 70u, 71u}) {
        lat[i] = 0.F;
        lon[i] = 0.F;
    }
    lat[50] = 0.F;
    std::vector<model::Channel> channels;
    channels.push_back(test::uniformF32(cache, "GPS Latitude", test::unitOf("rad"), Rate::fromHz(10), lat));
    if (withLongitude) {
        channels.push_back(
            test::uniformF32(cache, "GPS Longitude", test::unitOf("rad"), Rate::fromHz(10), lon));
    }
    channels.push_back(
        test::uniformF32(cache, "rpm", units::kNoUnit, Rate::fromHz(10), std::vector<float>(100, 1.F)));
    return test::makeSource(std::move(channels), 10'000'000, "gps-source");
}

} // namespace

TEST_CASE("gps corrections module: identity, defaults and the settings schema", "[unit][gps]") {
    GpsCorrectionsModule module;
    CHECK(module.id() == "esses.gps_corrections");
    CHECK(module.version() == 1);
    CHECK(module.settingsSchemaId() == "builtin-esses.gps_corrections-1");
    CHECK_FALSE(module.disabledItemDisablesOutputs());
    const model::OpaqueJson defaults = module.defaultSettings();
    REQUIRE_FALSE(defaults.isNull());
    // The defaults equal the shipped example of 03 section 9.2.
    CHECK(defaults.compact() ==
          readJson(schemasDir() / "examples" / "builtin-esses.gps_corrections-1" / "valid" / "default.json")
              .compact());

    CHECK(validateSettings(module, defaults).error().code == ErrorCode::NotFound); // no schema loaded yet
    CHECK(module.loadSchema(test::repoRoot() / "nowhere").error().code == ErrorCode::Io);
    REQUIRE(module.loadSchema(schemasDir()));
    CHECK(validateSettings(module, defaults));
    const std::filesystem::path examples = schemasDir() / "examples" / "builtin-esses.gps_corrections-1";
    for (const auto& e : std::filesystem::directory_iterator(examples / "valid")) {
        INFO(e.path().filename().string());
        CHECK(validateSettings(module, readJson(e.path())));
    }
    const nlohmann::json expect = nlohmann::json::parse(readJson(examples / "expect.json").compact());
    for (const auto& e : std::filesystem::directory_iterator(examples / "invalid")) {
        INFO(e.path().filename().string());
        const Result<void> r = validateSettings(module, readJson(e.path()));
        REQUIRE_FALSE(r);
        CHECK(r.error().context.detail ==
              expect.at(e.path().filename().string()).at("pointer").get<std::string>());
    }

    // expand: two F64 outputs that require both channels; invalid settings fail before any output exists.
    Diagnostics diags;
    Result<std::vector<BuiltinOutput>> outs = module.expand(defaults, *test::shippedUnits(), diags);
    REQUIRE(outs);
    REQUIRE(outs->size() == 2);
    CHECK((*outs)[0].name == "GPS Latitude");
    CHECK((*outs)[1].name == "GPS Longitude");
    CHECK((*outs)[0].dtype == store::Dtype::F64);
    CHECK((*outs)[0].required == std::vector<std::string>{"GPS Latitude", "GPS Longitude"});
    CHECK((*outs)[0].missingSummary == "Disabled (no GPS channels)");
    Diagnostics bad;
    CHECK_FALSE(
        module.expand(test::FakeModule::json(R"({"outputs": "replace"})"), *test::shippedUnits(), bad));
    REQUIRE_FALSE(bad.empty());
    CHECK(bad.back().code == "E-GPS-SETTINGS");
}

TEST_CASE("gps corrections module: the zero-fix step through MathGraph", "[unit][gps][graph]") {
    test::Runtime rt;
    auto src = gpsSource(rt.cache);
    MathGraph g = test::makeGraph(rt, src);
    GpsCorrectionsModule module;
    REQUIRE(module.loadSchema(schemasDir()));
    Diagnostics diags;
    Result<std::vector<ChannelKey>> keys = g.defineBuiltin(module, {}, diags);
    REQUIRE(keys);
    REQUIRE(keys->size() == 2);
    MathModule user = test::module("user", {expr("Lat2", "'GPS Latitude' * 2")});
    g.setModules(std::span<const MathModule>(&user, 1), nullptr, nullptr);

    const std::vector<MathNodeInfo> nodes = g.nodes();
    REQUIRE(nodes.size() == 3);
    CHECK(nodes[0].kind == ItemKind::Builtin);
    CHECK(nodes[0].selfMaths);
    CHECK(nodes[0].head);
    CHECK(nodes[0].inputs[0].kind == MathInput::Kind::Logged);
    CHECK(nodes[2].inputs[0].kind == MathInput::Kind::Maths); // the consumer reads the cleaned channel
    const std::optional<model::ChannelDescriptor> d = g.descriptor(test::keyOf(g, "GPS Latitude"));
    REQUIRE(d);
    CHECK(d->unit == test::unitOf("rad"));
    CHECK(d->dtype == store::Dtype::F64);
    CHECK(d->source == model::ChannelSource::Maths);

    Result<ColumnHandle> lat = test::evaluate(g, "GPS Latitude");
    Result<ColumnHandle> lon = test::evaluate(g, "GPS Longitude");
    REQUIRE((lat && lon));
    Result<analysis::UniformSignal> ls = readWholeSignal(lat->series, {});
    Result<analysis::UniformSignal> os = readWholeSignal(lon->series, {});
    REQUIRE((ls && os));
    REQUIRE(ls->size() == 100);
    for (std::size_t i = 0; i < 100; ++i) {
        const bool fix = !(i <= 4 || i == 70 || i == 71);
        INFO(i);
        CHECK(ls->validAt(i) == fix);
        CHECK(os->validAt(i) == fix);
    }
    CHECK(ls->values[50] == 0.0); // only the latitude is 0: a fix
    const std::vector<double> lat2 = test::values(g, "Lat2");
    CHECK(std::isnan(lat2[3]));
    CHECK(std::abs(lat2[10] - 2.0 * static_cast<double>(static_cast<float>(0.8 + 1e-3))) < 1e-6);
    CHECK(g.status(test::keyOf(g, "GPS Latitude")).state == MathState::Ok);

    SECTION("a disabled builtin item keeps the zero-fix step (it gates the v1.0 steps only)") {
        MathItem item;
        item.kind = ItemKind::Builtin;
        item.name = "gps";
        item.moduleId = "esses.gps_corrections";
        item.enabled = false;
        const MathModule local = test::module("local", {item}, MathModule::Origin::Local);
        g.setModules(std::span<const MathModule>(&user, 1), &local, nullptr);
        CHECK(g.status(test::keyOf(g, "GPS Latitude")).state == MathState::Ok);
    }
    SECTION("without a longitude channel the outputs are Disabled, never an error") {
        CHECK_FALSE(g.setSource(gpsSource(rt.cache, false)).empty());
        const MathStatus s = g.status(test::keyOf(g, "GPS Latitude"));
        CHECK(s.state == MathState::Disabled);
        CHECK(s.summary == "Disabled (no GPS channels)");
        // The consumer reads the logged latitude instead.
        CHECK(test::values(g, "Lat2")[3] == 0.0);
    }
    SECTION("prefixed outputs are new channels in the base unit") {
        Diagnostics d2;
        REQUIRE(g.defineBuiltin(module, test::FakeModule::json(R"({"outputs": {"prefix": "Corr "}})"), d2));
        REQUIRE(g.find("Corr GPS Latitude"));
        CHECK_FALSE(g.find("GPS Longitude")); // no longer shadowed
        const std::vector<double> corr = test::values(g, "Corr GPS Latitude");
        CHECK(std::isnan(corr[0]));
        CHECK(corr[10] == static_cast<double>(static_cast<float>(0.8 + 1e-3)));
        CHECK(g.descriptor(test::keyOf(g, "Corr GPS Latitude"))->unit == test::unitOf("rad"));
    }
}
