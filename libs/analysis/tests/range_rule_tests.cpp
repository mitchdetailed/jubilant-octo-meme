// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// rule-1 documents (02 section 4.5): the shipped data/rules files, the envelope parser, the kind-specific
// readers and RuleRunner::fromRuleIds.
#include "analysis_test_support.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::analysis;
using esses::analysis::test::repoRoot;

namespace {

std::filesystem::path rulesDir() {
    return repoRoot() / "data" / "rules";
}

// A minimal valid beacon-pair document with `extra` members spliced in (a JSON fragment ending in a comma).
std::string beaconDoc(const std::string& extra = {}, const std::string& classes = {}) {
    return R"({"$schema": "urn:esses:rule-1", "schemaVersion": 1, "id": "test.beacons", "kind": "beacon-pair", )" +
           extra +
           R"("produces": "Outings:Laps", "transitions": [{"from": "BCN", "to": "BCN", "class": "flying"}], )"
           R"("classes": {"flying": {"name": "Lap {n}", "abbrev": "L{n}", "numbered": true})" +
           classes + "}}";
}

Error settingsError(const std::string& json) {
    auto doc = RuleDocument::parse(json);
    REQUIRE(doc);
    auto s = BeaconPairSettings::fromDocument(*doc);
    REQUIRE_FALSE(s);
    return s.error();
}

} // namespace

TEST_CASE("rule documents: the shipped circuit rules load", "[unit]") {
    for (const char* id :
         {"circuit.beacon-pair", "circuit.whole-outing", "circuit.track-sections", "circuit.detail-groups"}) {
        INFO(id);
        auto doc = RuleDocument::load(rulesDir() / (std::string(id) + ".rule.json"));
        REQUIRE(doc);
        CHECK(doc->id == id);
    }
    auto beacons = RuleDocument::load(rulesDir() / "circuit.beacon-pair.rule.json");
    REQUIRE(beacons);
    CHECK(beacons->kind == "beacon-pair");
    CHECK(beacons->phase == 1);
    CHECK(beacons->produces == "Outings:Laps");
    CHECK(beacons->parent == "Outings");
    CHECK(beacons->layers == std::vector<std::string>{"Normal", "Fastest"});
    CHECK(beacons->appliesTo("Normal"));
    CHECK(beacons->appliesTo("")); // "" is Normal
    CHECK(beacons->appliesTo("fastest"));
    CHECK_FALSE(beacons->appliesTo("Burst0"));

    auto outing = RuleDocument::load(rulesDir() / "circuit.whole-outing.rule.json");
    REQUIRE(outing);
    CHECK(outing->layers == std::vector<std::string>{"*"});
    CHECK(outing->appliesTo("Burst0"));

    auto sections = RuleDocument::load(rulesDir() / "circuit.track-sections.rule.json");
    REQUIRE(sections);
    CHECK(sections->phase == 2);
    CHECK(sections->layers == std::vector<std::string>{"*"}); // absent: every layer
}

TEST_CASE("rule documents: the shipped beacon-pair rule equals the circuit default built in code", "[unit]") {
    auto doc = RuleDocument::load(rulesDir() / "circuit.beacon-pair.rule.json");
    REQUIRE(doc);
    auto parsed = BeaconPairSettings::fromDocument(*doc);
    REQUIRE(parsed);
    const BeaconPairSettings& a = *parsed;
    const BeaconPairSettings b = BeaconPairSettings::circuitDefault();
    CHECK(a.produces == b.produces);
    CHECK(a.parent == b.parent);
    CHECK(a.layers == b.layers);
    CHECK(a.markerGroups == b.markerGroups);
    CHECK(a.ignoreClasses == b.ignoreClasses);
    REQUIRE(a.transitions.size() == b.transitions.size());
    for (std::size_t i = 0; i < a.transitions.size(); ++i) {
        CHECK(a.transitions[i].from == b.transitions[i].from);
        CHECK(a.transitions[i].to == b.transitions[i].to);
        CHECK(a.transitions[i].lapClass == b.transitions[i].lapClass);
    }
    REQUIRE(a.classes.size() == b.classes.size());
    for (const auto& [key, c] : b.classes) {
        INFO(key);
        REQUIRE(a.classes.count(key) == 1);
        const BeaconPairClass& p = a.classes.at(key);
        CHECK(p.name == c.name);
        CHECK(p.abbrev == c.abbrev);
        CHECK(p.trusted == c.trusted);
        CHECK(p.numbered == c.numbered);
        CHECK(p.fixedNumber == c.fixedNumber);
        CHECK(p.numberIsLastPlusOne == c.numberIsLastPlusOne);
    }
    CHECK(a.startDetail == "Start Lap");
    CHECK(a.defaultStart == 1);
    CHECK(a.annotations.enabled);
    CHECK(a.annotations.counterChannel == "Lap Number");
    CHECK(a.annotations.dataGapUs == 10'000'000);
    CHECK(a.annotations.stationarySec == 15.0);
    CHECK(a.annotations.speedChannel == "Corr Speed");
    CHECK(a.annotations.stationaryBelowMps == Catch::Approx(1.0 / 3.6).epsilon(1e-12));
    CHECK(a.annotations.stationaryBelowMps == Catch::Approx(b.annotations.stationaryBelowMps).epsilon(1e-12));
    CHECK(a.supportsTrackGeneration);
    CHECK(a.supportsReporting);
}

