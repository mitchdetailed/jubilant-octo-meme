// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Number formatting shared by the plot-core builders: std::to_chars only, so labels are identical on every
// platform (the DrawList goldens compare them as text).
#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string>

namespace esses::plot::detail {

// `v` with exactly `decimals` digits after the point (0..9); never "-0" (a negative zero or a negative value
// that rounds to zero prints unsigned); "" for NaN and infinities.
inline std::string formatFixed(double v, int decimals) {
    if (!std::isfinite(v)) {
        return {};
    }
    std::array<char, 64> buf{};
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::fixed,
                                 std::clamp(decimals, 0, 9));
    std::string s(buf.data(), r.ptr);
    if (!s.empty() && s.front() == '-' && s.find_first_not_of("-0.") == std::string::npos) {
        s.erase(0, 1);
    }
    return s;
}

} // namespace esses::plot::detail
