// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathGraph: one per (DataSource, layer), mutated on its owning thread only (the GUI thread, or the CLI main
// thread; 03 section 7.1, 01 section 5.3 rule 10). M1 subset (07 section 4.1, D-044): built-in modules only
// (defineBuiltin), version(), require() into the ChunkCache through PlanProvider, status(), consumersOf(),
// outputs(). define(MathModule, MathItem) for expressions, shadowing chains (03 section 4.3), setSelection
// and MathsScheduler's per-chunk tasks arrive in M2.
//
// require() never blocks: it returns a Future that resolves on a worker (share key
// maths/<src>/<layer>/<channelKey>/<version>/all, 01 section 5.4); the caller posts completions to the owning
// thread, which applies them with applyCompletion() (status transitions Pending -> Evaluating -> Ok/Warning
// or Error, 03 section 7.3). applyReadyCompletions() applies every finished evaluation at once (the CLI after
// waiting, or a host that polls).
//
// Evaluation (0.1 whole-node path, 03 section 6.0, D-139): one TaskPool task per node evaluates the whole
// layer extent and seals it into ChunkCache chunks through a Column over a PlanProvider; parallelism is
// across nodes only. A node's task waits for its upstream nodes' tasks (submitted, or joined through their
// share keys, by require() on the owning thread; Future::get runs a still-queued upstream task inline, Rule
// W) and reads their sealed columns back as whole-extent signals, so a downstream output depends only on the
// stored upstream values.
//
// Caching (03 section 7.2): the evaluated ColumnHandle is kept per node for the node's version; require()
// returns it while the version holds. A version is
//   xxh3(defHash, enabled, moduleEnabled, sorted upstream versions, the identity, store version, unit and
//        time base of exactly the logged inputs the node reads (BuiltinOutput::inputs))
// with defHash = xxh3(module id and version, output definition, canonical settings JSON, compat switches,
// engine version). DataSource::version is never part of it, so annotations and other sidecar commits that
// share the channels' columns invalidate nothing (D-140).
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/future.hpp>
#include <esses/base/priority.hpp>
#include <esses/base/result.hpp>
#include <esses/maths/builtin_module.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/maths/diagnostics.hpp>
#include <esses/maths/math_status.hpp>
#include <esses/maths/maths_compat.hpp>
#include <esses/model/channel.hpp>
#include <esses/model/opaque_json.hpp>
#include <esses/model/source.hpp>
#include <esses/units/unit_registry.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses {
class TaskPool;
} // namespace esses

namespace esses::store {
class ChunkCache;
} // namespace esses::store

namespace esses::maths {

// One scheduled evaluation: what prefetch() returns per output, so the host can post each completion.
struct MathRequest {
    ChannelKey key;
    Version version;
    Future<ColumnHandle> future;
};

class MathGraph {
public:
    // `pool` nullptr: TaskPool::installed() at require time (the runtime's pool, 01 section 5.3 rule 11);
    // `cache` nullptr: ChunkCache::installed() at require time. require() fails with Invalid when neither is
    // available. `units` is needed by defineBuiltin (Invalid without it).
    MathGraph(std::shared_ptr<const model::DataSource> source, std::string layer,
              std::shared_ptr<const units::UnitRegistry> units, MathsCompat compat = {},
              TaskPool* pool = nullptr, store::ChunkCache* cache = nullptr);
    ~MathGraph();
    MathGraph(const MathGraph&) = delete;
    MathGraph& operator=(const MathGraph&) = delete;
    MathGraph(MathGraph&&) noexcept;
    MathGraph& operator=(MathGraph&&) noexcept;

