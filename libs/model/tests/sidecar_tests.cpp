// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SidecarDocument (02 section 5; D-015, D-065, D-069, D-130): sidecar-1 reading and canonical writing
// (byte-identical round trip, schema-valid output), the schema examples, atomic save and delete-when-empty,
// edit commands, application to a layer, source checks.
#include "model_test_support.hpp"

#include <esses/model/sidecar.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <string>

using namespace esses;
using namespace esses::model;
namespace fs = std::filesystem;

namespace {

SidecarDocument parseOk(const std::string& text) {
    auto r = SidecarDocument::parse(text);
    INFO((r.ok() ? std::string() : r.error().toString()));
    REQUIRE(r.ok());
    return std::move(r).value();
}

std::string jsonOk(const SidecarDocument& doc) {
    auto r = doc.toJson();
    INFO((r.ok() ? std::string() : r.error().toString()));
    REQUIRE(r.ok());
    return std::move(r).value();
}

// A document using every member, built in code.
SidecarDocument richDocument() {
    SidecarDocument doc;
    doc.sourcePathRel = "bathurst_r1.ld";
    doc.sourceSize = 48'213'344;
    doc.sourceHash = SourceId{0x0d4e5f60718293a4ull, 0x9f3c1b2a0d4e5f60ull};
    doc.sourceContentHash = Hash128{1, 2};
    doc.importerId = "motec.ld";
    doc.importerVersion = "0.1.0";
    doc.companions = {"bathurst_r1.ldx"};
    doc.edited = *DateTime::parseIso("2026-10-09T21:14:00+10:00");

    Detail driver{"Driver", std::string("M. Minton"), Provenance::User, DetailValue{std::string("MMINTON")}};
    doc.setDetail("Normal", driver);
    Quantityd ratio;
    ratio.v = 3.9;
    ratio.symbol = "ratio";
    ratio.dps = 2;
    doc.setDetail("", Detail{"Diff Ratio", ratio, Provenance::Logged, std::nullopt});
    doc.setDetail("Normal", Detail{"Log Date", *DateTime::parseIso("2026-10-09T14:32:11+10:00"),
                                   Provenance::User, std::nullopt});

    Marker user;
    user.id = MarkerId{Uuid::parse("3f6a2c1e-8d4b-4f0a-9c1d-2b7e5a9f0c11")};
    user.cls = MarkerClass::BCN;
    user.group = "Beacons";
    user.t = 412'356'789;
    user.name = "Manual.1";
    user.comment = "missed beacon T1";
    doc.addUserMarker("Normal", user);

    const MarkerId b3 = markerIdLogged(MarkerClass::BCN, 83'412'340, "Beacons", 0);
    const MarkerId b7 = markerIdLogged(MarkerClass::BCN, 500'000'000, "Beacons", 0);
    REQUIRE(doc.applyEdit("Normal", MarkerEdit{b3, MarkerEditOp::Offset, -120'000, 0.0, "m"}).ok());
    REQUIRE(doc.applyEdit("Normal", MarkerEdit{b7, MarkerEditOp::Ignore, 0, 0.0, "m"}).ok());
    REQUIRE(doc.applyEdit("Normal", MarkerEdit{MarkerId{}, MarkerEditOp::OffsetDistance, 0, 12.5, "m"}).ok());

    doc.setRangeOverride("Normal",
                         RangeOverride{"Outings:Laps", b3, "Lap 3", false, std::nullopt, std::nullopt});
    doc.setRangeOverride("Normal", RangeOverride{"Outings:Laps", b7, "Lap 7", std::nullopt, 350'000, 0.0});
    doc.setRangeOverride("Fastest", RangeOverride{"Outings:Laps:Track Sections:Default", std::nullopt,
                                                  "Turn 3", std::nullopt, std::nullopt, -1.5});

    SidecarLayer& n = doc.layer("Normal");
    n.localMaths = *OpaqueJson::parse(R"({"$schema": "urn:esses:maths-1", "schemaVersion": 1, "id": "local",
        "items": [{"kind": "scale_offset", "name": "Brake Pres F", "scale": 1.02}]})");
    n.annotations.push_back(Annotation{Uuid::parse("0d9e8f7a-6b5c-4d3e-8f1a-0b9c8d7e6f5a"), 456'000'000,
                                       "Wheel lock T4", std::string("Brake Pres F")});
    n.annotations.push_back(
        Annotation{Uuid::parse("c1a2b3c4-d5e6-4f70-8a9b-0c1d2e3f4a5b"), -5'000, "early", {}});
    n.videoLinks.push_back(VideoLink{"cam1.mp4", "Driver", -1'250'000, {{0, 1'250'000}, {10, 20}}});
    n.runs = *OpaqueJson::parse("[]");
    n.stages = *OpaqueJson::parse("[]");
    doc.unknown.emplace("x-esses-legacy", *OpaqueJson::parse(R"({"oldField": 1})"));
    return doc;
}

} // namespace

TEST_CASE("sidecar: path next to the log", "[unit]") {
    CHECK(SidecarDocument::pathFor(fs::path("logs") / "bathurst_r1.ld") ==
          fs::path("logs") / "bathurst_r1.ld.sidecar.json");
    CHECK(SidecarDocument::pathFor("session.csv").filename() == "session.csv.sidecar.json");
}

TEST_CASE("sidecar: a missing file is an empty document", "[unit]") {
    const auto dir = test::scratchDir("sidecar-missing");
    auto r = SidecarDocument::load(dir / "nope.ld.sidecar.json");
    REQUIRE(r.ok());
    CHECK(r->empty());
    CHECK(r->layers().empty());
    CHECK(r->layer("Normal").empty());
}

TEST_CASE("sidecar: the schema's valid examples load and round-trip canonically", "[unit][schema]") {
    const auto files = test::exampleFiles("sidecar-1", "valid");
    REQUIRE(files.size() >= 3);
    for (const auto& path : files) {
        INFO(path.filename().string());
        const SidecarDocument doc = parseOk(test::readFile(path));
        const std::string canonical = jsonOk(doc);
        CHECK(test::schemaErrors("sidecar-1", canonical).empty());
        const SidecarDocument again = parseOk(canonical);
        CHECK(again == doc);
        CHECK(jsonOk(again) == canonical); // byte-identical
    }
}

TEST_CASE("sidecar: full example content", "[unit]") {
    const SidecarDocument doc =
        parseOk(test::readFile(test::schemasDir() / "examples" / "sidecar-1" / "valid" / "full.json"));
    CHECK(doc.sourcePathRel == "bathurst_r1.ld");
    CHECK(doc.sourceSize == 48'213'344);
    CHECK(hashText(doc.sourceHash) == "xxh3:9f3c1b2a0d4e5f60718293a4b5c6d7e8");
    CHECK(hashText(*doc.sourceContentHash) == "xxh3:41d07a9e3c2b1f0e5d6c7b8a99a8b7c6");
    CHECK(doc.importerId == "motec.ld");
    CHECK(doc.companions == std::vector<std::string>{"bathurst_r1.ldx"});
    CHECK(doc.edited.tzOffsetMin == 600);
    const SidecarLayer& n = doc.layer("");
    REQUIRE(n.details.size() == 4);
    CHECK(n.details[0].prov == Provenance::User);
    CHECK(std::get<std::string>(*n.details[0].original) == "ADRIVER");
    const auto& ratio = std::get<Quantityd>(n.details[1].value);
    CHECK(ratio.v == 3.9);
    CHECK(ratio.symbol == "ratio");
    CHECK(ratio.unit == units::kNoUnit); // resolved later, with a registry
    CHECK(std::get<Quantityd>(n.details[3].value).symbol.empty());
    REQUIRE(n.markers.size() == 1);
    CHECK(n.markers[0].prov == Provenance::User);
    REQUIRE(n.markerEdits.size() == 3);
    CHECK(n.markerEdits[0].op == MarkerEditOp::Offset);
    CHECK(n.markerEdits[0].dtUs == -120'000);
    CHECK(n.markerEdits[1].op == MarkerEditOp::Ignore);
    CHECK(n.markerEdits[2].op == MarkerEditOp::OffsetDistance);
    CHECK(n.markerEdits[2].dM == 12.5);
    CHECK(n.markerEdits[2].dMUnit == "m");
    REQUIRE(n.rangeOverrides.size() == 2);
    CHECK(n.rangeOverrides[0].trusted == false);
    CHECK(n.rangeOverrides[1].offsetUs == 350'000);
    CHECK(n.rangeOverrides[1].offsetM == 0.0);
    REQUIRE(n.localMaths.has_value());
    CHECK(n.localMaths->compact().find("\"id\":\"local\"") != std::string::npos);
    REQUIRE(n.videoLinks.size() == 1);
    CHECK(n.videoLinks[0].syncPoints == std::vector<std::pair<Micros, Micros>>{{0, 1'250'000}});
    CHECK(n.runs->compact() == "[]");
    CHECK(n.stages->compact() == "[]");
}

TEST_CASE("sidecar: unknown top-level members are kept and re-emitted", "[unit]") {
    const SidecarDocument doc = parseOk(test::readFile(test::schemasDir() / "examples" / "sidecar-1" /
                                                       "valid" / "unknown-top-level-and-datetime.json"));
    REQUIRE(doc.unknown.size() == 2);
    CHECK(doc.unknown.at("futureMember").compact() ==
          R"({"kept":true,"written by":"a newer minor version"})");
    CHECK(doc.unknown.at("x-esses-legacy").compact() == R"({"oldField":1})");
    CHECK(doc.edited.iso() == "2026-10-10T08:00:00.250-03:30");
    CHECK(std::get<std::string>(doc.layer("Normal").details[1].value) == "Short form text");
    const std::string text = jsonOk(doc);
    CHECK(text.find("\"futureMember\": {") != std::string::npos);
    CHECK(text.find("\"x-esses-legacy\": {") != std::string::npos);
    CHECK_FALSE(doc.empty());
}

TEST_CASE("sidecar: a document built in code is canonical, schema-valid and round-trips byte-identically",
          "[unit][schema]") {
    const SidecarDocument doc = richDocument();
    const std::string text = jsonOk(doc);
    INFO(text);
    CHECK(test::schemaErrors("sidecar-1", text).empty());
    CHECK(
        text.starts_with("{\n  \"$schema\": \"urn:esses:sidecar-1\",\n  \"schemaVersion\": 1,\n  \"edited\": "
                         "\"2026-10-09T21:14:00+10:00\",\n  \"layers\": {\n    \"Fastest\": {\n"));
    CHECK(text.ends_with("}\n"));
    CHECK(text.find("\r") == std::string::npos);
    CHECK(text.find("\"syncPoints\": [[0, 1250000], [10, 20]]") != std::string::npos);
    CHECK(text.find("\"companions\": [\"bathurst_r1.ldx\"]") != std::string::npos);
    CHECK(text.find("\"offsetM\": 0") != std::string::npos);
    CHECK(text.find("\"dM\": {\n") != std::string::npos);
    CHECK(text.find("\"restore\"") == std::string::npos);
    const SidecarDocument back = parseOk(text);
    CHECK(back == doc);
    CHECK(jsonOk(back) == text);

    // Pinned bytes: the canonical form of this document (02 section 6.5 rule 1).
    const auto golden = test::repoRoot() / "libs" / "model" / "tests" / "data" / "rich.sidecar.json";
    const std::string expected = fs::exists(golden) ? test::readFile(golden) : std::string();
    if (text != expected) { // leave the actual bytes next to the test binary for review
        test::writeFile(pathFromUtf8(ESSES_TEST_BINARY_DIR) / "rich.sidecar.actual.json", text);
    }
    CHECK(text == expected);
}

TEST_CASE("sidecar: invalid schema examples are rejected where the loader is strict", "[unit][schema]") {
    const auto expect =
        nlohmann::json::parse(test::readFile(test::schemasDir() / "examples" / "sidecar-1" / "expect.json"));
    // Lexical forms the loader accepts and normalises on save (02 section 6.6 rule 1 spirit): an explicit
    // null for an absent optional, an override without effect (dropped, never written) and upper-case UUIDs.
    const std::map<std::string, std::string> lenient{
        {"null-optional.json", ""}, {"range-override-without-fields.json", ""}, {"uppercase-uuid.json", ""}};
    for (const auto& path : test::exampleFiles("sidecar-1", "invalid")) {
        const std::string name = path.filename().string();
        INFO(name);
        const auto r = SidecarDocument::parse(test::readFile(path));
        if (lenient.count(name) != 0) {
            REQUIRE(r.ok());
            const std::string text = jsonOk(*r);
            CHECK(test::schemaErrors("sidecar-1", text).empty()); // normalised on save
            continue;
        }
        REQUIRE_FALSE(r.ok());
        INFO(r.error().toString());
        CHECK(r.error().code == ErrorCode::Invalid);
        REQUIRE(r.error().context.detail.has_value());
        const std::string pointer = expect.at(name).at("pointer").get<std::string>();
        CHECK(r.error().context.detail->starts_with(pointer));
    }
}

TEST_CASE("sidecar: a document nested deeper than the limit is refused, not a stack overflow", "[unit]") {
    // An unknown top-level member nested 100,000 levels deep (a 200 KB sidecar shared with a log) used to
    // overflow the stack of the canonical writer (opaqueOrViolate -> toOpaque -> write).
    const std::string body = R"(, "schemaVersion": 1, "source": {"path": "a.ld", "size": 1,
        "hash": "xxh3:0123456789abcdef0123456789abcdef", "importer": {"id": "motec.ld", "version": "0.1.0"}},
        "edited": "2026-10-10T08:00:00Z", "layers": {})";
    const auto withUnknown = [&](std::size_t depth) {
        return std::string(R"({"$schema": "urn:esses:sidecar-1")") + body + R"(, "x-future": )" +
               std::string(depth, '[') + std::string(depth, ']') + "}";
    };
    auto deep = SidecarDocument::parse(withUnknown(100'000));
    REQUIRE_FALSE(deep.ok());
    CHECK(deep.error().code == ErrorCode::Format);
    CHECK(deep.error().message.find("nested") != std::string::npos);
    // Ten levels are an ordinary unknown member, kept as it was.
    const SidecarDocument shallow = parseOk(withUnknown(10));
    CHECK(jsonOk(shallow).find("\"x-future\"") != std::string::npos);
    // OpaqueJson::parse shares the limit.
    CHECK(OpaqueJson::parse(std::string(300, '[') + std::string(300, ']')).error().code == ErrorCode::Format);
}

TEST_CASE("sidecar: schema ids, versions and syntax errors", "[unit]") {
    const std::string body = R"(, "schemaVersion": 1, "source": {"path": "a.ld", "size": 1,
        "hash": "xxh3:0123456789abcdef0123456789abcdef", "importer": {"id": "motec.ld", "version": "0.1.0"}},
        "edited": "2026-10-10T08:00:00Z", "layers": {}})";
    for (const char* id : {"esses:sidecar-1", "https://esses.example/schemas/sidecar-1.schema.json",
                           "https://esses.example/sidecar-1.json"}) {
        INFO(id);
        const SidecarDocument doc = parseOk(std::string(R"({"$schema": ")") + id + "\"" + body);
        CHECK(jsonOk(doc).find("\"$schema\": \"urn:esses:sidecar-1\"") != std::string::npos);
    }
    auto r = SidecarDocument::parse(std::string(R"({"$schema": "urn:esses:sidecar-2")") + body);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Unsupported);
    CHECK(r.error().context.schemaId == "urn:esses:sidecar-2");

    const std::string newerMinor = std::string(R"({"$schema": "urn:esses:sidecar-1", "schemaVersion": 3)") +
                                   body.substr(std::string(R"(, "schemaVersion": 1)").size());
    const SidecarDocument doc = parseOk(newerMinor);
    CHECK(doc.schemaVersion == 3);
    CHECK(jsonOk(doc).find("\"schemaVersion\": 3") != std::string::npos);

    r = SidecarDocument::parse(R"({"$schema": "urn:esses:sidecar-1", "schemaVersion": )");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Format);
    CHECK(r.error().context.offset.has_value());

    r = SidecarDocument::parse("[]");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Invalid);

    // Empty layers are not kept: the document equals its canonical re-read.
    const std::string withEmpty = std::string(R"({"$schema": "urn:esses:sidecar-1")") +
                                  body.substr(0, body.size() - std::string(R"("layers": {}})").size()) +
                                  R"("layers": {"Fastest": {}, "Normal": {"details": []}}})";
    const SidecarDocument emptyLayers = parseOk(withEmpty);
    CHECK(emptyLayers.layers().empty());
    CHECK(emptyLayers.empty());
}

TEST_CASE("sidecar: duplicate edits are rejected on load (02 section 4.3 behaviour 7)", "[unit]") {
    const std::string head =
        R"({"$schema": "urn:esses:sidecar-1", "schemaVersion": 1, "source": {"path": "a.ld",
        "size": 1, "hash": "xxh3:0123456789abcdef0123456789abcdef", "importer": {"id": "x", "version": "0.1.0"}},
        "edited": "2026-10-10T08:00:00Z", "layers": {"Normal": {"markerEdits": )";
    const std::string ref = R"("7c0e9a2b-1d3f-5a6c-8e9f-0a1b2c3d4e5f")";
    auto r = SidecarDocument::parse(head + "[{\"ref\": " + ref +
                                    ", \"op\": \"offset\", \"dtUs\": 1}, {\"ref\": " + ref +
                                    ", \"op\": \"offset\", \"dtUs\": 2}]}}}");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().context.detail == "/layers/Normal/markerEdits/1");
    r = SidecarDocument::parse(head + R"([{"op": "offsetDistance", "dM": {"v": 1, "unit": "m"}},
                                          {"op": "offsetDistance", "dM": {"v": 2, "unit": "m"}}]}}})");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().context.detail == "/layers/Normal/markerEdits/1");
    // One offset and one ignore for the same marker are fine.
    r = SidecarDocument::parse(head + "[{\"ref\": " + ref + ", \"op\": \"offset\", \"dtUs\": 1}, {\"ref\": " +
                               ref + ", \"op\": \"ignore\"}]}}}");
    CHECK(r.ok());
}

