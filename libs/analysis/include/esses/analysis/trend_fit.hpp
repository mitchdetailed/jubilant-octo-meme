// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrendFit (04 section 10.1 item 4; ARCHITECTURE_OUTLINE section 4 esses_analysis; AC-M3-06): least-squares
// trend lines of a scatter trace: linear, or polynomial of order 2-6, optionally through zero (no constant
// term), solved by QR on the design matrix (never the normal equations; Givens rotations row by row on x
// scaled to about [-1, 1], so memory stays O(order^2); no Eigen, D-193), plus the equation text shown for
// Main ("y = 0.01234x^2 - 1.234x + 5.678", significantDigits default 4; Component menu -> Copy Equation).
// AC-M3-06: coefficients within 1e-6 relative of a least-squares reference for linear and polynomial 2-6,
// with and without through-zero (tests/trend_fit_tests.cpp against tests/data/trend_fit_cases.json, which the
// independent exact-arithmetic reference tools/trend_ref.py writes).
#pragma once

#include <esses/base/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace esses::analysis {

enum class TrendType : std::uint8_t { None, Linear, Polynomial };

struct TrendSpec {
    TrendType type = TrendType::Linear;
    int order = 2; // Polynomial: 2..TrendFit::kMaxOrder
    bool throughZero = false;
};

struct TrendResult {
    std::vector<double> coefficients; // c[0] + c[1] x + ... + c[n] x^n (c[0] == 0 when through zero)
    double r2 = 0.0;                  // 1 - SSres / SStot (SStot about the mean of y; 1 when y is constant)
    std::size_t samples = 0;          // the finite pairs used
    double evaluate(double x) const noexcept; // Horner; NaN without coefficients
};

class TrendFit {
public:
    static constexpr int kMaxOrder = 6;

    // Invalid for mismatched spans, TrendType::None, a polynomial order outside 2..6, fewer finite (x, y)
    // pairs than unknowns, or x values that do not determine the trend (all equal, all 0 through zero, or
    // fewer distinct values than unknowns); non-finite pairs are skipped. Linear ignores `order`.
    static Result<TrendResult> fit(std::span<const double> x, std::span<const double> y,
                                   const TrendSpec& spec);
    // "<y> = <c_n><x>^n + ... + <c_1><x> + <c_0>" from the highest power down, each coefficient with
    // significantDigits (clamped to 1..15) significant digits in the shortest %g form (locale independent),
    // signs as " + " / " - ", zero coefficients left out ("y = 0" when all are).
    static std::string equationText(const TrendResult& trend, int significantDigits = 4,
                                    std::string_view xName = "x", std::string_view yName = "y");
};

} // namespace esses::analysis