TEST_CASE("rule documents: a malformed envelope is rejected with its JSON pointer", "[unit]") {
    const auto detailOf = [](const std::string& json) {
        auto r = RuleDocument::parse(json);
        REQUIRE_FALSE(r);
        return r.error().context.detail.value_or("<none>");
    };
    CHECK(RuleDocument::parse("not json").error().code == ErrorCode::Format);
    CHECK_FALSE(RuleDocument::parse("[1]"));
    // A missing member is reported at its object (jsonio::ObjectReader::required).
    CHECK(detailOf("{}") == "");
    CHECK(detailOf(R"({"$schema": "urn:esses:rule-1", "schemaVersion": 1, "kind": "beacon-pair"})") == "");
    CHECK(detailOf(R"({"$schema": "urn:esses:rule-1", "schemaVersion": 1, "id": 5, "kind": "x"})") == "/id");
    CHECK(detailOf(R"({"$schema": "urn:esses:rule-1", "schemaVersion": 1, "id": "a", "kind": ""})") ==
          "/kind");
    CHECK(detailOf(
              R"({"$schema": "urn:esses:rule-1", "schemaVersion": 1, "id": "a", "kind": "x", "phase": 3})") ==
          "/phase");
    CHECK(
        detailOf(
            R"({"$schema": "urn:esses:rule-1", "schemaVersion": 1, "id": "a", "kind": "x", "layers": []})") ==
        "/layers");
    CHECK(detailOf(R"({"$schema": "urn:esses:rule-1", "schemaVersion": 1, "id": "a", "kind": "x",
                       "produces": "Outings:Laps", "parent": "Runs"})") == "/parent");
    auto major =
        RuleDocument::parse(R"({"$schema": "urn:esses:rule-2", "schemaVersion": 1, "id": "a", "kind": "x"})");
    REQUIRE_FALSE(major);
    CHECK(major.error().code == ErrorCode::Unsupported);

    auto missing = RuleDocument::load(rulesDir() / "no-such.rule.json");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::Io);
}

TEST_CASE("rule documents: makeRangeRule picks the implementation by kind", "[unit]") {
    auto beacons = RuleDocument::load(rulesDir() / "circuit.beacon-pair.rule.json");
    auto outing = RuleDocument::load(rulesDir() / "circuit.whole-outing.rule.json");
    auto sections = RuleDocument::load(rulesDir() / "circuit.track-sections.rule.json");
    auto details = RuleDocument::load(rulesDir() / "circuit.detail-groups.rule.json");
    REQUIRE((beacons && outing && sections && details));

    auto b = makeRangeRule(*beacons);
    REQUIRE(b);
    CHECK((*b)->kind() == "beacon-pair");
    CHECK((*b)->id() == "circuit.beacon-pair");
    CHECK((*b)->phase() == 1);
    CHECK((*b)->appliesTo("Fastest"));
    CHECK_FALSE((*b)->appliesTo("Burst0"));

    auto w = makeRangeRule(*outing);
    REQUIRE(w);
    CHECK((*w)->kind() == "whole-outing");
    CHECK((*w)->appliesTo("Burst0"));

    auto s = makeRangeRule(*sections);
    REQUIRE(s);
    CHECK((*s)->kind() == "track-sections");
    CHECK((*s)->phase() == 2);
    CHECK(makeRangeRule(*details).error().code == ErrorCode::Unsupported);

    RuleDocument unknown = *outing;
    unknown.kind = "lap-counter";
    CHECK(makeRangeRule(unknown).error().code == ErrorCode::Invalid);
}

