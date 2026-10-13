// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// .ldx companion (05 sections 4 and 5 item 7): markers, details, verbatim preservation of unknown elements
// and markers of unknown classes, malformed input, writer round trips.
#include "ld/ld_codes.hpp"
#include "ld/ldx.hpp"
#include "ld_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace esses;
using namespace esses::formats;
using namespace esses::formats::ld;
using namespace esses::formats::ld::test;

namespace {

std::span<const std::byte> asBytes(const std::string& s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

LdxDocument parseOk(const std::string& text, ImportLog& log, std::optional<Micros> duration = std::nullopt) {
    auto r = parseLdx(asBytes(text), log, duration);
    REQUIRE(r.ok());
    return std::move(*r);
}

// The shape every .ldx on the development machine has (S-1 D14): details only, no markers.
const std::string kDetailsOnly = "<?xml version=\"1.0\"?>\n"
                                 "<LDXFile Locale=\"English_United States.1252\" DefaultLocale=\"C\" "
                                 "Version=\"1.6\">\n"
                                 " <Layers>\n"
                                 "  <Details>\n"
                                 "   <String Id=\"Total Laps\" Value=\"1\"/>\n"
                                 "   <String Id=\"Fastest Time\" Value=\"1456:09.100\"/>\n"
                                 "   <String Id=\"Fastest Lap\" Value=\"0\"/>\n"
                                 "  </Details>\n"
                                 " </Layers>\n"
                                 "</LDXFile>\n";

const std::string kMarkers =
    "<?xml version=\"1.0\"?>\n"
    "<LDXFile Locale=\"English_United Kingdom.1252\" DefaultLocale=\"C\" Version=\"1.6\">\n"
    " <Layers>\n"
    "  <Layer>\n"
    "   <MarkerBlock>\n"
    "    <MarkerGroup Name=\"Beacons\" Index=\"3\">\n"
    "     <Marker Version=\"100\" ClassName=\"BCN\" Name=\"Manual.2\" Flags=\"77\" Time=\"183456789.00\"/>\n"
    "     <Marker Version=\"100\" ClassName=\"BCN\" Name=\"Manual.1\" Flags=\"77\" Time=\"123456789.50\"/>\n"
    "     <Marker Version=\"100\" ClassName=\"SOR\" Name=\"Odd\" Flags=\"1\" Time=\"5.00\">\n"
    "        <Extra a='x &amp; y'/>\n"
    "     </Marker>\n"
    "     <Marker Version=\"100\" ClassName=\"BCN\" Name=\"Same time\" Flags=\"77\" Time=\"123456789.50\" "
    "Colour=\"red\"/>\n"
    "     <Marker Version=\"100\" ClassName=\"BCN\" Name=\"Bad\" Flags=\"77\" Time=\"soon\"/>\n"
    "    </MarkerGroup>\n"
    "    <Unknown   attr = \"1\" ><!-- note --><![CDATA[ <raw> ]]><Child/></Unknown>\n"
    "   </MarkerBlock>\n"
    "  </Layer>\n"
    "  <Details>\n"
    "   <String Id=\"Total Laps\" Value=\"12\"/>\n"
    "   <DateTime Id=\"Log Date\" Value=\"2026-10-10\"/>\n"
    "  </Details>\n"
    "  <Session note=\"opaque\"/>\n"
    " </Layers>\n"
    "</LDXFile>\n";

} // namespace

TEST_CASE("ldx: details-only documents (the observed shape)", "[unit][ldx]") {
    ImportLog log;
    const LdxDocument d = parseOk(kDetailsOnly, log);
    CHECK(d.markers.empty());
    CHECK_FALSE(d.hasLayer);
    CHECK(d.hasDetails);
    REQUIRE(d.details.size() == 3);
    CHECK(d.details[1].id == "Fastest Time");
    CHECK(d.details[1].value == "1456:09.100"); // minutes not wrapped at 60
    REQUIRE(d.findDetail("Total Laps") != nullptr);
    CHECK(d.findDetail("Total Laps")->value == "1");
    CHECK(d.rootAttributes.size() == 3);
    CHECK(d.rootAttributes[0] == LdxAttribute{"Locale", "English_United States.1252"});
    CHECK(log.size() == 0);

    const std::string out = writeLdx(d);
    CHECK(out.find("<Layer>") == std::string::npos); // no Layer invented
    ImportLog log2;
    const LdxDocument again = parseOk(out, log2);
    CHECK(again.details == d.details);
    CHECK(again.rootAttributes == d.rootAttributes);
    CHECK(writeLdx(again) == out);
}

TEST_CASE("ldx: markers, ordinals and the verbatim preservation of unknown content", "[unit][ldx]") {
    ImportLog log;
    const LdxDocument d = parseOk(kMarkers, log, Micros{150'000'000});
    REQUIRE(d.groups.size() == 1);
    CHECK(d.groups[0].name == "Beacons");
    CHECK(d.groups[0].index == "3");
    REQUIRE(d.markers.size() == 3);
    CHECK(d.markers[0].name == "Manual.1");
    CHECK(d.markers[0].tUs == 123'456'790); // llround(123456789.50)
    CHECK(d.markers[0].ordinal == 0);
    CHECK(d.markers[1].name == "Same time");
    CHECK(d.markers[1].ordinal == 1);
    CHECK(d.markers[2].name == "Manual.2");
    CHECK(d.markers[2].flags == "77");
    CHECK(d.markers[2].flagsValue == 77u);
    CHECK(d.markers[2].className == "BCN");
    CHECK(log.contains(code::kLdxMarkerUnsorted));
    CHECK(log.contains(code::kLdxUnknownMarkerClass));
    CHECK(log.contains(code::kLdxMarkerTimeUnparsable));
    CHECK(log.contains(code::kLdxUnknownElement));
    CHECK(log.contains(code::kLdxUnknownAttribute));
    CHECK(log.contains(code::kLdxMarkerBeyondEnd));
    REQUIRE(d.details.size() == 1);

    // Opaque: the SOR marker, the bad-time marker, Unknown, DateTime, Session - each byte-identical.
    REQUIRE(d.opaque.size() == 5);
    const std::string sor =
        "<Marker Version=\"100\" ClassName=\"SOR\" Name=\"Odd\" Flags=\"1\" Time=\"5.00\">\n"
        "        <Extra a='x &amp; y'/>\n"
        "     </Marker>";
    const std::string unknown = "<Unknown   attr = \"1\" ><!-- note --><![CDATA[ <raw> ]]><Child/></Unknown>";
    CHECK(d.opaque[0].xml == sor);
    CHECK(d.opaque[0].parentPath == "LDXFile/Layers/Layer/MarkerBlock/MarkerGroup#0");
    CHECK(d.opaque[0].position == 2);
    CHECK(d.opaque[1].position == 4);
    CHECK(d.opaque[2].xml == unknown);
    CHECK(d.opaque[3].xml == "<DateTime Id=\"Log Date\" Value=\"2026-10-10\"/>");
    CHECK(d.opaque[4].xml == "<Session note=\"opaque\"/>");

    const std::string out = writeLdx(d);
    for (const LdxOpaque& o : d.opaque) {
        CHECK(out.find(o.xml) != std::string::npos);
    }
    // The unknown attribute on a known marker survives in its place.
    CHECK(out.find("Colour=\"red\"") != std::string::npos);
    CHECK(out.find("Time=\"123456789.50\"") != std::string::npos);

    ImportLog log2;
    const LdxDocument again = parseOk(out, log2);
    CHECK(again.markers == d.markers);
    CHECK(again.groups == d.groups);
    CHECK(again.details == d.details);
    REQUIRE(again.opaque.size() == d.opaque.size());
    for (std::size_t i = 0; i < d.opaque.size(); ++i) {
        CHECK(again.opaque[i] == d.opaque[i]);
    }
    CHECK(writeLdx(again) == out);
}

// Regression (fuzz_ldx, first CI run): elements kept verbatim that themselves contain the writer's reserved
// placeholder instruction ("<?esses-opaque N?>") as a processing instruction, in a comment, in CDATA or in an
// attribute value (pugixml accepts a raw '<' there). The writer used to search its whole output for each
// placeholder after splicing earlier elements in, so the comment in <Note> captured the bad-time EOL marker
// and the output was no longer well-formed. Every element must land at its own placeholder, byte-identical.
TEST_CASE("ldx: verbatim elements containing the reserved placeholder round-trip unchanged", "[unit][ldx]") {
    const std::string text =
        "<?xml version=\"1.0\"?>\n"
        "<LDXFile Locale=\"C\" DefaultLocale=\"C\" Version=\"1.6\">\n"
        " <Layers>\n"
        "  <Layer>\n"
        "   <MarkerBlock>\n"
        "    <MarkerGroup Name=\"Beacons\" Index=\"3\">\n"
        "     <Note><!-- <?esses-opaque 1?> --><?esses-opaque 2?><![CDATA[<?esses-opaque 3?>]]></Note>\n"
        "     <Marker Version=\"100\" ClassName=\"EOL\" Name=\"late\" Flags=\"77\" Time=\"soon\"><!-- x "
        "--></Marker>\n"
        "     <Marker Version=\"100\" ClassName=\"BCN\" Name=\"Manual.1\" Flags=\"77\" "
        "Time=\"5000000.00\"/>\n"
        "     <Odd a=\"]]>\"/>\n"
        "    </MarkerGroup>\n"
        "   </MarkerBlock>\n"
        "  </Layer>\n"
        "  <Details>\n"
        "   <String Id=\"Note\" Value=\"<?esses-opaque 0?>\"/>\n"
        "  </Details>\n"
        "  <Extra b='<?esses-opaque 0?>'/>\n"
        " </Layers>\n"
        "</LDXFile>\n";
    ImportLog log;
    const LdxDocument d = parseOk(text, log);
    REQUIRE(d.markers.size() == 1);
    REQUIRE(d.details.size() == 1);
    CHECK(d.details[0].value == "<?esses-opaque 0?>"); // a known attribute: the writer escapes it
    REQUIRE(d.opaque.size() == 4);
    CHECK(d.opaque[0].xml ==
          "<Note><!-- <?esses-opaque 1?> --><?esses-opaque 2?><![CDATA[<?esses-opaque 3?>]]></Note>");
    CHECK(d.opaque[1].xml == "<Marker Version=\"100\" ClassName=\"EOL\" Name=\"late\" Flags=\"77\" "
                             "Time=\"soon\"><!-- x --></Marker>");
    CHECK(d.opaque[2].xml == "<Odd a=\"]]>\"/>");
    CHECK(d.opaque[3].xml == "<Extra b='<?esses-opaque 0?>'/>");

    const std::string out = writeLdx(d);
    for (const LdxOpaque& o : d.opaque) {
        const std::size_t at = out.find(o.xml);
        CHECK(at != std::string::npos);
        CHECK(out.find(o.xml, at + 1) == std::string::npos); // each verbatim text exactly once
    }
    ImportLog log2;
    auto again = parseLdx(asBytes(out), log2);
    REQUIRE(again.ok()); // the fuzz_ldx invariant: the writer's output parses again
    CHECK(again->markers == d.markers);
    CHECK(again->details == d.details);
    REQUIRE(again->opaque.size() == d.opaque.size());
    for (std::size_t i = 0; i < d.opaque.size(); ++i) {
        CHECK(again->opaque[i] == d.opaque[i]);
    }
    CHECK(writeLdx(*again) == out);
}

TEST_CASE("ldx: malformed documents log ldx.parse_error and fail", "[unit][ldx]") {
    ImportLog log;
    const std::string broken = "<LDXFile><Layers></LDXFile>";
    auto r = parseLdx(asBytes(broken), log);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Format);
    CHECK(log.contains(code::kLdxParseError));

