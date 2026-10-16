// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "stream_provider.hpp"

#include "../chunk_values.hpp"

#include <esses/store/chunk.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace esses::maths::graph {

namespace {

// A chunk of a column whose dtype is the plan's output dtype, from the last stage's doubles: the samples that
// lang::finaliseResult() and then PlanProvider's conversion give (F32: the value as float, F64: the value,
// I32: truncated toward zero and clamped; a sample that is not finite after that is invalid). F32 and F64 in
// one pass: an invalid sample is written as NaN and the store's seal gives the chunk its bitmap with that
// sample's bit cleared (02 section 3.1 behaviour 2), the bitmap PlanProvider writes. I32 (no NaN to carry
// the mark) counts the invalid samples first.
Result<void> storeSamples(const double* v, std::uint32_t count, store::Dtype dtype,
                          store::ChunkBuilder& out) {
    switch (dtype) {
    case store::Dtype::F32: {
        if (auto r = out.allocate(count, dtype, false); !r) {
            return r;
        }
        float* o = out.samples<float>().data();
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto f = static_cast<float>(v[i]);
            o[i] = f - f == 0.0F
                       ? f
                       : std::numeric_limits<float>::quiet_NaN(); // NOLINT(misc-redundant-expression)
        }
        return ok();
    }
    case store::Dtype::F64: {
        if (auto r = out.allocate(count, dtype, false); !r) {
            return r;
        }
        double* o = out.samples<double>().data();
        for (std::uint32_t i = 0; i < count; ++i) {
            o[i] = v[i] - v[i] == 0.0 ? v[i] : std::numeric_limits<double>::quiet_NaN(); // NOLINT
        }
        return ok();
    }
    case store::Dtype::I32: {
        if (detail::finiteBelow(v, count, 2147483647.0)) {
            // Every sample valid and inside the I32 range: the conversion truncates as truncToI32 does.
            if (auto r = out.allocate(count, dtype, false); !r) {
                return r;
            }
            std::int32_t* o = out.samples<std::int32_t>().data();
            for (std::uint32_t i = 0; i < count; ++i) {
                o[i] = static_cast<std::int32_t>(v[i]);
            }
            return ok();
        }
        std::uint32_t invalid = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            invalid +=
                v[i] - v[i] == 0.0 ? 0u : 1u; // NOLINT(misc-redundant-expression): trunc keeps finiteness
        }
        if (auto r = out.allocate(count, dtype, invalid > 0); !r) {
            return r;
        }
        std::int32_t* o = out.samples<std::int32_t>().data();
        if (invalid == 0) {
            for (std::uint32_t i = 0; i < count; ++i) {
                o[i] = detail::truncToI32(v[i]);
            }
            return ok();
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const bool valid = v[i] - v[i] == 0.0; // NOLINT(misc-redundant-expression)
            o[i] = valid ? detail::truncToI32(v[i]) : 0;
            if (!valid) {
                out.setValid(i, false);
            }
        }
        return ok();
    }
    case store::Dtype::I64:
        break;
    }
    return fail(ErrorCode::Unsupported, "a maths output is never an I64 column");
}

// The chunk's samples into the builder: as evaluateWhole() returns them (rounded with the plan's output
// dtype), then stored in the column's dtype as PlanProvider stores them.
Result<void> storeChunk(double* v, std::uint32_t count, store::Dtype planDtype, store::Dtype dtype,
                        store::ChunkBuilder& out) {
    if (planDtype == dtype) {
        return storeSamples(v, count, dtype, out);
    }
    const bool bitmap = lang::finaliseResult(v, count, planDtype) > 0;
    if (auto r = out.allocate(count, dtype, bitmap); !r) {
        return r;
    }
    const auto write = [&](auto samples) {
        if (bitmap) {
            detail::fillWithBitmap(samples, v, [v](std::size_t i) { return v[i] == v[i]; }, out); // not NaN
        } else {
            detail::fillValid(samples, v);
        }
    };
    switch (dtype) {
    case store::Dtype::F32:
        write(out.samples<float>());
        break;
    case store::Dtype::F64:
        write(out.samples<double>());
        break;
    case store::Dtype::I32:
        write(out.samples<std::int32_t>());
        break;
    case store::Dtype::I64:
        return fail(ErrorCode::Unsupported, "a maths output is never an I64 column");
    }
    return ok();
}

} // namespace