TEST_CASE("sidecar: save is atomic and an empty document deletes the file (D-069)", "[unit]") {
    const auto dir = test::scratchDir("sidecar-save");
    const auto path = SidecarDocument::pathFor(dir / "run.ld");
    const SidecarDocument doc = richDocument();
    REQUIRE(doc.save(path).ok());
    REQUIRE(fs::exists(path));
    const std::string written = test::readFile(path);
    CHECK(written == jsonOk(doc));
    auto loaded = SidecarDocument::load(path);
    REQUIRE(loaded.ok());
    CHECK(*loaded == doc);
    // Saving the loaded document reproduces the file byte for byte.
    REQUIRE(loaded->save(path).ok());
    CHECK(test::readFile(path) == written);
    for (const auto& e : fs::directory_iterator(dir)) { // no temporary file left behind
        CHECK(e.path().filename() == path.filename());
    }

    SidecarDocument emptied = *loaded;
    for (const auto& [name, layer] : loaded->layers()) {
        emptied.layer(name) = SidecarLayer{};
    }
    emptied.unknown.clear();
    CHECK(emptied.empty());
    REQUIRE(emptied.save(path).ok());
    CHECK_FALSE(fs::exists(path));
    CHECK(emptied.save(path).ok()); // deleting a file that is not there is fine

    test::writeFile(path, "{ not json");
    const auto bad = SidecarDocument::load(path);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == ErrorCode::Format);
    CHECK(bad.error().context.path == path);
}

