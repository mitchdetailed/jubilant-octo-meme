// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// StreamPlanProvider (D-191): the store::IChunkProvider of an expression node's column. MathGraph's task
// compiles the node and starts a PlanStream over the layer extent (so the definition's failures known at
// evaluation, E-ARG-VALUE, E-RANGE-UNKNOWN, ..., come before any column exists), hands it to this provider
// and requires every chunk: each materialise() takes the chunk's samples from a stream, rounds them as the
// output dtype stores them (lang::finaliseResult) and writes the chunk. The result never exists as a
// whole-extent buffer, and a chunk is sealed (its pyramid built) right after its samples are made. The
// samples are the ones PlanProvider gets from evaluateWhole, chunk for chunk.
//
// The provider is chunk-independent (02 section 3.2 behaviour 6): Column::require offers the node's other
// chunks to idle workers while the node's task builds them in order, so the sealing of one node's chunks
// spreads over the pool once other nodes are done. How a chunk's samples are made depends on the plan:
//   * every stage restarts inside the extent (PlanStream::restartable(): elementwise, range, table, smooth,
//     derivative, time_shift, previous_sample): each worker evaluates its chunk with an evaluation of its
//     own, restarted at the chunk (a pool of evaluations; the one that stopped where the chunk starts is
//     used first, so the task going in order never restarts);
//   * otherwise (filters, integrals, latches, statistics) one stream runs through the extent in order under
//     a lock: a worker that needs a later chunk advances it and, while the column is being sealed, leaves
//     the chunks it passes for their own materialise() calls, so only the evaluation is sequential and the
//     rounding, writing and sealing of the chunks run in parallel.
// Once every chunk has been produced the evaluations are released; a later request (a chunk evicted, a
// summary rebuild) starts a fresh evaluation through the factory. A Cancelled evaluation is dropped and not
// memoised; any other failure is (the same inputs fail the same way). Internal to esses_maths.
#pragma once

#include "../lang/plan_stream.hpp"

#include <esses/base/cancel.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/result.hpp>
#include <esses/maths/plan.hpp>
#include <esses/store/chunk_provider.hpp>
#include <esses/store/types.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace esses::maths::graph {

// One evaluation of an expression node: its plan and a started stream over it.
struct NodeEvaluation {
    Plan plan;
    std::unique_ptr<lang::PlanStream> stream; // reads `plan`
};

// Compiles the node and starts its stream with the given token (as the node's task does).
using NodeEvaluationFactory = std::function<Result<std::unique_ptr<NodeEvaluation>>(const CancelToken&)>;

class StreamPlanProvider final : public store::IChunkProvider {
public:
    struct Info {
        Version version;
        store::Dtype dtype = store::Dtype::F32; // of the column
        SampleIdx size = 0;
        std::string name; // output name as defined (debugName)
    };

    // `first` is a started evaluation of the node (its stream at sample 0).
    StreamPlanProvider(Info info, NodeEvaluationFactory factory, std::unique_ptr<NodeEvaluation> first);
    ~StreamPlanProvider() override;
    StreamPlanProvider(const StreamPlanProvider&) = delete;
    StreamPlanProvider& operator=(const StreamPlanProvider&) = delete;
    StreamPlanProvider(StreamPlanProvider&&) = delete;
    StreamPlanProvider& operator=(StreamPlanProvider&&) = delete;

    SampleIdx size() const override { return info_.size; }
    store::Dtype dtype() const override { return info_.dtype; }
    std::uint64_t version() const override { return info_.version.lo; }
    bool chunkIndependent() const override { return true; }
    Result<void> materialise(SampleIdx chunkNo, store::ChunkBuilder& out, CancelToken cancel) override;
    std::string debugName() const override; // "stream:<name>@<version hex>"

    // True when each chunk is evaluated on its own (every stage restarts), false for one shared stream.
    bool restarting() const noexcept { return restarting_; }
    // Evaluations started through the factory (the first one handed in is not counted).
    std::uint32_t evaluationsStarted() const noexcept { return started_.load(std::memory_order_acquire); }
    // True while an evaluation or a produced chunk is held (until every chunk has been produced once).
    bool holdsEvaluation() const;

private:
    Result<std::vector<double>> samplesRestarting(SampleIdx chunkNo, std::uint32_t count,
                                                  const CancelToken& cancel);
    Result<std::vector<double>> samplesShared(SampleIdx chunkNo, const CancelToken& cancel);
    Result<std::unique_ptr<NodeEvaluation>> fresh(const CancelToken& cancel);
    Failure remember(const Error& e);
    std::vector<double> buffer(); // a chunk buffer (mutex_ held)
    void served(SampleIdx chunkNo, std::vector<double> samples);

    Info info_;
    NodeEvaluationFactory factory_;
    bool restarting_ = false;
    store::Dtype planDtype_ = store::Dtype::F32; // the plan's output dtype (lang::finaliseResult)
    // Guards idle_, ready_, spare_, served_, servedCount_, sealedOnce_ and failure_.
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<NodeEvaluation>> idle_;     // restarting: evaluations not in use
    std::vector<std::optional<std::vector<double>>> ready_; // shared: chunks produced, not yet served
    std::vector<std::vector<double>> spare_;                // chunk buffers to reuse
    std::vector<bool> served_;                              // chunks produced since the last release
    std::size_t servedCount_ = 0;
    bool sealedOnce_ = false; // every chunk was produced once (the column was sealed)
    std::optional<Error> failure_;
    // Shared: the stream and the next chunk it produces; streamMutex_ is taken before mutex_.
    mutable std::mutex streamMutex_;
    std::unique_ptr<NodeEvaluation> live_;
    SampleIdx nextChunk_ = 0;
    std::atomic<std::uint32_t> started_{0};
};

} // namespace esses::maths::graph
