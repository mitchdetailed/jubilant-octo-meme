// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The built-in module esses.corrected_speed_distance (03 section 9.1, D-016). AC-M1-10 core part: the
// module's settings validate against schemas/builtin-esses.corrected_speed_distance-1.schema.json (defaults,
// every schema example through the module's validator, rejection before the outputs are built), and Corr
// Speed / Corr Dist are produced through MathGraph and PlanProvider as this module (the GUI prefetch is
// esses_qtmodel's, tests/ui/corr_dist_prefetch). The numbers come from esses_analysis (AC-M1-11 there): the
// tests that need them skip, naming the stub, until analysis::CorrectedSpeed lands.

#include "maths_test_support.hpp"

#include <esses/maths/corrected_speed_distance.hpp>
#include <esses/maths/math_graph.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::maths;
using nlohmann::json;

namespace {

std::filesystem::path schemasDir() {
    return test::repoRoot() / "schemas";
}

std::filesystem::path examplesDir() {
    return schemasDir() / "examples" / std::string(CorrectedSpeedDistanceModule::kSettingsSchemaId);
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

model::OpaqueJson opaque(const std::string& text) {
    Result<model::OpaqueJson> r = model::OpaqueJson::parse(text);
    REQUIRE(r);
    return std::move(r).value();
}

CorrectedSpeedDistanceModule loadedModule() {
    CorrectedSpeedDistanceModule module;
    REQUIRE(module.loadSchema(schemasDir()));
    return module;
}

std::vector<std::filesystem::path> jsonFiles(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() == ".json") {
            out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// True while esses_analysis still returns its M1 contract stub for `r`.
template <class T> bool analysisStubbed(const Result<T>& r) {
    return !r && r.error().code == ErrorCode::Unsupported && r.error().message.starts_with("M1-STUB");
}

} // namespace

TEST_CASE("corrected speed module: identity and the default settings of 03 section 9.1", "[unit][csd]") {
    CorrectedSpeedDistanceModule module;
    CHECK(module.id() == "esses.corrected_speed_distance");
    CHECK(module.version() == 1);
    CHECK(module.settingsSchemaId() == "builtin-esses.corrected_speed_distance-1");
    CHECK(module.settingsSchema().isNull());
    const model::OpaqueJson defaults = module.defaultSettings();
    REQUIRE(defaults.isObject());
    // Canonical (OpaqueJson) and equal to the shipped example of the defaults.
    CHECK(defaults == opaque(readFile(examplesDir() / "valid" / "default.json")));
    const json d = json::parse(defaults.compact());
    CHECK(d["method"] == "fastestWheel");
    CHECK(d["fastestWheel"]["channels"] == json::array({"Wheel Speed FL", "Wheel Speed FR"}));
    CHECK(d["alternateList"]["channels"].size() == 11);
    CHECK(d["alternateList"]["channels"].back() == "Speed"); // M1: the sim-exporter SPEED (libs/maths README)
    CHECK(d["clamp"]["min"] == json({{"v", 0}, {"unit", "km/h"}}));
    CHECK(d["clamp"]["max"].is_null());
    CHECK(d["distance"]["source"] == "integrate");
    CHECK(d["distance"]["integration"] == "trapezoid");
    CHECK(d["outputs"] == json({{"speed", "Corr Speed"}, {"distance", "Corr Dist"}}));
}

TEST_CASE("corrected speed module: the settings validate against the shipped schema (AC-M1-10)",
          "[unit][csd]") {
    CorrectedSpeedDistanceModule module;
    CHECK(module.validateSettings(module.defaultSettings()).error().code ==
          ErrorCode::NotFound); // not loaded
    REQUIRE(module.loadSchema(schemasDir()));
    REQUIRE(module.settingsSchema().isObject());
    CHECK(module.validateSettings(module.defaultSettings()));
    CHECK(validateSettings(module, module.defaultSettings()));

    const std::vector<std::filesystem::path> valid = jsonFiles(examplesDir() / "valid");
    REQUIRE(valid.size() >= 3);
    for (const auto& path : valid) {
        INFO(path.filename().string());
        const Result<void> r = module.validateSettings(opaque(readFile(path)));
        CHECK(r);
    }
    const json expect = json::parse(readFile(examplesDir() / "expect.json"));
    const std::vector<std::filesystem::path> invalid = jsonFiles(examplesDir() / "invalid");
    REQUIRE(invalid.size() == expect.size());
    for (const auto& path : invalid) {
        const std::string name = path.filename().string();
        INFO(name);
        REQUIRE(expect.contains(name));
        const Result<void> r = module.validateSettings(opaque(readFile(path)));
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::Invalid);
        CHECK(r.error().context.schemaId == std::string(CorrectedSpeedDistanceModule::kSettingsSchemaId));
        CHECK_THAT(r.error().message, Catch::Matchers::ContainsSubstring(
                                          "'" + expect[name]["pointer"].get<std::string>() + "'"));
    }
    // A settings object that is not an object at all.
    CHECK(module.validateSettings(opaque("[1,2]")).error().code == ErrorCode::Invalid);
}

TEST_CASE("corrected speed module: invalid settings fail expand before any output exists", "[unit][csd]") {
    const CorrectedSpeedDistanceModule module = loadedModule();
    Diagnostics diags;
    Result<std::vector<BuiltinOutput>> r =
        module.expand(opaque(R"({"method":"slowestWheel"})"), *test::shippedUnits(), diags);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::Invalid);
    CHECK(r.error().context.detail == "/method");
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == "E-CSD-SETTINGS");
    CHECK(diags[0].level == Diagnostic::Error);
    CHECK(diags[0].hint == "at '/method'");

    // Through the graph: nothing is defined.
    test::Runtime rt;
    auto src = test::makeSource({}, 1'000'000);
    MathGraph g(src, "Normal", test::shippedUnits(), {}, &rt.pool, &rt.cache);
    Diagnostics graphDiags;
    CHECK(
        g.defineBuiltin(module, opaque(R"({"fastestWheel":{"channels":["A"]}})"), graphDiags).error().code ==
        ErrorCode::Invalid);
    CHECK(g.outputs().empty());
    CHECK(graphDiags.at(0).code == "E-CSD-SETTINGS");
}

