// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// EvalContext and MathSelection (03 sections 7.5 and 7.6; D-050; 01 section 5.3 rule 9). EvalContext holds no
// reference or raw pointer to GUI-lifetime state: shared inputs are std::shared_ptr<const ...> snapshots and
// the services a task calls back into (TaskPool, Log) are process-lifetime runtime objects.
// MathSelection is the maths-local copy of the selection: esses_maths (layer 2) cannot see
// plot::SelectionSnapshot (layer 3), so MathsService builds it from the snapshot (06 section 5.1).
//
// M1 deviation: EvalContext::getChannel (03 section 7.5, a MathInputGatherer factory for Variance and
// External kernels) arrives with MathInputGatherer in M2.
#pragma once

#include <esses/analysis/distance_index.hpp>
#include <esses/analysis/range_tables.hpp>
#include <esses/base/cancel.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/rate.hpp>
#include <esses/base/time.hpp>
#include <esses/model/range.hpp>
#include <esses/model/source.hpp>
#include <esses/units/unit_registry.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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

struct EvalContext {
    std::shared_ptr<const model::DataSource> source;
    std::string layer;
    MathSelection selection;
    std::shared_ptr<const analysis::DistanceIndex> mainDist; // may be null
    std::shared_ptr<const analysis::DistanceIndex> refDist;  // may be null
    Micros mathStartUs = 0;                                  // math window (03 section 7.6)
    Micros mathEndUs = 0;
    Rate resultRate;
    SampleIdx resultFirst = 0;
    std::uint64_t resultCount = 0;
    std::shared_ptr<const analysis::RangeTables> ranges;
    std::shared_ptr<const units::UnitRegistry> units; // process-lifetime registry, shared
    CancelToken cancel;
};

} // namespace esses::maths
