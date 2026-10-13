// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "chunk_values.hpp"

#include <esses/maths/plan_provider.hpp>
#include <esses/store/chunk.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace esses::maths {

namespace {

Result<void> checkExtent(const PlanSnapshot& plan, const analysis::UniformSignal& whole) {
    if (whole.values.size() != static_cast<std::size_t>(plan.size) ||
        (!whole.valid.empty() && whole.valid.size() != whole.values.size())) {
        return fail(ErrorCode::Internal, "maths output '" + plan.name + "' evaluated to " +
                                             std::to_string(whole.values.size()) +
                                             " samples where its plan has " + std::to_string(plan.size));
    }
    return ok();
}

template <class T>
void fill(std::span<T> out, const analysis::UniformSignal& whole, std::size_t first, store::ChunkBuilder& b) {
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::size_t at = first + i;
        const double v = whole.values[at];
        const bool valid = whole.validAt(at) && !std::isnan(v);
        if constexpr (std::is_floating_point_v<T>) {
            out[i] = valid ? static_cast<T>(v) : std::numeric_limits<T>::quiet_NaN();
        } else {
            out[i] = valid ? detail::toI32(v) : T{0};
        }
        if (!valid && b.hasBitmap()) {
            b.setValid(static_cast<std::uint32_t>(i), false);
        }
    }
}

} // namespace

PlanProvider::PlanProvider(std::shared_ptr<const PlanSnapshot> plan) : plan_(std::move(plan)) {
    served_.assign(static_cast<std::size_t>(store::chunkCountFor(plan_->size)), false);
}

PlanProvider::PlanProvider(std::shared_ptr<const PlanSnapshot> plan, analysis::UniformSignal whole)
    : PlanProvider(std::move(plan)) {
    whole_.emplace(std::move(whole));
}

SampleIdx PlanProvider::size() const {
    return plan_->size;
}

store::Dtype PlanProvider::dtype() const {
    return plan_->outDtype;
}

std::uint64_t PlanProvider::version() const {
    return plan_->version.lo;
}

bool PlanProvider::holdsExtent() const {
    std::lock_guard lock(mutex_);
    return whole_.has_value() && whole_->ok();
}

Result<void> PlanProvider::materialise(SampleIdx chunkNo, store::ChunkBuilder& out, CancelToken cancel) {
    const std::uint32_t count = store::chunkSampleCount(plan_->size, chunkNo);
    if (count == 0) {
        return fail(ErrorCode::Invalid, "chunk number out of range");
    }
    if (plan_->outDtype == store::Dtype::I64) {
        return fail(ErrorCode::Unsupported, "a maths output is never an I64 column");
    }
    std::lock_guard lock(mutex_);
    if (!whole_.has_value()) {
        if (!plan_->evaluate) {
            return fail(ErrorCode::Internal, "maths output '" + plan_->name + "' has no evaluate function");
        }
        if (cancel.cancelled()) {
            return fail(ErrorCode::Cancelled, "cancelled before evaluating a maths output");
        }
        evaluations_.fetch_add(1, std::memory_order_acq_rel);
        Result<analysis::UniformSignal> r = plan_->evaluate(cancel);
        if (r) {
            if (auto c = checkExtent(*plan_, *r); !c) {
                r = Failure{c.error()};
            }
        }
        if (!r && r.error().code == ErrorCode::Cancelled) {
            return Failure{r.error()}; // not memoised: the next request evaluates again
        }
        whole_.emplace(std::move(r));
        served_.assign(served_.size(), false);
        servedCount_ = 0;
    }
    if (!whole_->ok()) {
        return Failure{whole_->error()}; // deterministic: the same inputs fail the same way
    }
    const analysis::UniformSignal& whole = **whole_;
    if (auto c = checkExtent(*plan_, whole); !c) {
        return c;
    }

    const auto first = static_cast<std::size_t>(store::chunkFirstSample(chunkNo));
    // Any invalid sample (a cleared flag or NaN)? Counted branch-free so the scan vectorises.
    const double* v = whole.values.data() + first;
    std::size_t invalid = 0;
    if (whole.valid.empty()) {
        for (std::size_t i = 0; i < count; ++i) {
            invalid += v[i] != v[i] ? 1u : 0u; // NaN
        }
    } else {
        const std::uint8_t* flags = whole.valid.data() + first;
        for (std::size_t i = 0; i < count; ++i) {
            invalid += (flags[i] == 0 || v[i] != v[i]) ? 1u : 0u;
        }
    }
    const bool bitmap = invalid > 0;
    if (auto r = out.allocate(count, plan_->outDtype, bitmap); !r) {
        return r;
    }
    const auto write = [&](auto samples) {
        if (bitmap) {
            fill(samples, whole, first, out);
        } else {
            detail::fillValid(samples, v);
        }
    };
    switch (plan_->outDtype) {
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
        break; // rejected above
    }

    const auto slot = static_cast<std::size_t>(chunkNo);
    if (slot < served_.size() && !served_[slot]) {
        served_[slot] = true;
        if (++servedCount_ == served_.size()) {
            whole_.reset(); // every chunk sealed: the column holds the samples now (03 section 6.0)
        }
    }
    return ok();
}

std::string PlanProvider::debugName() const {
    return "plan:" + plan_->name + "@" + plan_->version.hex();
}

} // namespace esses::maths
