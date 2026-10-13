// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// esses-cli eval / maths check / maths functions (03 section 13, AC-M2-01, AC-M2-14): the compat switches,
// the CLI's source scope (channels, logged names, details, constants, maths outputs), the diagnostic JSON,
// eval's refusals and the per-item status of maths check. The evaluation over a real log is the ctest
// cli.eval.* (fixture inputs, apps/cli/CMakeLists.txt).

#include "app_context.hpp"
#include "maths_cli.hpp"

#include <esses/base/hash.hpp>
#include <esses/maths/function_registry.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/source.hpp>
#include <esses/units/unit_registry.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <system_error>

using namespace esses;
using namespace esses::cli;

namespace {

std::filesystem::path repoRoot() {
    return std::filesystem::path(ESSES_TEST_REPO_ROOT);
}

const units::UnitRegistry& unitTable() {
    static const units::UnitRegistry registry = [] {
        Result<units::UnitRegistry> r = units::UnitRegistry::loadFiles(repoRoot() / "data" / "units.json");
        REQUIRE(r.ok());
        return std::move(r).value();
    }();
    return registry;
}

AppContext context() {
    GlobalOptions g;
    g.dataDir = repoRoot() / "data";
    g.unitsFile = repoRoot() / "data" / "units.json";
    g.schemaDir = repoRoot() / "schemas";
    return AppContext(g);
}

std::shared_ptr<model::DataSource> source() {
    auto src = std::make_shared<model::DataSource>();
    src->id = hash128("maths-cli-test");
    model::DataLayer layer;
    layer.duration = 1'000'000;
    model::Channel speed;
    speed.desc.name = "Ground Speed";
    speed.desc.loggedName = "SPEED";
    speed.desc.unit = *unitTable().find("m/s");
    speed.desc.rate = Rate::fromHz(100);
    layer.addChannel(speed);
    model::Channel gear;
    gear.desc.name = "Gear";
    gear.desc.loggedName = "Gear";
    gear.desc.dtype = store::Dtype::I32;
    gear.desc.rate = Rate::fromHz(10);
    layer.addChannel(gear);
    src->layers.push_back(std::move(layer));
    model::Detail driver;
    driver.id = "Driver";
    driver.value = std::string("Joe");
    src->details.push_back(driver);
    model::Detail wheelbase;
    wheelbase.id = "Vehicle Wheelbase";
    wheelbase.value = model::makeQuantity(2.7, *unitTable().find("m"), unitTable());
    src->details.push_back(wheelbase);
    return src;
}

// A fresh, empty directory under the system temporary directory, removed on destruction. The constructor
// creates it (create_directory) and never takes a name that exists, so a case that checks a directory sees
// only its own modules, and test processes running at once, in one build tree or in several, never share one.
class TempDir {
public:
    explicit TempDir(const std::string& tag) {
        static std::atomic<unsigned> counter{0};
        static const unsigned salt = std::random_device{}();
        const std::filesystem::path base = std::filesystem::temp_directory_path();
        for (;;) {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ = base / ("esses-maths-cli-" + tag + "-" + std::to_string(salt) + "-" +
                            std::to_string(now) + "-" + std::to_string(++counter));
            std::error_code ec;
            if (std::filesystem::create_directory(path_, ec)) {
                break;
            }
            if (ec) {
                FAIL("cannot create a temporary directory: " << ec.message());
            }
        }
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::filesystem::path writeTemp(const TempDir& dir, const std::string& name, const std::string& text) {
    const std::filesystem::path p = dir.path() / name;
    std::ofstream(p, std::ios::binary) << text;
    return p;
}

} // namespace

TEST_CASE("maths cli: compat switches", "[unit][cli]") {
    Result<maths::MathsCompat> c = parseCompat({"unaryMinusAbovePower=1", "alias:MIN_RUNNING=running_min"});
    REQUIRE(c.ok());
    CHECK(c->unaryMinusAbovePower);
    CHECK(c->functionAliases.at("min_running") == "running_min");
    CHECK_FALSE(parseCompat({"unaryMinusAbovePower=maybe"}).ok());
    CHECK_FALSE(parseCompat({"nonsense"}).ok());
    CHECK_FALSE(parseCompat({"other=1"}).ok());
}

TEST_CASE("maths cli: the source scope resolves channels, logged names, details, constants and outputs",
          "[unit][cli]") {
    maths::MathModule module;
    module.constants.push_back(maths::MathConstant{"Gain", maths::UnitValue{2.0, units::kNoUnit}, true, ""});
    module.constants.push_back(maths::MathConstant{"Gear", maths::UnitValue{1.0, units::kNoUnit}, true, ""});
    module.constants.push_back(
        maths::MathConstant{"Ref Speed", maths::UnitValue{36.0, *unitTable().find("km/h")}, true, ""});
    maths::MathItem out;
    out.name = "Speed x2";
    out.expr = "'Ground Speed'[m/s] * 2";
    out.resultUnit = *unitTable().find("km/h");
    module.items.push_back(out);
    const SourceScope scope(source(), "Normal", &module, maths::FunctionRegistry::builtin(), unitTable());
    CHECK(scope.dataBacked());

    auto e = scope.lookup("ground speed", {});
    REQUIRE(e.has_value());
    CHECK(e->what == maths::ScopeEntry::Channel);
    CHECK(e->type.rate == Rate::fromHz(100));
    CHECK(scope.lookup("speed", {}).has_value()); // the logged name
    e = scope.lookup("gear", {});
    REQUIRE(e.has_value());
    CHECK(e->type.integer);
    CHECK(e->diags.size() == 1); // also a constant: W-CONST-SHADOWED
    e = scope.lookup("driver", {});
    REQUIRE(e.has_value());
    CHECK(e->what == maths::ScopeEntry::DetailText);
    CHECK(e->text == "Joe");
    e = scope.lookup("vehicle wheelbase", {});
    REQUIRE(e.has_value());
    CHECK(e->what == maths::ScopeEntry::DetailNumber);
    CHECK(*e->number == 2.7);
    e = scope.lookup("ref speed", {});
    REQUIRE(e.has_value());
    CHECK(e->what == maths::ScopeEntry::Constant);
    CHECK(*e->number == 10.0); // in the base unit, m/s
    e = scope.lookup("speed x2", {});
    REQUIRE(e.has_value());
    CHECK(e->type.unit == *unitTable().find("m/s")); // a maths output, stored in base units
    CHECK(scope.lookup("corr dist", {}).has_value());
    CHECK_FALSE(scope.lookup("nope", {}).has_value());

    const SourceScope dataless(nullptr, "Normal", &module, maths::FunctionRegistry::builtin(), unitTable());
    CHECK_FALSE(dataless.dataBacked());
    CHECK(dataless.lookup("gain", {}).has_value());
    CHECK_FALSE(dataless.lookup("ground speed", {}).has_value());
}

TEST_CASE("maths cli: diagnostics as JSON and text", "[unit][cli]") {
    maths::Diagnostics d;
    d.push_back(maths::Diagnostic{
        "E-UNIT-UNKNOWN", maths::Diagnostic::Error, {4, 9}, "unknown unit [kmh]", "did you mean [km/h]?"});
    d.push_back(maths::Diagnostic{"I-COMPAT-POW", maths::Diagnostic::Info, {0, 4}, "power", {}});
    const nlohmann::json j = diagnosticsJson(d);
    REQUIRE(j.size() == 2);
    CHECK(j[0].at("code") == "E-UNIT-UNKNOWN");
    CHECK(j[0].at("level") == "error");
    CHECK(j[0].at("begin") == 4);
    CHECK(j[0].at("hint") == "did you mean [km/h]?");
    CHECK_FALSE(j[1].contains("hint"));
    CHECK(diagnosticText(d[0]) == "E-UNIT-UNKNOWN error 4-9: unknown unit [kmh] (did you mean [km/h]?)");
}

TEST_CASE("maths cli: eval refuses bad expressions and options", "[unit][cli]") {
    AppContext ctx = context();
    EvalOptions o;
    o.files = {repoRoot() / "no-such-log.ld"};
    std::ostringstream out;
    std::ostringstream err;
    o.expression = "'X' +";
    o.json = true;
    CHECK(runEval(ctx, o, out, err) == 1);
    const nlohmann::json doc = nlohmann::json::parse(out.str());
    CHECK(doc.at("diagnostics").at(0).at("code") == "E-SYNTAX");
    CHECK(doc.at("results").empty());

    o.expression = "'X' * 2";
    o.json = false;
    o.csv = false;
    std::ostringstream err2;
    CHECK(runEval(ctx, o, out, err2) == 1); // the log does not exist
    CHECK(err2.str().find("no-such-log.ld") != std::string::npos);
    o.rate = "fast";
    CHECK(runEval(ctx, o, out, err) == 2);
    o.rate = "auto";
    o.unit = "furlongs";
    CHECK(runEval(ctx, o, out, err) == 2);
    o.unit.clear();
    o.compat = {"unaryMinusAbovePower=2"};
    CHECK(runEval(ctx, o, out, err) == 2);
    o.compat.clear();
    o.json = true;
    o.csv = true;
    CHECK(runEval(ctx, o, out, err) == 2);
}

TEST_CASE("maths cli: maths check reports per-item status and summary counts", "[unit][cli]") {
    const TempDir dir("status-counts");
    const std::filesystem::path file = writeTemp(dir, "demo.maths.json", R"json({
  "$schema": "urn:esses:maths-1", "schemaVersion": 1, "id": "demo",
  "condition": "'Driver' == \"Joe\"",
  "constants": [{"name": "Gain", "value": {"v": 2, "unit": ""}}],
  "items": [
    {"kind": "expression", "name": "A", "expr": "'Speed'[km/h] * 'Gain'", "resultUnit": "km/h"},
    {"kind": "expression", "name": "B", "expr": "'A'[km/h] + 'Other Speed'[km/h]"},
    {"kind": "expression", "name": "C", "expr": "smoth('A', 3)"},
    {"kind": "expression", "name": "D", "expr": "'A' +"},
    {"kind": "expression", "name": "A", "expr": "1"},
    {"kind": "filter", "name": "Lat G", "window": {"points": 5}},
    {"kind": "expression", "name": "E", "enabled": false, "expr": "'Nope'"},
    {"kind": "table3d", "name": "T3", "x": {"expr": "'A'", "unit": "", "breakpoints": [0, 1]},
     "y": {"expr": "'A'", "unit": "", "breakpoints": [0, 1]}, "values": {"unit": "", "data": [[1, 2], [3, 4]]}},
    {"kind": "builtin", "module": "esses.corrected_speed_distance", "version": 1}
  ]})json");
    AppContext ctx = context();
    MathsCheckOptions o;
    o.modules = {file};
    o.json = true;
    std::ostringstream out;
    std::ostringstream err;
    CHECK(runMathsCheck(ctx, o, out, err) == 1); // C, D and T3 are errors
    const nlohmann::json doc = nlohmann::json::parse(out.str());
    const nlohmann::json& items = doc.at("modules").at(0).at("items");
    REQUIRE(items.size() == 9);
    CHECK(items[0].at("status") == "Ok"); // without data: I-UNRESOLVED infos only
    CHECK(items[1].at("status") == "Ok");
    CHECK(items[2].at("status") == "Error");   // E-UNKNOWN-FUNCTION
    CHECK(items[3].at("status") == "Error");   // E-SYNTAX
    CHECK(items[4].at("status") == "Warning"); // W-DUP-IGNORED
    CHECK(items[5].at("status") == "Ok");
    CHECK(items[6].at("status") == "Disabled");
    CHECK(items[7].at("status") == "Error"); // E-KIND-UNSUPPORTED
    CHECK(items[8].at("status") == "Ok");
    const nlohmann::json& summary = doc.at("summary");
    CHECK(summary.at("items") == 9);
    CHECK(summary.at("ok") == 4);
    CHECK(summary.at("warning") == 1);
    CHECK(summary.at("error") == 3);
    CHECK(summary.at("disabled") == 1);
    CHECK(summary.at("parseErrors") == 1);
    CHECK(summary.at("codes").at("I-UNRESOLVED").get<int>() >= 2);
    CHECK(doc.at("modules").at(0).at("condition").at("status") == "Ok");

