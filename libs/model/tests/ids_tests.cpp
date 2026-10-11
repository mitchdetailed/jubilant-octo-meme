// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Deterministic ids (02 section 4.1 table, section 4.3 behaviour 5, section 4.4 rule 9; D-013, D-063, D-066,
// D-130): each derivation hashes exactly the parts the table lists, in order, and is pinned by a known answer
// so a change to any part shows up on every platform.
#include "model_test_support.hpp"

#include <esses/base/hash.hpp>
#include <esses/base/uuid.hpp>
#include <esses/model/channel.hpp>
#include <esses/model/marker.hpp>
#include <esses/model/range.hpp>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace esses;
using namespace esses::model;
using esses::model::test::testSource;

TEST_CASE("ids: a logged ChannelId hashes logged, source hex, layer, logged name, importer id", "[unit]") {
    const SourceId src = testSource();
    const ChannelId id = channelIdLogged(src, "Normal", "Engine RPM", "motec.ld");
    const ChannelId expected{
        uuidFromHash(hash128({"logged", src.hex(), "Normal", "Engine RPM", "motec.ld"}))};
    CHECK(id == expected);
    CHECK(id.v.version() == 5);
    CHECK((id.v.b[8] & 0xC0) == 0x80); // RFC 4122 variant
    // Known answer: pins the derivation across platforms and builds.
    CHECK(id.str() == "f782edca-6a96-557f-ae7b-d1f164baa3a1");
}

TEST_CASE("ids: ChannelId parts are distinct and ordered", "[unit]") {
    const SourceId src = testSource();
    const SourceId other{1, 2};
    const ChannelId base = channelIdLogged(src, "Normal", "Speed", "motec.ld");
    CHECK(channelIdLogged(src, "Normal", "Speed", "motec.ld") == base);   // stable without a registry
    CHECK(channelIdLogged(other, "Normal", "Speed", "motec.ld") != base); // two files, same channel
    CHECK(channelIdLogged(src, "Fastest", "Speed", "motec.ld") != base);
    CHECK(channelIdLogged(src, "Normal", "speed", "motec.ld") != base);  // the logged spelling, exactly
    CHECK(channelIdLogged(src, "Normal", "Speed", "motec.csv") != base); // D-130: the importer id is hashed
    CHECK(channelIdLogged(src, "", "Speed", "motec.ld") == base);        // "" is the alias of Normal
    // Length-prefixed parts: moving a character between parts changes the id.
    CHECK(channelIdLogged(src, "Normal", "ab", "c") != channelIdLogged(src, "Normal", "a", "bc"));
}

TEST_CASE("ids: non-logged ChannelIds use their own defining key (D-063)", "[unit]") {
    const SourceId src = testSource();
    const ChannelId global =
        channelIdMathsModule(src, "Normal", "Corr Speed", "esses.corrected_speed_distance");
    CHECK(global == ChannelId{uuidFromHash(hash128({"maths", src.hex(), "Normal", "Corr Speed",
                                                    "module:esses.corrected_speed_distance"}))});
    const ChannelId local = channelIdMathsLocal(src, "Normal", "Corr Speed");
    CHECK(local == ChannelId{uuidFromHash(hash128({"maths", src.hex(), "Normal", "Corr Speed", "local"}))});
    CHECK(global != local);
    CHECK(channelIdMathsModule(src, "Normal", "Corr Speed", "local") != local); // "module:local" != "local"

    std::set<ChannelId> ids{global, local};
    for (const ChannelSource kind : {ChannelSource::Detail, ChannelSource::Alias, ChannelSource::Mapped,
                                     ChannelSource::Constant, ChannelSource::Setup}) {
        const ChannelId id = channelIdOther(kind, src, "Normal", "Corr Speed");
        const std::string kindName(channelSourceName(kind));
        CHECK(id == ChannelId{uuidFromHash(hash128({kindName, src.hex(), "Normal", "Corr Speed", ""}))});
        CHECK(ids.insert(id).second);
    }
    CHECK(ids.size() == 7);
}

TEST_CASE("ids: std::hash makes the ids usable as unordered keys", "[unit]") {
    std::unordered_map<ChannelId, int> byId;
    for (int i = 0; i < 100; ++i) {
        byId.emplace(channelIdLogged(testSource(), "Normal", "ch" + std::to_string(i), "x"), i);
    }
    CHECK(byId.size() == 100);
    CHECK(byId.at(channelIdLogged(testSource(), "Normal", "ch42", "x")) == 42);

    std::unordered_set<MarkerId> markers{markerIdLogged(MarkerClass::BCN, 1, "Beacons", 0),
                                         markerIdLogged(MarkerClass::BCN, 1, "Beacons", 1)};
    CHECK(markers.size() == 2);
    std::unordered_set<RangeId> ranges{rangeId("Outings", MarkerId{}), rangeId("Outings:Laps", MarkerId{})};
    CHECK(ranges.size() == 2);
    std::unordered_set<EnumTableId> tables{EnumTableId{1}, EnumTableId{2}, EnumTableId{1}};
    CHECK(tables.size() == 2);
}

TEST_CASE("ids: MarkerId = hash of marker, class, logged time, group, ordinal", "[unit]") {
    const MarkerId id = markerIdLogged(MarkerClass::BCN, 83'412'340, "Beacons", 0);
    CHECK(id.v == uuidFromHash(hash128({"marker", "BCN", "83412340", "Beacons", "0"})));
    CHECK(id.str() == "5a73095a-1c57-5242-891a-156d576fb792");
    CHECK(markerIdLogged(MarkerClass::BCN, 83'412'340, "Beacons", 1) != id); // two beacons, same microsecond
    CHECK(markerIdLogged(MarkerClass::SPLTBCN, 83'412'340, "Beacons", 0) != id);
    CHECK(markerIdLogged(MarkerClass::BCN, 83'412'341, "Beacons", 0) != id);
    CHECK(markerIdLogged(MarkerClass::BCN, 83'412'340, "", 0) != id);
    CHECK(markerIdLogged(MarkerClass::SOL, -5, "", 0).v ==
          uuidFromHash(hash128({"marker", "SOL", "-5", "", "0"}))); // negative times print with a sign
}

TEST_CASE("ids: RangeId = hash of range, group path, anchor id, qualifier (D-066)", "[unit]") {
    const MarkerId anchor = markerIdLogged(MarkerClass::BCN, 1'000'000, "Beacons", 0);
    const RangeId lap = rangeId("Outings:Laps", anchor);
    CHECK(lap.v == uuidFromHash(hash128({"range", "Outings:Laps", anchor.str(), ""})));
    CHECK(lap.str() == "2543239f-eed0-54c4-a008-4bf0df2d1450");
    CHECK(rangeId("Outings:Laps", anchor) == lap);
    CHECK(rangeId("Outings", anchor) != lap);
    const RangeId t3 = rangeId("Outings:Laps:Track Sections:Default", anchor, "Turn 3");
    const RangeId t4 = rangeId("Outings:Laps:Track Sections:Default", anchor, "Turn 4");
    CHECK(t3 != t4);
    CHECK(t3 != rangeId("Outings:Laps:Track Sections:Default", anchor));
}
