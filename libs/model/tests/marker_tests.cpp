// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Markers, ordinals, device events, the companion union and marker edits (02 section 4.3, section 5.3 rule 2;
// D-014, D-064, D-065, D-132).
#include "model_test_support.hpp"

#include <esses/model/marker.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace esses;
using namespace esses::model;

TEST_CASE("marker: class and edit-op names round-trip", "[unit]") {
    for (const MarkerClass c :
         {MarkerClass::SOL, MarkerClass::EOL, MarkerClass::BCN, MarkerClass::SPLTBCN, MarkerClass::IGRDBCN,
          MarkerClass::RESET, MarkerClass::STGSTRT, MarkerClass::STGEND, MarkerClass::DeviceEvent}) {
        CHECK(parseMarkerClass(markerClassName(c)) == c);
    }
    CHECK_FALSE(parseMarkerClass("SOR").has_value()); // unknown .ldx classes are not markers
    CHECK_FALSE(parseMarkerClass("bcn").has_value());
    for (const MarkerEditOp op :
         {MarkerEditOp::Offset, MarkerEditOp::Ignore, MarkerEditOp::Restore, MarkerEditOp::OffsetDistance}) {
        CHECK(parseMarkerEditOp(markerEditOpName(op)) == op);
    }
    CHECK(markerEditOpName(MarkerEditOp::OffsetDistance) == "offsetDistance");
}

TEST_CASE("marker: model order is (t, class, id)", "[unit]") {
    std::vector<Marker> m{makeLoggedMarker(MarkerClass::EOL, 100, "", ""),
                          makeLoggedMarker(MarkerClass::BCN, 50, "Beacons", "B2"),
                          makeLoggedMarker(MarkerClass::DeviceEvent, 0, "DeviceEvents", "Logging on"),
                          makeLoggedMarker(MarkerClass::SOL, 0, "", ""),
                          makeLoggedMarker(MarkerClass::BCN, 50, "Beacons", "B1", "", 0, 1)};
    sortMarkers(m);
    CHECK(m[0].cls == MarkerClass::SOL); // SOL first among markers at the same time
    CHECK(m[1].cls == MarkerClass::DeviceEvent);
    CHECK(m[2].cls == MarkerClass::BCN);
    CHECK(m[3].cls == MarkerClass::BCN);
    CHECK(m[2].id < m[3].id);
    CHECK(m[4].cls == MarkerClass::EOL);
    // At equal times an EOL comes before a SOL (one outing ends where the next starts, D-158), then the rest
    // in declaration order.
    std::vector<Marker> joint{makeLoggedMarker(MarkerClass::BCN, 200, "Beacons", "B3"),
                              makeLoggedMarker(MarkerClass::SOL, 200, "", ""),
                              makeLoggedMarker(MarkerClass::RESET, 200, "", "Reset"),
                              makeLoggedMarker(MarkerClass::EOL, 200, "", "")};
    sortMarkers(joint);
    CHECK(joint[0].cls == MarkerClass::EOL);
    CHECK(joint[1].cls == MarkerClass::SOL);
    CHECK(joint[2].cls == MarkerClass::BCN);
    CHECK(joint[3].cls == MarkerClass::RESET);
}

TEST_CASE("marker: logged ids count ordinals per (class, group, logged time) in file order", "[unit]") {
    std::vector<Marker> m(4);
    m[0].cls = m[1].cls = m[2].cls = MarkerClass::BCN;
    m[0].group = m[1].group = m[2].group = "Beacons";
    m[0].t = m[1].t = 500;
    m[2].t = 600;
    m[3].cls = MarkerClass::BCN;
    m[3].group = "Beacons";
    m[3].t = 500;
    m[3].prov = Provenance::User;
    m[3].id = MarkerId{uuidRandom()};
    const MarkerId userId = m[3].id;
    assignLoggedIds(m);
    CHECK(m[0].id == markerIdLogged(MarkerClass::BCN, 500, "Beacons", 0));
    CHECK(m[1].id == markerIdLogged(MarkerClass::BCN, 500, "Beacons", 1));
    CHECK(m[2].id == markerIdLogged(MarkerClass::BCN, 600, "Beacons", 0));
    CHECK(m[3].id == userId); // user markers keep their random id

    // An offset marker keeps the id of its logged time.
    std::vector<Marker> moved{m[0]};
    moved[0].originalT = 500;
    moved[0].t = 900;
    assignLoggedIds(moved);
    CHECK(moved[0].id == markerIdLogged(MarkerClass::BCN, 500, "Beacons", 0));
}