TEST_CASE("corrected speed module: loading the schema", "[unit][csd]") {
    CorrectedSpeedDistanceModule module;
    const Result<void> missing = module.loadSchema(test::repoRoot() / "no-such-directory");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::Io);
    CHECK(module.settingsSchema().isNull());
    // Another schema under the expected file name is refused.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("esses-maths-schema-test-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / std::string(CorrectedSpeedDistanceModule::kSettingsSchemaFile),
                          std::ios::binary);
        out << R"({"$schema":"http://json-schema.org/draft-07/schema#","$id":"urn:esses:other-1","type":"object"})";
    }
    const Result<void> wrong = module.loadSchema(dir);
    REQUIRE_FALSE(wrong);
    CHECK(wrong.error().code == ErrorCode::Format);
    {
        std::ofstream out(dir / std::string(CorrectedSpeedDistanceModule::kSettingsSchemaFile),
                          std::ios::binary);
        out << "{ not json";
    }
    CHECK(module.loadSchema(dir).error().code == ErrorCode::Format);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    CHECK(module.settingsSchema().isNull());

    // The schemas directory: the data directory when it has the file, else beside the executable or the
    // source tree.
    CHECK(CorrectedSpeedDistanceModule::findSchemasDir(test::repoRoot()) == test::repoRoot() / "schemas");
    const std::filesystem::path found = CorrectedSpeedDistanceModule::findSchemasDir();
    REQUIRE_FALSE(found.empty());
    CHECK(module.loadSchema(found));
}

