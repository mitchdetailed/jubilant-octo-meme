// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Writer -> reader round trips for every synthetic family of 05 section 5 item 8 / AC-M0-03: each
// (dtype_a, dtype) pair, multi-rate, shift/mul variants, aux-chained records (long names, enumeration
// references, the 212-byte case), gap blocks, device events, details, truncated names, dialects, the header
// count mismatch and the header-only file. Expected values come from the generator's own input (never from
// the reader); the private-corpus test compares the reader with the independent oracle.
#include "ld/ld_codes.hpp"
#include "ld_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <random>

using namespace esses;
using namespace esses::formats;
using namespace esses::formats::ld;
using namespace esses::formats::ld::test;
namespace L = esses::formats::ld::layout;
using store::Dtype;
using store::Interp;
using store::SampleKind;

namespace {

struct PairCase {
    std::uint16_t code, width;
    SampleType type;
    std::string_view vendor;
    bool isFloat;
    bool guessed;
    bool unknown;
};

std::vector<std::int64_t> sampleInts(std::uint16_t width, bool isUnsigned, std::size_t n) {
    std::mt19937_64 rng(testSeed() + width);
    std::vector<std::int64_t> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto r = static_cast<std::int64_t>(rng());
        if (width == 8) {
            v[i] = r >> 12; // exact in double
        } else if (isUnsigned) {
            v[i] = static_cast<std::int64_t>(static_cast<std::uint64_t>(r) >> (64 - 8 * width));
        } else {
            v[i] = r >> (64 - 8 * width);
        }
    }
    // The extremes of the stored width.
    if (!isUnsigned && width < 8) {
        v[0] = -(std::int64_t{1} << (8 * width - 1));
        v[1] = (std::int64_t{1} << (8 * width - 1)) - 1;
    } else if (isUnsigned) {
        v[0] = 0;
        v[1] = (std::int64_t{1} << (8 * width)) - 1;
    }
    return v;
}

} // namespace

TEST_CASE("ld roundtrip: every (dtype_a, dtype) pair decodes to the generator's values",
          "[unit][ld_roundtrip]") {
    const std::vector<PairCase> cases = {
        {L::kTypeInt0, 2, SampleType::I16, "i16", false, false, false},
        {L::kTypeInt3, 2, SampleType::I16, "i16", false, false, false},
        {L::kTypeInt5, 2, SampleType::I16, "i16", false, false, false},
        {L::kTypeInt0, 4, SampleType::I32, "i32", false, false, false},
        {L::kTypeInt3, 4, SampleType::I32, "i32", false, false, false},
        {L::kTypeInt5, 4, SampleType::I32, "i32", false, false, false},
        {L::kTypeInt6, 4, SampleType::I32, "i32", false, false, false},
        {L::kTypeFloat, 2, SampleType::F16, "f16", true, false, false},
        {L::kTypeFloat, 4, SampleType::F32, "f32", true, false, false},
        {L::kTypeDouble, 8, SampleType::F64, "f64", true, false, false},
        {L::kTypeInt3, 1, SampleType::I8, "i8", false, true, false},
        {L::kTypeInt6, 2, SampleType::I16, "i16", false, true, false},
        {L::kTypeInt5, 8, SampleType::I64, "i64", false, true, false},
        {L::kTypeUnsigned4, 1, SampleType::U8, "u8", false, true, false},
        {L::kTypeUnsigned4, 2, SampleType::U16, "u16", false, true, false},
        {L::kTypeUnsigned4, 4, SampleType::U32, "u32", false, true, false},
        {L::kTypeFloat, 8, SampleType::F64, "f64", true, true, false},
        {L::kTypeDouble, 2, SampleType::I16, "i16", false, true, false},
        {L::kTypeDouble, 4, SampleType::I32, "i32", false, true, false},
        {9, 2, SampleType::I16, "i16", false, false, true},
        {42, 4, SampleType::I32, "i32", false, false, true},
        {9, 1, SampleType::I8, "i8", false, false, true},
    };
    constexpr std::size_t n = 257;
    LdWriteInput in;
    std::vector<std::vector<double>> expected;
    for (std::size_t k = 0; k < cases.size(); ++k) {
        const PairCase& pc = cases[k];
        const std::string name =
            "pair " + std::to_string(pc.code) + "/" + std::to_string(pc.width) + " #" + std::to_string(k);
        std::vector<double> exp(n);
        if (pc.isFloat) {
            std::vector<double> v(n);
            for (std::size_t i = 0; i < n; ++i) {
                // Values exactly representable in binary16, so every float width stores them unchanged.
                v[i] = (static_cast<double>(i) - 128.0) * 0.25;
            }
            v[3] = 65504.0;
            v[4] = -0.0;
            v[5] = std::ldexp(1.0, -24);
            exp = v;
            in.channels.push_back(floatChannel(name, pc.code, pc.width, 10, v));
        } else {
            auto v = sampleInts(pc.width, pc.code == L::kTypeUnsigned4, n);
            for (std::size_t i = 0; i < n; ++i) {
                // u32 is stored F64, exactly: 0xFFFFFFFF reads 4294967295 like u8/u16 read 255/65535.
                exp[i] = pc.type == SampleType::U32 ? static_cast<double>(static_cast<std::uint32_t>(v[i]))
                                                    : static_cast<double>(v[i]);
            }
            in.channels.push_back(intChannel(name, pc.code, pc.width, 10, std::move(v)));
        }
        expected.push_back(std::move(exp));
    }
    const auto f = roundTrip(in);
    REQUIRE(f->channels().size() == cases.size());
    for (std::size_t k = 0; k < cases.size(); ++k) {
        const PairCase& pc = cases[k];
        const LdChannel& c = f->channels()[k];
        INFO("channel " << c.loggedName);
        CHECK(c.sampleType == pc.type);
        CHECK(c.vendorDtype() == pc.vendor);
        CHECK(c.sampleCount == n);
        CHECK(((c.flags & channel_flag::kDtypeGuessed) != 0) == (pc.guessed || pc.unknown));
        if (pc.isFloat) {
            CHECK(c.kind == SampleKind::Real);
            CHECK(c.dtype == (pc.type == SampleType::F64 ? Dtype::F64 : Dtype::F32));
        } else if (pc.type == SampleType::I64 || pc.type == SampleType::U32) {
            CHECK(c.dtype == Dtype::F64); // neither fits I32 (05 section 3.5)
            CHECK(c.kind == (pc.code == L::kTypeUnsigned4 ? SampleKind::Bitfield : SampleKind::Integer));
        } else {
            CHECK(c.dtype == Dtype::I32);
            CHECK(c.kind == (pc.code == L::kTypeUnsigned4 ? SampleKind::Bitfield : SampleKind::Integer));
        }
        const auto got = decodeAll(*f, static_cast<std::uint32_t>(k));
        for (std::size_t i = 0; i < n; ++i) {
            if (got[i] != expected[k][i] || std::signbit(got[i]) != std::signbit(expected[k][i])) {
                FAIL("sample " << i << ": " << got[i] << " != " << expected[k][i]);
            }
        }
    }
    CHECK(hasCode(*f, code::kDtypeGuessed));
    CHECK(hasCode(*f, code::kUnknownDtype));
    CHECK(f->dialectString() == "pc-layout");
    CHECK(f->stringEncoding() == "ascii");
}

