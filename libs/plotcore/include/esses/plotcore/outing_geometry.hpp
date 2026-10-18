// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The outing component (04 section 9): its persisted body (time_distance's groups/traces body minus variance,
// key.stats and statusChannels, plus enableOverlays; schemas/components/outing-1) and its layer builders
// (OutingGeometry; ARCHITECTURE_OUTLINE section 4 esses_plotcore).
//   buildStatic   worker: the whole Main outing (every lap) drawn from SummaryPyramid only (04 section
//                 4.8: never pins chunks, within the first-paint budget), the range band with lap boundaries
//                 (BCN ticks; double-click zooms the worksheet to a lap); enableOverlays draws the visible
//                 overlays' outings aligned at lap start (time mode only); distance mode: Main only, x in
//                 metres from the outing start (the Main DistanceChain with its origin there)
//   buildDynamic  GUI thread: the shared zoom as a translucent rectangle (role cursorZoom) across all panels,
//                 shifted by -map_M.outingExtent().a, with HandleKind::ZoomStart/ZoomEnd/ZoomPan regions
//                 (drag ends: setZoom; drag bottom edge: pan at constant span), the cursor line through it
//   buildChrome   GUI thread: caption, focus border, pending stripe
// Not zoomable: UP/DOWN/F2/W, the wheel and the rubber band go to the worksheet zoom; links.zoom is forced
// on.
// src/geometry/outing_geometry.cpp (layout in its header comment). A Dynamic build carries no HitTester, so
// the widget derives the ZoomStart / ZoomEnd / ZoomPan handle regions from zoomRect() (its ends and bottom
// edge); channelNames() is final.
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/plotcore/component_build.hpp>
#include <esses/plotcore/draw_list.hpp>
#include <esses/plotcore/time_distance_builder.hpp>

#include <string>
#include <vector>

namespace esses::plot {

struct OutingBody {         // 04 section 9 item 6
    TimeDistanceBody graph; // the shared groups/traces/display members
    bool enableOverlays = false;

    static Result<OutingBody> parse(const JsonDoc& body);
    JsonDoc toJson() const;
    std::vector<std::string> channelNames() const;
};

class OutingGeometry {
public:
    static Result<ComponentBuild> buildStatic(const ComponentBuildInputs& inputs, const CancelToken& cancel);
    static DrawList buildDynamic(const ComponentBuildInputs& inputs);
    static DrawList buildChrome(const ComponentBuildInputs& inputs, bool pending);
    // The zoom rectangle in device px (w == 0 when the zoom lies outside the outing).
    static RectPx zoomRect(const ComponentBuildInputs& inputs);
};

} // namespace esses::plot