TEST_CASE("corrected speed module: expand builds Corr Speed and Corr Dist", "[unit][csd]") {
    const CorrectedSpeedDistanceModule module = loadedModule();
    const units::UnitRegistry& units = *test::shippedUnits();
    Diagnostics diags;
    Result<std::vector<BuiltinOutput>> r = module.expand({}, units, diags);
    if (analysisStubbed(r)) {
        SKIP("analysis::CorrectedSpeedSettings::fromJson is still an M1 stub: " << r.error().message);
    }
    REQUIRE(r);
    REQUIRE(r->size() == 2);
    const BuiltinOutput& speed = (*r)[0];
    const BuiltinOutput& dist = (*r)[1];
    CHECK(speed.name == "Corr Speed");
    CHECK(speed.upstream.empty());
    REQUIRE(speed.inputs.size() >= 2);
    CHECK(speed.inputs[0] == "Wheel Speed FL"); // fastestWheel first, then its alternate-list fallback
    CHECK(speed.inputs[1] == "Wheel Speed FR");
    CHECK(std::find(speed.inputs.begin(), speed.inputs.end(), "GPS Speed") != speed.inputs.end());
    CHECK(speed.quantity == *units.quantityByName("Speed"));
    CHECK(speed.resultUnit == *units.find("m/s"));
    CHECK(speed.dtype == store::Dtype::F32);
    CHECK(speed.interp == store::Interp::Linear);
    CHECK(dist.name == "Corr Dist");
    CHECK(dist.upstream == std::vector<std::string>{"Corr Speed"});
    CHECK(dist.inputs.empty());
    CHECK(dist.quantity == *units.quantityByName("Distance"));
    CHECK(dist.resultUnit == *units.find("m"));
    CHECK(dist.dtype == store::Dtype::F64);

    Diagnostics loggedDiags;
    Result<std::vector<BuiltinOutput>> logged =
        module.expand(opaque(R"({"distance":{"source":"logged","loggedChannel":"Lap Distance"},
                   "outputs":{"speed":"S","distance":"D"}})"),
                      units, loggedDiags);
    REQUIRE(logged);
    CHECK((*logged)[0].name == "S");
    CHECK((*logged)[1].upstream.empty());
    CHECK((*logged)[1].inputs == std::vector<std::string>{"Lap Distance"});

    Diagnostics same;
    Result<std::vector<BuiltinOutput>> clash =
        module.expand(opaque(R"({"outputs":{"speed":"X","distance":"x"}})"), units, same);
    REQUIRE_FALSE(clash);
    CHECK(same.back().code == "E-CSD-SETTINGS");
}

