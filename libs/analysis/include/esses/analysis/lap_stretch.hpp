// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LapStretch (D-016; ARCHITECTURE_OUTLINE section 4; 06 sections 13.3 item 1 and 13.6 item 4; moved from M1
// to M2 by D-044): lap distance stretching. Each lap's DistanceIndex holds a per-lap scalar `stretch` that
// scales the lap's measured length to the track length when the two agree within the tolerance
// (|lengthM - trackLength| / trackLength <= tolerance; default 5 %, project option options.lapStretch,
// toolbar toggle data.lap_stretch_toggle). The Corr Dist channel itself is never stretched (03 section 9.1
// item 7): the index keeps its unstretched tables and DistanceChain / DomainMapper multiply by stretch() (04
// section 3.1). The stretch is part of the index's key (D-140), so apply() runs on the index before it is
// published (an index already shared is copied, never changed). The report rows feed the Lap Editor's
// Stretch column (blank outside the tolerance) and the Corrected Speed and Distance dialog's read-only table.
#pragma once

#include <esses/analysis/distance_index.hpp>
#include <esses/model/ids.hpp>

#include <span>
#include <utility>
#include <vector>

namespace esses::analysis {

// project.json options.lapStretch (02 section 6.2): the toolbar toggle and the tolerance as a fraction
// (tolerancePct / 100).
struct LapStretchSettings {
    bool enabled = true;
    double tolerance = 0.05; // LapStretch::kDefaultTolerance
    friend bool operator==(const LapStretchSettings&, const LapStretchSettings&) = default;
};

struct LapStretchRow {
    model::RangeId lap;
    double lengthM = 0.0;         // measured lap length (DistanceIndex::lengthM)
    double trackLengthM = 0.0;    // the track length it was compared with (0: unknown)
    double stretch = 1.0;         // the factor in effect (1 when off or outside the tolerance)
    bool applied = false;         // stretching is on and the lap is within the tolerance
    bool withinTolerance = false; // the D-016 test alone, also while the toggle is off
    double candidate = 1.0;       // trackLength / length within the tolerance (also when off), else 1
    // The Lap Editor's "Stretch (%)": (stretch - 1) * 100.
    double percent() const noexcept { return (stretch - 1.0) * 100.0; }
};

class LapStretch {
public:
    static constexpr double kDefaultTolerance = 0.05; // 5 % (D-016)

    // trackLengthM / lengthM when both are positive and |lengthM - trackLengthM| / trackLengthM <= tolerance,
    // else 1.
    static double factor(double lengthM, double trackLengthM, double tolerance) noexcept;
    // Sets the stretch of `index` from factor() and returns the report row for `lap` (stretching on).
    static LapStretchRow apply(DistanceIndex& index, const model::RangeId& lap, double trackLengthM,
                               double tolerance);
    // The same with the project settings: with stretching off the index's stretch is 1 and the row still
    // reports whether the lap is within the tolerance.
    static LapStretchRow apply(DistanceIndex& index, const model::RangeId& lap, double trackLengthM,
                               const LapStretchSettings& settings);
    // The row of one lap without changing anything (the report of an index that is already published).
    static LapStretchRow row(const DistanceIndex& index, const model::RangeId& lap, double trackLengthM,
                             const LapStretchSettings& settings);
    // The per-lap report in the order given (06 section 13.3 item 1, 13.6 item 4); a lap without an index
    // (null) reports length 0 and no stretch.
    static std::vector<LapStretchRow>
    report(std::span<const std::pair<model::RangeId, const DistanceIndex*>> laps, double trackLengthM,
           const LapStretchSettings& settings);
};

} // namespace esses::analysis
