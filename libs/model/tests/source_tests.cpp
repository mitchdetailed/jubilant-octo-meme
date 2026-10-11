// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DataSource and the merge steps of the open pipeline: file, companion, sidecar (05 section 2 step 5, 02
// section 5.3 rule 2; D-015, D-031, D-130).
#include "model_test_support.hpp"

#include <esses/model/source.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using namespace esses;
using namespace esses::model;

namespace {

// What an importer hands over: details, one Normal layer with two channels and file markers.
DataSource importedSource() {
    DataSource src;
    src.id = test::testSource();
    src.prov.path = std::filesystem::path("logs") / "bathurst_r1.ld";
    src.prov.companions = {std::filesystem::path("logs") / "bathurst_r1.ldx"};
    src.prov.importerId = "motec.ld";
    src.prov.importerVersion = "0.1.0";
    src.prov.size = 48'213'344;
    src.prov.hash = src.id;
    src.details = {Detail{"Driver", std::string("MMINTON"), Provenance::Logged, std::nullopt},
                   Detail{"Venue", std::string("Bathurst"), Provenance::Logged, std::nullopt}};
    src.layers.resize(1);
    DataLayer& l = src.layers[0];
    l.duration = 100'000'000;
    l.addChannel(test::makeChannel("Engine RPM"));
    l.addChannel(test::makeChannel("Speed"));
    l.markers = {makeLoggedMarker(MarkerClass::SOL, 0, "", "", "", 0, 0, Provenance::Derived),
                 makeDeviceEventMarker(3, 0, 0),
                 makeLoggedMarker(MarkerClass::EOL, 100'000'000, "", "", "", 0, 0, Provenance::Derived)};
    return src;
}

CompanionData companion() {
    CompanionData c;
    c.details = {Detail{"Total Laps", std::string("12"), Provenance::Logged, std::nullopt},
                 Detail{"Venue", std::string("Mount Panorama"), Provenance::Logged, std::nullopt}};
    c.markers[""] = {makeLoggedMarker(MarkerClass::BCN, 30'000'000, "Beacons", "Manual.1", "", 77),
                     makeLoggedMarker(MarkerClass::BCN, 60'000'000, "Beacons", "Manual.2", "", 77)};
    c.markers["Burst0"] = {makeLoggedMarker(MarkerClass::BCN, 1, "Beacons", "x")};
    c.opaque = {R"(<Marker Version="100" ClassName="SOR" Name="S" Flags="0" Time="5.00"/>)"};
    return c;
}

} // namespace

TEST_CASE("source: layer lookup accepts the empty alias", "[unit]") {
    DataSource src = importedSource();
    CHECK(src.layer("") == &src.layers[0]);
    CHECK(src.layer("Normal") == &src.layers[0]);
    CHECK(src.layer("Fastest") == nullptr);
    const DataSource& c = src;
    CHECK(c.layer("") == &c.layers[0]);
}

TEST_CASE("source: companion merge sets the base (sidecar > companion > file)", "[unit]") {
    DataSource src = importedSource();
    const auto notes = mergeCompanion(src, companion());
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].code == kNoteCompanionUnknownLayer);
    CHECK(notes[0].layer == "Burst0");

    REQUIRE(src.details.size() == 3);
    CHECK(std::get<std::string>(src.details[1].value) == "Mount Panorama");
    CHECK(src.details[1].prov == Provenance::Companion);
    CHECK(std::get<std::string>(*src.details[1].original) == "Bathurst");
    CHECK(src.details[2].id == "Total Laps");
    CHECK(src.baseDetails == src.details);

    const DataLayer& l = src.layers[0];
    REQUIRE(l.baseMarkers.size() == 5);
    CHECK(l.markers == l.baseMarkers);
    CHECK(l.baseMarkers[0].cls == MarkerClass::SOL);
    CHECK(l.baseMarkers[1].cls == MarkerClass::DeviceEvent);
    CHECK(l.baseMarkers[2].prov == Provenance::Companion);
    CHECK(l.baseMarkers[4].cls == MarkerClass::EOL);
    CHECK(src.companionOpaque.size() == 1);
}

