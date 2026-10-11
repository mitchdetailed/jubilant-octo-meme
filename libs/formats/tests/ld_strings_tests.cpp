// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Field strings (UTF-8 else cp1252), half-precision conversion, the scaling plan and its inverse.
#include "ld/ld_decode.hpp"
#include "ld/ld_strings.hpp"
#include "ld/ld_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace esses::formats::ld;

namespace {

std::vector<std::byte> bytesOf(std::initializer_list<unsigned> v) {
    std::vector<std::byte> out;
    for (unsigned b : v) {
        out.push_back(static_cast<std::byte>(b));
    }
    return out;
}

std::vector<std::byte> field(std::string_view s, std::size_t width) {
    std::vector<std::byte> out(width);
    for (std::size_t i = 0; i < s.size() && i < width; ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(s[i]));
    }
    return out;
}

std::string bytesToString(const std::vector<std::byte>& b) {
    std::string s;
    for (std::byte x : b) {
        s.push_back(static_cast<char>(x));
    }
    return s;
}

} // namespace

TEST_CASE("ld strings: fields cut at the first NUL, right-trimmed, bounded by width", "[unit][ld_strings]") {
    const FieldText a = decodeField(field("Engine RPM  ", 32));
    CHECK(a.text == "Engine RPM");
    CHECK(a.encoding == TextEncoding::Ascii);
    CHECK(a.terminated);
    CHECK_FALSE(a.trailingBytes);

    const FieldText full = decodeField(field("0123456789ABCDEF0123456789ABCDEF", 32));
    CHECK(full.text == "0123456789ABCDEF0123456789ABCDEF");
    CHECK_FALSE(full.terminated);

    auto stale = field("rpm", 8);
    stale[5] = std::byte{'x'};
    const FieldText s = decodeField(stale);
    CHECK(s.text == "rpm");
    CHECK(s.trailingBytes);
}

TEST_CASE("ld strings: valid UTF-8 decodes as UTF-8, anything else as cp1252", "[unit][ld_strings]") {
    const FieldText u = decodeField(bytesOf({0x43, 0x61, 0x66, 0xC3, 0xA9, 0}));
    CHECK(u.text == "Caf\xC3\xA9");
    CHECK(u.encoding == TextEncoding::Utf8);

    // E9 80 81 alone would be valid UTF-8 (U+9001): the space keeps these bytes cp1252.
    const FieldText c = decodeField(bytesOf({0x43, 0x61, 0x66, 0xE9, 0x20, 0x80, 0x81, 0}));
    CHECK(c.text == "Caf\xC3\xA9 \xE2\x82\xAC\xC2\x81"); // e-acute, euro sign, U+0081 for the undefined byte
    CHECK(c.encoding == TextEncoding::Cp1252);
    CHECK(textEncodingName(TextEncoding::Cp1252) == "cp1252");
    CHECK(cp1252ToUtf8(bytesOf({0x9F})) == "\xC5\xB8"); // Y with diaeresis
}

TEST_CASE("ld strings: encoding picks cp1252 when it reads back unambiguously", "[unit][ld_strings]") {
    const EncodedText ascii = encodeText("Throttle", 32);
    CHECK(bytesToString(ascii.bytes) == "Throttle");
    CHECK_FALSE(ascii.utf8);

    const EncodedText e = encodeText("Caf\xC3\xA9 \xE2\x82\xAC", 32); // "Cafe-acute euro"
    CHECK(e.bytes == bytesOf({0x43, 0x61, 0x66, 0xE9, 0x20, 0x80}));
    CHECK_FALSE(e.utf8);

    const EncodedText omega = encodeText("\xCE\xA9 ohm", 32); // not in cp1252
    CHECK(omega.utf8);
    CHECK(bytesToString(omega.bytes) == "\xCE\xA9 ohm");

    // "A-tilde copyright" in cp1252 is C3 A9, valid UTF-8 for e-acute: written as UTF-8 so it reads back.
    const EncodedText trap = encodeText("\xC3\x83\xC2\xA9", 32);
    CHECK(trap.utf8);
    CHECK(decodeField(trap.bytes).text == "\xC3\x83\xC2\xA9");

    for (const std::string s :
         {"Engine Speed", "Caf\xC3\xA9", "\xCE\xA9\xCE\xA9", "\xE2\x82\xAC 5", "\xC3\x83\xC2\xA9"}) {
        const EncodedText t = encodeText(s, 64);
        CHECK(decodeField(t.bytes).text == s);
    }
}