TEST_CASE("ld roundtrip: multi-rate channels keep rate, length and the layer duration",
          "[unit][ld_roundtrip]") {
    LdWriteInput in;
    const std::vector<std::pair<int, std::int64_t>> rates = {
        {1, 61}, {5, 301}, {10, 600}, {50, 3000}, {100, 6001}, {1000, 60000}, {20000, 1200007}};
    for (const auto& [hz, count] : rates) {
        in.channels.push_back(intChannel("rate " + std::to_string(hz), L::kTypeInt5, 4,
                                         static_cast<std::uint16_t>(hz), ramp(count, 0, 1)));
    }
    LdWriteOptions wopt;
    wopt.dataAlignment = 1;
    const auto f = roundTrip(in, wopt);
    REQUIRE(f->channels().size() == rates.size());
    Micros longest = 0;
    for (std::size_t k = 0; k < rates.size(); ++k) {
        const LdChannel& c = f->channels()[k];
        CHECK(c.enc.rateHz == rates[k].first);
        CHECK(c.rate() == Rate::fromHz(static_cast<std::uint32_t>(rates[k].first)));
        CHECK(c.sampleCount == static_cast<std::uint64_t>(rates[k].second));
        longest = std::max(
            longest, Rate::fromHz(static_cast<std::uint32_t>(rates[k].first)).timeAt(rates[k].second, 0));
        const auto tail = f->decodeAsDouble(static_cast<std::uint32_t>(k), c.sampleCount - 3, 3);
        REQUIRE(tail.ok());
        CHECK((*tail)[2] == static_cast<double>(rates[k].second - 1));
    }
    CHECK(f->durationUs() == longest);
    // Contiguous data (alignment 1): the header data pointer names the first array.
    CHECK(f->header().dataPtr == f->channels().front().enc.dataOffset);
    CHECK_FALSE(hasCode(*f, code::kDataPtrMismatch));
}