TEST_CASE("sidecar: non-finite numbers cannot be written", "[unit]") {
    SidecarDocument doc = richDocument();
    Quantityd q;
    q.v = std::numeric_limits<double>::quiet_NaN();
    doc.setDetail("Normal", Detail{"Bad", q, Provenance::User, std::nullopt});
    const auto r = doc.toJson();
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Invalid);
    CHECK_FALSE(doc.save(test::scratchDir("sidecar-nan") / "x.json").ok());
    CHECK_FALSE(doc.documentHash().ok());
}

TEST_CASE("sidecar: document hash", "[unit]") {
    SidecarDocument doc = richDocument();
    const auto h = doc.documentHash();
    REQUIRE(h.ok());
    CHECK(*h == hash128(std::string_view(jsonOk(doc))));
    CHECK(*doc.documentHash() == *h);
    doc.layer("Normal").annotations.clear();
    CHECK(*doc.documentHash() != *h);
}

TEST_CASE("sidecar: marker edit commands (D-065)", "[unit]") {
    SidecarDocument doc;
    const MarkerId m1 = markerIdLogged(MarkerClass::BCN, 1'000, "Beacons", 0);
    const MarkerId m2 = markerIdLogged(MarkerClass::BCN, 2'000, "Beacons", 0);
    auto& edits = doc.layer("Normal").markerEdits;
    REQUIRE(doc.applyEdit("", MarkerEdit{m1, MarkerEditOp::Offset, 100, 0.0, "m"}).ok());
    REQUIRE(doc.applyEdit("Normal", MarkerEdit{m1, MarkerEditOp::Offset, 250, 0.0, "m"}).ok()); // replaces
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].dtUs == 250);
    REQUIRE(doc.applyEdit("Normal", MarkerEdit{m1, MarkerEditOp::Ignore, 0, 0.0, "m"}).ok());
    REQUIRE(doc.applyEdit("Normal", MarkerEdit{m1, MarkerEditOp::Ignore, 0, 0.0, "m"}).ok()); // idempotent
    REQUIRE(doc.applyEdit("Normal", MarkerEdit{m2, MarkerEditOp::Ignore, 0, 0.0, "m"}).ok());
    CHECK(edits.size() == 3);
    REQUIRE(
        doc.applyEdit("Normal", MarkerEdit{m1, MarkerEditOp::Restore, 0, 0.0, "m"}).ok()); // "restore logged"
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].ref == m2);
    REQUIRE(doc.applyEdit("Normal", MarkerEdit{m2, MarkerEditOp::Offset, 5, 0.0, "m"}).ok());
    REQUIRE(doc.applyEdit("Normal", MarkerEdit{m2, MarkerEditOp::Offset, 0, 0.0, "m"}).ok()); // zero removes
    CHECK(edits.size() == 1);
    REQUIRE(doc.applyEdit("Normal", MarkerEdit{MarkerId{}, MarkerEditOp::OffsetDistance, 0, 3.0, "m"}).ok());
    REQUIRE(doc.applyEdit("Normal", MarkerEdit{MarkerId{}, MarkerEditOp::OffsetDistance, 0, 4.0, "m"}).ok());
    REQUIRE(edits.size() == 2);
    CHECK(edits[1].dM == 4.0);
    REQUIRE(doc.applyEdit("Normal", MarkerEdit{MarkerId{}, MarkerEditOp::OffsetDistance, 0, 0.0, "m"}).ok());
    CHECK(edits.size() == 1);

    CHECK(doc.applyEdit("Normal", MarkerEdit{MarkerId{}, MarkerEditOp::Offset, 1, 0.0, "m"}).error().code ==
          ErrorCode::Invalid);
    CHECK_FALSE(doc.applyEdit("Normal", MarkerEdit{MarkerId{}, MarkerEditOp::OffsetDistance, 0,
                                                   std::numeric_limits<double>::infinity(), "m"})
                    .ok());
}