TEST_CASE("ld strings: truncation never splits a UTF-8 sequence", "[unit][ld_strings]") {
    const std::string s = "ab\xCE\xA9\xCE\xA9"; // a b omega omega: 6 bytes, UTF-8 only
    const EncodedText t = encodeText(s, 5);
    CHECK(t.truncated);
    CHECK(bytesToString(t.bytes) == "ab\xCE\xA9");
    const EncodedText c = encodeText("Caf\xC3\xA9s", 4); // cp1252: one byte per character
    CHECK(c.truncated);
    CHECK(c.bytes == bytesOf({0x43, 0x61, 0x66, 0xE9}));
}

TEST_CASE("ld strings: a full UTF-8 field cut inside a sequence decodes as UTF-8 without the partial tail",
          "[unit][ld_strings]") {
    // Port specification Q-W19/Q-W20: a writer that cuts names at 32 bytes can split the last character.
    const std::string name = "Temp\xC3\xA9rature huile moteur  \xC3\xA9\xC3\xA9\xC3\xA9"; // 33 bytes
    REQUIRE(name.size() == 33);
    const FieldText t = decodeField(field(name.substr(0, 32), 32)); // no NUL: the field is full
    CHECK_FALSE(t.terminated);
    CHECK(t.cutSequence);
    CHECK(t.encoding == TextEncoding::Utf8);
    CHECK(t.text == "Temp\xC3\xA9rature huile moteur  \xC3\xA9\xC3\xA9");

    // Two bytes of a three-byte sequence after ASCII: still evidently UTF-8.
    const FieldText euro = decodeField(bytesOf({0x41, 0x42, 0xE2, 0x82}));
    CHECK(euro.cutSequence);
    CHECK(euro.text == "AB");

    // A lone lead byte after pure ASCII is a cp1252 letter, not a cut sequence ("Caf" + e-acute).
    const FieldText cafe = decodeField(bytesOf({0x43, 0x61, 0x66, 0xE9}));
    CHECK_FALSE(cafe.cutSequence);
    CHECK(cafe.encoding == TextEncoding::Cp1252);
    CHECK(cafe.text == "Caf\xC3\xA9");

    // A NUL-terminated field is never shortened: the invalid byte is cp1252.
    const FieldText terminated = decodeField(bytesOf({0xC3, 0xA9, 0x41, 0xC3, 0}));
    CHECK_FALSE(terminated.cutSequence);
    CHECK(terminated.encoding == TextEncoding::Cp1252);
}

TEST_CASE("ld decode: binary16 conversions", "[unit][ld_decode]") {
    CHECK(halfToDouble(0x3C00) == 1.0);
    CHECK(halfToDouble(0xC000) == -2.0);
    CHECK(halfToDouble(0x7BFF) == 65504.0);
    CHECK(halfToDouble(0x0001) == std::ldexp(1.0, -24));
    CHECK(halfToDouble(0x0400) == std::ldexp(1.0, -14));
    CHECK(std::isinf(halfToDouble(0x7C00)));
    CHECK(std::isnan(halfToDouble(0x7E00)));
    CHECK(std::signbit(halfToDouble(0x8000)));

    CHECK(doubleToHalf(1.0) == 0x3C00);
    CHECK(doubleToHalf(-2.0) == 0xC000);
    CHECK(doubleToHalf(65504.0) == 0x7BFF);
    CHECK(doubleToHalf(65520.0) == 0x7C00); // ties to even -> infinity
    CHECK(doubleToHalf(std::ldexp(1.0, -24)) == 0x0001);
    CHECK(doubleToHalf(std::ldexp(1.0, -25)) == 0x0000);       // tie -> even (zero)
    CHECK(doubleToHalf(std::ldexp(3.0, -25)) == 0x0002);       // 1.5 subnormal units -> 2
    CHECK(doubleToHalf(1.0 + std::ldexp(1.0, -11)) == 0x3C00); // tie between 1 and 1+2^-10 -> even
    CHECK(doubleToHalf(1.0 + std::ldexp(3.0, -11)) == 0x3C02);
    CHECK(doubleToHalf(std::numeric_limits<double>::quiet_NaN()) == 0x7E00);
    for (std::uint32_t bits = 0; bits < 0x10000; ++bits) {
        const auto h = static_cast<std::uint16_t>(bits);
        const double v = halfToDouble(h);
        if (std::isnan(v)) {
            continue;
        }
        REQUIRE(doubleToHalf(v) == h);
    }
}

