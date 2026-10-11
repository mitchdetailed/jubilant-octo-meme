// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Series kinds (02 section 3.3, D-008, D-009, D-062): never resampled at import.
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/result.hpp>
#include <esses/store/column.hpp>
#include <esses/store/types.hpp>

#include <variant>

namespace esses::store {

struct UniformSeries {
    Rate rate;
    Micros t0 = 0;
    Column values;

    Micros timeAt(SampleIdx i) const noexcept { return rate.timeAt(i, t0); }
    SampleIdx indexAt(Micros t) const noexcept { return rate.indexAt(t, t0); }
    SampleIdx size() const noexcept { return values.size(); }
    // Time covered by the samples, timeAt(size()) - t0 (n samples at period p last n * p); 0 when empty.
    Micros duration() const noexcept;
};

// Strictly increasing exact microsecond timestamps (rule 12: importers de-duplicate, last wins); timesUs is
// an I64 column with the same size as values. tMin/tMax are the first and last timestamp.
struct TimestampedSeries {
    Column timesUs;
    Column values;
    Micros tMin = 0;
    Micros tMax = -1;

    SampleIdx size() const noexcept { return values.size(); }
};

using Series = std::variant<UniformSeries, TimestampedSeries>;

SampleIdx seriesSize(const Series& series) noexcept;
const Column& seriesValues(const Series& series) noexcept;

// Worker: validates the pair (I64 times, equal sizes) and reads tMin/tMax from the first and last timestamp.
Result<TimestampedSeries> makeTimestampedSeries(Column timesUs, Column values,
                                                const CancelToken& cancel = {});

} // namespace esses::store