    // Registers (or re-registers after a settings change) a built-in module's outputs (03 section 9; D-016).
    // Null settings: module.defaultSettings(). Returns the output keys in the module's order; a new node, or
    // one whose version changed, becomes Pending (Disabled while the module is disabled); an unchanged output
    // keeps its evaluated column. Outputs the module no longer produces are removed. Errors (nothing
    // changes): the module's expand() error; Invalid for an output name that is empty, repeated or already
    // defined by another module (E-DUPLICATE-OUTPUT; shadowing rules arrive in M2), for an upstream name that
    // is not an output of the module (E-UNKNOWN-UPSTREAM) and for a missing evaluate function; Cycle for an
    // upstream cycle (E-CYCLE).
    Result<std::vector<ChannelKey>> defineBuiltin(const IBuiltinModule& module,
                                                  const model::OpaqueJson& settings, Diagnostics& diags);
    // Disabled status for the module's outputs (their versions change, so cached columns are dropped); true
    // brings them back as Pending. Returns the keys whose version changed.
    std::vector<ChannelKey> setModuleEnabled(std::string_view moduleId, bool enabled);
    // A new snapshot of the same source: node versions are re-hashed from exactly what each node reads (03
    // section 7.2, D-140); nodes whose version changed become Pending and drop their column. Returns them.
    std::vector<ChannelKey> setSource(std::shared_ptr<const model::DataSource> source);

    // The node version (03 section 7.2); the zero Version for an unknown key.
    Version version(const ChannelKey& key) const;
    // NotFound for an unknown key, Invalid for a disabled one (ready Futures); never blocks (the GUI thread
    // may call it, 01 section 5.3 rule 4). The cached column when the version holds; otherwise one task per
    // node at `priority` (share key taskKey(key): a repeated require joins the queued or running task,
    // registers `cancel` as one more requester and raises its priority), after requiring the upstream nodes.
    // The 0.1 path always evaluates the whole extent; `range` is accepted for the v1.0 chunk path.
    Future<ColumnHandle> require(const ChannelKey& key, SampleRange range, Priority priority,
                                 const CancelToken& cancel);
    // The Background prefetch hook (D-016, AC-M1-10): require() of every enabled output at `priority`;
    // outputs already evaluated come back as ready Futures. The host posts each completion.
    std::vector<MathRequest> prefetch(Priority priority = Priority::Background,
                                      const CancelToken& cancel = {});
    // Applies a posted completion on the owning thread (03 section 7.3 transitions): ignored when `version`
    // is no longer the node's; Ok/Warning with the column cached, Pending after Cancelled, Error otherwise
    // (the error's context.detail when it is a diagnostic code such as E-CSD-NO-SPEED, else E-EVAL).
    void applyCompletion(const ChannelKey& key, const Version& version, const Result<ColumnHandle>& result);
    // Applies every in-flight evaluation whose Future is ready; returns the keys whose status was applied.
    std::vector<ChannelKey> applyReadyCompletions();

    // The node's status; an unknown key reports Error E-NOT-MATHS.
    MathStatus status(const ChannelKey& key) const;
    // Direct consumers (the outputs whose upstream names `key`), in definition order.
    std::vector<ChannelKey> consumersOf(const ChannelKey& key) const;
    // Every output key in definition order.
    std::vector<ChannelKey> outputs() const;
    // The model::ChannelDescriptor of an output (03 section 8), or nullopt: id channelIdMathsModule, source
    // Maths, statusText from the status ("", "Pending", "Error: <summary>"), rate once evaluated.
    std::optional<model::ChannelDescriptor> descriptor(const ChannelKey& key) const;
    // The key of an output by name (case-folded), or nullopt.
    std::optional<ChannelKey> find(std::string_view name) const;
    // The share key of the node's evaluation task for its current version
    // (maths/<src hex>/<layer>/<name>/<version hex>/all; '/' and '%' in names are percent-escaped).
    TaskKey taskKey(const ChannelKey& key) const;
    // "maths/<src hex>/": TaskPool::cancelAll(prefix) stops every evaluation of the source.
    static std::string taskKeyPrefix(const model::SourceId& source);

    const std::shared_ptr<const model::DataSource>& source() const noexcept;
    const std::string& layer() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace esses::maths