TEST_CASE("ld decode: scaling orders, zero factors and exactness", "[unit][ld_decode]") {
    // The S-1 self-test case: raw 250, shift 32, mul 9, scale 5, dec 1 -> afterMul 77, beforeMul 333.
    const ScalingPlan after = makeScalingPlan(SampleType::I16, 32, 9, 5, 1, ShiftOrder::AfterMul);
    const ScalingPlan before = makeScalingPlan(SampleType::I16, 32, 9, 5, 1, ShiftOrder::BeforeMul);
    CHECK_FALSE(after.identity);
    CHECK(after.exact);
    CHECK(applyScaling(after, 250.0) == 77.0);
    CHECK(applyScaling(before, 250.0) == 333.0);
    CHECK(shiftOrderName(ShiftOrder::AfterMul) == "afterMul");

    // M1-style fixed point: div 10000, exp 4 -> a single correctly rounded division.
    const ScalingPlan m1 = makeScalingPlan(SampleType::I32, 0, 1, 10000, 4, ShiftOrder::AfterMul);
    CHECK(m1.exact);
    CHECK(applyScaling(m1, 123456789.0) == 123456789.0 / 1.0e8);
    CHECK(applyScaling(m1, -7.0) == -7.0 / 1.0e8);

    // Zero scale and zero mul count as 1; a zero scale keeps mul (05 section 3.6).
    const ScalingPlan z = makeScalingPlan(SampleType::I16, 0, 3, 0, 0, ShiftOrder::AfterMul);
    CHECK(applyScaling(z, 7.0) == 21.0);
    CHECK(makeScalingPlan(SampleType::I16, 0, 0, 0, 0, ShiftOrder::AfterMul).identity);

    // Negative dec multiplies.
    CHECK(applyScaling(makeScalingPlan(SampleType::I16, 0, 1, 1, -2, ShiftOrder::AfterMul), 3.0) == 300.0);
    // Huge dec: finite, no exactness claim.
    const ScalingPlan huge = makeScalingPlan(SampleType::I32, 0, 1, 1, 400, ShiftOrder::AfterMul);
    CHECK_FALSE(huge.exact);
    CHECK(applyScaling(huge, 5.0) == 0.0);
    // Floats are never "exact" (non-integer raws).
    CHECK_FALSE(makeScalingPlan(SampleType::F32, 0, 2, 1, 0, ShiftOrder::AfterMul).exact);

    CHECK(narrowToFloat(1e300) == std::numeric_limits<float>::infinity());
    CHECK(narrowToFloat(-1e300) == -std::numeric_limits<float>::infinity());
    CHECK(narrowToFloat(static_cast<double>(std::numeric_limits<float>::max()) * (1.0 + 1e-9)) ==
          std::numeric_limits<float>::max());
}

TEST_CASE("ld writer: quantiseRaw inverts both scaling orders", "[unit][ld_decode]") {
    CHECK(quantiseRaw(77.0, 32, 9, 5, 1, ShiftOrder::AfterMul) == 250);
    CHECK(quantiseRaw(333.0, 32, 9, 5, 1, ShiftOrder::BeforeMul) == 250);
    CHECK(quantiseRaw(12.3456, 0, 1, 10000, 4, ShiftOrder::AfterMul) == 1234560000);
    CHECK(quantiseRaw(300.0, 0, 1, 1, -2, ShiftOrder::AfterMul) == 3);
    CHECK(quantiseRaw(std::numeric_limits<double>::quiet_NaN(), 0, 1, 1, 0, ShiftOrder::AfterMul) == 0);
}
