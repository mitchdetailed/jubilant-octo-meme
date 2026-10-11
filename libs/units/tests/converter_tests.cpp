// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Converter: the affine map y = a*x + b, its inverse, composition and block conversion (02 section 2.1).
#include <esses/units/converter.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

using namespace esses::units;

TEST_CASE("converter: default is the identity", "[unit][converter]") {
    const Converter c;
    CHECK(c.identity());
    CHECK(c(42.5) == 42.5);
    CHECK(c(1.25f) == 1.25f);
    CHECK_FALSE((Converter{2.0, 0.0}).identity());
    CHECK_FALSE((Converter{1.0, 0.5}).identity());
}

TEST_CASE("converter: applies y = a*x + b in double and rounds once for float", "[unit][converter]") {
    const Converter kToC{1.0, -273.15};
    CHECK(kToC(373.15) == Catch::Approx(100.0).epsilon(1e-15));
    const Converter fToK{1.0 / 1.8, 459.67 / 1.8};
    CHECK(fToK(32.0) == Catch::Approx(273.15).epsilon(1e-15));
    const Converter big{3.6, 0.0};
    const float x = 16777217.0f; // not representable: rounds to 16777216 before the call
    CHECK(big(x) == static_cast<float>(3.6 * static_cast<double>(x)));
}

TEST_CASE("converter: inverse and composition", "[unit][converter]") {
    const Converter cToF{1.8, 32.0};
    const Converter fToC = cToF.inverse();
    CHECK(fToC(212.0) == Catch::Approx(100.0).epsilon(1e-15));
    CHECK(fToC(cToF(-40.0)) == Catch::Approx(-40.0).epsilon(1e-15));
    const Converter kToC{1.0, -273.15};
    const Converter kToF = kToC.then(cToF);
    CHECK(kToF.a == Catch::Approx(1.8));
    CHECK(kToF.b == Catch::Approx(-459.67).epsilon(1e-15));
    CHECK(kToF(273.15) == Catch::Approx(32.0).epsilon(1e-14));
    const Converter id = cToF.then(fToC);
    CHECK(id.a == Catch::Approx(1.0).epsilon(1e-15));
    CHECK(id.b == Catch::Approx(0.0).margin(1e-13));
    CHECK(Converter{}.then(cToF) == cToF);
}

TEST_CASE("converter: applyInPlace converts a block and leaves it alone for the identity",
          "[unit][converter]") {
    std::vector<double> d{0.0, 1.0, -2.5};
    Converter{2.0, 1.0}.applyInPlace(d);
    CHECK(d == std::vector<double>{1.0, 3.0, -4.0});
    std::array<float, 3> f{0.0f, 10.0f, 100.0f};
    Converter{3.6, 0.0}.applyInPlace(f);
    CHECK(f[1] == static_cast<float>(36.0));
    CHECK(f[2] == static_cast<float>(360.0));
    std::vector<double> same{1.0, 2.0};
    Converter{}.applyInPlace(same);
    CHECK(same == std::vector<double>{1.0, 2.0});
}