StreamPlanProvider::StreamPlanProvider(Info info, NodeEvaluationFactory factory,
                                       std::unique_ptr<NodeEvaluation> first)
    : info_(std::move(info)), factory_(std::move(factory)) {
    const auto chunks = static_cast<std::size_t>(store::chunkCountFor(info_.size));
    served_.assign(chunks, false);
    ready_.resize(chunks);
    if (first != nullptr && first->stream != nullptr) {
        planDtype_ = first->plan.outDtype;
        restarting_ = first->stream->restartable();
        if (restarting_) {
            idle_.push_back(std::move(first));
        } else {
            live_ = std::move(first);
        }
    }
}

StreamPlanProvider::~StreamPlanProvider() = default;

std::string StreamPlanProvider::debugName() const {
    return "stream:" + info_.name + "@" + info_.version.hex();
}

bool StreamPlanProvider::holdsEvaluation() const {
    std::lock_guard streamLock(streamMutex_);
    if (live_ != nullptr) {
        return true;
    }
    std::lock_guard lock(mutex_);
    bool ready = false;
    for (const std::optional<std::vector<double>>& r : ready_) {
        ready = ready || r.has_value();
    }
    return !idle_.empty() || ready;
}

Result<std::unique_ptr<NodeEvaluation>> StreamPlanProvider::fresh(const CancelToken& cancel) {
    if (!factory_) {
        return fail(ErrorCode::Internal, "maths output '" + info_.name + "' has no evaluate function");
    }
    if (cancel.cancelled()) {
        return fail(ErrorCode::Cancelled, "cancelled before evaluating a maths output");
    }
    started_.fetch_add(1, std::memory_order_acq_rel);
    Result<std::unique_ptr<NodeEvaluation>> made = factory_(cancel);
    if (made &&
        ((*made)->stream == nullptr || (*made)->stream->size() != static_cast<std::uint64_t>(info_.size) ||
         (*made)->plan.outDtype != planDtype_ || (*made)->stream->restartable() != restarting_)) {
        return fail(ErrorCode::Internal,
                    "maths output '" + info_.name + "' evaluates differently from its column");
    }
    return made;
}

Failure StreamPlanProvider::remember(const Error& e) {
    if (e.code != ErrorCode::Cancelled) {
        std::lock_guard lock(mutex_);
        failure_ = e; // deterministic: the same inputs fail the same way
    }
    return Failure{e};
}

std::vector<double> StreamPlanProvider::buffer() {
    std::vector<double> v;
    if (!spare_.empty()) {
        v = std::move(spare_.back());
        spare_.pop_back();
    }
    v.resize(store::kChunkSamples);
    return v;
}

void StreamPlanProvider::served(SampleIdx chunkNo, std::vector<double> samples) {
    constexpr std::size_t kSpares = 4;
    std::lock_guard lock(mutex_);
    const auto slot = static_cast<std::size_t>(chunkNo);
    if (slot < served_.size() && !served_[slot]) {
        served_[slot] = true;
        ++servedCount_;
    }
    if (servedCount_ == served_.size()) {
        // Every chunk produced: the column holds the samples now (03 section 6.0). A later request starts
        // afresh.
        idle_.clear();
        spare_.clear();
        served_.assign(served_.size(), false);
        servedCount_ = 0;
        sealedOnce_ = true;
        return;
    }
    if (spare_.size() < kSpares) {
        spare_.push_back(std::move(samples));
    }
}

