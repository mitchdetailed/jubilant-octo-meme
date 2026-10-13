// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// PlanProvider: the store::IChunkProvider that materialises a maths output into the shared ChunkCache
// (03 section 7.2, ARCHITECTURE_OUTLINE section 4 esses_maths), so maths outputs are plotted, pyramided and
// (from M5) evicted like logged data. Through 0.1 every plan runs as ExtentMode::Whole (03 section 6.0): the
// whole extent is evaluated once (one task, key maths/<src>/<layer>/<key>/<version>/all) and every chunk is
// then sliced from that buffer; chunkIndependent() is false. MathGraph seeds the provider with the extent its
// task has just evaluated; the buffer is released once every chunk has been served, and a chunk asked for
// after that (Column::buildSummary's scratch pass, M5 eviction of the output's eviction group) evaluates the
// whole extent again through PlanSnapshot::evaluate.
//
// Chunks: F32/F64 samples (I32 rounded), invalid samples with their valid bit cleared and, in float chunks,
// NaN (03 section 6.6); a chunk without an invalid sample carries no bitmap.
//
// PlanSnapshot is the immutable dependency-cone snapshot a maths task captures at require time (01
// section 5.3 rules 9-10): it holds values and std::shared_ptr<const ...> only.
#pragma once

#include <esses/analysis/corrected_speed.hpp>
#include <esses/base/cancel.hpp>
#include <esses/base/rate.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/store/chunk_provider.hpp>
#include <esses/store/types.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace esses::maths {

struct PlanSnapshot {
    ChannelKey key;
    Version version;
    store::Dtype outDtype = store::Dtype::F32;
    Rate rate;          // of the result
    Micros t0 = 0;      // time of sample 0
    SampleIdx size = 0; // samples of the result
    std::string name;   // output name as defined (debugName)
    // Evaluates the whole extent (03 section 6.0) on a worker; must return `size` samples. Called whenever
    // the provider holds no evaluated extent (never while it holds the seeded one).
    std::function<Result<analysis::UniformSignal>(const CancelToken&)> evaluate;
};

class PlanProvider final : public store::IChunkProvider {
public:
    explicit PlanProvider(std::shared_ptr<const PlanSnapshot> plan);
    // Seeded with the extent the caller has just evaluated (must hold plan->size samples).
    PlanProvider(std::shared_ptr<const PlanSnapshot> plan, analysis::UniformSignal whole);

    SampleIdx size() const override;
    store::Dtype dtype() const override;
    std::uint64_t version() const override; // the low 64 bits of PlanSnapshot::version
    bool chunkIndependent() const override { return false; }
    Result<void> materialise(SampleIdx chunkNo, store::ChunkBuilder& out, CancelToken cancel) override;
    std::string debugName() const override; // "plan:Corr Dist@<version hex>"

    const PlanSnapshot& plan() const noexcept { return *plan_; }
    // How often PlanSnapshot::evaluate ran (tests: once per whole-extent evaluation).
    std::uint32_t evaluations() const noexcept { return evaluations_.load(std::memory_order_acquire); }
    // True while the evaluated extent is held (until every chunk has been served once).
    bool holdsExtent() const;

private:
    std::shared_ptr<const PlanSnapshot> plan_;
    // Guards whole_ (the evaluated extent, or its deterministic error), served_ (the chunks sliced from it)
    // and servedCount_.
    mutable std::mutex mutex_;
    std::optional<Result<analysis::UniformSignal>> whole_;
    std::vector<bool> served_;
    std::size_t servedCount_ = 0;
    std::atomic<std::uint32_t> evaluations_{0};
};

} // namespace esses::maths
