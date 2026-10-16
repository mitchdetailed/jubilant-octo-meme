// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The time_distance component (04 section 8): its persisted body (04 section 8.14) and its three layer
// builders (04 sections 5.2 and 6.3). Qt-free so that the widget (esses_widgets TimeDistanceGraph) and
// OffscreenRenderer / esses-render produce identical images (AC-M1-05) and the DrawList JSON goldens run on
// every OS (AC-M1-04, AC-M3-02).
//   buildStatic   worker, Priority::Visible: background, key column background, the range / section / marker
//                 bands (BandLayout), grid, group panels (or the overlapped area), the zero / min / max scale
//                 lines, traces (TraceBuilder::decimateMany, overlays n..1, Overlay 0, Main on top; the
//                 active channel last within a slot), annotation lines, the status strip, the variance panel
//                 (one analysis::VarianceTable per visible overlay, none without one: AC-M3-05), y axes, the
//                 x axis (time or distance), the offset axis, the scrollbars, the Lo/Hi handles, key labels
//                 and the measurement lines (min / max / avg over the visible span, read on the worker); plus
//                 the HitTester (panels, key labels, values and measurements, axes, bands, markers,
//                 annotations, status rows, the variance panel, the offset sub-axes, scrollbars, Lo/Hi and
//                 splitter handles)
//   buildDynamic  GUI thread, never blocks: cursor (line, crosshair or none) and datum lines, key values at
//   the
//                 cursor per visible slot, with a datum value@datum and delta (Resampler::sampleAt over
//                 Column::tryPin; "..." when absent or pending)
//   buildChrome   GUI thread: caption bar, focus border, active-channel square, pending stripe, the
//                 "no distance data" banner and the variance panel's "variance needs distance data" banner
// All three share one layout (layoutOf), a pure function of the inputs that does not read samples, so the
// Dynamic and Chrome lists line up with the Static layer without being rebuilt with it (a datum appearing or
// disappearing changes the key columns, so the widget rebuilds the Static layer then).
// Layout (04 section 8.1, logical px x dpr): caption bar 18 (when caption.show); the bands (14 each: range
// when showBands, section when showBands and bandMode "sections", marker when showMarkers) over the trace
// area; group panels with the key column (key.pos left|right, KeyLayout width), a 36 px vertical axis strip
// and the trace area; then the status strip (10 per status channel, showStatusStrip), the variance panel
// (variance.heightFrac of the trace area, while variance.show and an overlay is visible), the x axis (18),
// the offset axis (2 x 12, while ViewState::offsetAxis) and the horizontal scrollbar (12,
// axis.showXScrollbar). Panels mode divides the trace area by each group's edges [top, bottom] (fractions, 0
// = top); when every group keeps the default [0, 1] and there are several, they share the height equally.
// Overlapped mode (overlapMode "overlapped", G) gives each group its [lo, hi] region of the whole area
// (regions may overlap), draws the active group's y axis and grid, and puts a Lo and a Hi handle per group at
// the right edge. With no M3 element shown the layout and every list equal the M1 builder's (the M1 goldens).
// Scales (04 section 8.3): manual {scaleMin, scaleMax} converted to the group's axis unit (the display unit
// of its first resolved trace); channel = the union over the group's visible traces of each channel's
// override limits (ResolvedChannel::limitMin / limitMax) where set, else (and for auto) the Main source's
// whole-column min/max from the SummaryPyramid (the M1 stand-in for AutoscaleCache: the whole file, all
// laps), falling back to the drawn columns while the summary is incomplete; degenerate spans widen to +-1
// display unit; a group without valid data draws no axis labels; the widget's vertical zoom
// (TdLocalState::vZoom) then shows a fraction of that scale.
#pragma once

#include <esses/analysis/band_axis.hpp>
#include <esses/base/cancel.hpp>
#include <esses/base/result.hpp>
#include <esses/model/ids.hpp>
#include <esses/model/opaque_json.hpp>
#include <esses/plotcore/band_layout.hpp>
#include <esses/plotcore/component_build.hpp>
#include <esses/plotcore/draw_list.hpp>
#include <esses/plotcore/key_layout.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace esses::plot {

enum class ScaleMode : std::uint8_t { Channel, Auto, Manual };

struct TdTrace {
    std::string channel;
    bool visible = true;
    std::string displayUnit; // "" = the channel override's
    std::optional<int> dps;
};