TEST_CASE("ld roundtrip: shift/mul variants in both orders, zero factors, F64 for dec >= 5",
          "[unit][ld_roundtrip]") {
    LdWriteInput in;
    auto temp = intChannel("Gbox Oil Temp", L::kTypeInt3, 2, 2, {250, 238, 770, 0, -400});
    temp.shift = 32;
    temp.mul = 9;
    temp.scale = 5;
    temp.dec = 1;
    in.channels.push_back(temp);
    auto fixed = intChannel("GPS Latitude", L::kTypeInt5, 4, 10, {-338765432, 1515151515, 0});
    fixed.scale = 1;
    fixed.dec = 7;
    in.channels.push_back(fixed);
    auto zeroScale = intChannel("zero scale", L::kTypeInt3, 2, 10, {7, -7});
    zeroScale.scale = 0;
    zeroScale.mul = 3;
    in.channels.push_back(zeroScale);
    auto zeroMul = intChannel("zero mul", L::kTypeInt3, 2, 10, {7, -7});
    zeroMul.mul = 0;
    zeroMul.dec = 1;
    in.channels.push_back(zeroMul);
    auto negDec = intChannel("neg dec", L::kTypeInt3, 2, 10, {3, -4});
    negDec.dec = -2;
    in.channels.push_back(negDec);
    auto scaledFloat = floatChannel("scaled float", L::kTypeFloat, 4, 10, {1.5, -2.0});
    scaledFloat.mul = 2;
    scaledFloat.shift = 1;
    in.channels.push_back(scaledFloat);

    SECTION("afterMul (default)") {
        const auto f = roundTrip(in);
        const auto t = decodeAll(*f, 0);
        // Exact rationals rounded once, computed here from the textbook formula raw/scale*10^-dec*mul+shift.
        CHECK(static_cast<float>(t[0]) == 77.0f);
        CHECK(static_cast<float>(t[1]) == static_cast<float>(238.0 * 9.0 / 50.0 + 32.0));
        CHECK(static_cast<float>(t[2]) == static_cast<float>(770.0 * 9.0 / 50.0 + 32.0));
        CHECK(static_cast<float>(t[3]) == 32.0f);
        CHECK(static_cast<float>(t[4]) == static_cast<float>(-400.0 * 9.0 / 50.0 + 32.0));
        CHECK(f->channels()[0].dtype == Dtype::F32);
        CHECK(f->channels()[0].kind == SampleKind::Real);
        CHECK(codeDetail(*f, code::kShiftMulOrder) == "afterMul");
        CHECK(hasCode(*f, code::kShiftNonzero));

        const auto g = decodeAll(*f, 1);
        CHECK(f->channels()[1].dtype == Dtype::F64);
        CHECK(g[0] == -338765432.0 / 1e7);
        CHECK(g[1] == 1515151515.0 / 1e7);

        CHECK(decodeAll(*f, 2) == std::vector<double>{21.0, -21.0});
        CHECK(hasCode(*f, code::kZeroScale));
        // The stored f32 values, widened exactly.
        CHECK(decodeAll(*f, 3) == std::vector<double>{static_cast<double>(static_cast<float>(0.7)),
                                                      static_cast<double>(static_cast<float>(-0.7))});
        CHECK(hasCode(*f, code::kZeroMul));
        CHECK(decodeAll(*f, 4) == std::vector<double>{300.0, -400.0});
        CHECK(decodeAll(*f, 5) == std::vector<double>{4.0, -3.0});
        CHECK(hasCode(*f, code::kFloatScalingNonidentity));
    }
    SECTION("beforeMul (dialect option)") {
        LdOptions ropt;
        ropt.shiftOrder = ShiftOrder::BeforeMul;
        const auto f = roundTrip(in, {}, ropt);
        const auto t = decodeAll(*f, 0);
        CHECK(static_cast<float>(t[0]) == 333.0f);
        CHECK(static_cast<float>(t[3]) == 288.0f);
        CHECK(codeDetail(*f, code::kShiftMulOrder) == "beforeMul");
        CHECK(decodeAll(*f, 5) == std::vector<double>{5.0, -2.0});
    }
}

TEST_CASE("ld roundtrip: aux blocks carry long names and enumeration references", "[unit][ld_roundtrip]") {
    LdWriteInput in;
    LdEnumTable gear;
    gear.index = 7;
    gear.entries = {{-1, "Reverse", true, 2},
                    {0, "Neutral", false, 0},
                    {1, "First", true, 0},
                    {2, "Second gear with a long label", false, 0}};
    LdEnumTable state;
    state.index = 3;
    state.entries = {{0, "Off", true, 0}, {1, "On", true, 1}, {2, "Fault", true, 3}};
    in.enumTables = {gear, state};

    auto c0 = intChannel("Gear", L::kTypeInt3, 2, 10, {-1, 0, 1, 2, 2, 1});
    c0.enumRef = std::uint16_t{7}; // enumeration reference only: the 212-byte record + aux case
    in.channels.push_back(c0);
    auto c1 = intChannel("Engine Coolant Temperature Sensor Voltage Raw", L::kTypeInt3, 2, 10, {1, 2, 3});
    in.channels.push_back(c1); // long name only
    auto c2 = intChannel("Fuel_Pump_State_With_A_Very_Long_Name_Indeed", L::kTypeInt0, 2, 10, {0, 1, 2, 1});
    c2.enumRef = std::uint16_t{3};
    c2.longNamePadByte = true; // converter dialect: one pad byte after the long name
    in.channels.push_back(c2);
    auto c3 = intChannel("Missing table", L::kTypeInt3, 2, 10, {0, 1});
    c3.enumRef = std::uint16_t{99};
    in.channels.push_back(c3);
    auto c4 = intChannel("Gear 2", L::kTypeInt3, 2, 10, {0});
    c4.enumRef = std::uint16_t{7};
    c4.longName = "Gear (secondary)"; // explicit long name shorter than 32 bytes
    c4.longNamePadByte = true;        // last argument: no following type to detect the pad by
    in.channels.push_back(c4);

    LdWriteOptions wopt;
    wopt.dataAlignment = 1;
    const auto f = roundTrip(in, wopt);
    REQUIRE(f->channels().size() == 5);
    REQUIRE(f->enumTables().size() == 2);
    CHECK(f->enumTables()[0].index == 7);
    CHECK(f->enumTables()[0].entries == gear.entries);
    CHECK(f->enumTables()[1].entries == state.entries);
    REQUIRE(f->findEnumTable(3) != nullptr);
    CHECK(f->findEnumTable(3)->entries[2].severity == 3);

    const LdChannel& g = f->channels()[0];
    CHECK(g.enumRef == std::optional<std::uint16_t>(std::uint16_t{7}));
    CHECK(g.enumResolved);
    CHECK(g.kind == SampleKind::Enum);
    CHECK(g.interp == Interp::Previous);
    CHECK(g.dtype == Dtype::I32);
    // 124-byte record + aux block holding only the enumeration reference = the 212-byte record distance.
    CHECK(f->channels()[1].enc.recordOffset - g.enc.recordOffset == 212);

    const LdChannel& ln = f->channels()[1];
    CHECK(ln.loggedName == "Engine Coolant Temperature Sensor Voltage Raw");
    CHECK(ln.name32 == "Engine Coolant Temperature Senso");
    CHECK(ln.longName == ln.loggedName);
    CHECK_FALSE(ln.enumRef.has_value());

    const LdChannel& both = f->channels()[2];
    CHECK(both.loggedName == "Fuel_Pump_State_With_A_Very_Long_Name_Indeed");
    CHECK(both.enumResolved);
    CHECK_FALSE(both.longNamePadByte); // the pad follows the last argument: nothing to skip
    CHECK(decodeAll(*f, 2) == std::vector<double>{0, 1, 2, 1});

    const LdChannel& missing = f->channels()[3];
    CHECK_FALSE(missing.enumResolved);
    CHECK(missing.kind == SampleKind::Integer);
    CHECK(hasCode(*f, code::kEnumRefMissing));
    CHECK(codeDetail(*f, code::kEnumRefMissing) == "99");

    CHECK(f->channels()[4].loggedName == "Gear (secondary)");
    CHECK(f->channels()[4].name32 == "Gear 2");
}