    std::ostringstream text;
    o.json = false;
    CHECK(runMathsCheck(ctx, o, text, err) == 1);
    CHECK(text.str().find("9 items: 4 ok, 1 warning, 3 error, 1 disabled; 1 parse errors") !=
          std::string::npos);

    const std::filesystem::path broken =
        writeTemp(dir, "broken.maths.json", R"({"$schema": "urn:esses:maths-1"})");
    o.modules = {broken};
    o.json = true;
    std::ostringstream out2;
    CHECK(runMathsCheck(ctx, o, out2, err) == 1);
    CHECK(nlohmann::json::parse(out2.str()).at("summary").at("moduleErrors") == 1);
}

// D-188: maths check reports what MathGraph finds, so cycles between items, filter and scale_offset
// synthesis errors and the table stage's errors are Errors (the item-by-item checker it
// replaces reported every one of them Ok or Warning), across modules in the order given.
TEST_CASE("maths cli: maths check finds cycles, synthesis and table errors through MathGraph",
          "[unit][cli]") {
    const TempDir dir("graph-check");
    writeTemp(dir, "a-first.maths.json", R"json({
  "$schema": "urn:esses:maths-1", "schemaVersion": 1, "id": "first",
  "constants": [{"name": "Gain", "value": {"v": 2, "unit": ""}}],
  "items": [
    {"kind": "expression", "name": "A", "expr": "'B' + 1"},
    {"kind": "expression", "name": "B", "expr": "'A' * 2"},
    {"kind": "filter", "name": "Lat G", "window": {"points": -3}},
    {"kind": "table2d", "name": "T", "axis": {"expr": "'Gain' * 2", "unit": "", "breakpoints": [2, 1]},
     "values": {"unit": "", "data": [0, 1]}},
    {"kind": "expression", "name": "Fine", "expr": "'Gain' * 3"}
  ]})json");
    writeTemp(dir, "b-second.maths.json", R"json({
  "$schema": "urn:esses:maths-1", "schemaVersion": 1, "id": "first",
  "items": [{"kind": "expression", "name": "C", "expr": "'A' + 'Unknown'"}]})json");
    AppContext ctx = context();
    MathsCheckOptions o;
    o.modules = {dir.path()};
    o.json = true;
    std::ostringstream out;
    std::ostringstream err;
    CHECK(runMathsCheck(ctx, o, out, err) == 1);
    const nlohmann::json doc = nlohmann::json::parse(out.str());
    REQUIRE(doc.at("modules").size() == 2);
    const nlohmann::json& items = doc.at("modules").at(0).at("items");
    REQUIRE(items.size() == 5);
    const auto codes = [](const nlohmann::json& item) {
        std::set<std::string> found;
        for (const nlohmann::json& d : item.at("diagnostics")) {
            found.insert(d.at("code").get<std::string>());
        }
        return found;
    };
    CHECK(items[0].at("status") == "Error");
    CHECK(codes(items[0]).count("E-CYCLE") == 1);
    CHECK(items[1].at("status") == "Error");
    CHECK(codes(items[1]).count("E-CYCLE") == 1);
    CHECK(items[2].at("status") == "Error"); // a negative filter window
    CHECK(codes(items[2]).count("E-ARG-VALUE") == 1);
    CHECK(items[3].at("status") == "Error");
    CHECK(codes(items[3]).count("E-TABLE-AXIS") == 1);
    CHECK(items[4].at("status") == "Ok");
    // The second module (the same id, given later) is checked after the first: without data, the name it
    // cannot resolve is I-UNRESOLVED and the maths output A of the first module resolves.
    const nlohmann::json& c = doc.at("modules").at(1).at("items").at(0);
    CHECK(doc.at("modules").at(1).at("id") == "first");
    CHECK(c.at("status") == "Ok");
    CHECK(codes(c) == std::set<std::string>{"I-UNRESOLVED"});
    CHECK(doc.at("summary").at("parseErrors") == 0);
}

TEST_CASE("maths cli: maths functions lists the registry", "[unit][cli]") {
    AppContext ctx = context();
    std::ostringstream out;
    std::ostringstream err;
    CHECK(runMathsFunctions(ctx, true, out, err) == 0);
    const nlohmann::json doc = nlohmann::json::parse(out.str());
    CHECK(doc.at("functions").size() == maths::FunctionRegistry::builtin().all().size());
}
