// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The Mapping-panel hand-off of the open pipeline (05 sections 2 and 8, D-028, D-029; AC-M2-11 at the
// registry level): unknown units end task A with a PendingResolution through the MappingHook instead of
// publishing; resume runs task B with the user's MappingDecision (unit map, analysis names, open anyway); a
// complete unit map applies silently; headless opens go unitless or fail with strictUnits.
#include "text_test_support.hpp"

#include <catch2/catch_approx.hpp>

using namespace esses;
using namespace esses::formats;
using namespace esses::formats::texttest;
using Catch::Approx;

namespace {

// Two unknown units (zorks, blips) and one the registry knows.
constexpr std::string_view kFile = "Time,Boost,Coolant,Speed\n"
                                   "s,zorks,blips,km/h\n"
                                   "0,100,20,36\n"
                                   "0.5,150,21,72\n"
                                   "1.0,200,22,108\n";

UnitMappingProfile completeMap() {
    UnitMappingProfile m;
    m.importer = std::string(kGenericCsvImporterId);
    m.rules.push_back(
        {UnitMappingProfile::Match::NameAndUnit, "Boost", "zorks", "Pressure", "kPa", "Boost Pressure"});
    m.rules.push_back({UnitMappingProfile::Match::UnitOnly, "", "blips", "Temperature", "deg C", ""});
    return m;
}

struct Fixture {
    TempDir dir{"mapping"};
    std::filesystem::path file;
    Env env;
    TaskPool pool{2};
    Fixture() {
        file = dir.path() / "two-unknown-units.csv";
        writeText(file, kFile);
    }
};

MappingHook packagingHook(std::atomic<int>& calls) {
    return [&calls](std::vector<UnresolvedRow> rows, std::shared_ptr<const OpenState> skeleton) {
        ++calls;
        PendingResolution p;
        p.rows = std::move(rows);
        p.skeleton = std::move(skeleton);
        return p;
    };
}

} // namespace

TEST_CASE("text mapping: two unknown units block publish until resolved", "[unit]") {
    Fixture f;
    std::atomic<int> calls{0};
    auto a = f.env.registry->load(f.file, {}, f.pool, f.env.context(), {}, packagingHook(calls));
    const Result<LoadResult>& ra = a.get();
    REQUIRE(ra.ok());
    REQUIRE(std::holds_alternative<PendingResolution>(*ra));
    CHECK(calls == 1);
    const PendingResolution& pending = std::get<PendingResolution>(*ra);
    CHECK(pending.importerId == kGenericCsvImporterId);
    CHECK(pending.path.find("two-unknown-units.csv") != std::string::npos);
    REQUIRE(pending.rows.size() == 2);
    CHECK(pending.rows[0].loggedName == "Boost");
    CHECK(pending.rows[0].vendorUnit == "zorks");
    CHECK(pending.rows[0].sampleValues == std::vector<double>{100, 150, 200});
    CHECK(pending.rows[1].vendorUnit == "blips");

    // Task B with the user's answers: units converted to base at decode, the analysis name as a Mapping.
    MappingDecision decision;
    decision.unitMap = completeMap();
    decision.saveUnitMap = true;
    auto b = f.env.registry->resume(pending, std::move(decision), f.pool, f.env.context());
    const Result<OpenedSource>& rb = b.get();
    REQUIRE(rb.ok());
    const model::Channel& boost = channel(*rb, "Boost Pressure");
    CHECK(boost.desc.loggedName == "Boost");
    CHECK(f.env.units->unit(boost.desc.unit).symbol == "Pa");
    CHECK(dumpSeries(boost.series).values[1] == Approx(150000.0));
    CHECK(boost.status == model::ChannelStatus::Ok);
    CHECK(boost.desc.statusText.empty());
    const model::Channel& coolant = channel(*rb, "Coolant");
    CHECK(dumpSeries(coolant.series).values[0] == Approx(293.15));
    CHECK_FALSE(hasCode(rb->log, "open.unit_unknown"));
}

TEST_CASE("text mapping: a complete unit map applies silently, without the hook", "[unit]") {
    Fixture f;
    std::atomic<int> calls{0};
    OpenContext ctx = f.env.context();
    ctx.unitMaps.emplace(UnitMappingProfile::fileStem(kGenericCsvImporterId), completeMap());
    auto a = f.env.registry->load(f.file, {}, f.pool, ctx, {}, packagingHook(calls));
    const Result<LoadResult>& ra = a.get();
    REQUIRE(ra.ok());
    REQUIRE(std::holds_alternative<OpenedSource>(*ra));
    CHECK(calls == 0);
    const OpenedSource& o = std::get<OpenedSource>(*ra);
    CHECK(f.env.units->quantity(channel(o, "Boost Pressure").desc.quantity).name == "Pressure");
    CHECK(f.env.units->quantity(channel(o, "Speed").desc.quantity).name == "Speed");

    // The same map from a file named by ImportOptions::unitMap (headless, esses-cli --unitmap).
    const auto mapFile = f.dir.path() / "generic.csv.json";
    REQUIRE(completeMap().save(mapFile).ok());
    ImportOptions opt;
    opt.unitMap = mapFile;
    auto h = f.env.registry->open(f.file, opt, f.env.context());
    REQUIRE(h.ok());
    CHECK(channel(*h, "Boost Pressure").desc.quantity == channel(o, "Boost Pressure").desc.quantity);
    ImportOptions broken;
    broken.unitMap = f.dir.path() / "missing.json";
    auto hb = f.env.registry->open(f.file, broken, f.env.context());
    REQUIRE(hb.ok());
    CHECK(hasCode(hb->log, "open.unitmap_load_failed"));
}

