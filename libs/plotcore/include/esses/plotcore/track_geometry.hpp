// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The track_position component (04 section 14; the Track Map dock embeds the same builders, 06 section 9):
// its persisted body (04 section 14 item 6, schemas/components/track_position-1) and its layer builders
// (TrackGeometry; ARCHITECTURE_OUTLINE section 4 esses_plotcore). The map is ComponentBuildInputs::trackMap.
//   buildStatic   worker: the centreline (scheme role trackLine), optional inner/outer, section colouring by
//                 category, section marks and labels, fitted to the component with scale {fit | manual, zoom,
//                 panX, panY, rotation}; HitKind::Section regions
//   buildDynamic  GUI thread: the zoom span as a thicker sub-polyline between the track positions of zoom.a
//                 and zoom.b (role cursorZoom; it may cross the start line), cursor dots per visible slot
//                 (Main red at main.start + cursorT, overlays at k.start + cursorT in time mode, all at the
//                 same distance in distance mode), sizes {cursor, detail, material}
//   buildChrome   GUI thread: caption, focus border, the "no track map (Tools -> Track Editor)" banner
// The track position of a time t: the lap containing t through the slot's DistanceChain and its stretched
// DistanceIndex distance; a lap that does not start at a beacon is measured backwards from its end beacon
// (analysis::trackDistanceOfLapDistance). No index: the dot is hidden and the banner shows.
// The body's members are final; src/geometry/track_geometry.cpp (fit, sizes and the origin rule in its header
// comment). The positions interpolate the map's centreline in plotcore (TrackMap::pointAt is not needed for
// sub-polylines), and the origin rule is applied there too.
#pragma once

#include <esses/analysis/track_map.hpp>
#include <esses/base/cancel.hpp>
#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/plotcore/component_build.hpp>
#include <esses/plotcore/draw_list.hpp>
#include <esses/plotcore/hit_tester.hpp>

#include <optional>
#include <string>

namespace esses::plot {

struct TrackPositionBody { // 04 section 14 item 6
    bool showZoomSpan = true;
    bool showSectionLabels = false;
    bool showSectionMarks = true;
    bool showSectionColours = true;
    float sizeCursor = 6.0F; // sizes.cursor (logical px)
    float sizeDetail = 2.0F;
    float sizeMaterial = 2.0F;
    std::string scaleMode = "fit"; // scale.mode: fit | manual
    double zoom = 1.0;
    double panX = 0.0;
    double panY = 0.0;
    double rotationDeg = 0.0;
    bool background = false;
    JsonDoc rest;

    static Result<TrackPositionBody> parse(const JsonDoc& body);
    JsonDoc toJson() const;
};

// ENU metres -> device px.
struct TrackFit {
    double scale = 1.0; // px per metre
    double offsetX = 0.0;
    double offsetY = 0.0;
    double rotationRad = 0.0;
    PointPx toPx(double xM, double yM) const noexcept;
};

class TrackGeometry {
public:
    static TrackFit fit(const analysis::TrackMap& map, RectPx area, const TrackPositionBody& body) noexcept;
    // The track distance of layer time `t` in a slot (nullopt: no index for that lap).
    static std::optional<double> trackDistanceAt(const SlotData& slot, const analysis::TrackMap& map,
                                                 Micros t);
    static Result<ComponentBuild> buildStatic(const ComponentBuildInputs& inputs, const CancelToken& cancel);
    static DrawList buildDynamic(const ComponentBuildInputs& inputs);
    static DrawList buildChrome(const ComponentBuildInputs& inputs, bool pending);
};

} // namespace esses::plot
