// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathSelection (03 section 7.5, D-017, D-023): the maths-local copy of the data selection. esses_maths
// (layer 2) cannot see plot::SelectionSnapshot (layer 3), so MathsService builds this copy from the snapshot
// (06 section 5.1) and hands it to MathGraph::setSelection; SelectionDependent nodes (ungated stat_*,
// variance_*, and everything downstream of them) re-key on MathSelection::key (03 section 7.2). Copied by
// value into every EvalContext (D-031). Moved here from eval_context.hpp in M2 (03 section 1 lists
// MathSelection.h).
#pragma once

#include <esses/base/hash.hpp>
#include <esses/base/time.hpp>
#include <esses/model/range.hpp>

#include <optional>
#include <vector>

namespace esses::maths {

struct MathRangeGeom { // copied from model::Range
    model::RangeRef ref;
    Micros start = 0;
    Micros end = 0;
    Micros offsetUs = 0;
    double offsetM = 0.0;
    double stretch = 1.0;
};

struct MathSelection {
    MathRangeGeom main;
    std::optional<MathRangeGeom> reference; // SelectionSnapshot::referenceOverlay() (D-017)
    std::vector<model::RangeRef>
        overlays; // Overlay 0 then Overlays 1..n (per-source math window, 03 section 7.6)
    Hash128 key;  // selectionKey (03 section 7.2), computed once by MathsService
};

} // namespace esses::maths