TEST_CASE("marker: device events (port specification section 4)", "[unit]") {
    CHECK(deviceEventName(0) == "Firmware start");
    CHECK(deviceEventName(3) == "Logging on");
    CHECK(deviceEventName(5) == "Log discontinuity");
    CHECK(deviceEventName(52) == "Event 52");
    const Marker e = makeDeviceEventMarker(4, 1'234'567, 0);
    CHECK(e.cls == MarkerClass::DeviceEvent);
    CHECK(e.group == "DeviceEvents");
    CHECK(e.name == "Logging off");
    CHECK(e.comment.empty());
    CHECK(e.flags == 4);
    CHECK(e.prov == Provenance::Logged);
    CHECK(e.id == markerIdLogged(MarkerClass::DeviceEvent, 1'234'567, "DeviceEvents", 0));
}

TEST_CASE("marker: the companion union collapses identical ids to prov Companion", "[unit]") {
    const Marker sol = makeLoggedMarker(MarkerClass::SOL, 0, "", "", "", 0, 0, Provenance::Derived);
    const Marker eol = makeLoggedMarker(MarkerClass::EOL, 9'000, "", "", "", 0, 0, Provenance::Derived);
    const Marker b1 = makeLoggedMarker(MarkerClass::BCN, 1'000, "Beacons", "Lap");
    Marker b1c = b1;
    b1c.name = "Manual.1";
    b1c.flags = 77;
    b1c.prov = Provenance::Logged; // the reader's provenance is overridden
    const Marker b2c = makeLoggedMarker(MarkerClass::BCN, 2'000, "Beacons", "Manual.2", "", 77);

    std::vector<ModelNote> notes;
    const auto merged = mergeMarkers({eol, b1, sol}, {b2c, b1c, b2c}, &notes, "Normal");
    REQUIRE(merged.size() == 4);
    CHECK(merged[0].id == sol.id);
    CHECK(merged[1].id == b1.id);
    CHECK(merged[1].prov == Provenance::Companion);
    CHECK(merged[1].name == "Manual.1"); // companion > file
    CHECK(merged[1].flags == 77);
    CHECK(merged[2].id == b2c.id);
    CHECK(merged[2].prov == Provenance::Companion);
    CHECK(merged[3].id == eol.id);
    CHECK(merged[3].prov == Provenance::Derived);
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].code == kNoteDuplicateMarkerId);
    CHECK(notes[0].layer == "Normal");
}

TEST_CASE("marker: offset and ignore edits; orphans are reported, never dropped", "[unit]") {
    std::vector<Marker> m{makeLoggedMarker(MarkerClass::BCN, 1'000, "Beacons", "B1"),
                          makeLoggedMarker(MarkerClass::BCN, 2'000, "Beacons", "B2")};
    const MarkerId ghost = markerIdLogged(MarkerClass::BCN, 3'000, "Beacons", 0);
    std::vector<MarkerEdit> edits(5);
    edits[0] = MarkerEdit{m[0].id, MarkerEditOp::Offset, -120, 0.0, "m"};
    edits[1] = MarkerEdit{m[1].id, MarkerEditOp::Ignore, 0, 0.0, "m"};
    edits[2] = MarkerEdit{ghost, MarkerEditOp::Ignore, 0, 0.0, "m"};
    edits[3] = MarkerEdit{MarkerId{}, MarkerEditOp::OffsetDistance, 0, 12.5, "m"};
    edits[4] =
        MarkerEdit{m[0].id, MarkerEditOp::Offset, 50, 0.0, "m"}; // a later offset replaces, from loggedT
    const auto notes = applyMarkerEdits(m, edits, "Normal");
    CHECK(m[0].t == 1'050);
    CHECK(m[0].originalT == 1'000);
    CHECK(m[0].loggedT() == 1'000);
    CHECK(m[0].enabled);
    CHECK_FALSE(m[1].enabled);
    CHECK(m[1].t == 2'000);
    CHECK_FALSE(m[1].originalT.has_value());
    REQUIRE(notes.size() == 2);
    CHECK(notes[0].code == kNoteOrphanMarkerEdit);
    CHECK(notes[0].ref == ghost.str());
    CHECK(notes[1].code == kNoteOffsetDistancePending);
}

TEST_CASE("marker: markerSetHash follows ids, times and the enabled flag", "[unit]") {
    std::vector<Marker> m{makeLoggedMarker(MarkerClass::SOL, 0, "", ""),
                          makeLoggedMarker(MarkerClass::BCN, 1'000, "Beacons", "B1")};
    const Hash128 h = markerSetHash(m);
    CHECK(markerSetHash(m) == h);
    auto renamed = m;
    renamed[1].name = "Other"; // names do not change laps
    CHECK(markerSetHash(renamed) == h);
    auto moved = m;
    moved[1].t = 1'001;
    CHECK(markerSetHash(moved) != h);
    auto ignored = m;
    ignored[1].enabled = false;
    CHECK(markerSetHash(ignored) != h);
    CHECK(markerSetHash(std::vector<Marker>{}) != h);
}