TEST_CASE("text mapping: open anyway, cancel-free headless opens and strict units", "[unit]") {
    Fixture f;
    std::atomic<int> calls{0};
    auto a = f.env.registry->load(f.file, {}, f.pool, f.env.context(), {}, packagingHook(calls));
    REQUIRE(a.get().ok());
    const PendingResolution pending = std::get<PendingResolution>(*a.get());
    MappingDecision anyway;
    anyway.openAnyway = true;
    anyway.mappings.push_back(model::MappingRow{"Coolant", "Water Temp", std::nullopt, false});
    auto b = f.env.registry->resume(pending, anyway, f.pool, f.env.context());
    REQUIRE(b.get().ok());
    const OpenedSource& o = *b.get();
    const model::Channel& boost = channel(o, "Boost");
    CHECK(boost.desc.unit == units::kNoUnit);
    CHECK(boost.desc.statusText == "Unit unknown: \"zorks\"");
    CHECK(boost.status == model::ChannelStatus::Ok);
    CHECK(channel(o, "Water Temp").desc.loggedName == "Coolant");
    CHECK(o.log.end() != std::find_if(o.log.begin(), o.log.end(), [](const ImportLogEntry& e) {
              return e.code == "open.unit_unknown" && e.ctx.detail == "blips";
          }));

    // Without a hook the open publishes at once, unitless (05 section 2 step 4, headless).
    auto plain = f.env.registry->open(f.file, {}, f.env.context());
    REQUIRE(plain.ok());
    CHECK(channel(*plain, "Coolant").desc.statusText == "Unit unknown: \"blips\"");
    ImportOptions strict;
    strict.strictUnits = true;
    auto s = f.env.registry->open(f.file, strict, f.env.context());
    REQUIRE_FALSE(s.ok());
    CHECK(s.error().code == ErrorCode::NotFound);
    CHECK(s.error().message.find("zorks") != std::string::npos);
}

TEST_CASE("text mapping: a quantity without a unit is unresolved; a channel with neither is not", "[unit]") {
    TempDir dir("hint");
    const auto file = dir.path() / "hint.csv";
    writeText(file, "Time,Throttle,Count\n0,10,1\n1,20,2\n");
    Env env;
    std::vector<std::string> log;
    auto profile = TextProfile::parse(
        R"({"$schema": "urn:esses:importer-profile-1", "schemaVersion": 1, "id": "hinted", "name": "H",
            "version": "1", "delimiter": ",", "sniff": {"extensions": [".csv"], "confidence": 45},
            "columns": {"Throttle": {"quantity": "Ratio", "name": "Throttle Pos"}}})",
        log);
    REQUIRE(profile.ok());
    REQUIRE(env.registry->addProfile(*profile).ok());
    TaskPool pool(1);
    std::atomic<int> calls{0};
    ImportOptions opt;
    opt.importerId = "text.profile:hinted";
    auto a = env.registry->load(file, opt, pool, env.context(), {}, packagingHook(calls));
    REQUIRE(a.get().ok());
    REQUIRE(std::holds_alternative<PendingResolution>(*a.get()));
    const PendingResolution& p = std::get<PendingResolution>(*a.get());
    REQUIRE(p.rows.size() == 1);
    CHECK(p.rows[0].loggedName == "Throttle");
    CHECK(p.rows[0].inferredQuantity == "Ratio");
    // Resolved by a nameOnly rule; the column hint's name is the channel's analysis name (step 4a).
    MappingDecision d;
    UnitMappingProfile m;
    m.importer = "text.profile:hinted";
    m.rules.push_back({UnitMappingProfile::Match::NameOnly, "Throttle", "", "Ratio", "%", ""});
    d.unitMap = m;
    auto b = env.registry->resume(p, d, pool, env.context());
    REQUIRE(b.get().ok());
    const model::Channel& t = channel(*b.get(), "Throttle Pos");
    CHECK(t.desc.loggedName == "Throttle");
    CHECK(dumpSeries(t.series).values[1] == Approx(0.2));
    CHECK(channel(*b.get(), "Count").desc.unit == units::kNoUnit);
}