Result<std::vector<double>> StreamPlanProvider::samplesRestarting(SampleIdx chunkNo, std::uint32_t count,
                                                                  const CancelToken& cancel) {
    const auto first = static_cast<std::uint64_t>(store::chunkFirstSample(chunkNo));
    std::unique_ptr<NodeEvaluation> ev;
    std::vector<double> samples;
    {
        std::lock_guard lock(mutex_);
        // The evaluation that stopped where this chunk starts (the node's task going in order), else any.
        std::size_t pick = idle_.size();
        for (std::size_t k = 0; k < idle_.size() && pick == idle_.size(); ++k) {
            if (idle_[k]->stream->produced() == first) {
                pick = k;
            }
        }
        if (pick == idle_.size() && !idle_.empty()) {
            pick = idle_.size() - 1;
        }
        if (pick < idle_.size()) {
            ev = std::move(idle_[pick]);
            idle_.erase(idle_.begin() + static_cast<std::ptrdiff_t>(pick));
        }
        samples = buffer();
    }
    if (ev == nullptr) {
        Result<std::unique_ptr<NodeEvaluation>> made = fresh(cancel);
        if (!made) {
            return remember(made.error());
        }
        ev = std::move(made).value();
    }
    if (ev->stream->produced() != first) {
        if (Result<void> r = ev->stream->restartAt(first, cancel); !r) {
            return remember(r.error());
        }
    }
    if (Result<void> r = ev->stream->pull(samples.data(), count, cancel); !r) {
        return remember(r.error());
    }
    std::lock_guard lock(mutex_);
    idle_.push_back(std::move(ev));
    return samples;
}

Result<std::vector<double>> StreamPlanProvider::samplesShared(SampleIdx chunkNo, const CancelToken& cancel) {
    const auto take = [&]() -> std::optional<std::vector<double>> {
        std::lock_guard lock(mutex_);
        std::optional<std::vector<double>> v = std::move(ready_[static_cast<std::size_t>(chunkNo)]);
        ready_[static_cast<std::size_t>(chunkNo)].reset();
        return v;
    };
    if (std::optional<std::vector<double>> v = take()) {
        return std::move(*v);
    }
    std::lock_guard streamLock(streamMutex_);
    if (std::optional<std::vector<double>> v = take()) {
        return std::move(*v); // produced while this thread waited for the stream
    }
    {
        std::lock_guard lock(mutex_);
        if (failure_) {
            return Failure{*failure_}; // the stream failed while this thread waited for it
        }
    }
    if (live_ == nullptr || nextChunk_ > chunkNo) {
        live_.reset();
        Result<std::unique_ptr<NodeEvaluation>> made = fresh(cancel);
        if (!made) {
            return remember(made.error());
        }
        live_ = std::move(made).value();
        nextChunk_ = 0;
    }
    const SampleIdx chunks = store::chunkCountFor(info_.size);
    for (;;) {
        const SampleIdx k = nextChunk_;
        std::vector<double> samples;
        {
            std::lock_guard lock(mutex_);
            samples = buffer();
        }
        if (Result<void> r =
                live_->stream->pull(samples.data(), store::chunkSampleCount(info_.size, k), cancel);
            !r) {
            live_.reset();
            return remember(r.error());
        }
        if (++nextChunk_ == chunks) {
            live_.reset(); // every chunk produced
        }
        if (k == chunkNo) {
            return samples;
        }
        // A chunk before the one asked for: kept for its own materialise() while the column is being sealed
        // (afterwards a request is for an evicted chunk, and the ones before it are still in the cache).
        std::lock_guard lock(mutex_);
        const auto slot = static_cast<std::size_t>(k);
        if (!sealedOnce_ && !served_[slot] && !ready_[slot].has_value()) {
            ready_[slot] = std::move(samples);
        }
    }
}

Result<void> StreamPlanProvider::materialise(SampleIdx chunkNo, store::ChunkBuilder& out,
                                             CancelToken cancel) {
    const std::uint32_t count = store::chunkSampleCount(info_.size, chunkNo);
    if (count == 0) {
        return fail(ErrorCode::Invalid, "chunk number out of range");
    }
    if (info_.dtype == store::Dtype::I64) {
        return fail(ErrorCode::Unsupported, "a maths output is never an I64 column");
    }
    {
        std::lock_guard lock(mutex_);
        if (failure_) {
            return Failure{*failure_};
        }
    }
    Result<std::vector<double>> samples =
        restarting_ ? samplesRestarting(chunkNo, count, cancel) : samplesShared(chunkNo, cancel);
    if (!samples) {
        return Failure{samples.error()};
    }
    std::vector<double> v = std::move(samples).value();
    Result<void> stored = storeChunk(v.data(), count, planDtype_, info_.dtype, out);
    served(chunkNo, std::move(v));
    return stored;
}

} // namespace esses::maths::graph