TEST_CASE("source: sidecar application, whole-outing ranges and snapshots", "[unit]") {
    DataSource src = importedSource();
    mergeCompanion(src, companion());
    deriveFileDetails(src);
    CHECK(std::get<std::string>(findDetail(src.details, "File Name")->value) == "bathurst_r1.ld");
    CHECK(findDetail(src.details, "File Size")->prov == Provenance::Derived);
    CHECK(std::get<Quantityd>(findDetail(src.details, "File Size")->value).v == 48'213'344.0);

    SidecarDocument doc = newSidecarFor(src);
    CHECK(doc.sourcePathRel == "bathurst_r1.ld");
    CHECK(doc.sourceHash == src.id);
    CHECK(doc.companions == std::vector<std::string>{"bathurst_r1.ldx"});
    CHECK(doc.importerId == "motec.ld");
    doc.setDetail("Normal", Detail{"Driver", std::string("M. Minton"), Provenance::User, std::nullopt});
    doc.setDetail("Fastest", Detail{"Team", std::string("X"), Provenance::User, std::nullopt});
    const MarkerId beacon2 = src.layers[0].baseMarkers[3].id;
    REQUIRE(doc.applyEdit("Normal", MarkerEdit{beacon2, MarkerEditOp::Ignore, 0, 0.0, "m"}).ok());
    REQUIRE(test::schemaErrors("sidecar-1", *doc.toJson()).empty());

    SidecarApplyReport rep = applySidecar(src, doc);
    CHECK(rep.has(kNoteUnknownLayer)); // "Fastest" is not in this log
    CHECK_FALSE(rep.has(kNoteSourceChanged));
    const Detail* driver = findDetail(src.details, "Driver");
    CHECK(std::get<std::string>(driver->value) == "M. Minton");
    CHECK(driver->prov == Provenance::User);
    CHECK(std::get<std::string>(*driver->original) == "MMINTON");
    CHECK(findDetail(src.details, "File Name") != nullptr); // derived details survive
    CHECK_FALSE(src.layers[0].findMarker(beacon2)->enabled);
    CHECK(src.sidecar == doc);

    rep = rebuildWholeOutingRanges(src);
    const RangeGroup* outings = src.layers[0].ranges.find("Outings");
    REQUIRE(outings != nullptr);
    REQUIRE(outings->ranges.size() == 1);
    CHECK(outings->ranges[0].name == "bathurst_r1.ld");
    CHECK(outings->ranges[0].end == 100'000'000);

    // Publish, then edit: the new snapshot shares the columns and bumps the version.
    const auto published = std::make_shared<const DataSource>(src);
    SidecarDocument edited = published->sidecar;
    REQUIRE(edited.applyEdit("Normal", MarkerEdit{beacon2, MarkerEditOp::Restore, 0, 0.0, "m"}).ok());
    edited.setRangeOverride("Normal", RangeOverride{"Outings", src.layers[0].baseMarkers[0].id,
                                                    "bathurst_r1.ld", false, std::nullopt, std::nullopt});
    SidecarApplyReport report;
    const auto next = published->withSidecar(edited, &report);
    CHECK(next->version == published->version + 1);
    CHECK(next->layers[0].findMarker(beacon2)->enabled);
    CHECK(published->layers[0].findMarker(beacon2)->enabled == false); // the old snapshot is untouched
    CHECK_FALSE(next->layers[0].ranges.find("Outings")->ranges[0].trusted);
    CHECK(next->layers[0].ranges.rangeVersion("Outings") ==
          published->layers[0].ranges.rangeVersion("Outings") + 1);
    CHECK(next->layers[0].channels[0].series.index() == published->layers[0].channels[0].series.index());
    CHECK(store::seriesValues(next->layers[0].channels[0].series).id() ==
          store::seriesValues(published->layers[0].channels[0].series).id()); // shared column
    CHECK(std::get<std::string>(findDetail(next->details, "Driver")->value) == "M. Minton");

    // A custom rule runner replaces the whole-outing rebuild.
    bool called = false;
    const auto custom = published->withSidecar(edited, nullptr, [&called](DataSource&) {
        called = true;
        return SidecarApplyReport{};
    });
    CHECK(called);
    CHECK(custom->layers[0].ranges.find("Outings")->ranges[0].trusted); // overrides not applied by the runner
}

TEST_CASE("source: a sidecar written for another file still applies, with a warning", "[unit]") {
    DataSource src = importedSource();
    mergeCompanion(src, {});
    SidecarDocument doc = newSidecarFor(src);
    doc.sourceHash = SourceId{1, 1};
    doc.setDetail("Normal", Detail{"Session", std::string("Q"), Provenance::User, std::nullopt});
    const SidecarApplyReport rep = applySidecar(src, doc);
    CHECK(rep.has(kNoteSourceChanged));
    CHECK(std::get<std::string>(findDetail(src.details, "Session")->value) == "Q");
    CHECK_FALSE(findDetail(src.details, "Session")->original.has_value());
}

TEST_CASE("source: applying a sidecar never adds layers to the document", "[unit]") {
    DataSource src = importedSource();
    mergeCompanion(src, {});
    SidecarDocument doc = newSidecarFor(src);
    doc.setDetail("Fastest", Detail{"Session", std::string("Q"), Provenance::User, std::nullopt});
    REQUIRE_FALSE(doc.hasLayer("Normal"));
    const SidecarApplyReport rep = applySidecar(src, doc);
    CHECK(rep.has(kNoteUnknownLayer));
    CHECK(src.sidecar == doc);
    CHECK_FALSE(src.sidecar.hasLayer("Normal"));
    CHECK(findDetail(src.details, "Session") == nullptr);
}
