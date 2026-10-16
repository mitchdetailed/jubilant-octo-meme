// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// BandLayout (04 sections 5.5, 8.1 and 8.11; ARCHITECTURE_OUTLINE section 4 esses_plotcore): the range,
// section and marker bands above the panels of time_distance (and the range band of outing, 04 section 9 item
// 5), each 14 logical px, stacked top to bottom and omitted when hidden.
//   range band    one cell per visible slot's range covering [xOf(start), xOf(end)), labelled with the range
//                 name (and the source short name when several sources are selected); clicks never move the
//                 cursor, a double-click zooms to the range
//   section band  the ranges of the body's bandGroup under the Main lap, coloured by category (scheme roles
//                 sectionCorner / sectionStraight), labelled when wider than the label
//   marker band   beacons (BCN, SOL, EOL) as ticks, annotations as rectangles with hover text, device
//                 events as triangles (scheme role deviceEvent)
// The caller maps times to x (DomainMapper per slot); the builder only lays out and emits, Static layer, and
// adds HitKind::Band / Section / Marker / Annotation regions (band = cell index, section = section index,
// markerId) to the HitTester.
// layout() and emit(): src/band_layout.cpp. Range labels are elided to their cell, section labels drawn only
// when they fit; cell edges are drawn as ticks (the lap boundaries of the outing graph); later cells and
// markers paint over earlier ones and win hits.
#pragma once

#include <esses/model/ids.hpp>
#include <esses/plotcore/draw_list.hpp>
#include <esses/plotcore/hit_tester.hpp>
#include <esses/plotcore/text_metrics.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace esses::plot {

struct BandCell {
    float x0 = 0.0F; // device px, trace-area coordinates (may extend beyond it; clipped)
    float x1 = 0.0F;
    std::string label;
    Rgba colour = 0;
    int slot = -1;        // range band: the slot
    int index = -1;       // range or section index the HitResult carries
    std::string category; // section band: "corner", "straight", or a user category
};

enum class BandMarkerKind : std::uint8_t { Beacon, Annotation, DeviceEvent };

struct BandMarker {
    float x = 0.0F;
    BandMarkerKind kind = BandMarkerKind::Beacon;
    std::string text; // hover text (annotation text, device event comment, beacon class)
    model::MarkerId id;
    Rgba colour = 0;
};

struct BandLayoutInput {
    bool showRanges = true;
    bool showSections = true; // bandMode == "sections"
    bool showMarkers = true;
    std::vector<BandCell> ranges;
    std::vector<BandCell> sections;
    std::vector<BandMarker> markers;
};

struct BandColours {
    Rgba text = 0x000000FF;
    Rgba background = 0xFFFFFFFF;
    Rgba frame = 0x808080FF;
    Rgba sectionCorner = 0xFFD0A0FF;
    Rgba sectionStraight = 0xD0E0FFFF;
    Rgba marker = 0x000000FF;
    Rgba annotation = 0x2060FFFF;
    Rgba deviceEvent = 0xC04000FF;
};

struct BandGeometry {
    RectPx range;          // h == 0 when hidden
    RectPx section;        // h == 0 when hidden
    RectPx marker;         // h == 0 when hidden
    float heightPx = 0.0F; // total
};

class BandLayout {
public:
    static constexpr float kBandHeight = 14.0F; // logical px (04 section 8.1)

    // The bands stacked from the top of `area` (device px); the trace x origin is area.x.
    static BandGeometry layout(const BandLayoutInput& input, RectPx area, float dpr);
    static void emit(DrawList& list, const BandLayoutInput& input, const BandGeometry& geometry,
                     const ITextMetrics& metrics, const BandColours& colours, float dpr, HitTester* hits);
};

} // namespace esses::plot
