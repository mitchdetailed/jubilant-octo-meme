// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Synthetic .ld (+ .ldx) inputs for the importer-layer tests and the probe_json goldens. Every value is
// chosen here, so the expectations in the tests and in tests/data/probe/*.probe.json follow from this file,
// never from the reader.
#pragma once

#include "formats_test_support.hpp"

#include <optional>
#include <string>
#include <vector>

namespace esses::formats::test {

namespace L = ld::layout;

// A fixed device identity, so the goldens do not move with the project version.
inline ld::LdWriteOptions fixedIdentity(ld::WriteDialect dialect = ld::WriteDialect::PcLayout) {
    ld::LdWriteOptions o;
    o.dialect = dialect;
    if (dialect == ld::WriteDialect::PcLayout) {
        o.deviceType = "TESTLOG";
        o.serial = std::uint32_t{12345};
        o.deviceVersion = std::uint16_t{7};
    }
    return o;
}

inline ld::LdWriteDetails richDetails() {
    ld::LdWriteDetails d;
    d.date = "10/10/2026";
    d.time = "09:15:30";
    d.driver = "Test Driver";
    d.vehicleId = "GT3-07";
    d.engineId = "E-01";
    d.venue = "Test Circuit";
    d.session = "Practice 1";
    d.shortComment = "short";
    d.team = "Team Blue";
    d.eventName = "Club Day";
    d.longComment = "A long comment";
    d.venueName = "Test Circuit";
    d.venueLengthMm = 6'213'000;
    d.venueCategory = "Club";
    d.vehicleRecordId = "GT3-07";
    d.vehicleDesc = "Test car";
    d.vehicleEngineId = "E-02"; // the vehicle record's copy wins (05 section 3.2)
    d.weightKg = 1250;
    d.fuelTankX10 = 650;
    d.vehicleType = "Saloon";
    d.driveType = "RWD";
    d.diffX1000 = 3545;
    d.gearsX1000 = {3200, 2400, 1900, 1550, 1300, 1100, 0, 0, 0, 0};
    d.trackMm = 1600;
    d.wheelbaseMm = 2700;
    d.vehicleComment = "car comment";
    d.vehicleNumber = "7";
    d.sky = "Sunny";
    d.airTemp = {"23.5", "C"};
    d.trackTemp = {"31", "C"};
    d.baroPressure = {"1013.2", "mbar"};
    d.relHumidity = {"55", "%"};
    d.windSpeed = {"12", "km/h"};
    d.windDirection = "NE";
    d.weatherComment = "dry";
    return d;
}

inline ld::LdEnumTable engineStateTable() {
    ld::LdEnumTable t;
    t.index = 3;
    t.entries = {{-1, "Unknown", false, 0},
                 {0, "Stopped", false, 0},
                 {1, "Cranking", true, 1},
                 {2, "Running", false, 0},
                 {3, "Overheat", true, 3}};
    return t;
}

// pc-layout with every kind of channel the importer maps: scaled and unscaled integers, a shift/mul channel,
// floats with gap blocks, an enumeration, a long name, an F64 fixed-point channel, an unknown unit.
inline ld::LdWriteInput mixedInput() {
    ld::LdWriteInput in;
    in.details = richDetails();
    {
        auto c = ld::test::intChannel("Engine Speed", L::kTypeInt3, 2, 50, ld::test::ramp(500, 800, 10));
        c.unit = "rpm";
        c.interpByte = L::kInterpLinear;
        in.channels.push_back(std::move(c));
    }
    {
        auto c = ld::test::intChannel("Gbox Oil Temp", L::kTypeInt3, 2, 2, ld::test::ramp(50, 238, 1));
        c.unit = "F";
        c.shift = 32;
        c.mul = 9;
        c.scale = 5;
        c.dec = 1;
        c.dpsByte = 1;
        c.interpByte = L::kInterpLinear;
        in.channels.push_back(std::move(c));
    }
    {
        std::vector<std::int64_t> gears;
        for (int i = 0; i < 250; ++i) {
            gears.push_back(i / 40);
        }
        in.channels.push_back(ld::test::intChannel("Gear", L::kTypeInt5, 4, 10, std::move(gears)));
    }
    {
        std::vector<double> v;
        for (int i = 0; i < 200; ++i) {
            v.push_back(0.75 + 0.001953125 * i); // exact in binary32
        }
        auto c = ld::test::floatChannel("Lambda", L::kTypeFloat, 4, 20, std::move(v));
        c.unit = "lambda";
        c.dpsByte = 3;
        c.invalid = {{10, 20}, {90, 100}};
        in.channels.push_back(std::move(c));
    }
    {
        std::vector<std::int64_t> v;
        for (int i = 0; i < 100; ++i) {
            v.push_back((i / 20) - 1); // -1 .. 3
        }
        auto c = ld::test::intChannel("Engine State", L::kTypeInt3, 2, 10, std::move(v));
        c.enumRef = std::uint16_t{3};
        in.channels.push_back(std::move(c));
    }
    {
        std::vector<double> v;
        for (int i = 0; i < 60; ++i) {
            v.push_back(i * 0.5);
        }
        auto c = ld::test::floatChannel("A channel name far longer than thirty-two bytes", L::kTypeFloat, 4,
                                        1, std::move(v));
        c.unit = "V";
        in.channels.push_back(std::move(c));
    }
    {
        auto c =
            ld::test::intChannel("GPS Latitude", L::kTypeInt5, 4, 10, ld::test::ramp(100, -338765432, 1234));
        c.dec = 7;
        c.unit = "deg";
        c.dpsByte = 7;
        in.channels.push_back(std::move(c));
    }
    {
        auto c = ld::test::intChannel("Brake Bias", L::kTypeInt3, 2, 10, ld::test::ramp(100, 500, 1));
        c.unit = "furlongs";
        c.scale = 10;
        c.dpsByte = 1;
        in.channels.push_back(std::move(c));
    }
    in.enumTables = {engineStateTable()};
    in.events = {{3, 0.0}, {17, 0.0}, {4, 24'999'999.6}};
    return in;
}

// A companion with markers (one of an unknown class), details (one overriding the .ld's Driver, one with an
// unknown attribute) and unknown elements at several levels, with unusual quoting and spacing.
inline std::string richLdx() {
    return "<?xml version=\"1.0\"?>\n"
           "<LDXFile Locale=\"English_United Kingdom.1252\" DefaultLocale=\"C\" Version=\"1.6\">\n"
           " <Layers>\n"
           "  <Layer>\n"
           "   <MarkerBlock>\n"
           "    <MarkerGroup Name=\"Beacons\" Index=\"3\">\n"
           "     <Marker Version=\"100\" ClassName=\"BCN\" Name=\"Manual.1\" Flags=\"77\" "
           "Time=\"5000000.00\"/>\n"
           "     <Marker Version=\"100\" ClassName=\"SOR\" Name=\"Start of run\" Flags=\"77\" "
           "Time=\"5500000.00\"/>\n"
           "     <Marker Version=\"100\" ClassName=\"BCN\" Name=\"Manual.2\" Flags=\"77\" "
           "Time=\"15000000.25\"/>\n"
           "     <Marker Version=\"100\" ClassName=\"SPLTBCN\" Name=\"Split.1\" Flags=\"5\" "
           "Time=\"9000000.00\" "
           "Colour=\"red\"/>\n"
           "     <Unknown1 a='1'>text<b/></Unknown1>\n"
           "    </MarkerGroup>\n"
           "   </MarkerBlock>\n"
           "   <LayerExtra kind=\"x\"/>\n"
           "  </Layer>\n"
           "  <Details>\n"
           "   <String Id=\"Total Laps\" Value=\"2\"/>\n"
           "   <String Id=\"Fastest Time\" Value=\"0:10.000\"/>\n"
           "   <String Id=\"Driver\" Value=\"Companion Driver\" Source=\"user\"/>\n"
           "   <Note>keep &amp; me</Note>\n"
           "  </Details>\n"
           " </Layers>\n"
           " <Custom  attr = \"spaced\" >\n"
           "   <Deep><Deeper x=\"1\"/></Deep>\n"
           " </Custom>\n"
           "</LDXFile>\n";
}

// The unknown elements of richLdx() exactly as they appear in it.
inline std::vector<std::string> richLdxUnknownElements() {
    return {
        "<Marker Version=\"100\" ClassName=\"SOR\" Name=\"Start of run\" Flags=\"77\" Time=\"5500000.00\"/>",
        "<Unknown1 a='1'>text<b/></Unknown1>", "<LayerExtra kind=\"x\"/>", "<Note>keep &amp; me</Note>",
        "<Custom  attr = \"spaced\" >\n   <Deep><Deeper x=\"1\"/></Deep>\n </Custom>"};
}

struct GoldenFamily {
    std::string name;
    ld::LdWriteInput input;
    ld::LdWriteOptions options;
    std::optional<std::string> ldx;
};

inline std::vector<GoldenFamily> goldenFamilies() {
    std::vector<GoldenFamily> out;
    out.push_back({"pc-layout-mixed", mixedInput(), fixedIdentity(), std::nullopt});
    {
        GoldenFamily f{"m1-dialect", {}, fixedIdentity(ld::WriteDialect::M1), std::nullopt};
        f.input.details.date = "11/10/2026";
        f.input.details.time = "14:02:44";
        auto state = ld::test::intChannel("Engine State", L::kTypeInt3, 2, 20, {0, 1, 2, 2, 2, 3, 2, 0});
        state.enumRef = std::uint16_t{3};
        state.dpsByte = 0;
        f.input.channels.push_back(std::move(state));
        auto sats = ld::test::intChannel("GPS Sats Used", L::kTypeInt6, 4, 1, {7, 8, 9, 9});
        f.input.channels.push_back(std::move(sats));
        auto lat =
            ld::test::floatChannel("GPS Latitude", L::kTypeDouble, 8, 10, {-33.4471, -33.4472, -33.4473});
        lat.unit = "deg";
        lat.dpsByte = 7;
        f.input.channels.push_back(std::move(lat));
        auto oil = ld::test::intChannel("Gbox Oil Temp", L::kTypeInt3, 2, 10, {238, 239, 240});
        oil.unit = "F";
        oil.shift = 32;
        oil.mul = 9;
        oil.scale = 5;
        oil.dec = 1;
        oil.dpsByte = 1;
        f.input.channels.push_back(std::move(oil));
        f.input.enumTables = {engineStateTable()};
        f.input.events = {{0, 0.0}, {3, 0.0}, {4, 799'999.75}};
        out.push_back(std::move(f));
    }
    {
        GoldenFamily f{"c-series", {}, fixedIdentity(ld::WriteDialect::CSeries), std::nullopt};
        auto oil = ld::test::intChannel("Gbox Oil Temp", L::kTypeInt3, 2, 2, {238, 240});
        oil.shortName = "F"; // a unit in the short-name field, no unit at 0x48: unit rule (c)
        oil.shift = 32;
        oil.mul = 9;
        oil.scale = 5;
        oil.dec = 1;
        f.input.channels.push_back(std::move(oil));
        auto rpm = ld::test::intChannel("Engine Speed", L::kTypeInt3, 2, 100, {800, 810, 820});
        rpm.shortName = "RPM";
        rpm.unit = "rpm"; // unit rule (b): short name and unit
        f.input.channels.push_back(std::move(rpm));
        auto th = ld::test::intChannel("Throttle Pos", L::kTypeInt3, 2, 50, {0, 10, 20});
        th.shortName = "TPS"; // does not resolve as a unit: stays the short name
        th.scale = 10;
        f.input.channels.push_back(std::move(th));
        out.push_back(std::move(f));
    }
    {
        GoldenFamily f{"header-only", {}, fixedIdentity(), std::nullopt};
        f.input.details.date = "10/10/2026";
        f.input.details.time = "09:15:00";
        out.push_back(std::move(f));
    }
    {
        GoldenFamily f{"with-ldx", {}, fixedIdentity(), richLdx()};
        f.input.details.date = "12/10/2026";
        f.input.details.time = "08:00:00";
        f.input.details.driver = "Test Driver";
        f.input.channels.push_back(
            ld::test::intChannel("Lap Number", L::kTypeInt5, 4, 1, ld::test::ramp(25, 0, 0)));
        auto v = ld::test::floatChannel("Ground Speed", L::kTypeFloat, 4, 10, std::vector<double>(250, 27.5));
        v.unit = "km/h";
        f.input.channels.push_back(std::move(v));
        out.push_back(std::move(f));
    }
    {
        GoldenFamily f{"sim-exporter", {}, fixedIdentity(ld::WriteDialect::SimExporter), std::nullopt};
        f.input.details.driver = "Sim Driver";
        f.input.details.venue = "Sim Track";
        auto rpm = ld::test::intChannel("RPMS", L::kTypeInt3, 2, 20, {1000, 1100, 1200, 1300});
        rpm.unit = "rpm";
        f.input.channels.push_back(std::move(rpm));
        auto t = ld::test::floatChannel("SPEED", L::kTypeFloat, 4, 20, {0.0, 1.5, 3.0, 4.5});
        t.unit = "m/s";
        f.input.channels.push_back(std::move(t));
        out.push_back(std::move(f));
    }
    return out;
}

// Writes a family to <dir>/<name>.ld (and .ldx); returns the .ld path.
inline std::filesystem::path writeFamily(const std::filesystem::path& dir, const GoldenFamily& f) {
    const std::filesystem::path ld = dir / (f.name + ".ld");
    writeLd(ld, f.input, f.options);
    if (f.ldx) {
        writeText(dir / (f.name + ".ldx"), *f.ldx);
    }
    return ld;
}

} // namespace esses::formats::test