TEST_CASE("ld roundtrip: a long name followed by another argument skips the converter pad byte",
          "[unit][ld_roundtrip]") {
    // Hand-built aux block (type 4 then type 5, one pad byte between) to exercise the reader's two forms.
    LdWriteInput in;
    LdEnumTable t;
    t.index = 1;
    t.entries = {{0, "A", false, 0}};
    in.enumTables = {t};
    auto c = intChannel("Short", L::kTypeInt3, 2, 1, {0, 0});
    c.longName = "A long name that is padded for room";
    in.channels.push_back(c);
    auto bytes = LdWriter::build(in);
    REQUIRE(bytes.ok());
    auto f0 = parseBytes(*bytes);
    const std::uint64_t aux = f0->channels()[0].enc.auxPtr;
    REQUIRE(aux != 0);
    // Rewrite the argument list: argc 2, [4: long name][pad 0][5: enum ref 1].
    std::vector<std::byte>& b = *bytes;
    const std::uint64_t content = aux + L::kAuxContentOffset;
    const auto put32 = [&](std::uint64_t o, std::uint32_t v) {
        for (int k = 0; k < 4; ++k) {
            b[o + k] = static_cast<std::byte>((v >> (8 * k)) & 0xFF);
        }
    };
    const auto put16 = [&](std::uint64_t o, std::uint16_t v) {
        b[o] = static_cast<std::byte>(v & 0xFF);
        b[o + 1] = static_cast<std::byte>(v >> 8);
    };
    const std::string ln = "Padded long name";
    put32(content + 4, 2);
    std::uint64_t o = content + 8;
    put32(o, 4);
    put16(o + 4, 0);
    put16(o + 6, static_cast<std::uint16_t>(ln.size() + 1));
    for (std::size_t k = 0; k < ln.size(); ++k) {
        b[o + 8 + k] = static_cast<std::byte>(ln[k]);
    }
    b[o + 8 + ln.size()] = std::byte{0};
    o += 8 + ln.size() + 1;
    b[o] = std::byte{0}; // the pad byte
    ++o;
    put32(o, 5);
    put16(o + 4, 0);
    put16(o + 6, 2);
    put16(o + 8, 1);
    REQUIRE(o + 10 <= f0->channels()[0].enc.dataOffset); // still inside the block area written before
    const auto f = parseBytes(std::move(b));
    const LdChannel& c0 = f->channels()[0];
    CHECK(c0.loggedName == ln);
    CHECK(c0.longNamePadByte);
    CHECK(c0.enumRef == std::optional<std::uint16_t>(std::uint16_t{1}));
    CHECK(c0.enumResolved);
}

TEST_CASE("ld roundtrip: gap blocks become half-open validity intervals", "[unit][ld_roundtrip]") {
    LdWriteInput in;
    auto a = intChannel("Engine Speed", L::kTypeInt5, 4, 100, ramp(1000, 5000, 3));
    a.invalid = {{0, 51}, {400, 410}, {990, 1000}};
    in.channels.push_back(a);
    auto fl = floatChannel("Lambda", L::kTypeFloat, 4, 50, std::vector<double>(300, 0.95));
    fl.invalid = {{100, 300}};
    in.channels.push_back(fl);
    auto never = intChannel("Throttle.Aim", L::kTypeInt3, 2, 10, std::vector<std::int64_t>(77, 0));
    never.invalid = {{0, 77}};
    in.channels.push_back(never);
    const auto f = roundTrip(in);

    const LdChannel& c = f->channels()[0];
    CHECK(c.invalid == std::vector<LdInterval>{{0, 51}, {400, 410}, {990, 1000}});
    CHECK_FALSE(f->validAt(0, 0));
    CHECK_FALSE(f->validAt(0, 50));
    CHECK(f->validAt(0, 51));
    CHECK(f->validAt(0, 399));
    CHECK_FALSE(f->validAt(0, 409));
    CHECK(f->validAt(0, 410));
    CHECK_FALSE(f->validAt(0, 999));
    CHECK(f->validity(0, 51, 349) == LdFile::RangeValidity::AllValid);
    CHECK(f->validity(0, 0, 51) == LdFile::RangeValidity::AllInvalid);
    CHECK(f->validity(0, 40, 20) == LdFile::RangeValidity::Mixed);
    CHECK(f->validity(1, 100, 200) == LdFile::RangeValidity::AllInvalid);
    CHECK(f->validity(2, 0, 77) == LdFile::RangeValidity::AllInvalid);

    // Bitmap: bit (i - first) cleared for invalid samples, across a word boundary.
    std::vector<std::uint64_t> words(2, ~std::uint64_t{0});
    f->clearInvalidBits(0, 380, 100, words);
    for (std::uint64_t i = 380; i < 480; ++i) {
        const std::uint64_t k = i - 380;
        const bool valid = ((words[k >> 6] >> (k & 63)) & 1u) != 0;
        CHECK(valid == !(i >= 400 && i < 410));
    }
    // NaN for float channels.
    std::vector<float> v(300, 1.0f);
    f->writeInvalidNaN(1, 0, 300, Dtype::F32, std::as_writable_bytes(std::span<float>(v)));
    CHECK(v[99] == 1.0f);
    CHECK(std::isnan(v[100]));
    CHECK(std::isnan(v[299]));
    // Stale stored values stay readable through the raw decode.
    CHECK(decodeAll(*f, 0)[10] == 5030.0);
    CHECK_FALSE(hasCode(*f, code::kGapUnpaired));
    CHECK_FALSE(hasCode(*f, code::kGapRateMismatch));
}

