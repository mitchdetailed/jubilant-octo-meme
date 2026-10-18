// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The gauge_numeric and gauge_bar components (04 sections 16-17): their persisted bodies (schemas/components/
// gauge_numeric-1, gauge_bar-1) and their layer builders (GaugeGeometry, a name added by the M3 contract so
// that the gauges render through OffscreenRenderer / esses-render like every drawn component, 04 section 7.2
// item 9). Both are MainAndReference (Main plus the reference overlay) and cursor-driven:
//   numeric  Static = frame and caption; Dynamic = the value at the cursor (large; dynamicFont scales the
//            point size to fit with a 6 pt minimum), the reference overlay's value beneath in its key colour,
//            units when showUnits; the colour band containing the value colours the text (bandMode text) or
//            the background (bandMode background)
//   bar      Static = scale (scaleDps, showScale), colour bands (bandLines none | lines | fill), frame;
//            Dynamic = the bar from the origin (min | zero | max | custom) to the value in barColour (default
//            the channel colour; bandMode bar recolours it by band), the reference overlay as a thin marker
//            line in its key colour, the value text at the bar end; orientation vertical | horizontal
// SlotData::channels holds the one channel; the reference overlay is the second visible slot. The bodies'
// members are final (the schemas are the contract, AC-M3-08); src/geometry/gauge_geometry.cpp has the layouts
// (D-198 (a)): the numeric value sized through TextRun::scale, colour bands converted to the display unit,
// scales channel/auto from the Main channel's SummaryPyramid.
#pragma once

#include <esses/analysis/band_axis.hpp>
#include <esses/base/cancel.hpp>
#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/plotcore/component_bodies.hpp>
#include <esses/plotcore/component_build.hpp>
#include <esses/plotcore/draw_list.hpp>

#include <optional>
#include <string>
#include <vector>

namespace esses::plot {

struct GaugeNumericBody { // 04 section 16 item 4
    std::string channel;
    std::string displayUnit;
    std::optional<int> displayDps; // nullopt: the channel override's dps
    bool showUnits = true;
    bool dynamicFont = true;
    JsonDoc font; // null = the scheme's
    std::vector<ColourBand> colourBands;
    std::string bandMode = "text"; // text | background
    JsonDoc rest;

    static Result<GaugeNumericBody> parse(const JsonDoc& body);
    JsonDoc toJson() const;
};

struct GaugeBarBody { // 04 section 17 item 5
    std::string channel;
    std::string displayUnit;
    ScaleSpec scale;                      // mode channel | manual
    std::string orientation = "vertical"; // vertical | horizontal
    std::string originStyle = "min";      // min | zero | max | custom
    std::optional<analysis::UnitValue> customOrigin;
    std::optional<Rgba> barColour; // null = the channel colour
    int scaleDps = 0;
    bool showScale = true;
    std::vector<ColourBand> colourBands;
    std::string bandLines = "lines"; // none | lines | fill
    std::string bandMode = "none";   // none | bar
    JsonDoc rest;

    static Result<GaugeBarBody> parse(const JsonDoc& body);
    JsonDoc toJson() const;
};

class GaugeGeometry {
public:
    static Result<ComponentBuild> buildNumericStatic(const ComponentBuildInputs& inputs,
                                                     const CancelToken& cancel);
    static DrawList buildNumericDynamic(const ComponentBuildInputs& inputs);
    static DrawList buildNumericChrome(const ComponentBuildInputs& inputs, bool pending);
    static Result<ComponentBuild> buildBarStatic(const ComponentBuildInputs& inputs,
                                                 const CancelToken& cancel);
    static DrawList buildBarDynamic(const ComponentBuildInputs& inputs);
    static DrawList buildBarChrome(const ComponentBuildInputs& inputs, bool pending);
};

} // namespace esses::plot