    ImportLog log2;
    auto wrongRoot = parseLdx(asBytes(std::string("<Other/>")), log2);
    REQUIRE_FALSE(wrongRoot.ok());
    CHECK(log2.contains(code::kLdxParseError));

    ImportLog log3;
    CHECK_FALSE(parseLdx({}, log3).ok());
}

TEST_CASE("ldx: documents built through the API", "[unit][ldx]") {
    LdxDocument d = LdxDocument::makeDefault();
    d.addMarker("Beacons", "BCN", "Manual.1", 60'000'000.0);
    d.addMarker("Beacons", "BCN", "Manual.0", 1'500'000.25);
    d.addMarker("Splits", "SPLTBCN", "S1", 30'000'000.0);
    d.setDetail("Total Laps", "2");
    d.setDetail("Total Laps", "3");
    CHECK(d.details.size() == 1);
    CHECK(isKnownMarkerClass("IGRDBCN"));
    CHECK_FALSE(isKnownMarkerClass("SOR"));
    CHECK(formatLdxTime(1'500'000.25) == "1500000.25");
    CHECK(formatLdxTime(7.0) == "7.00");

    TempDir dir("ldx");
    const auto path = dir.path() / "doc.ldx";
    REQUIRE(writeLdxFile(path, d).ok());
    ImportLog log;
    auto back = readLdxFile(path, log);
    REQUIRE(back.ok());
    REQUIRE(back->markers.size() == 3);
    CHECK(back->markers[0].name == "Manual.0");
    CHECK(back->markers[0].timeUs == 1'500'000.25);
    CHECK(back->markers[1].className == "SPLTBCN");
    CHECK(back->groups.size() == 2);
    CHECK(back->findDetail("Total Laps")->value == "3");
    for (const auto& e : log.entries()) {
        UNSCOPED_INFO(e.code << ": " << e.message);
    }
    CHECK(log.size() == 0);
}