TEST_CASE("ld roundtrip: device events are ordered by time with ordinals and labels",
          "[unit][ld_roundtrip]") {
    LdWriteInput in;
    in.channels.push_back(intChannel("x", L::kTypeInt3, 2, 1, {0}));
    in.events = {{7, 2'000'000.4}, {6, 1'000'000.6}, {3, 0.0}, {5, 1'000'001.0}, {52, 1'000'000.9}};
    const auto f = roundTrip(in);
    const auto& ev = f->deviceEvents();
    REQUIRE(ev.size() == 5);
    CHECK(ev[0].code == 3);
    CHECK(ev[0].tUs == 0);
    CHECK(ev[1].code == 6);
    CHECK(ev[1].tUs == 1'000'001);
    CHECK(ev[1].ordinal == 0);
    CHECK(ev[2].code == 5); // same rounded time, stored later
    CHECK(ev[2].ordinal == 1);
    CHECK(ev[3].code == 52);
    CHECK(ev[3].ordinal == 2);
    CHECK(ev[4].code == 7);
    CHECK(ev[4].timeUs == 2'000'000.4);
    CHECK(ev[4].tUs == 2'000'000);
    CHECK(deviceEventLabel(3) == "Logging on");
    CHECK(deviceEventLabel(52) == "Event 52");
}

TEST_CASE("ld roundtrip: header and detail records", "[unit][ld_roundtrip]") {
    LdWriteInput in;
    LdWriteDetails& d = in.details;
    d.date = "16/07/2025";
    d.time = "19:19:55";
    d.driver = "A. Driver";
    d.vehicleId = "GT3-07";
    d.engineId = "V8 #2";
    d.venue = "Bathurst";
    d.session = "Practice 1";
    d.shortComment = "short";
    d.team = "Team \xC3\x89lan"; // E-acute: cp1252 on disk
    d.eventName = "Round 4";
    d.eventSession = "P1 (event copy)";
    d.longComment = std::string(1100, 'c'); // cut to 1023 bytes
    d.venueName = "Mount Panorama";
    d.venueLengthMm = 6'213'000;
    d.venueCategory = "Circuit";
    d.vehicleRecordId = "GT3-07";
    d.vehicleDesc = "Coupe";
    d.vehicleEngineId = "V8";
    d.weightKg = 1300;
    d.fuelTankX10 = 1200;
    d.vehicleType = "Car";
    d.driveType = "RWD";
    d.diffX1000 = 3545;
    d.gearsX1000 = {3000, 2100, 1600, 1300, 1100, 950, 0, 0, 0, 0};
    d.trackMm = 1600;
    d.wheelbaseMm = 2700;
    d.vehicleComment = "comment";
    d.vehicleNumber = "7";
    d.sky = "Overcast";
    d.airTemp = {"21.5", "C"};
    d.trackTemp = {"30", "C"};
    d.baroPressure = {"1013", "hPa"};
    d.relHumidity = {"55", "%"};
    d.windSpeed = {"12", "km/h"};
    d.windDirection = "NW";
    d.weatherComment = "dry";
    in.channels.push_back(intChannel("x", L::kTypeInt3, 2, 1, {0}));
    ImportLog notes;
    auto bytes = LdWriter::build(in, {}, &notes);
    REQUIRE(bytes.ok());
    CHECK(notes.contains(code::kTruncated));
    const auto f = parseBytes(std::move(*bytes));
    const LdHeader& h = f->header();
    CHECK(h.magic == L::kMagic);
    CHECK(h.ticks == 1'000'000u);
    CHECK(h.date == "16/07/2025");
    CHECK(h.time == "19:19:55");
    REQUIRE(h.dateTime.has_value());
    CHECK(*h.dateTime == LocalDateTime{2025, 7, 16, 19, 19, 55});
    CHECK(h.driver == d.driver);
    CHECK(h.vehicleId == d.vehicleId);
    CHECK(h.engineId == d.engineId);
    CHECK(h.venue == d.venue);
    CHECK(h.session == d.session);
    CHECK(h.shortComment == d.shortComment);
    CHECK(h.team == d.team);
    CHECK_FALSE(h.proLoggingNonZero);
    CHECK(f->rawHeader().size() == L::kHeaderSize);
    REQUIRE(f->eventRecord().has_value());
    CHECK(f->eventRecord()->offset == L::kHeaderSize);
    CHECK(f->eventRecord()->name == "Round 4");
    CHECK(f->eventRecord()->session == "P1 (event copy)");
    CHECK(f->eventRecord()->longComment == std::string(1023, 'c'));
    REQUIRE(f->venueRecord().has_value());
    CHECK(f->venueRecord()->offset == 0x1336);
    CHECK(f->venueRecord()->name == "Mount Panorama");
    CHECK(f->venueRecord()->lengthMm == 6'213'000u);
    CHECK(f->venueRecord()->category == "Circuit");
    REQUIRE(f->vehicleRecord().has_value());
    const LdVehicleRecord& v = *f->vehicleRecord();
    CHECK(v.offset == 0x1F54);
    CHECK(v.desc == "Coupe");
    CHECK(v.weightKg == 1300);
    CHECK(v.fuelTankX10 == 1200);
    CHECK(v.driveType == "RWD");
    CHECK(v.diffX1000 == 3545);
    CHECK(v.gearsX1000 == d.gearsX1000);
    CHECK(v.trackMm == 1600);
    CHECK(v.wheelbaseMm == 2700);
    CHECK(v.number == "7");
    REQUIRE(f->weatherRecord().has_value());
    const LdWeatherRecord& w = *f->weatherRecord();
    CHECK(w.offset == 0x2C48);
    CHECK(w.airTemp == LdWeatherValue{"21.5", "C"});
    CHECK(w.windSpeed == LdWeatherValue{"12", "km/h"});
    CHECK(w.windDirection == "NW");
    CHECK(w.comment == "dry");
    CHECK(f->stringEncoding() == "cp1252");
    CHECK(codeDetail(*f, code::kStringEncoding) == "cp1252");
    // The first free byte after the four detail records is 0x3448 (port specification E-12).
    CHECK(f->channels()[0].enc.recordOffset == 0x3448);
}

