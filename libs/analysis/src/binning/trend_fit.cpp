// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrendFit (04 section 10.1 item 4, AC-M3-06): least squares by QR, built row by row with Givens rotations
// (O(p^2) memory however many points), on x scaled to about [-1, 1] so that the columns of the design matrix
// stay comparable up to x^6; the coefficients are converted back to powers of x at the end.

#include <esses/analysis/trend_fit.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>
#include <utility>

namespace esses::analysis {

namespace {

constexpr std::size_t kMaxUnknowns = TrendFit::kMaxOrder + 1;

Failure invalid(std::string message) {
    return Failure{Error{ErrorCode::Invalid, std::move(message), {}}};
}

// Binomial coefficient C(n, k) for n <= kMaxOrder.
double binomial(int n, int k) noexcept {
    double c = 1.0;
    for (int i = 1; i <= k; ++i) {
        c = c * static_cast<double>(n - k + i) / static_cast<double>(i);
    }
    return c;
}

// A coefficient with `digits` significant digits, shortest form (printf's %g rules, locale independent).
std::string number(double v, int digits) {
    std::array<char, 64> buf{};
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::general, digits);
    return r.ec == std::errc{} ? std::string(buf.data(), r.ptr) : std::string("?");
}

} // namespace

double TrendResult::evaluate(double x) const noexcept {
    if (coefficients.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double y = 0.0;
    for (std::size_t n = coefficients.size(); n-- > 0;) {
        y = y * x + coefficients[n];
    }
    return y;
}

Result<TrendResult> TrendFit::fit(std::span<const double> x, std::span<const double> y,
                                  const TrendSpec& spec) {
    if (x.size() != y.size()) {
        return invalid("a trend needs as many y values as x values");
    }
    int degree = 0;
    switch (spec.type) {
    case TrendType::None:
        return invalid("no trend type");
    case TrendType::Linear:
        degree = 1;
        break;
    case TrendType::Polynomial:
        if (spec.order < 2 || spec.order > kMaxOrder) {
            return invalid("a polynomial trend has an order from 2 to 6");
        }
        degree = spec.order;
        break;
    }
    // Unknowns b_j, j = 0..p-1, of the basis lead(x) u^j with u = (x - shift) / scale mapping the x range
    // onto
    // [-1, 1]: lead = 1 without through-zero (degree + 1 unknowns); through zero lead = x / absMax and degree
    // unknowns, x times a centred polynomial of degree - 1 (D-194), which keeps the columns well conditioned
    // when the x values lie in a narrow band far from 0 (engine speed 7900-8000 rpm); plain powers of x /
    // absMax there are nearly collinear.
    const bool throughZero = spec.throughZero;
    const int p = throughZero ? degree : degree + 1;

    std::size_t m = 0;
    double xMin = std::numeric_limits<double>::infinity();
    double xMax = -xMin;
    double absMax = 0.0;
    double yMean = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!std::isfinite(x[i]) || !std::isfinite(y[i])) {
            continue;
        }
        ++m;
        xMin = std::min(xMin, x[i]);
        xMax = std::max(xMax, x[i]);
        absMax = std::max(absMax, std::abs(x[i]));
        yMean += (y[i] - yMean) / static_cast<double>(m);
    }
    if (m < static_cast<std::size_t>(p)) {
        return invalid("a trend of this order needs at least " + std::to_string(p) + " points");
    }
    const double shift = 0.5 * (xMin + xMax);
    double scale = 0.5 * (xMax - xMin);
    if (throughZero && !(absMax > 0.0)) {
        return invalid("the x values do not determine a trend");
    }
    if (throughZero && !(scale > 0.0)) {
        scale = absMax; // one x value: u = 0, so only the linear term is determined (the rank test below)
    }
    if (!(scale > 0.0)) {
        return invalid("the x values do not determine a trend");
    }

    // R (upper triangular, p x p, row-major) and Q^T y accumulated by Givens rotations, one point at a time.
    std::array<double, kMaxUnknowns * kMaxUnknowns> r{};
    std::array<double, kMaxUnknowns> qty{};
    double ssRes = 0.0;
    double ssTot = 0.0;
    std::array<double, kMaxUnknowns> row{};
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!std::isfinite(x[i]) || !std::isfinite(y[i])) {
            continue;
        }
        const double u = (x[i] - shift) / scale;
        double up = throughZero ? x[i] / absMax : 1.0;
        for (int j = 0; j < p; ++j) {
            row[static_cast<std::size_t>(j)] = up;
            up *= u;
        }
        double rhs = y[i];
        ssTot += (y[i] - yMean) * (y[i] - yMean);
        for (int j = 0; j < p; ++j) {
            const auto uj = static_cast<std::size_t>(j);
            const double a = row[uj];
            if (a == 0.0) {
                continue;
            }
            double& rjj = r[uj * kMaxUnknowns + uj];
            const double h = std::hypot(rjj, a);
            const double c = rjj / h;
            const double s = a / h;
            rjj = h;
            for (int k = j + 1; k < p; ++k) {
                const auto uk = static_cast<std::size_t>(k);
                double& rjk = r[uj * kMaxUnknowns + uk];
                const double t = row[uk];
                row[uk] = c * t - s * rjk;
                rjk = c * rjk + s * t;
            }
            const double q = qty[uj];
            qty[uj] = c * q + s * rhs;
            rhs = c * rhs - s * q;
        }
        ssRes += rhs * rhs; // what the rotations leave of the point is its residual
    }
    double rMax = 0.0;
    for (int j = 0; j < p; ++j) {
        rMax = std::max(
            rMax, std::abs(r[static_cast<std::size_t>(j) * kMaxUnknowns + static_cast<std::size_t>(j)]));
    }
    std::array<double, kMaxUnknowns> b{}; // coefficients of lead(x) u^j
    for (int j = p - 1; j >= 0; --j) {
        const auto uj = static_cast<std::size_t>(j);
        const double rjj = r[uj * kMaxUnknowns + uj];
        if (!(std::abs(rjj) > 1e-12 * rMax)) {
            return invalid("the x values do not determine a trend of this order");
        }
        double v = qty[uj];
        for (int k = j + 1; k < p; ++k) {
            const auto uk = static_cast<std::size_t>(k);
            v -= r[uj * kMaxUnknowns + uk] * b[uk];
        }
        b[uj] = v / rjj;
    }
    // Back to powers of x: sum_k b_k ((x - shift) / scale)^k = sum_n d_n x^n with
    // d_n = sum_{k >= n} b_k C(k, n) (-shift)^(k - n) / scale^k; through zero the sum is multiplied by
    // x / absMax, so d_n is the coefficient of x^(n + 1) divided by absMax.
    TrendResult out;
    out.coefficients.assign(static_cast<std::size_t>(degree) + 1, 0.0);
    const int lift = throughZero ? 1 : 0;
    const double leadScale = throughZero ? absMax : 1.0;
    for (int k = 0; k < p; ++k) {
        const double bk = b[static_cast<std::size_t>(k)] / (std::pow(scale, k) * leadScale);
        for (int n = 0; n <= k; ++n) {
            out.coefficients[static_cast<std::size_t>(n) + static_cast<std::size_t>(lift)] +=
                bk * binomial(k, n) * std::pow(-shift, k - n);
        }
    }
    out.r2 = ssTot > 0.0 ? 1.0 - ssRes / ssTot : 1.0;
    out.samples = m;
    return out;
}

std::string TrendFit::equationText(const TrendResult& trend, int significantDigits, std::string_view xName,
                                   std::string_view yName) {
    const int digits = std::clamp(significantDigits, 1, 15);
    std::string out(yName);
    out += " = ";
    bool first = true;
    for (std::size_t n = trend.coefficients.size(); n-- > 0;) {
        const double c = trend.coefficients[n];
        if (c == 0.0 || !std::isfinite(c)) {
            continue;
        }
        const std::string magnitude = number(std::abs(c), digits);
        if (first) {
            out += c < 0.0 ? "-" : "";
        } else {
            out += c < 0.0 ? " - " : " + ";
        }
        first = false;
        out += magnitude;
        if (n >= 1) {
            out += xName;
        }
        if (n >= 2) {
            out += "^" + std::to_string(n);
        }
    }
    if (first) {
        out += "0";
    }
    return out;
}

} // namespace esses::analysis
