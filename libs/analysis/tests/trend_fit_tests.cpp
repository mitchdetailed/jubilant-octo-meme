// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrendFit (04 section 10.1 item 4; AC-M3-06): the coefficients match an independent least-squares reference
// within 1e-6 relative for linear and polynomial 2-6, with and without through-zero. The reference is
// tests/data/trend_fit_cases.json, written by tools/trend_ref.py from exact rational normal equations (D-041:
// never from the code under test; the ctest entry analysis.trend_reference_current keeps it current).
#include "analysis_test_support.hpp"

#include <esses/base/json_nlohmann.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::analysis;
using namespace esses::analysis::test;

namespace {

nlohmann::json referenceCases() {
    auto text =
        jsonio::readFile(repoRoot() / "libs" / "analysis" / "tests" / "data" / "trend_fit_cases.json");
    REQUIRE(text.ok());
    auto parsed = jsonio::parseJson(*text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

std::vector<double> numbers(const nlohmann::json& j) {
    std::vector<double> out;
    for (const nlohmann::json& v : j) {
        out.push_back(v.get<double>());
    }
    return out;
}

TrendSpec specOf(const nlohmann::json& c) {
    TrendSpec spec;
    spec.type = c.at("type").get<std::string>() == "linear" ? TrendType::Linear : TrendType::Polynomial;
    spec.order = c.at("order").get<int>();
    spec.throughZero = c.at("throughZero").get<bool>();
    return spec;
}

} // namespace

TEST_CASE("trend_fit: coefficients match the exact least-squares reference within 1e-6 relative",
          "[unit][AC-M3-06]") {
    const nlohmann::json doc = referenceCases();
    const nlohmann::json& cases = doc.at("cases");
    REQUIRE(cases.size() >= 12);
    int linear = 0;
    int polynomial = 0;
    int throughZero = 0;
    for (const nlohmann::json& c : cases) {
        const std::string id = c.at("id").get<std::string>();
        INFO("case " << id);
        const TrendSpec spec = specOf(c);
        const std::vector<double> x = numbers(c.at("x"));
        const std::vector<double> y = numbers(c.at("y"));
        const std::vector<double> want = numbers(c.at("coefficients"));
        auto fit = TrendFit::fit(x, y, spec);
        REQUIRE(fit.ok());
        REQUIRE(fit->coefficients.size() == want.size());
        for (std::size_t n = 0; n < want.size(); ++n) {
            INFO("c[" << n << "] = " << fit->coefficients[n] << ", reference " << want[n]);
            if (want[n] == 0.0) {
                CHECK(fit->coefficients[n] == 0.0); // the through-zero constant
            } else {
                CHECK(std::abs(fit->coefficients[n] - want[n]) <= 1e-6 * std::abs(want[n]));
            }
        }
        CHECK(fit->r2 == Catch::Approx(c.at("r2").get<double>()).margin(1e-9));
        CHECK(fit->samples == c.at("samples").get<std::size_t>());
        (spec.type == TrendType::Linear ? linear : polynomial) += 1;
        throughZero += spec.throughZero ? 1 : 0;

        // Non-finite pairs are skipped: the same fit with NaN and infinite points mixed in.
        std::vector<double> xs = x;
        std::vector<double> ys = y;
        xs.insert(xs.begin() + 3, std::numeric_limits<double>::quiet_NaN());
        ys.insert(ys.begin() + 3, 1.0);
        xs.push_back(2.0);
        ys.push_back(std::numeric_limits<double>::infinity());
        auto mixed = TrendFit::fit(xs, ys, spec);
        REQUIRE(mixed.ok());
        CHECK(mixed->coefficients == fit->coefficients);
        CHECK(mixed->samples == fit->samples);
    }
    CHECK(linear >= 2);
    CHECK(polynomial >= 10); // orders 2-6, with and without through-zero
    CHECK(throughZero >= 6);
}

TEST_CASE("trend_fit: exact data, evaluation and refusals", "[unit]") {
    // Points on y = 2 - 3x + 0.5x^3 give back the polynomial (the x^2 term vanishes), r^2 = 1.
    std::vector<double> x;
    std::vector<double> y;
    for (int i = -10; i <= 10; ++i) {
        const double v = 0.3 * i;
        x.push_back(v);
        y.push_back(2.0 - 3.0 * v + 0.5 * v * v * v);
    }
    auto cubic = TrendFit::fit(x, y, TrendSpec{TrendType::Polynomial, 3, false});
    REQUIRE(cubic.ok());
    CHECK(cubic->coefficients[0] == Catch::Approx(2.0).epsilon(1e-12));
    CHECK(cubic->coefficients[1] == Catch::Approx(-3.0).epsilon(1e-12));
    CHECK(std::abs(cubic->coefficients[2]) < 1e-12);
    CHECK(cubic->coefficients[3] == Catch::Approx(0.5).epsilon(1e-12));
    CHECK(cubic->r2 == Catch::Approx(1.0).margin(1e-12));
    CHECK(cubic->evaluate(1.5) == Catch::Approx(2.0 - 4.5 + 0.5 * 3.375).epsilon(1e-12));
    CHECK(std::isnan(TrendResult{}.evaluate(1.0)));

    // Linear ignores `order`; a constant y has r^2 = 1.
    auto flat = TrendFit::fit(x, std::vector<double>(x.size(), 4.0), TrendSpec{TrendType::Linear, 5, false});
    REQUIRE(flat.ok());
    REQUIRE(flat->coefficients.size() == 2);
    CHECK(flat->coefficients[0] == Catch::Approx(4.0));
    CHECK(std::abs(flat->coefficients[1]) < 1e-12);
    CHECK(flat->r2 == 1.0);

    const auto code = [](const Result<TrendResult>& r) { return r ? ErrorCode::Ok : r.error().code; };
    const std::vector<double> few{1.0, 2.0};
    CHECK(code(TrendFit::fit(x, few, TrendSpec{})) == ErrorCode::Invalid); // mismatched
    CHECK(code(TrendFit::fit(x, y, TrendSpec{TrendType::None, 2, false})) == ErrorCode::Invalid); // no type
    CHECK(code(TrendFit::fit(x, y, TrendSpec{TrendType::Polynomial, 7, false})) == ErrorCode::Invalid);
    CHECK(code(TrendFit::fit(x, y, TrendSpec{TrendType::Polynomial, 1, false})) == ErrorCode::Invalid);
    CHECK(code(TrendFit::fit(few, few, TrendSpec{TrendType::Polynomial, 2, false})) == ErrorCode::Invalid);
    const std::vector<double> same(5, 3.0);
    const std::vector<double> five{1.0, 2.0, 3.0, 4.0, 5.0};
    CHECK(code(TrendFit::fit(same, five, TrendSpec{})) == ErrorCode::Invalid); // one distinct x
    const std::vector<double> two{1.0, 1.0, 2.0, 2.0};
    CHECK(code(TrendFit::fit(two, two, TrendSpec{TrendType::Polynomial, 2, false})) == ErrorCode::Invalid);
    const std::vector<double> zeros(4, 0.0);
    CHECK(code(TrendFit::fit(zeros, std::vector<double>(five.begin(), five.begin() + 4),
                             TrendSpec{TrendType::Linear, 2, true})) == ErrorCode::Invalid); // x all 0
    // Through zero with two distinct x values determines a line through the origin.
    auto origin = TrendFit::fit(std::vector<double>{1.0, 2.0}, std::vector<double>{3.0, 5.0},
                                TrendSpec{TrendType::Linear, 2, true});
    REQUIRE(origin.ok());
    CHECK(origin->coefficients[0] == 0.0);
    CHECK(origin->coefficients[1] == Catch::Approx(13.0 / 5.0).epsilon(1e-14));
}

TEST_CASE("trend_fit: equation text", "[unit]") {
    TrendResult t;
    t.coefficients = {5.678, -1.234, 0.01234};
    CHECK(TrendFit::equationText(t) == "y = 0.01234x^2 - 1.234x + 5.678");
    CHECK(TrendFit::equationText(t, 2) == "y = 0.012x^2 - 1.2x + 5.7");
    CHECK(TrendFit::equationText(t, 4, "v", "g") == "g = 0.01234v^2 - 1.234v + 5.678");
    t.coefficients = {0.0, -2.5};
    CHECK(TrendFit::equationText(t) == "y = -2.5x");
    t.coefficients = {-1.0, 0.0, 0.0, 3.0e-7};
    CHECK(TrendFit::equationText(t, 3) == "y = 3e-07x^3 - 1");
    t.coefficients = {123456.0, 1.0};
    CHECK(TrendFit::equationText(t, 4) == "y = 1x + 1.235e+05");
    t.coefficients = {0.0, 0.0};
    CHECK(TrendFit::equationText(t) == "y = 0");
    CHECK(TrendFit::equationText(TrendResult{}) == "y = 0");
    t.coefficients = {0.5, 1.0 / 3.0};
    CHECK(TrendFit::equationText(t, 99) == "y = 0.333333333333333x + 0.5"); // clamped to 15 digits
    CHECK(TrendFit::equationText(t, 0) == "y = 0.3x + 0.5");                // and to at least 1
}
