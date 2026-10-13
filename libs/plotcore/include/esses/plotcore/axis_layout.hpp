// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AxisLayout (04 section 5.5): tick placement with "nice" steps 1/2/5 x 10^n in the axis unit, minimum tick
// spacing 6 x the average label advance; time labels in the project time format (s.mmm, m:ss.mmm, h:mm:ss),
// distance labels in the Corr Dist display unit. A pure function of its inputs, unit-tested with
// FixedTextMetrics.
#pragma once

#include <esses/base/time.hpp>
#include <esses/plotcore/draw_list.hpp>
#include <esses/plotcore/text_metrics.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace esses::plot {

enum class AxisKind : std::uint8_t { Time, Distance, Value };
enum class TimeFormat : std::uint8_t { Seconds /*ss.fff*/, MinutesSeconds /*m:ss.fff*/, Hours /*h:mm:ss.f*/ };

struct AxisSpec {
    AxisKind kind = AxisKind::Time;
    double lo = 0.0; // axis value at posPx = 0 (time: microseconds; distance and value: display units)
    double hi = 1.0; // axis value at posPx = lengthPx
    float lengthPx = 0.0F;
    bool vertical = false;
    TimeFormat timeFormat = TimeFormat::MinutesSeconds;
    int decimals = -1; // -1: from the step
    std::string unit;  // shown in the axis title; "" for none
};

struct AxisTick {
    double value = 0.0;
    float posPx = 0.0F;
    std::string label;
    bool major = true;
};

class AxisLayout {
public:
    static std::vector<AxisTick> ticks(const AxisSpec& spec, const ITextMetrics& metrics, float dpr);
    // The smallest nice step (1, 2 or 5 x 10^n) >= rawStep; rawStep > 0.
    static double niceStep(double rawStep) noexcept;
    static std::string formatTime(Micros t, TimeFormat format, int decimals);
    // Tick marks and labels of `ticks` along `axisRect` into `list`.
    static void emit(DrawList& list, const AxisSpec& spec, const std::vector<AxisTick>& ticks,
                     RectPx axisRect, Rgba colour, float dpr);
};

} // namespace esses::plot
