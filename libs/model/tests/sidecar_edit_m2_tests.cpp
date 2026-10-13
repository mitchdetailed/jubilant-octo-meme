// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The M2 sidecar edit commands (02 section 7, D-032; 06 section 13.3 item 6): annotation
// insert/replace/remove, local maths replace/remove and "restore logged laps", each leaving a document that
// saves canonically.

#include <esses/base/json_doc.hpp>
#include <esses/model/sidecar.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace esses;
using namespace esses::model;

TEST_CASE("sidecar M2 edits: annotations by id", "[unit]") {
    SidecarDocument doc;
    Annotation a;
    a.tUs = 456'000'000;
    a.text = "Wheel lock T4";
    const Uuid id = doc.setAnnotation("Normal", a);
    CHECK_FALSE(id.isNil());
    REQUIRE(doc.layer("Normal").annotations.size() == 1);

    Annotation edited = doc.layer("Normal").annotations.front();
    edited.text = "Lock-up T4";
    CHECK(doc.setAnnotation("", edited) == id); // "" is Normal; same id replaces
    REQUIRE(doc.layer("Normal").annotations.size() == 1);
    CHECK(doc.layer("Normal").annotations.front().text == "Lock-up T4");

    CHECK(doc.removeAnnotation("Normal", id));
    CHECK_FALSE(doc.removeAnnotation("Normal", id));
    CHECK_FALSE(doc.removeAnnotation("Other", id));
    CHECK(doc.layer("Normal").annotations.empty());
    CHECK(doc.toJson().ok());
}

TEST_CASE("sidecar M2 edits: local maths and restore logged", "[unit]") {
    SidecarDocument doc;
    doc.setLocalMaths("Other", std::nullopt); // removing from an absent layer creates nothing
    CHECK_FALSE(doc.hasLayer("Other"));

    const auto module = JsonDoc::parse(R"({"$schema":"urn:esses:maths-1","schemaVersion":1,"id":"local",)"
                                       R"("enabled":true,"precision":"single","constants":[],)"
                                       R"("enumerations":[],"items":[]})");
    REQUIRE(module.ok());
    doc.setLocalMaths("Normal", *module);
    REQUIRE(doc.layer("Normal").localMaths.has_value());
    CHECK(doc.layer("Normal").localMaths->schemaId() == "urn:esses:maths-1");
    doc.setLocalMaths("Normal", std::nullopt);
    CHECK_FALSE(doc.layer("Normal").localMaths.has_value());

    CHECK_FALSE(doc.restoreLogged("Normal")); // nothing to remove
    Marker m;
    m.cls = MarkerClass::BCN;
    m.group = "Beacons";
    m.t = 1'000'000;
    const MarkerId added = doc.addUserMarker("Normal", m);
    MarkerEdit ignore;
    ignore.ref = added;
    ignore.op = MarkerEditOp::Ignore;
    REQUIRE(doc.applyEdit("Normal", ignore).ok());
    CHECK(doc.restoreLogged("Normal"));
    CHECK(doc.layer("Normal").markers.empty());
    CHECK(doc.layer("Normal").markerEdits.empty());
    CHECK_FALSE(doc.restoreLogged("Absent"));
}
