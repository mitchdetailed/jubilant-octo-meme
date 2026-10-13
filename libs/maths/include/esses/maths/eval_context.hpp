// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// EvalContext (03 sections 7.5 and 7.6; D-050; 01 section 5.3 rule 9). EvalContext holds no reference or raw
// pointer to GUI-lifetime state: shared inputs are std::shared_ptr<const ...> snapshots and the services a
// task calls back into (TaskPool, Log) are process-lifetime runtime objects. MathSelection (the maths-local
// copy of the selection) lives in math_selection.hpp since M2.
//
// getChannel (M2 contract): the MathInputGatherer factory that Plan evaluation, Variance and External kernels
// use to read an input at the result rate in a requested unit (03 section 6.3).
#pragma once

#include <esses/analysis/distance_index.hpp>
#include <esses/analysis/range_tables.hpp>
#include <esses/base/cancel.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/rate.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/maths/math_input_gatherer.hpp>
#include <esses/maths/math_selection.hpp>
#include <esses/model/range.hpp>
#include <esses/model/source.hpp>
#include <esses/units/unit_registry.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace esses::maths {

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
    // The input `key` (a chain head, 03 section 4.3) at `rate` converted into `unit`; set by the host
    // (MathGraph) for plan evaluation, Variance and External kernels. Empty in contexts that read no channel.
    std::function<Result<MathInputGatherer>(const ChannelKey& key, units::UnitId unit, Rate rate)> getChannel;
    CancelToken cancel;
};

} // namespace esses::maths