// A scale line of a group (04 section 8.3 item 5): lines.min / lines.max ("normal operating range" at the
// channel override limits). The zero line keeps the M1 members zeroLine / zeroColour.
struct TdScaleLine {
    bool enabled = false;
    std::optional<Rgba> colour; // nullopt: the scheme role (cptForeground2)
    friend bool operator==(const TdScaleLine&, const TdScaleLine&) = default;
};

struct TdGroup {
    std::string name;
    ScaleMode scaleMode = ScaleMode::Channel;
    double edgeTop = 0.0; // "edges": [top, bottom] fractions (panels mode)
    double edgeBottom = 1.0;
    double lo = 0.0; // overlapped mode: the group's region [lo, hi] (fractions, 0 = top)
    double hi = 1.0;
    std::optional<double> scaleMin; // manual scale, in scaleUnit ("" = base units)
    std::optional<double> scaleMax;
    std::string scaleUnit;          // the unit the manual scale was persisted with ({v, unit})
    bool zeroLine = true;           // lines.zero.enabled: drawn when the scale crosses 0
    std::optional<Rgba> zeroColour; // lines.zero.colour
    TdScaleLine minLine;            // lines.min
    TdScaleLine maxLine;            // lines.max
    std::vector<TdTrace> traces;
};

// display.variance (04 section 8.8, D-023): manual limits in seconds (distance mode) or metres (time mode).
struct TdVariance {
    bool show = false;
    double heightFrac = 0.15;
    bool autoScale = true;
    analysis::UnitValue min{-2.0, "s"};
    analysis::UnitValue max{2.0, "s"};
    bool hasMin = false; // the source had the member (written back unchanged)
    bool hasMax = false;
};

struct TimeDistanceBody {            // 04 section 8.14 (the rest of the object is preserved in `rest`)
    std::string traceStyle = "line"; // line | points (S)
    float lineWidth = 1.0F;
    float pointSize = 2.0F;
    bool showGrid = true;
    bool showMarkers = true;
    bool showBands = true;
    std::string bandMode = "sections"; // sections | none
    std::string bandGroup = "Outings:Laps:Track Sections:Default";
    std::string overlapMode = "panels"; // panels | overlapped (G)
    std::string cursorStyle = "line";   // line | crosshair | none (Y)
    bool cursorSnap = true;
    std::string keyMode = "labels"; // labels | values | none
    std::string keyPos = "left";    // left | right
    bool keyValues = true;
    bool keyStats = false;        // key.stats: the measurements row (M)
    bool showStatusStrip = false; // E
    std::vector<std::string> statusChannels;
    bool showDeviceEvents = true; // SHIFT+E
    TdVariance variance;          // F3
    bool showXAxis = true;        // display.axis.showX
    bool showYAxis = true;        // display.axis.showY
    bool showXScrollbar = true;   // display.axis.showXScrollbar
    bool showYScrollbar = true;   // display.axis.showYScrollbar
    std::vector<TdGroup> groups;
    model::OpaqueJson rest; // the body as parsed: unknown members survive toJson()

    bool overlapped() const noexcept { return overlapMode == "overlapped"; }

    // Invalid with context.detail (a JSON pointer) when a known member has the wrong type or value.
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
    RectPx panel;             // trace area of the group (overlapped mode: its [lo, hi] region)
    RectPx axisStrip;         // vertical axis strip
    RectPx key;               // the group's part of the key column
    std::vector<int> traces;  // flat channel index (channelNames order) per key row
    std::vector<KeyRow> rows; // Static labels, value width templates
    KeyGeometry keyGeometry;  // rows relative to `key`
    RectPx loHandle;          // overlapped mode: the Lo and Hi handles at the right edge (w == 0 otherwise)
    RectPx hiHandle;
    RectPx vScroll; // the active group's vertical scrollbar (axis.showYScrollbar; w == 0 otherwise)
};

