// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Dimension: products, quotients, integer and rational powers, unknown() propagation and formatting
// (02 section 2.1, D-085, 03 section 3.2 rules 4-5).
#include <esses/units/dimension.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace esses::units;

namespace {

constexpr Dimension kL = Dimension::basis(Dimension::Length);
constexpr Dimension kT = Dimension::basis(Dimension::Time);
constexpr Dimension kM = Dimension::basis(Dimension::Mass);

Dimension dim(std::int8_t l, std::int8_t m, std::int8_t t, std::int8_t i = 0, std::int8_t th = 0,
              std::int8_t n = 0, std::int8_t j = 0) {
    return Dimension{{l, m, t, i, th, n, j}};
}

} // namespace

TEST_CASE("dimension: none is dimensionless and known; unknown is neither", "[unit][dimension]") {
    STATIC_REQUIRE(Dimension::none().isDimensionless());
    STATIC_REQUIRE(Dimension::none().isKnown());
    STATIC_REQUIRE(Dimension::unknown().isUnknown());
    STATIC_REQUIRE(!Dimension::unknown().isDimensionless());
    STATIC_REQUIRE(Dimension{}.exp[Dimension::Time] == 0);
    STATIC_REQUIRE(kL.exp[Dimension::Length] == 1);
    STATIC_REQUIRE(Dimension::basis(Dimension::Time, -2).exp[Dimension::Time] == -2);
    CHECK(Dimension::none() != Dimension::unknown());
    CHECK(Dimension::unknown() == Dimension::unknown());
}

TEST_CASE("dimension: products and quotients add and subtract exponents", "[unit][dimension]") {
    const Dimension speed = kL / kT;
    CHECK(speed == dim(1, 0, -1));
    const Dimension accel = speed / kT;
    CHECK(accel == dim(1, 0, -2));
    const Dimension force = kM * accel;
    CHECK(force == dim(1, 1, -2));
    CHECK(force * kL == dim(2, 1, -2)); // torque and energy share a dimension
    CHECK((force / (kL * kL)) == dim(-1, 1, -2));
    CHECK(speed * kT == kL);
    CHECK((kL / kL).isDimensionless());
    CHECK(kT.inverse() == dim(0, 0, -1));
    CHECK(Dimension::none().inverse() == Dimension::none());
}

TEST_CASE("dimension: unknown propagates through every operation", "[unit][dimension]") {
    const Dimension u = Dimension::unknown();
    CHECK((u * kL).isUnknown());
    CHECK((kL * u).isUnknown());
    CHECK((u / kT).isUnknown());
    CHECK((kT / u).isUnknown());
    CHECK(u.inverse().isUnknown());
    CHECK(u.pow(2).isUnknown());
    CHECK(u.pow(Rational{1, 2}).isUnknown());
    CHECK(u.pow(0).isUnknown());
}

TEST_CASE("dimension: integer powers multiply exponents", "[unit][dimension]") {
    CHECK(kL.pow(3) == dim(3, 0, 0));
    CHECK((kL / kT).pow(2) == dim(2, 0, -2));
    CHECK((kL / kT).pow(-1) == dim(-1, 0, 1));
    CHECK(kL.pow(0).isDimensionless());
    CHECK(Dimension::none().pow(5).isDimensionless());
}

TEST_CASE("dimension: rational powers are exact or unknown", "[unit][dimension]") {
    CHECK(kL.pow(2).pow(Rational{1, 2}) == kL);                // sqrt(L^2) = L
    CHECK(dim(2, 0, -4).pow(Rational{1, 2}) == dim(1, 0, -2)); // sqrt
    CHECK(dim(3, 0, 6).pow(Rational{2, 3}) == dim(2, 0, 4));
    CHECK(dim(2, 0, -4).pow(Rational{-1, 2}) == dim(-1, 0, 2));
    CHECK(dim(2, 0, -4).pow(Rational{1, -2}) == dim(-1, 0, 2));     // the sign may sit on the denominator
    CHECK(kL.pow(Rational{1, 2}).isUnknown());                      // sqrt(L): fractional, D-085
    CHECK(dim(2, 0, -3).pow(Rational{1, 2}).isUnknown());           // one exponent fractional is enough
    CHECK(kL.pow(Rational{1, 0}).isUnknown());                      // invalid exponent
    CHECK(Dimension::none().pow(Rational{1, 3}).isDimensionless()); // a pure number stays a pure number
}

TEST_CASE("dimension: exponents outside [-127, 127] become unknown", "[unit][dimension]") {
    const Dimension big = Dimension::basis(Dimension::Length, 127);
    CHECK((big * kL).isUnknown());
    CHECK((Dimension::basis(Dimension::Length, -127) / kL).isUnknown());
    CHECK(big.pow(2).isUnknown());
    CHECK(big.pow(-1) == Dimension::basis(Dimension::Length, -127));
    CHECK(kL.pow(1000).isUnknown());
    CHECK(kL.pow(Rational{INT32_MIN, 1}).isUnknown());
}

TEST_CASE("dimension: formatting", "[unit][dimension]") {
    CHECK((kL / kT).toString() == "L·T⁻¹");
    CHECK((kL / (kT * kT)).toString() == "L·T⁻²");
    CHECK(dim(-1, 1, -2).toString() == "L⁻¹·M·T⁻²");
    CHECK(dim(0, 0, 0, 0, 1).toString() == "Θ");
    CHECK(dim(0, 0, 12).toString() == "T¹²");
    CHECK(Dimension::none().toString() == "1");
    CHECK(Dimension::unknown().toString() == "?");
    CHECK(dim(1, 0, -2).toAsciiString() == "L.T^-2");
    CHECK(dim(2, 1, -3, -1).toAsciiString() == "L^2.M.T^-3.I^-1");
    CHECK(dim(0, 0, 0, 0, 1, 1, 1).toAsciiString() == "Theta.N.J");
    CHECK(Dimension::none().toAsciiString() == "1");
    CHECK(Dimension::unknown().toAsciiString() == "?");
}
