// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The time_distance component (04 section 8): its persisted body (04 section 8.14) and its three layer
// builders (04 sections 5.2 and 6.3). Qt-free so that the widget (esses_widgets TimeDistanceGraph) and
// OffscreenRenderer / esses-render produce identical images (AC-M1-05) and the DrawList JSON goldens run on
// every OS (AC-M1-04).
//   buildStatic   worker, Priority::Visible: background, key column background, grid, group panels, the zero
//                 scale line, traces (TraceBuilder::decimateMany, overlays n..1, Overlay 0, Main on top; the
//                 active channel last within a slot), y axes per group, the x axis (time or distance), key
//                 labels; plus the HitTester (panels, key labels and values, axes)
//   buildDynamic  GUI thread, never blocks: cursor and datum lines, key values at the cursor per visible slot
//                 (Resampler::sampleAt over Column::tryPin; "..." when absent or pending)
//   buildChrome   GUI thread: caption bar, focus border, active-channel square, pending stripe and the
//                 "no distance data" banner (04 section 5.2 places the banner in Chrome)
// All three share one layout (layoutOf), a pure function of the inputs that does not read samples, so the
// Dynamic and Chrome lists line up with the Static layer without being rebuilt with it.
// Layout (04 section 8.1, logical px x dpr): caption bar 18 (when caption.show), group panels with the key
// column (key.pos left|right, KeyLayout width), a 36 px vertical axis strip and the trace area, then the
// x axis (18). Panels divide the trace area by each group's edges [top, bottom] (fractions, 0 = top); when
// every group keeps the default [0, 1] and there are several, they share the height equally.
// Scales (04 section 8.3): manual {scaleMin, scaleMax} converted to the group's axis unit (the display unit
// of its first resolved trace); channel/auto = the union over the group's visible traces of the Main source's
// whole-column min/max from the SummaryPyramid (the M1 stand-in for AutoscaleCache: the whole file, all
// laps), falling back to the drawn columns while the summary is incomplete; degenerate spans widen to +-1
// display unit; a group without valid data draws no axis labels. M1 scope (07 section 4.1): groups and
// panels, Manual vs Channel/Auto scale, cursor, zoom, pan, F9, overlays in both domains, D-017 colours;
// overlapped mode, measurements, variance, bands, the status strip, the scrollbar and the AutoscaleCache
// options later.
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/result.hpp>
#include <esses/model/opaque_json.hpp>
#include <esses/plotcore/component_build.hpp>
#include <esses/plotcore/draw_list.hpp>
#include <esses/plotcore/key_layout.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::plot {

enum class ScaleMode : std::uint8_t { Channel, Auto, Manual };

struct TdTrace {
    std::string channel;
    bool visible = true;
    std::string displayUnit; // "" = the channel override's
    std::optional<int> dps;
};

struct TdGroup {
    std::string name;
    ScaleMode scaleMode = ScaleMode::Channel;
    double edgeTop = 0.0; // "edges": [top, bottom] fractions (panels mode)
    double edgeBottom = 1.0;
    std::optional<double> scaleMin; // manual scale, in scaleUnit ("" = base units)
    std::optional<double> scaleMax;
    std::string scaleUnit; // the unit the manual scale was persisted with ({v, unit})
    bool zeroLine = true;  // lines.zero.enabled: drawn when the scale crosses 0
    std::vector<TdTrace> traces;
};

struct TimeDistanceBody { // 04 section 8.14 (M1 subset; the rest of the object is preserved in `rest`)
    std::string traceStyle = "line";
    float lineWidth = 1.0F;
    bool showGrid = true;
    std::string keyMode = "labels"; // labels | values | none
    std::string keyPos = "left";    // left | right
    bool keyValues = true;
    bool showXAxis = true; // display.axis.showX
    bool showYAxis = true; // display.axis.showY
    std::vector<TdGroup> groups;
    model::OpaqueJson rest; // the body as parsed: unknown members survive toJson()

    // Invalid with context.detail (a JSON pointer) when a known member has the wrong type.
    static Result<TimeDistanceBody> parse(const model::OpaqueJson& body);
    // `rest` with the typed members written over it: members the source had are replaced, members it lacked
    // are added only when they differ from their defaults, so parse(x).toJson() == x for any canonical x.
    model::OpaqueJson toJson() const;
    // Channel names in display order (Values window "focused component" rows, 06 section 8); the order of
    // ResolvedChannel in SlotData::channels.
    std::vector<std::string> channelNames() const;
};

// The shared geometry of the three layers (device px).
struct TdGroupLayout {
    int group = 0;            // index into TimeDistanceBody::groups
    RectPx panel;             // trace area of the group
    RectPx axisStrip;         // vertical axis strip
    RectPx key;               // the group's part of the key column
    std::vector<int> traces;  // flat channel index (channelNames order) per key row
    std::vector<KeyRow> rows; // Static labels, value width templates
    KeyGeometry keyGeometry;  // rows relative to `key`
};

struct TdLayout {
    float dpr = 1.0F;
    RectPx bounds;
    RectPx caption; // h == 0 when hidden
    RectPx keyColumn;
    RectPx traceArea; // all panels
    RectPx xAxis;     // h == 0 when hidden
    int traceWidthPx = 0;
    std::vector<TdGroupLayout> groups;
};

class TimeDistanceBuilder {
public:
    static Result<ComponentBuild> buildStatic(const ComponentBuildInputs& inputs, const CancelToken& cancel);
    static DrawList buildDynamic(const ComponentBuildInputs& inputs);
    static DrawList buildChrome(const ComponentBuildInputs& inputs, bool pending);
    // The layout every builder uses (exposed for hit tests in widgets and for tests). A body that does not
    // parse lays out as an empty graph.
    static TdLayout layoutOf(const ComponentBuildInputs& inputs);
};

} // namespace esses::plot
