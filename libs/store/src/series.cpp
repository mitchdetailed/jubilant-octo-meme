// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/store/series.hpp>

#include <utility>

namespace esses::store {

Micros UniformSeries::duration() const noexcept {
    const SampleIdx n = values.size();
    if (n == 0 || !rate.valid()) {
        return 0;
    }
    return rate.timeAt(n, 0);
}

SampleIdx seriesSize(const Series& series) noexcept {
    if (const auto* u = std::get_if<UniformSeries>(&series)) {
        return u->size();
    }
    if (const auto* t = std::get_if<TimestampedSeries>(&series)) {
        return t->size();
    }
    return 0;
}

const Column& seriesValues(const Series& series) noexcept {
    if (const auto* t = std::get_if<TimestampedSeries>(&series)) {
        return t->values;
    }
    return std::get_if<UniformSeries>(&series)->values;
}

Result<TimestampedSeries> makeTimestampedSeries(Column timesUs, Column values, const CancelToken& cancel) {
    if (timesUs.dtype() != Dtype::I64) {
        return fail(ErrorCode::Invalid, "a timestamped series needs an I64 time column");
    }
    if (timesUs.size() != values.size()) {
        return fail(ErrorCode::Invalid, "time and value columns differ in size");
    }
    TimestampedSeries ts;
    const SampleIdx n = timesUs.size();
    if (n > 0) {
        Result<ChunkHandle> head = timesUs.require(0, 0, Priority::Interactive, cancel);
        if (!head) {
            return Failure{head.error()};
        }
        Result<ChunkHandle> tail = timesUs.require(n - 1, n - 1, Priority::Interactive, cancel);
        if (!tail) {
            return Failure{tail.error()};
        }
        ts.tMin = head->int64At(0).value_or(0);
        ts.tMax = tail->int64At(n - 1).value_or(-1);
    }
    ts.timesUs = std::move(timesUs);
    ts.values = std::move(values);
    return ts;
}

} // namespace esses::store
