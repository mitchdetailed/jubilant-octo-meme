// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "text_format.hpp"

#include <esses/plotcore/axis_layout.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace esses::plot {

namespace {

constexpr int kMaxAttempts = 8;
constexpr double kMaxTicks = 1000.0;

// 10^e for |e| <= 22, exact for e >= 0 and one correctly rounded division below.
double pow10(int e) noexcept {
    double p = 1.0;
    for (int i = 0; i < std::abs(e); ++i) {
        p *= 10.0;
    }
    return e >= 0 ? p : 1.0 / p;
}

struct Nice {
    int mantissa = 1; // 1, 2 or 5
    int exponent = 0;
    double step() const noexcept { return static_cast<double>(mantissa) * pow10(exponent); }
};

Nice niceOf(double raw) noexcept {
    if (!(raw > 0.0) || !std::isfinite(raw)) {
        return Nice{1, 0};
    }
    int e = 0;
    while (e < 22 && pow10(e + 1) <= raw) {
        ++e;
    }
    while (e > -22 && pow10(e) > raw) {
        --e;
    }
    for (const int m : {1, 2, 5}) {
        if (static_cast<double>(m) * pow10(e) >= raw) {
            return Nice{m, e};
        }
    }
    return Nice{1, e + 1};
}

Nice nextNice(Nice n) noexcept {
    if (n.mantissa == 1) {
        return Nice{2, n.exponent};
    }
    if (n.mantissa == 2) {
        return Nice{5, n.exponent};
    }
    return Nice{1, n.exponent + 1};
}

void appendPadded(std::string& out, std::int64_t v, int width) {
    std::array<char, 24> buf{};
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    const auto len = static_cast<int>(r.ptr - buf.data());
    for (int i = len; i < width; ++i) {
        out += '0';
    }
    out.append(buf.data(), r.ptr);
}

} // namespace

double AxisLayout::niceStep(double rawStep) noexcept {
    return niceOf(rawStep).step();
}

std::string AxisLayout::formatTime(Micros t, TimeFormat format, int decimals) {
    const int d = std::clamp(decimals, 0, 6);
    std::int64_t q = 1;
    for (int i = d; i < 6; ++i) {
        q *= 10;
    }
    const bool negative = t < 0;
    // |t| rounded half away from zero to a multiple of q microseconds.
    const std::uint64_t mag =
        negative ? static_cast<std::uint64_t>(-(t + 1)) + 1U : static_cast<std::uint64_t>(t);
    const auto uq = static_cast<std::uint64_t>(q);
    const std::uint64_t rounded = (mag + uq / 2U) / uq * uq;
    const auto seconds = static_cast<std::int64_t>(rounded / 1'000'000U);
    const auto fracUs = static_cast<std::int64_t>(rounded % 1'000'000U);

    std::string out;
    if (negative && rounded != 0) {
        out += '-';
    }
    switch (format) {
    case TimeFormat::Seconds:
        appendPadded(out, seconds, 1);
        break;
    case TimeFormat::MinutesSeconds:
        appendPadded(out, seconds / 60, 1);
        out += ':';
        appendPadded(out, seconds % 60, 2);
        break;
    case TimeFormat::Hours:
        appendPadded(out, seconds / 3600, 1);
        out += ':';
        appendPadded(out, (seconds / 60) % 60, 2);
        out += ':';
        appendPadded(out, seconds % 60, 2);
        break;
    }
    if (d > 0) {
        out += '.';
        appendPadded(out, fracUs / q, d);
    }
    return out;
}

std::vector<AxisTick> AxisLayout::ticks(const AxisSpec& spec, const ITextMetrics& metrics, float dpr) {
    std::vector<AxisTick> out;
    if (!(spec.lengthPx > 0.0F) || !std::isfinite(spec.lo) || !std::isfinite(spec.hi) || spec.lo == spec.hi) {
        return out;
    }
    const bool time = spec.kind == AxisKind::Time;
    const double scale = time ? 1e-6 : 1.0; // axis units (seconds for time) per value unit (microseconds)
    const double vmin = std::min(spec.lo, spec.hi);
    const double vmax = std::max(spec.lo, spec.hi);
    const double spanU = (vmax - vmin) * scale;
    const auto length = static_cast<double>(spec.lengthPx);
    const auto charAdv = static_cast<double>(metrics.advancePx(TextStyleId::Axis, "0", dpr));
    const auto lineH = static_cast<double>(metrics.lineHeightPx(TextStyleId::Axis, dpr));
    const double minSpacing = std::max(6.0 * charAdv, 1.0); // 04 section 5.5
    Nice nice = niceOf(spanU * minSpacing / length);

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt, nice = nextNice(nice)) {
        const double step = nice.step();
        const double first = std::ceil(vmin * scale / step - 1e-9);
        const double last = std::floor(vmax * scale / step + 1e-9);
        if (last - first > kMaxTicks) {
            continue;
        }
        const int decimals = spec.decimals >= 0 ? spec.decimals : std::max(0, -nice.exponent);
        out.clear();
        double widest = 0.0;
        for (double k = first; k <= last; k += 1.0) {
            AxisTick tick;
            const double valueU = k * step;
            tick.value = valueU / scale;
            tick.posPx = static_cast<float>((tick.value - spec.lo) / (spec.hi - spec.lo) * length);
            tick.label =
                time ? formatTime(static_cast<Micros>(std::llround(tick.value)), spec.timeFormat, decimals)
                     : detail::formatFixed(valueU, decimals);
            widest =
                std::max(widest, static_cast<double>(metrics.advancePx(TextStyleId::Axis, tick.label, dpr)));
            out.push_back(std::move(tick));
        }
        const double stepPx = step / spanU * length;
        const bool fits = spec.vertical ? stepPx >= 1.2 * lineH : stepPx >= widest + 2.0 * charAdv;
        if (fits) {
            return out;
        }
    }
    return out;
}

void AxisLayout::emit(DrawList& list, const AxisSpec& spec, const std::vector<AxisTick>& ticks,
                      RectPx axisRect, Rgba colour, float dpr) {
    if (ticks.empty()) {
        return;
    }
    const float tickLen = 3.0F * dpr;
    const float gap = 4.0F * dpr;
    std::vector<float> marks;
    marks.reserve(ticks.size() * 4);
    for (const AxisTick& t : ticks) {
        if (spec.vertical) {
            const float y = axisRect.y + t.posPx;
            const float x1 = axisRect.x + axisRect.w;
            marks.insert(marks.end(), {x1 - tickLen, y, x1, y});
        } else {
            const float x = axisRect.x + t.posPx;
            marks.insert(marks.end(), {x, axisRect.y, x, axisRect.y + tickLen});
        }
    }
    list.lines(marks.data(), marks.size() / 2, colour, std::max(1.0F, dpr));
    for (const AxisTick& t : ticks) {
        if (spec.vertical) {
            const float y = axisRect.y + t.posPx;
            list.textRun(t.label, axisRect.x + axisRect.w - tickLen - gap * 0.5F, y, TextStyleId::Axis,
                         TextAnchor::MiddleRight, colour, std::max(0.0F, axisRect.w - tickLen - gap));
        } else {
            list.textRun(t.label, axisRect.x + t.posPx, axisRect.y + tickLen + gap * 0.25F, TextStyleId::Axis,
                         TextAnchor::TopCentre, colour);
        }
    }
    if (!spec.unit.empty() && !spec.vertical) {
        list.textRun(spec.unit, axisRect.x + axisRect.w, axisRect.y + axisRect.h, TextStyleId::Axis,
                     TextAnchor::BottomRight, colour);
    }
}

} // namespace esses::plot
