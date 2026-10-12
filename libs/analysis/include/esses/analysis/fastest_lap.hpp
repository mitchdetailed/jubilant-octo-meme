// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The fastest-lap candidate rule (06 section 5.2 selectFastest, AC-M1-07; used by CTRL+F, CTRL+ALT+F, the
// Fastest / Fastest 3 / Within 1 % filters and the Data window's delta-to-fastest column). Qt-free so that
// esses-cli laps --json and DataSelectionModel share one implementation.
//
// If the laps contain a trusted lap of class Flying, the candidates are the trusted laps of class Flying;
// otherwise the laps of class Flying or Partial, trusted or not (AC-M1-07's "trusted flying laps if any, else
// flying/partial"; D-135 and 06 section 5.2 write "if any trusted lap exists", which differs only when a user
// trusts an out, in, partial or pit lap while no flying lap is trusted: this reading then still offers the
// flying and partial laps instead of none). Out, In and Pit laps are never candidates. Disabled ranges are
// ignored. The fastest is the candidate with the minimum duration; ties go to the lowest number, then the
// earliest start.
#pragma once

#include <esses/model/range.hpp>

#include <span>
#include <vector>

namespace esses::analysis {

// Candidates in the order of `laps` (time order for a RangeGroup's ranges).
std::vector<const model::Range*> fastestLapCandidates(std::span<const model::Range> laps);
// nullptr when there is no candidate.
const model::Range* fastestLap(std::span<const model::Range> laps);
// Every candidate whose duration <= fastest * (1 + tolerancePct / 100), in lap order (CTRL+ALT+F, "Within 1
// %"); a negative tolerance counts as 0.
std::vector<const model::Range*> fastLapsWithin(std::span<const model::Range> laps, double tolerancePct);

} // namespace esses::analysis
