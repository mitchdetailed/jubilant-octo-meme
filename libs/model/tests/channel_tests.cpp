// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelDescriptor, Channel, EnumTable, SourceEncoding and the name tables (02 section 4.1; D-013, D-060).
#include "model_test_support.hpp"

#include <esses/model/channel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <type_traits>

using namespace esses;
using namespace esses::model;

namespace {

struct FakeEncoding final : SourceEncoding {
    std::string_view importerId() const override { return "motec.ld"; }
    int counter = 7;
};

} // namespace

TEST_CASE("channel: SampleKind is store's enum, re-exported", "[unit]") {
    STATIC_REQUIRE(std::is_same_v<SampleKind, store::SampleKind>);
    CHECK(store::sampleKindName(SampleKind::Enum) == "Enum");
}

TEST_CASE("channel: descriptor defaults", "[unit]") {
    const ChannelDescriptor d;
    CHECK(d.unit == units::kNoUnit);
    CHECK(d.quantity == units::kUnitless);
    CHECK(d.dtype == store::Dtype::F32);
    CHECK(d.kind == SampleKind::Real);
    CHECK(d.interp == store::Interp::Linear);
    CHECK(d.dps == 2);
    CHECK(d.colourIndex == -1);
    CHECK_FALSE(d.enums.has_value());
    CHECK(d.source == ChannelSource::Logged);
    CHECK(d.statusText.empty());
    CHECK_FALSE(d.rate.has_value());
    CHECK(d.sourceEncoding == nullptr);
}

TEST_CASE("channel: source names round-trip", "[unit]") {
    for (const ChannelSource s :
         {ChannelSource::Logged, ChannelSource::Maths, ChannelSource::Detail, ChannelSource::Alias,
          ChannelSource::Mapped, ChannelSource::Constant, ChannelSource::Setup}) {
        const auto name = channelSourceName(s);
        CHECK(parseChannelSource(name) == s);
    }
    CHECK(channelSourceName(ChannelSource::Mapped) == "mapped");
    CHECK_FALSE(parseChannelSource("Logged").has_value()); // lower case only
}

TEST_CASE("channel: displayShortName derives eight code points from the name", "[unit]") {
    ChannelDescriptor d;
    d.name = "Engine Oil Temperature";
    CHECK(d.displayShortName() == "Engine O");
    d.name = "\u00b0Celsius Long";                  // starts with U+00B0, two bytes in UTF-8
    CHECK(d.displayShortName() == "\u00b0Celsius"); // eight code points, nine bytes
    d.name = "RPM";
    CHECK(d.displayShortName() == "RPM");
    d.shortName = "EngSpd";
    CHECK(d.displayShortName() == "EngSpd");
}

TEST_CASE("channel: status mirrors statusText (D-060)", "[unit]") {
    Channel c = test::makeChannel("Speed");
    CHECK(c.status == ChannelStatus::Ok);
    CHECK(c.desc.statusText.empty());
    c.setStatus(ChannelStatus::Pending);
    CHECK(c.desc.statusText == "Pending");
    c.setStatus(ChannelStatus::Error, "division by zero");
    CHECK(c.desc.statusText == "Error: division by zero");
    CHECK(statusFromText(c.desc.statusText) == ChannelStatus::Error);
    CHECK(statusFromText("Pending") == ChannelStatus::Pending);
    CHECK(statusFromText("") == ChannelStatus::Ok);
    CHECK(statusFromText("Unit unknown: \"psi x\"") == ChannelStatus::Ok);
    c.setStatus(ChannelStatus::Ok);
    CHECK(c.desc.statusText.empty());
    CHECK(channelStatusName(ChannelStatus::Pending) == "Pending");
}

TEST_CASE("channel: enumeration tables carry a severity per entry", "[unit]") {
    EnumTable t{EnumTableId{3}, "Gear", {{-1, "R", 0}, {0, "N", 1}, {1, "1st", 0}, {99, "Fault", 3}}};
    REQUIRE(t.find(99) != nullptr);
    CHECK(t.find(99)->severity == 3);
    CHECK(t.find(0)->label == "N");
    CHECK(t.find(5) == nullptr);
    EnumTable copy = t;
    CHECK(copy == t);
    copy.entries[0].severity = 2;
    CHECK_FALSE(copy == t);
}

TEST_CASE("channel: the source encoding is shared on copy and compared by pointer", "[unit]") {
    ChannelDescriptor a;
    a.name = "RPM";
    auto enc = std::make_shared<FakeEncoding>();
    a.sourceEncoding = enc;
    const ChannelDescriptor b = a;
    CHECK(b.sourceEncoding.get() == enc.get());
    CHECK(b.sourceEncoding->importerId() == "motec.ld");
    CHECK(a == b);
    ChannelDescriptor c = a;
    c.sourceEncoding = std::make_shared<FakeEncoding>();
    CHECK_FALSE(a == c);
    CHECK(enc.use_count() == 3);
}

TEST_CASE("channel: descriptor fields that M0 importers fill", "[unit]") {
    ChannelDescriptor d;
    d.id = channelIdLogged(test::testSource(), "Normal", "Gbox Oil Temp", "motec.ld");
    d.name = d.loggedName = "Gbox Oil Temp";
    d.unit = test::shippedUnits().find("K").value();
    d.quantity = test::shippedUnits().quantityOf(d.unit);
    d.dtype = store::Dtype::F32;
    d.kind = SampleKind::Real;
    d.interp = store::Interp::Linear;
    d.dps = 1; // .ld: byte 0x64 of the channel record
    d.rate = Rate::fromHz(20);
    CHECK(test::shippedUnits().quantity(d.quantity).name == "Temperature");
    CHECK(d.rate->hz() == 20.0);
    const ChannelDescriptor copy = d;
    CHECK(copy == d);
}
