// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/maths/whole_signal.hpp>
#include <esses/store/chunk.hpp>
#include <esses/store/column.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <variant>

namespace esses::maths {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

template <class T>
void copyChunk(const store::Chunk& chunk, std::size_t base, const units::Converter& toBase,
               analysis::UniformSignal& out, bool& anyInvalid) {
    const std::span<const T> samples = chunk.samples<T>();
    const std::uint32_t n = chunk.count();
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::size_t at = base + i;
        const double v = samples.empty() ? kNaN : static_cast<double>(samples[i]);
        if (!chunk.validAt(i) || std::isnan(v)) {
            out.values[at] = kNaN;
            out.valid[at] = 0;
            anyInvalid = true;
        } else {
            out.values[at] = toBase(v);
        }
    }
}

} // namespace

Result<analysis::UniformSignal> readWholeSignal(const store::Series& series, const CancelToken& cancel,
                                                const units::Converter& toBase, Priority priority) {
    const auto* uniform = std::get_if<store::UniformSeries>(&series);
    if (uniform == nullptr) {
        return fail(ErrorCode::Unsupported,
                    "a timestamped series is not a whole-extent maths input yet (M2)");
    }
    analysis::UniformSignal out;
    out.rate = uniform->rate;
    out.t0 = uniform->t0;
    const store::Column& column = uniform->values;
    const SampleIdx size = column.size();
    if (size <= 0) {
        return out;
    }
    if (cancel.cancelled()) {
        return fail(ErrorCode::Cancelled, "cancelled before reading a maths input");
    }
    Result<store::ChunkHandle> pinned = column.require(0, size - 1, priority, cancel);
    if (!pinned) {
        return Failure{pinned.error()};
    }
    const store::ChunkHandle& handle = *pinned;
    const auto n = static_cast<std::size_t>(size);
    out.values.assign(n, 0.0);
    out.valid.assign(n, 1);
    bool anyInvalid = false;
    for (SampleIdx c = handle.firstChunk(); c <= handle.lastChunk(); ++c) {
        if (cancel.cancelled()) {
            return fail(ErrorCode::Cancelled, "cancelled while reading a maths input");
        }
        const store::Chunk* chunk = handle.chunk(c);
        if (chunk == nullptr) {
            return fail(ErrorCode::Internal, "a required chunk is not pinned");
        }
        const auto base = static_cast<std::size_t>(store::chunkFirstSample(c));
        switch (chunk->dtype()) {
        case store::Dtype::F32:
            copyChunk<float>(*chunk, base, toBase, out, anyInvalid);
            break;
        case store::Dtype::F64:
            copyChunk<double>(*chunk, base, toBase, out, anyInvalid);
            break;
        case store::Dtype::I32:
            copyChunk<std::int32_t>(*chunk, base, toBase, out, anyInvalid);
            break;
        case store::Dtype::I64:
            return fail(ErrorCode::Unsupported, "an I64 time column is not a maths input");
        }
    }
    if (!anyInvalid) {
        out.valid.clear();
    }
    return out;
}

} // namespace esses::maths