TEST_CASE("sidecar: overrides, details and user markers", "[unit]") {
    SidecarDocument doc;
    const MarkerId start = markerIdLogged(MarkerClass::BCN, 1'000, "Beacons", 0);
    doc.setRangeOverride(
        "Normal", RangeOverride{"Outings:Laps", std::nullopt, "Lap 3", true, std::nullopt, std::nullopt});
    doc.setRangeOverride("Normal",
                         RangeOverride{"Outings:Laps", start, "Lap 3", false, std::nullopt, std::nullopt});
    auto& list = doc.layer("Normal").rangeOverrides;
    REQUIRE(list.size() == 1); // the name entry became the id entry
    CHECK(list[0].startMarker == start);
    CHECK(list[0].trusted == false);
    doc.setRangeOverride("Normal",
                         RangeOverride{"Outings:Laps", start, "Lap 3", std::nullopt, 10, std::nullopt});
    REQUIRE(list.size() == 1);
    CHECK_FALSE(list[0].trusted.has_value());
    CHECK(list[0].offsetUs == 10);
    doc.setRangeOverride("Normal", RangeOverride{"Outings:Laps", start, "Lap 3", std::nullopt, std::nullopt,
                                                 std::nullopt}); // pressing O again: no effect left
    CHECK(list.empty());

    doc.setDetail("Normal", Detail{"Driver", std::string("A"), Provenance::Logged, std::nullopt});
    doc.setDetail("Normal", Detail{"driver", std::string("B"), Provenance::Logged, std::nullopt});
    REQUIRE(doc.layer("Normal").details.size() == 1);
    CHECK(doc.layer("Normal").details[0].prov == Provenance::User);
    CHECK(std::get<std::string>(doc.layer("Normal").details[0].value) == "B");
    CHECK(doc.revertDetail("Normal", "DRIVER"));
    CHECK_FALSE(doc.revertDetail("Normal", "Driver"));
    CHECK_FALSE(doc.revertDetail("Burst0", "Driver"));
    CHECK_FALSE(doc.hasLayer("Burst0"));

    Marker m;
    m.cls = MarkerClass::BCN;
    m.group = "Beacons";
    m.t = 7;
    m.prov = Provenance::Logged;
    const MarkerId id = doc.addUserMarker("Normal", m);
    CHECK(id.v.version() == 4);
    CHECK(doc.layer("Normal").markers[0].prov == Provenance::User);
    CHECK(doc.removeUserMarker("Normal", id));
    CHECK_FALSE(doc.removeUserMarker("Normal", id));

    CHECK(doc.hasLayer(""));
    CHECK(doc.empty());
    doc.pruneEmptyLayers();
    CHECK(doc.layers().empty());
}

TEST_CASE("sidecar: apply rebuilds markers from the base and stamps overrides", "[unit]") {
    DataLayer layer;
    layer.duration = 10'000;
    const Marker sol = makeLoggedMarker(MarkerClass::SOL, 0, "", "", "", 0, 0, Provenance::Derived);
    const Marker b1 = makeLoggedMarker(MarkerClass::BCN, 1'000, "Beacons", "B1");
    const Marker b2 = makeLoggedMarker(MarkerClass::BCN, 2'000, "Beacons", "B2");
    const Marker eol = makeLoggedMarker(MarkerClass::EOL, 10'000, "", "", "", 0, 0, Provenance::Derived);
    layer.baseMarkers = {sol, b1, b2, eol};
    RangeGroup laps = RangeGroup::make("Outings:Laps");
    for (const auto& [n, s, e] : {std::tuple{1, b1, b2}, std::tuple{2, b2, eol}}) {
        Range r;
        r.kind = RangeKind::Lap;
        r.startMarker = s.id;
        r.endMarker = e.id;
        r.id = rangeId(laps.path, s.id);
        r.start = s.t;
        r.end = e.t;
        r.number = n;
        r.name = "Lap " + std::to_string(n);
        laps.ranges.push_back(r);
    }
    layer.ranges.roots.push_back(RangeGroup::make("Outings"));
    REQUIRE(layer.ranges.setGroup(laps));

    SidecarDocument doc;
    const MarkerId ghost = markerIdLogged(MarkerClass::BCN, 3'000, "Beacons", 0);
    REQUIRE(
        doc.applyEdit("Normal", MarkerEdit{b1.id, MarkerEditOp::Offset, 2'500, 0.0, "m"}).ok()); // past b2
    REQUIRE(doc.applyEdit("Normal", MarkerEdit{ghost, MarkerEditOp::Ignore, 0, 0.0, "m"}).ok());
    Marker user;
    user.cls = MarkerClass::BCN;
    user.group = "Beacons";
    user.t = 500;
    const MarkerId userId = doc.addUserMarker("Normal", user);
    doc.setRangeOverride("Normal",
                         RangeOverride{"Outings:Laps", b1.id, "Lap 1", false, std::nullopt, std::nullopt});
    doc.setRangeOverride("Normal",
                         RangeOverride{"Outings:Laps", std::nullopt, "Lap 2", std::nullopt, 42, 1.5});
    doc.setRangeOverride(
        "Normal", RangeOverride{"Outings:Laps", std::nullopt, "Lap 9", true, std::nullopt, std::nullopt});
    doc.setRangeOverride(
        "Normal", RangeOverride{"Outings:Missing", std::nullopt, "X", true, std::nullopt, std::nullopt});

    const SidecarApplyReport report = doc.apply(layer);
    REQUIRE(layer.markers.size() == 5);
    CHECK(layer.markers[0].id == sol.id);
    CHECK(layer.markers[1].id == userId);
    CHECK(layer.markers[1].prov == Provenance::User);
    CHECK(layer.markers[2].id == b2.id);
    CHECK(layer.markers[3].id == b1.id); // moved after b2, model order
    CHECK(layer.markers[3].t == 3'500);
    CHECK(layer.markers[3].originalT == 1'000);
    CHECK(layer.markers[4].id == eol.id);
    CHECK(layer.baseMarkers.size() == 4); // the base is never touched

    const RangeGroup* g = layer.ranges.find("Outings:Laps");
    CHECK_FALSE(g->ranges[0].trusted);
    CHECK(g->ranges[1].offsetUs == 42);
    CHECK(g->ranges[1].offsetM == 1.5);
    CHECK(countNotes(report.notes, kNoteOrphanMarkerEdit) == 1);
    CHECK(countNotes(report.notes, kNoteOrphanRangeOverride) == 2);
    CHECK(countNotes(report.notes, kNoteRangeOverrideByName) == 1);
    REQUIRE(report.heals.size() == 1);
    CHECK(report.heals[0].startMarker == b2.id);

    // The by-name entry heals itself on the next save (02 section 5.3 rule 5).
    doc.applyHeals(report);
    CHECK(doc.layer("Normal").rangeOverrides[1].startMarker == b2.id);
    CHECK(doc.layer("Normal").rangeOverrides.size() == 4); // orphans stay

    // Re-applying is idempotent.
    const auto markersOnce = layer.markers;
    doc.applyMarkers(layer);
    CHECK(layer.markers == markersOnce);

    // A layer whose base was never captured takes it from its markers (edits undone, user markers dropped).
    DataLayer bare;
    bare.markers = layer.markers;
    doc.applyMarkers(bare);
    CHECK(bare.baseMarkers == layer.baseMarkers);
    CHECK(bare.markers == layer.markers);
}

TEST_CASE("sidecar: source checks (02 section 5.3 rule 1, D-130)", "[unit]") {
    SidecarDocument doc = richDocument();
    CHECK(doc.checkSource(doc.sourceHash, doc.sourceContentHash).empty());
    CHECK(doc.checkSource(doc.sourceHash, std::nullopt).empty());
    auto notes = doc.checkSource(SourceId{7, 7}, std::nullopt);
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].code == kNoteSourceChanged);
    CHECK(notes[0].level == ModelNote::Level::Warning);
    notes = doc.checkSource(doc.sourceHash, Hash128{9, 9});
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].code == kNoteSourceChanged);
    CHECK(SidecarDocument{}.checkSource(SourceId{7, 7}, std::nullopt).empty()); // nothing to apply
}

TEST_CASE("sidecar: hash text", "[unit]") {
    const Hash128 h{0x0011223344556677ull, 0x8899aabbccddeeffull};
    CHECK(hashText(h) == "xxh3:8899aabbccddeeff0011223344556677");
    CHECK(parseHashText(hashText(h)) == h);
    CHECK(parseHashText("xxh3:8899AABBCCDDEEFF0011223344556677") == h);
    CHECK_FALSE(parseHashText("8899aabbccddeeff0011223344556677").has_value());
    CHECK_FALSE(parseHashText("xxh3:8899aabbccddeeff001122334455667").has_value());
    CHECK_FALSE(parseHashText("xxh3:8899aabbccddeeff00112233445566zz").has_value());
}
