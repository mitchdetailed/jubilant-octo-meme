// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SectionDetector (ARCHITECTURE_OUTLINE section 4 esses_analysis; 06 section 13.5 item 3; D-027): the default
// sections of a track map: Turns and Straights from the centreline curvature, each at least
// TrackMap::kMinSectionM long. defaults() replaces the "Default" set's generated sections (userDefined ==
// false) and keeps every user section; generated sections are named "Turn n" / "Straight n" in track order
// with ids "t<n>" / "s<n>" and categories "corner" / "straight", and they tile [0, lengthM] without gaps.
// AC-M3-04: the 10 m minimum holds; apexes are compared with the GPS map's within 20 m of lap distance.
// Curvature: the heading of chords of +-smoothingM / 2 and its change over the same span, so a corner's
// detected extent reaches about smoothingM into the straights around it. Short runs are absorbed shortest
// first (a short corner joins the straight; a short straight joins two corners turning the same way and is
// split between an S-bend's two), and a corner turning less than minTurnDeg joins the straight (a kink, or
// the wander of a noisy GPS line); a run across the start line of a closed map is cut there. The generated
// sections come first in track order, then the kept sections in their order; a generated id already used by
// a kept section gets a suffix ("t1-2"). minSectionM below kMinSectionM is raised to it. Owner: src/track/.
#pragma once

#include <esses/analysis/track_map.hpp>
#include <esses/base/result.hpp>

namespace esses::analysis {

struct SectionDetectorOptions {
    double minSectionM = TrackMap::kMinSectionM;
    double cornerRadiusM = 200.0; // curvature above 1 / cornerRadiusM (after smoothing) is a corner
    double smoothingM = 20.0;     // curvature smoothing window
    double minTurnDeg = 15.0;     // a corner turns at least this much; a weaker bend is part of the straight
};

class SectionDetector {
public:
    // Invalid when the map has no centreline or lengthM <= 0, or for options out of range.
    static Result<void> defaults(TrackMap& map, const SectionDetectorOptions& options = {});
};

} // namespace esses::analysis