// A status-strip channel (04 section 8.10 item 1) as resolved in the Main slot.
struct TdStatusChannel {
    std::string name;
    std::optional<model::ChannelDescriptor> descriptor;
    std::optional<store::Series> series;
    Rgba colour = 0x000000FF;
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
    // M3 (all h == 0 when hidden)
    BandLayoutInput bandFlags; // which bands show (the cells are filled by the Static build)
    BandGeometry bands;        // range, section and marker bands over the trace area's x range
    RectPx statusStrip;        // the status rows, statusRowHeight each
    float statusRowHeight = 0.0F;
    std::vector<TdStatusChannel> statusChannels;
    RectPx variance;     // the variance panel (trace area x range)
    RectPx varianceAxis; // its axis strip
    RectPx offsetMain;   // offset axis: the Main and the reference overlay sub-axes
    RectPx offsetReference;
    RectPx scrollbar;       // horizontal scrollbar
    int referenceSlot = -1; // index into inputs.slots of the reference overlay (-1: none)
    int activeGroup = -1;   // the group of the active channel (else 0; -1 without groups)
    bool overlapped = false;
    KeyColumns keyColumns;
};

// Widget-local state the builders read besides ComponentBuildInputs (04 section 8.3 item 6: not persisted,
// not linked). The default reproduces what OffscreenRenderer draws.
struct TdGroupZoom { // the shown fraction of a group's scale (0 = scale min, 1 = scale max)
    double lo = 0.0;
    double hi = 1.0;
    friend bool operator==(const TdGroupZoom&, const TdGroupZoom&) = default;
};
struct TdLocalState {
    std::map<int, TdGroupZoom> vZoom; // by group index
    // The scales (display units, vertical zoom applied) of the last Static build by group, so the Dynamic
    // crosshair matches the drawn axes; empty: the Dynamic layer computes them from the summaries.
    std::vector<std::optional<std::pair<double, double>>> builtScales;
};

// What a Static build placed, for the widget's mouse and tooltips (domain units are those of the x axis:
// microseconds or metres from the Main range start).
struct TdBandEntry {
    double x0 = 0.0;
    double x1 = 0.0;
    std::string label;
    int slot = -1; // range band: the visible slot, -1 for another range of the Main source's group
};
struct TdMarkerEntry {
    model::MarkerId id; // an annotation's id is its Uuid
    BandMarkerKind kind = BandMarkerKind::Beacon;
    std::string text;
    double x = 0.0;
    Micros t = 0; // absolute layer time
};
struct TdMeasurementEntry {
    int group = -1;
    int trace = -1; // flat channel index
    int slot = -1;
    bool valid = false;
    double minX = 0.0; // domain units
    double maxX = 0.0;
    Micros minT = 0; // absolute time in the slot's source
    Micros maxT = 0;
};
struct TdStaticInfo {
    std::vector<TdBandEntry> ranges;   // HitResult::band
    std::vector<TdBandEntry> sections; // HitResult::section
    std::vector<TdMarkerEntry> markers;
    std::vector<TdMeasurementEntry> measurements;
    std::vector<std::optional<std::pair<double, double>>> scales; // per group, display units
    std::vector<std::string> statusNames;
    XSpan scrollExtent;     // the Main outing extent the scrollbar spans
    int varianceTables = 0; // VarianceTable::build calls (AC-M3-05: none without an overlay)
    bool varianceNoData = false;
    bool varianceSpansLaps = false; // Main or an overlay spans laps: no table, the banner (D-201)
};

class TimeDistanceBuilder {
public:
    static Result<ComponentBuild> buildStatic(const ComponentBuildInputs& inputs, const CancelToken& cancel);
    static Result<ComponentBuild> buildStatic(const ComponentBuildInputs& inputs, const TdLocalState& local,
                                              const CancelToken& cancel, TdStaticInfo* info = nullptr);
    static DrawList buildDynamic(const ComponentBuildInputs& inputs);
    static DrawList buildDynamic(const ComponentBuildInputs& inputs, const TdLocalState& local);
    static DrawList buildChrome(const ComponentBuildInputs& inputs, bool pending);
    // The layout every builder uses (exposed for hit tests in widgets and for tests). A body that does not
    // parse lays out as an empty graph.
    static TdLayout layoutOf(const ComponentBuildInputs& inputs);

    // The visible span of a group's vertical zoom applied to a scale (04 section 8.3 item 6).
    static std::pair<double, double> zoomedScale(double vMin, double vMax, const TdGroupZoom& zoom) noexcept;
    // The offset of a slot as the offset axis writes it: "+0.120 s" (time) or "-3.5 m" (distance).
    static std::string offsetText(Domain domain, const RangeGeom& range);
};

} // namespace esses::plot
