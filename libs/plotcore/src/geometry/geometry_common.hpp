// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Internal helpers shared by the M3 geometry builders (src/geometry/ and src/band_layout.cpp): scheme roles,
// list set-up, the Quads command, channel lookup, scales from the SummaryPyramid (the M1 autoscale stand-in
// that time_distance uses, so a GUI-thread Dynamic build reaches the same axes as the worker's Static build
// without reading samples), manual limits in other units, a slot's visible span cut per lap (trusted flags
// for the binners), and the caption / focus / pending Chrome every component draws alike. Not installed.
#pragma once

#include <esses/analysis/band_axis.hpp>
#include <esses/plotcore/axis_layout.hpp>
#include <esses/plotcore/component_build.hpp>
#include <esses/plotcore/domain_mapper.hpp>
#include <esses/plotcore/draw_list.hpp>
#include <esses/plotcore/text_metrics.hpp>
#include <esses/project/scheme.hpp>
#include <esses/units/unit_registry.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace esses::plot::geom {

inline constexpr float kCaptionLogical = 18.0F;
inline constexpr float kAxisStripLogical = 36.0F;
inline constexpr float kXAxisLogical = 18.0F;
inline constexpr float kMarginLogical = 6.0F;
inline constexpr const char* kPendingText = "\xE2\x80\xA6"; // U+2026 (D-022)

// Scheme roles of 06 section 16 used by the geometry builders (fallbacks as in time_distance).
struct Colours {
    Rgba background, keyBackground, foreground, foreground2, frame, grid, cursor, datum, zoom;
    Rgba captionBackground, captionForeground, focus, pending;
    Rgba sectionCorner, sectionStraight, marker, annotation, deviceEvent, trackLine;
    explicit Colours(const project::Scheme& s);
};

const project::Scheme& schemeOf(const ComponentBuildInputs& in);
const ITextMetrics& metricsOf(const ComponentBuildInputs& in);
float dprOf(const ComponentBuildInputs& in);
DrawList newList(const ComponentBuildInputs& in);
inline float snap(float v) {
    return std::round(v);
}
inline float lineWidthOf(float dpr) {
    return std::max(1.0F, std::round(dpr));
}

// A Quads command (four vertices per quad, the command colour); returns its index.
std::size_t quads(DrawList& list, const float* xy, std::size_t vertexCount, Rgba colour);
inline Rgba withAlpha(Rgba c, std::uint8_t a) {
    return (c & 0xFFFFFF00U) | Rgba{a};
}

// ---- channels and slots

// The channel `name` of a slot: at `index` when the name matches there, else the first of that name.
const ResolvedChannel* channelOf(const SlotData& slot, std::size_t index, std::string_view name);
// Present in the slot's source with a series that is not pending.
bool readable(const ResolvedChannel* rc);
DomainMapper mapperOf(const ComponentBuildInputs& in, const SlotData& slot, const store::Series* series);
bool slotHasDistance(const SlotData& slot);

// ---- scales

struct Scale {
    double vMin = std::numeric_limits<double>::quiet_NaN();
    double vMax = std::numeric_limits<double>::quiet_NaN();
    bool valid() const noexcept { return std::isfinite(vMin) && std::isfinite(vMax); }
    void unite(double lo, double hi) noexcept;
    // A zero span widens to +-1 display unit (04 section 8.3 item 4).
    void widenDegenerate() noexcept;
};

// The whole-column min/max of a channel from its SummaryPyramid through `toDisplay` (base -> the axis unit).
// `covered` is false while the summary is incomplete (the caller falls back to the data); an empty result
// with covered == true means no valid sample.
struct SummaryRange {
    bool covered = false;
    Scale range;
};
SummaryRange summaryRange(const ResolvedChannel& rc, const units::Converter& toDisplay);

// A persisted {v, unit} in `toUnit` (the axis display unit): equal units as written, "" taken as base units
// of the axis channel when `axis` is given, others converted through the registry; nullopt when no conversion
// exists.
std::optional<double> unitValueIn(const analysis::UnitValue& value, const std::string& toUnit,
                                  const ResolvedChannel* axis, const units::UnitRegistry* registry);

// ---- bands

// The band containing v: lo <= v < hi, an open band below (lessMin) takes v < hi and one above (greaterMax)
// v >= lo; nullopt for NaN or outside every band. The BandAxis rule of analysis::HistogramBinner::binOf,
// applied here when colouring points so the DrawLists do not depend on the binner.
std::optional<std::size_t> bandIndexOf(double v, std::span<const analysis::Band> bands);

// c[0] + c[1] x + ... + c[n] x^n by Horner's rule (TrendResult's coefficients).
inline double polyval(const std::vector<double>& c, double x) {
    double y = 0.0;
    for (std::size_t k = c.size(); k-- > 0;) {
        y = y * x + c[k];
    }
    return y;
}

// ---- spans

// The slot's visible span (the worksheet zoom through the slot's mapper, clamped to its outing) cut at the
// boundaries of the source's "Outings:Laps" ranges so each piece carries its lap's trust flag. Empty in
// distance mode for a slot without distance data.
std::vector<analysis::SampleSpan> visibleSpans(const ComponentBuildInputs& in, const SlotData& slot);

// ---- text

std::string formatValue(double v, int decimals);
// Decimals that show a step of `step` exactly enough (0..6).
int decimalsForStep(double step);

// ---- axes

// Ticks whose labels stay inside [minPx, maxPx] along a horizontal axis / whose line stays inside along a
// vertical one.
std::vector<AxisTick> keepHorizontal(std::vector<AxisTick> ticks, const ITextMetrics& metrics, float dpr,
                                     float minPx, float maxPx);
std::vector<AxisTick> keepVertical(std::vector<AxisTick> ticks, float lineH, float minPx, float maxPx);

// ---- Chrome shared by every component

// The caption bar at the top (h == 0 when spec.caption.show is false).
RectPx captionRect(const ComponentBuildInputs& in);
// The caption text: the spec's text, or `autoText` when spec.caption.autoText.
std::string captionText(const ComponentBuildInputs& in, const std::string& autoText);
void emitCaption(DrawList& list, const RectPx& rect, const std::string& text, const Colours& colours,
                 float dpr);
// A Pending channel among the slots (D-022).
bool anyPending(const ComponentBuildInputs& in);
void emitPendingStripe(DrawList& list, const RectPx& area, const Colours& colours, float dpr);
void emitFocus(DrawList& list, const ComponentBuildInputs& in, const Colours& colours, float dpr);
// A banner line centred at the top of `area`.
void emitBanner(DrawList& list, const RectPx& area, float y, const std::string& text, const Colours& colours,
                float dpr);

} // namespace esses::plot::geom