TEST_CASE("ld roundtrip: unit placement, display range, dps and interpolation bytes",
          "[unit][ld_roundtrip]") {
    LdWriteInput in;
    auto a = intChannel("long unit", L::kTypeInt3, 2, 10, {1});
    a.unit = "kilopascal"; // no NUL in the first 8 bytes: one 20-byte unit (rule a)
    a.dpsByte = 2;
    a.interpByte = 3;
    a.displayRange = std::make_pair(10.0f, -10.0f); // stored in either order
    in.channels.push_back(a);
    auto b = intChannel("short unit", L::kTypeInt3, 2, 10, {1});
    b.unit = "rpm"; // rule (c): the unit sits in the short-name field
    b.dec = 2;
    b.dpsByte = 0; // dps from dec
    b.interpByte = 1;
    in.channels.push_back(b);
    auto c = intChannel("no unit", L::kTypeInt3, 2, 10, {1});
    c.interpByte = 2;
    in.channels.push_back(c);
    auto d = intChannel("closest", L::kTypeInt3, 2, 10, {1});
    d.interpByte = 0;
    in.channels.push_back(d);
    auto e = floatChannel("auto interp", L::kTypeFloat, 4, 10, {1.0});
    e.interpByte = 4;
    in.channels.push_back(e);
    const auto f = roundTrip(in);

    const LdChannel& ca = f->channels()[0];
    CHECK(ca.unit == "kilopascal");
    CHECK(ca.shortName.empty());
    CHECK((ca.flags & channel_flag::kUnitFromShortName) == 0);
    CHECK(ca.dps == 2);
    CHECK(ca.interp == Interp::Linear);
    REQUIRE(ca.displayRange.has_value());
    CHECK(ca.displayRange->first == -10.0f);
    CHECK(ca.displayRange->second == 10.0f);
    CHECK((ca.flags & channel_flag::kHasDisplayRange) != 0);

    const LdChannel& cb = f->channels()[1];
    CHECK(cb.unit == "rpm");
    CHECK((cb.flags & channel_flag::kUnitFromShortName) != 0);
    CHECK(cb.dps == 2);
    CHECK(cb.interp == Interp::Previous);
    CHECK_FALSE(cb.displayRange.has_value());
    CHECK(hasCode(*f, code::kUnitFromShortName));
    CHECK(hasCode(*f, code::kDpsFromDec));

    CHECK(f->channels()[2].unit.empty());
    CHECK(f->channels()[2].interp == Interp::Next);
    CHECK(f->channels()[3].interp == Interp::Closest);
    CHECK(f->channels()[4].interp == Interp::Linear); // kind default for Real
    CHECK(hasCode(*f, code::kInterpDefault));
    CHECK(f->channels()[0].vendorId == "20101");
    CHECK(f->channels()[4].vendorId == "20105");
}