TEST_CASE("corrected speed module: Corr Speed and Corr Dist through MathGraph and PlanProvider",
          "[unit][csd]") {
    // A synthetic four-wheel fixture: FL locks (0 km/h) over samples [300, 400), FR is invalid over [600,
    // 650); both at 100 Hz in km/h. fastestWheel = max(FL, FR) (the valid one where one is invalid), clamped
    // at 0 km/h; Corr Dist = the trapezoid integral of Corr Speed as stored (F32).
    test::Runtime rt;
    constexpr std::size_t n = 1'000;
    std::vector<float> fl(n);
    std::vector<float> fr(n);
    std::vector<bool> frValid(n, true);
    for (std::size_t i = 0; i < n; ++i) {
        const double base = 120.0 + 30.0 * std::sin(static_cast<double>(i) * 0.01);
        fl[i] = static_cast<float>(i >= 300 && i < 400 ? 0.0 : base + 1.0);
        fr[i] = static_cast<float>(base);
        if (i >= 600 && i < 650) {
            frValid[i] = false;
        }
    }
    const units::UnitId kmh = test::unitOf("km/h");
    std::vector<model::Channel> channels;
    channels.push_back(test::uniformF32(rt.cache, "Wheel Speed FL", kmh, Rate::fromHz(100), fl));
    channels.push_back(test::uniformF32(rt.cache, "Wheel Speed FR", kmh, Rate::fromHz(100), fr, frValid));
    auto src = test::makeSource(std::move(channels), 10'000'000, "csd-four-wheel");

    CorrectedSpeedDistanceModule module = loadedModule();
    MathGraph g(src, "Normal", test::shippedUnits(), {}, &rt.pool, &rt.cache);
    Diagnostics diags;
    Result<std::vector<ChannelKey>> keys = g.defineBuiltin(module, {}, diags);
    if (analysisStubbed(keys)) {
        SKIP("analysis::CorrectedSpeedSettings::fromJson is still an M1 stub: " << keys.error().message);
    }
    REQUIRE(keys);
    REQUIRE(keys->size() == 2);
    const ChannelKey speedKey = (*keys)[0];
    const ChannelKey distKey = (*keys)[1];
    CHECK(g.descriptor(speedKey)->id ==
          model::channelIdMathsModule(src->id, "Normal", "Corr Speed", "esses.corrected_speed_distance"));

    std::vector<MathRequest> prefetched = g.prefetch(Priority::Background);
    REQUIRE(prefetched.size() == 2);
    const Result<ColumnHandle>& distResult = prefetched[1].future.get();
    if (analysisStubbed(distResult)) {
        SKIP("analysis::CorrectedSpeed is still an M1 stub: " << distResult.error().message);
    }
    REQUIRE(distResult);
    static_cast<void>(g.applyReadyCompletions());
    CHECK(g.status(speedKey).state == MathState::Ok);
    CHECK(g.status(distKey).state == MathState::Ok);
    const Result<ColumnHandle>& speedResult = prefetched[0].future.get();
    REQUIRE(speedResult);

    Result<analysis::UniformSignal> speed = readWholeSignal(speedResult->series, {});
    Result<analysis::UniformSignal> dist = readWholeSignal(distResult->series, {});
    REQUIRE(speed);
    REQUIRE(dist);
    REQUIRE(speed->size() == n);
    REQUIRE(dist->size() == n);
    const units::Converter toMps = test::shippedUnits()->toBase(kmh);
    for (std::size_t i = 0; i < n; ++i) {
        INFO("sample " << i);
        const bool frOk = frValid[i];
        const double expected =
            frOk ? std::max(toMps(static_cast<double>(fl[i])), toMps(static_cast<double>(fr[i])))
                 : toMps(static_cast<double>(fl[i]));
        CHECK(speed->validAt(i));
        CHECK(std::abs(speed->values[i] - static_cast<double>(static_cast<float>(expected))) <= 1e-5);
    }
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (i > 0) {
            acc += 0.5 * (speed->values[i - 1] + speed->values[i]) * 0.01;
        }
        CHECK(std::abs(dist->values[i] - acc) <= 1e-9 * std::max(1.0, std::abs(acc)));
    }
    CHECK(std::get<store::UniformSeries>(distResult->series).values.dtype() == store::Dtype::F64);
    CHECK(std::get<store::UniformSeries>(speedResult->series).values.dtype() == store::Dtype::F32);
}

