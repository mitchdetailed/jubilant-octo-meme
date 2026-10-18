// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Writing a maths output's samples into a chunk (PlanProvider and the expression nodes' StreamPlanProvider):
// F32/F64 samples as cast from the doubles, I32 rounded half away from zero and clamped; an invalid sample is
// NaN in a float chunk, 0 in an I32 chunk, with its bit cleared. A chunk without an invalid sample carries no
// bitmap. Internal to esses_maths.
#pragma once

#include <esses/store/chunk.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace esses::maths::detail {

// Round half away from zero (as std::llround) without the library call; values outside the I32 range clamp.
inline std::int32_t toI32(double v) noexcept {
    const double r = v >= 0.0 ? v + 0.5 : v - 0.5;
    if (r >= 2147483647.0) {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (r <= -2147483648.0) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(r);
}

// toI32(std::trunc(v)) for a finite v, without the library call: trunc then round-half-away gives the value
// truncated toward zero, which the conversion itself does inside the I32 range; outside it both clamp (a
// form the compiler vectorises).
inline std::int32_t truncToI32(double v) noexcept {
    return static_cast<std::int32_t>(std::min(std::max(v, -2147483648.0), 2147483647.0));
}

// True when every one of the n values is finite and of magnitude below `bound` (the pre-pass of the I32
// conversion): v - v is 0 for a finite v and NaN otherwise, and the sums and maxima run in two lanes so the
// loop vectorises.
inline bool finiteBelow(const double* v, std::size_t n, double bound) noexcept {
    double s0 = 0.0;
    double s1 = 0.0;
    double m0 = 0.0;
    double m1 = 0.0;
    std::size_t i = 0;
    for (; i + 2 <= n; i += 2) {
        s0 += v[i] - v[i];         // NOLINT(misc-redundant-expression)
        s1 += v[i + 1] - v[i + 1]; // NOLINT(misc-redundant-expression)
        const double a = std::fabs(v[i]);
        const double b = std::fabs(v[i + 1]);
        m0 = a > m0 ? a : m0;
        m1 = b > m1 ? b : m1;
    }
    for (; i < n; ++i) {
        s0 += v[i] - v[i]; // NOLINT(misc-redundant-expression)
        const double a = std::fabs(v[i]);
        m0 = a > m0 ? a : m0;
    }
    const double s = s0 + s1;
    return s == s && m0 < bound && m1 < bound; // NOLINT(misc-redundant-expression): not NaN
}

// A chunk without an invalid sample: a straight conversion loop the compiler vectorises.
template <class T> void fillValid(std::span<T> out, const double* v) noexcept {
    for (std::size_t i = 0; i < out.size(); ++i) {
        if constexpr (std::is_same_v<T, std::int32_t>) {
            out[i] = toI32(v[i]);
        } else {
            out[i] = static_cast<T>(v[i]);
        }
    }
}

// A chunk with invalid samples (`valid(i)` false); `b` was allocated with a bitmap.
template <class T, class Valid>
void fillWithBitmap(std::span<T> out, const double* v, Valid valid, store::ChunkBuilder& b) noexcept {
    for (std::size_t i = 0; i < out.size(); ++i) {
        const bool ok = valid(i);
        if constexpr (std::is_floating_point_v<T>) {
            out[i] = ok ? static_cast<T>(v[i]) : std::numeric_limits<T>::quiet_NaN();
        } else {
            out[i] = ok ? toI32(v[i]) : T{0};
        }
        if (!ok) {
            b.setValid(static_cast<std::uint32_t>(i), false);
        }
    }
}

} // namespace esses::maths::detail