TEST_CASE("ld roundtrip: dialect families", "[unit][ld_roundtrip]") {
    LdWriteInput in;
    auto a = intChannel("Device Up Time", L::kTypeInt5, 4, 10, ramp(20, 1000, 100));
    a.shortName = "Up Time";
    a.unit = "s";
    a.dec = 3;
    a.interpByte = 0;
    in.channels.push_back(a);
    auto b = intChannel("Counter 1", L::kTypeInt3, 2, 10, ramp(20));
    b.shortName = "Cnt1"; // C-series: a short name and no unit
    b.interpByte = 0;
    in.channels.push_back(b);

    SECTION("pc-layout (default)") {
        const auto f = roundTrip(in);
        CHECK(f->dialectString() == "pc-layout");
        CHECK(f->header().unknown054 == 0x0080);
        CHECK(f->header().channelCount2 == 2);
    }
    SECTION("c-series: short name at 0x40, unit at 0x48, interpolation byte 0 unset") {
        LdWriteOptions o;
        o.dialect = WriteDialect::CSeries;
        const auto f = roundTrip(in, o);
        CHECK(f->dialectString() == "c-series");
        CHECK(f->header().unknown040 == 0);
        CHECK(f->header().channelCount2 == 0);
        CHECK(f->header().eventPtr > 0x10000u);
        const LdChannel& c0 = f->channels()[0];
        CHECK(c0.shortName == "Up Time");
        CHECK(c0.unit == "s");
        CHECK(c0.interp == Interp::Linear); // scaled -> Real -> Linear (kind default)
        CHECK(c0.dps == 3);                 // dps byte 0, dec 3
        const LdChannel& c1 = f->channels()[1];
        CHECK(c1.shortName == "Cnt1");
        CHECK(c1.unit.empty());
        CHECK(c1.interp == Interp::Previous);
        CHECK(hasCode(*f, code::kUnitFromShortNameCandidate));
        CHECK(hasCode(*f, code::kInterpDefault));

        LdOptions resolving;
        resolving.unitResolves = [](std::string_view u) { return u == "Cnt1"; };
        const auto g = roundTrip(in, o, resolving);
        CHECK(g->channels()[1].unit == "Cnt1");
        CHECK((g->channels()[1].flags & channel_flag::kUnitFromShortName) != 0);
    }
    SECTION("m1: terminator record counted in the header, not a channel") {
        LdWriteOptions o;
        o.dialect = WriteDialect::M1;
        const auto f = roundTrip(in, o);
        CHECK(f->dialectString() == "m1,pc-layout");
        CHECK(f->channels().size() == 2);
        CHECK(f->recordsWalked() == 3);
        CHECK(f->header().channelCount == 3);
        CHECK(hasCode(*f, code::kTerminatorEntry));
        CHECK_FALSE(hasCode(*f, code::kCountMismatch));
    }
    SECTION("sim-exporter") {
        LdWriteOptions o;
        o.dialect = WriteDialect::SimExporter;
        const auto f = roundTrip(in, o);
        CHECK(f->dialectString() == "sim-exporter");
        CHECK(f->header().deviceType == "ADL");
        CHECK(f->channels()[1].unit.empty());
    }
    SECTION("converter-legacy: list at 0x1336 with 132-byte records; no recovery") {
        LdWriteOptions o;
        o.dialect = WriteDialect::ConverterLegacy;
        in.channels[0].name = "A channel name longer than thirty-two bytes";
        const auto f = roundTrip(in, o);
        CHECK(f->header().metaPtr == L::kLegacyListOffset);
        CHECK(f->dialectString() == "pc-layout,converter-legacy");
        CHECK(hasCode(*f, code::kLegacyLayout));
        CHECK(f->channels().size() == 2);
        CHECK(f->channels()[0].loggedName == "A channel name longer than thirt");
        CHECK_FALSE(f->venueRecord().has_value());
        CHECK(decodeAll(*f, 1) == decodeAll(*roundTrip(in), 1));
    }
}

TEST_CASE("ld roundtrip: truncated names, count mismatch, duplicates, header-only", "[unit][ld_roundtrip]") {
    SECTION("names that fill the 32-byte field") {
        LdWriteInput in;
        in.channels.push_back(intChannel("0123456789abcdef0123456789abcdef", L::kTypeInt3, 2, 1, {1}));
        LdWriteChannel c = intChannel("Fuel Film Cylinder 1 Secondary F", L::kTypeInt3, 2, 1, {1});
        c.unit = "ul";
        in.channels.push_back(c);
        const auto f = roundTrip(in);
        CHECK(f->channels()[0].loggedName == "0123456789abcdef0123456789abcdef");
        CHECK(f->channels()[0].longName.empty());
        CHECK(f->channels()[1].loggedName == "Fuel Film Cylinder 1 Secondary F"); // never "...Secondary Ful"
        CHECK(f->channels()[1].unit == "ul");
        CHECK(hasCode(*f, code::kNameUnterminated));
    }
    SECTION("header count differs from the walk") {
        LdWriteInput in;
        in.channels.push_back(intChannel("a", L::kTypeInt3, 2, 1, {1}));
        in.channels.push_back(intChannel("b", L::kTypeInt3, 2, 1, {1}));
        LdWriteOptions o;
        o.headerChannelCount = std::uint16_t{44};
        const auto f = roundTrip(in, o);
        CHECK(f->channels().size() == 2);
        CHECK(hasCode(*f, code::kCountMismatch));
        CHECK(codeDetail(*f, code::kCountMismatch) == "2");
    }
    SECTION("duplicate names get a suffix") {
        LdWriteInput in;
        in.channels.push_back(intChannel("Speed", L::kTypeInt3, 2, 1, {1}));
        in.channels.push_back(intChannel("Speed (2)", L::kTypeInt3, 2, 1, {2}));
        in.channels.push_back(intChannel("Speed", L::kTypeInt3, 2, 1, {3}));
        const auto f = roundTrip(in);
        CHECK(f->channels()[0].loggedName == "Speed");
        CHECK(f->channels()[1].loggedName == "Speed (2)");
        CHECK(f->channels()[2].loggedName == "Speed (3)");
        CHECK(f->channels()[2].name32 == "Speed");
        CHECK(hasCode(*f, code::kDuplicateName));
    }
    SECTION("header-only file: a valid empty source") {
        LdWriteInput in;
        in.details.venue = "Nowhere";
        auto bytes = LdWriter::build(in);
        REQUIRE(bytes.ok());
        const auto size = bytes->size();
        const auto f = parseBytes(std::move(*bytes));
        CHECK(f->headerOnly());
        CHECK(f->channels().empty());
        CHECK(f->header().metaPtr == size);
        CHECK(f->header().dataPtr == size);
        CHECK(hasCode(*f, code::kHeaderOnly));
        CHECK_FALSE(f->log().hasErrors());
        CHECK(f->header().venue == "Nowhere");
        CHECK(f->durationUs() == 0);
    }
}