TEST_CASE("beacon-pair settings: kind-specific errors carry their JSON pointer", "[unit]") {
    REQUIRE(RuleDocument::parse(beaconDoc()));
    REQUIRE(BeaconPairSettings::fromDocument(*RuleDocument::parse(beaconDoc())));

    CHECK(settingsError(beaconDoc({}, R"(, "warmup": {"name": "W", "abbrev": "W"})")).context.detail ==
          "/classes/warmup");
    CHECK(settingsError(
              beaconDoc({}, R"(, "out": {"name": "O", "abbrev": "O", "numbered": true, "number": 0})"))
              .context.detail == "/classes/out/number");
    CHECK(settingsError(beaconDoc(R"("colour": "red", )")).context.detail == "/colour");
    CHECK(settingsError(beaconDoc(R"("ignoreClasses": ["SOR"], )")).context.detail == "/ignoreClasses/0");
    CHECK(settingsError(beaconDoc(R"("annotations": {"phase": 1}, )")).context.detail ==
          "/annotations/phase");
    CHECK(settingsError(
              beaconDoc(R"("annotations": {"pitStop": {"stationaryBelow": {"v": 1, "unit": "rpm"}}}, )"))
              .context.detail == "/annotations/pitStop/stationaryBelow/unit");
    CHECK(settingsError(beaconDoc(R"("annotations": {"stint": {"dataGapUs": 0}}, )")).context.detail ==
          "/annotations/stint/dataGapUs");

    const std::string badTransition =
        R"({"$schema": "urn:esses:rule-1", "schemaVersion": 1, "id": "t", "kind": "beacon-pair",
            "produces": "Laps", "transitions": [{"from": "SOR", "to": "BCN", "class": "flying"}],
            "classes": {"flying": {"name": "L", "abbrev": "L", "numbered": true}}})";
    CHECK(settingsError(badTransition).context.detail == "/transitions/0/from");
    const std::string undefinedClass =
        R"({"$schema": "urn:esses:rule-1", "schemaVersion": 1, "id": "t", "kind": "beacon-pair",
            "produces": "Laps", "transitions": [{"from": "BCN", "to": "BCN", "class": "in"}],
            "classes": {"flying": {"name": "L", "abbrev": "L", "numbered": true}}})";
    CHECK(settingsError(undefinedClass).context.detail == "/transitions/0/class");

    auto mph = RuleDocument::parse(beaconDoc(
        R"("annotations": {"pitStop": {"stationaryBelow": {"v": 2, "unit": "mph"}}}, "layers": ["*"], )"));
    REQUIRE(mph);
    auto s = BeaconPairSettings::fromDocument(*mph);
    REQUIRE(s);
    CHECK(s->annotations.stationaryBelowMps == Catch::Approx(2 * 0.44704).epsilon(1e-12));
    CHECK(s->layers == std::vector<std::string>{"*"});
    CHECK(s->parent == "Outings");
    auto wrongKind = *mph;
    wrongKind.kind = "whole-outing";
    CHECK(BeaconPairSettings::fromDocument(wrongKind).error().code == ErrorCode::Invalid);
}

TEST_CASE("whole-outing rule: the document options", "[unit]") {
    const auto doc = [](const std::string& extra) {
        auto d = RuleDocument::parse(
            R"({"$schema": "urn:esses:rule-1", "schemaVersion": 1, "id": "w", "kind": "whole-outing", )" +
            extra + "}");
        REQUIRE(d);
        return *d;
    };
    auto session = WholeOutingRule::fromDocument(doc(R"("produces": "Sessions", "name": "'Session'",
                                                         "trusted": false, "layers": ["Normal"])"));
    REQUIRE(session);
    CHECK((*session)->options().groupPath == "Sessions");
    CHECK((*session)->options().nameDetail == "Session");
    CHECK_FALSE((*session)->options().trusted);
    CHECK((*session)->appliesTo("Normal"));
    CHECK_FALSE((*session)->appliesTo("Fastest"));

    auto literal = WholeOutingRule::fromDocument(doc(R"("name": "Run")"));
    REQUIRE(literal);
    CHECK((*literal)->options().nameDetail.empty());
    CHECK((*literal)->options().fallbackName == "Run");

    CHECK(WholeOutingRule::fromDocument(doc(R"("produces": "Outings:All")")).error().context.detail ==
          "/produces");
    CHECK(WholeOutingRule::fromDocument(doc(R"("abbrev": "{n}")")).error().context.detail == "/abbrev");
    CHECK(WholeOutingRule::fromDocument(doc(R"("colour": "red")")).error().code == ErrorCode::Invalid);
}

TEST_CASE("RuleRunner::fromRuleIds loads the named rules in order and skips later kinds", "[unit]") {
    // The order of data/profiles/circuit.profile.json.
    const std::vector<std::string> ids = {"circuit.detail-groups", "circuit.whole-outing",
                                          "circuit.beacon-pair", "circuit.track-sections"};
    std::vector<std::string> skipped;
    auto runner = RuleRunner::fromRuleIds(rulesDir(), ids, &skipped);
    REQUIRE(runner);
    REQUIRE(runner->rules().size() == 3);
    CHECK(runner->rules()[0]->id() == "circuit.whole-outing");
    CHECK(runner->rules()[1]->id() == "circuit.beacon-pair");
    CHECK(runner->rules()[2]->id() == "circuit.track-sections"); // phase 2 (M3)
    CHECK(skipped == std::vector<std::string>{"circuit.detail-groups"});

    const std::vector<std::string> missing = {"circuit.no-such-rule"};
    CHECK_FALSE(RuleRunner::fromRuleIds(rulesDir(), missing));

    // A file whose id differs from its name.
    const std::filesystem::path dir =
        std::filesystem::path(ESSES_TEST_BINARY_DIR) / "analysis-rules-mismatch";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "renamed.rule.json", std::ios::binary);
        out << R"({"$schema": "urn:esses:rule-1", "schemaVersion": 1, "id": "original", "kind": "whole-outing"})";
    }
    const std::vector<std::string> renamed = {"renamed"};
    auto mismatch = RuleRunner::fromRuleIds(dir, renamed);
    REQUIRE_FALSE(mismatch);
    CHECK(mismatch.error().code == ErrorCode::Invalid);
    CHECK(mismatch.error().context.detail == "/id");
}
