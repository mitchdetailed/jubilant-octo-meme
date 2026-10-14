// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The histogram component (04 section 11): its persisted body (04 section 11.2,
// schemas/components/histogram-1) and its layer builders (HistogramGeometry; ARCHITECTURE_OUTLINE section 4
// esses_plotcore), shared by the widget (esses_widgets Histogram) and OffscreenRenderer / esses-render.
//   buildStatic   worker, Priority::Visible, on Zoom | Selection | Data | Offsets: bins from the BandAxis
//                 (width | count; dataMin/dataMax auto = the active channel's scale), HistogramBinner per
//                 trace and slot (count | percent | time, gating, excludeUntrusted, colour sub-bars), style
//                 bar | line | text (a table of TextRuns), layout overlapped | tiled, overlays outlined
//                 beneath Main, the y scale auto | manual, binLabelStep, the colour legend (K); HitKind::Bin
//                 regions
//   buildDynamic  GUI thread: the bin of the active channel's value at the cursor highlighted (no datum)
//   buildChrome   GUI thread: caption, focus border, pending stripe
// SlotData::channels order (componentChannelUses): the traces, then the colour channel and the gating channel
// when set.
// The body's members are final; src/geometry/histogram_geometry.cpp (layout and styles in its header comment;
// the frame / scene / emit split in src/geometry/plot_scenes.hpp). The bins and colour bands come from
// analysis::generateBands, the counts from analysis::HistogramBinner; a failing (Unsupported) binner fails
// the Static build.
#pragma once

#include <esses/analysis/band_axis.hpp>
#include <esses/analysis/histogram_binner.hpp>
#include <esses/base/cancel.hpp>
#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/plotcore/component_bodies.hpp>
#include <esses/plotcore/component_build.hpp>
#include <esses/plotcore/draw_list.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace esses::plot {

struct HistogramTrace {
    std::string channel;
    bool visible = true;
};

struct HistogramBody { // 04 section 11.2
    std::vector<HistogramTrace> traces;
    analysis::BandAxis bins;
    std::string yMode = "percent"; // count | percent | time
    ScaleSpec yScale;              // mode auto | manual
    bool excludeUntrusted = true;
    bool stackLines = false;
    std::string style = "bar";         // bar | line | text
    std::string layout = "overlapped"; // overlapped | tiled
    int tileCols = 0;
    int binLabelStep = 1;
    bool condensedAxes = false;
    std::string colourChannel;                    // colourChannel.channel ("" = none)
    std::optional<analysis::BandAxis> colourAxis; // colourChannel.axis (null = none)
    std::string gatingChannel;                    // gating.channel ("" = none)
    JsonDoc rest;

    static Result<HistogramBody> parse(const JsonDoc& body);
    JsonDoc toJson() const;
    std::vector<ChannelUse> channels() const; // traces, colour, gating
};

// The values of a Static build as a table (Export CSV, 04 section 11.1 item 10; D-198): the bins in binUnit
// and one column per drawn set in the body's yMode.
struct HistogramExportColumn {
    int trace = 0;              // body trace
    std::size_t slot = 0;       // index into ComponentBuildInputs::slots
    std::vector<double> values; // per bin
};
struct HistogramExport {
    std::vector<analysis::Band> bins;
    std::string binUnit;
    std::string yMode = "percent";
    std::vector<HistogramExportColumn> columns; // Main first, then Overlay 0 and Overlays 1..n; trace order
};

class HistogramGeometry {
public:
    static Result<ComponentBuild> buildStatic(const ComponentBuildInputs& inputs, const CancelToken& cancel);
    static DrawList buildDynamic(const ComponentBuildInputs& inputs);
    static DrawList buildChrome(const ComponentBuildInputs& inputs, bool pending);
    // Worker: the bins and values buildStatic draws (HistogramBinner per visible slot and trace); a body that
    // does not parse, Cancelled and Unsupported fail.
    static Result<HistogramExport> exportTable(const ComponentBuildInputs& inputs, const CancelToken& cancel);
};

} // namespace esses::plot