TEST_CASE("ld roundtrip: files on disk are read through the mapping", "[unit][ld_roundtrip]") {
    TempDir dir("mapped");
    LdWriteInput in;
    auto c = intChannel("Wheel Speed", L::kTypeInt5, 4, 100, ramp(100000, -50000, 1));
    c.scale = 10000;
    c.dec = 2;
    c.invalid = {{10, 20}};
    in.channels.push_back(c);
    const auto path = dir.path() / "synthetic.ld";
    auto written = LdWriter::write(path, in);
    REQUIRE(written.ok());
    CHECK(*written == std::filesystem::file_size(path));
    auto f = LdFile::open(path);
    REQUIRE(f.ok());
    REQUIRE((*f)->mapped() != nullptr);
    const auto v = decodeAll(**f, 0);
    CHECK(v.size() == 100000);
    CHECK(static_cast<float>(v[0]) == static_cast<float>(-50000.0 / 1e6));
    CHECK(static_cast<float>(v[99999]) == static_cast<float>(49999.0 / 1e6));
    const auto raw = (*f)->rawValues(0, 99998, 2);
    REQUIRE(raw.ok());
    CHECK(*raw == std::vector<double>{49998.0, 49999.0});
    CHECK_FALSE((*f)->decode(0, 99999, 2, Dtype::F32, {}).ok()); // range beyond the channel
    std::vector<std::byte> small(8);
    CHECK_FALSE((*f)->decode(0, 0, 2, Dtype::I32, small).ok()); // scaled channel cannot be I32
    CHECK_FALSE((*f)->decode(5, 0, 1, Dtype::F32, small).ok()); // no such channel
    CHECK((*f)->path() == path);
}

TEST_CASE("ld roundtrip: read -> write -> read keeps samples, validity and unknown header bytes",
          "[unit][ld_roundtrip]") {
    LdWriteInput in;
    in.details.driver = "Driver";
    in.details.venueName = "Venue";
    auto a = intChannel("Engine Speed", L::kTypeInt5, 4, 100, ramp(5000, 0, 7));
    a.scale = 10;
    a.dec = 1;
    a.invalid = {{0, 51}, {4000, 4100}};
    in.channels.push_back(a);
    auto g = intChannel("Gear", L::kTypeInt3, 2, 10, {0, 1, 2, 3});
    g.enumRef = std::uint16_t{2};
    in.channels.push_back(g);
    in.channels.push_back(floatChannel("A float channel with a name over thirty-two bytes", L::kTypeFloat, 4,
                                       20, {1.0, std::numeric_limits<double>::quiet_NaN(), -3.5}));
    LdEnumTable t;
    t.index = 2;
    t.entries = {{0, "N", false, 0}, {1, "1", true, 1}};
    in.enumTables = {t};
    in.events = {{0, 0.0}, {3, 5.5}};
    LdWriteOptions o;
    o.dialect = WriteDialect::M1;
    auto first = LdWriter::build(in, o);
    REQUIRE(first.ok());
    // Unknown header bytes and a non-zero pro-logging field in the source.
    std::vector<std::byte> src = *first;
    src[0x1DE] = std::byte{'n'};
    src[0x1DF] = std::byte{'o'};
    src[L::kHdrUnknown687] = std::byte{0x15};
    src[L::kHdrProLogging] = std::byte{0x5A};
    const auto f1 = parseBytes(src);
    CHECK(f1->header().proLoggingNonZero);
    CHECK(f1->dialectString() == "m1,pc-layout,pro-logging");
    CHECK(hasCode(*f1, code::kProLoggingNonzero));
    CHECK(f1->rawHeader()[L::kHdrProLogging] == std::byte{0}); // held as 0 (D-143)

    auto in2 = LdWriter::inputFromFile(*f1);
    REQUIRE(in2.ok());
    auto out = LdWriter::build(*in2, LdWriter::optionsFromFile(*f1));
    REQUIRE(out.ok());
    const auto f2 = parseBytes(*out);
    CHECK_FALSE(f2->header().proLoggingNonZero); // always written as 0
    CHECK((*out)[L::kHdrProLogging] == std::byte{0});
    CHECK((*out)[0x1DE] == std::byte{'n'});
    CHECK((*out)[L::kHdrUnknown687] == std::byte{0x15});
    REQUIRE(f2->channels().size() == f1->channels().size());
    for (std::uint32_t k = 0; k < f1->channels().size(); ++k) {
        const LdChannel& c1 = f1->channels()[k];
        const LdChannel& c2 = f2->channels()[k];
        CHECK(c2.loggedName == c1.loggedName);
        CHECK(c2.invalid == c1.invalid);
        CHECK(c2.enumRef == c1.enumRef);
        CHECK(c2.dtype == c1.dtype);
        const auto b1 = f1->bytes().subspan(c1.enc.dataOffset, c1.sampleCount * c1.enc.bytesPerSample);
        const auto b2 = f2->bytes().subspan(c2.enc.dataOffset, c2.sampleCount * c2.enc.bytesPerSample);
        CHECK(std::equal(b1.begin(), b1.end(), b2.begin(), b2.end()));
        // Record bytes outside the pointers are verbatim.
        CHECK(std::equal(c1.enc.raw.begin() + L::kRecCounter, c1.enc.raw.begin() + L::kRecGapPtr,
                         c2.enc.raw.begin() + L::kRecCounter));
    }
    CHECK(f2->enumTables()[0].entries == t.entries);
    CHECK(f2->deviceEvents().size() == 2);
    CHECK(f2->header().driver == "Driver");
    CHECK(f2->venueRecord()->name == "Venue");
    CHECK(f2->recordsWalked() == f1->recordsWalked());
}