TEST_CASE("corrected speed module: an input found by its logged name and its status are in the version",
          "[unit][csd]") {
    // "Wheel Speed FL" exists only as the logged name of a channel renamed "Front Left": the evaluation uses
    // it (the logged-name fallback), so a new column or an Error status of that channel must re-version Corr
    // Speed.
    test::Runtime rt;
    const units::UnitId kmh = test::unitOf("km/h");
    const auto wheel = [&](const std::string& logged, const std::string& name, float add) {
        std::vector<float> v(200);
        for (std::size_t i = 0; i < v.size(); ++i) {
            v[i] = 100.F + add + static_cast<float>(i % 7);
        }
        model::Channel c = test::uniformF32(rt.cache, logged, kmh, Rate::fromHz(100), v);
        c.desc.name = name;
        return c;
    };
    std::vector<model::Channel> channels;
    channels.push_back(wheel("Wheel Speed FL", "Front Left", 0.F));
    channels.push_back(wheel("Wheel Speed FR", "Wheel Speed FR", -50.F));
    auto src = test::makeSource(std::move(channels), 2'000'000, "csd-logged-name");
    REQUIRE(src->layers[0].find("Wheel Speed FL") == nullptr);
    REQUIRE(src->layers[0].findLogged("Wheel Speed FL") != nullptr);

    CorrectedSpeedDistanceModule module = loadedModule();
    MathGraph g(src, "Normal", test::shippedUnits(), {}, &rt.pool, &rt.cache);
    Diagnostics diags;
    Result<std::vector<ChannelKey>> keys = g.defineBuiltin(module, {}, diags);
    REQUIRE(keys);
    const ChannelKey speedKey = (*keys)[0];
    const Version v0 = g.version(speedKey);

    // A new column for the renamed front-left wheel: Corr Speed and Corr Dist are re-versioned.
    auto next = std::make_shared<model::DataSource>(*src);
    model::Channel faster = wheel("Wheel Speed FL", "Front Left", 20.F);
    next->layers[0].findMutable("Front Left")->series = faster.series;
    CHECK(g.setSource(next).size() == 2);
    const Version v1 = g.version(speedKey);
    CHECK(v1 != v0);

    // The same channel in Error (rejected by the evaluation): re-versioned again.
    auto broken = std::make_shared<model::DataSource>(*next);
    broken->layers[0].findMutable("Front Left")->setStatus(model::ChannelStatus::Error, "decode failed");
    CHECK(g.setSource(broken).size() == 2);
    CHECK(g.version(speedKey) != v1);
}

TEST_CASE("corrected speed module: no speed channel gives E-CSD-NO-SPEED on both outputs", "[unit][csd]") {
    // D-016: without a usable speed channel the outputs are in Error and the graph shows the "no distance
    // data" banner for the source (consumers read the Corr Dist status).
    test::Runtime rt;
    std::vector<model::Channel> channels;
    channels.push_back(
        test::uniformF32(rt.cache, "Engine RPM", units::kNoUnit, Rate::fromHz(20), {1.F, 2.F}));
    auto src = test::makeSource(std::move(channels), 100'000, "csd-no-speed");
    CorrectedSpeedDistanceModule module = loadedModule();
    MathGraph g(src, "Normal", test::shippedUnits(), {}, &rt.pool, &rt.cache);
    Diagnostics diags;
    Result<std::vector<ChannelKey>> keys = g.defineBuiltin(module, {}, diags);
    if (analysisStubbed(keys)) {
        SKIP("analysis::CorrectedSpeedSettings::fromJson is still an M1 stub: " << keys.error().message);
    }
    REQUIRE(keys);
    // A named Future: get() returns a reference into the shared state, which a temporary Future would release
    // at the end of the statement (GCC 13 -Wdangling-reference). Not take(): the graph reads the result as
    // well.
    const Future<ColumnHandle> distFuture =
        g.require((*keys)[1], SampleRange::all(), Priority::Background, {});
    const Result<ColumnHandle>& dist = distFuture.get();
    if (analysisStubbed(dist)) {
        SKIP("analysis::CorrectedSpeed is still an M1 stub: " << dist.error().message);
    }
    REQUIRE_FALSE(dist);
    static_cast<void>(g.applyReadyCompletions());
    for (const ChannelKey& k : *keys) {
        INFO(k.nameCf);
        const MathStatus st = g.status(k);
        CHECK(st.state == MathState::Error);
        REQUIRE_FALSE(st.diags.empty());
        CHECK(st.diags[0].code == "E-CSD-NO-SPEED");
        CHECK(toChannelStatus(st.state) == model::ChannelStatus::Error);
    }
}
