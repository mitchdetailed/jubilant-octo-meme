// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathGraph evaluation on the 0.1 whole-node path (03 sections 6.0, 7.4; D-139): require() submits one task
// per node through MathsScheduler (share key maths/<src>/<layer>/<name>/<version>[/<selection>]/all) after
// requiring the node's upstream nodes. A task holds immutable snapshots only (01 section 5.3 rule 9) and
// never touches the graph: it waits for its upstream Futures (Rule W: a still-queued one runs inline), then
// seals the node's column into the ChunkCache:
//   * an item: a fresh Plan from its AST and frozen scope and a PlanStream over the extent (the evaluation's
//     failures come here, before any column), then a Column over a StreamPlanProvider whose chunks are
//     produced and sealed as the stream reaches them (D-191; idle workers seal the node's later chunks);
//   * a built-in output: its WholeExtentFn over the upstream signals, sealed through a PlanProvider.
// Either provider evaluates the same definition over the same stored inputs again if a chunk is asked for
// after it released its evaluation (03 section 6.2 Whole).

#include "../kernels/kernels_internal.hpp"
#include "graph_internal.hpp"
#include "stream_provider.hpp"

#include <esses/base/taskpool.hpp>
#include <esses/maths/math_input_gatherer.hpp>
#include <esses/maths/plan_provider.hpp>
#include <esses/maths/whole_signal.hpp>
#include <esses/store/column.hpp>
#include <esses/store/series.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace esses::maths {

namespace graph {

Result<Plan> buildPlan(const Ast& ast, const Scope& scope, const MathItem& item, const MathsCompat& compat,
                       Diagnostics& diags) {
    Result<Plan> plan = compile(ast, scope, item, compat, diags);
    if (!plan || item.kind != ItemKind::Table2D) {
        return plan;
    }
    if (!plan->stages.empty() && plan->stages.back().kernel != nullptr &&
        plan->stages.back().kernel->cls() == KernelClass::Table) {
        return plan; // compile() appended the table stage itself
    }
    Result<std::unique_ptr<Kernel>> table = kern::makeTableKernel(item.table, scope.units());
    if (!table) {
        Error e = table.error();
        if (!e.context.detail) {
            e.context.detail = "E-TABLE-AXIS";
        }
        Diagnostic d;
        d.code = *e.context.detail;
        d.level = Diagnostic::Error;
        d.message = e.message;
        diags.push_back(std::move(d));
        return Failure{std::move(e)};
    }
    Stage stage;
    stage.kernel = std::move(table).value();
    Stage::Ref in;
    in.from = Stage::Ref::From::Stage;
    in.index = static_cast<std::uint32_t>(plan->stages.size() - 1);
    stage.inputs.push_back(in);
    stage.outTemp = static_cast<std::uint32_t>(plan->stages.size());
    plan->stages.push_back(std::move(stage));
    const units::UnitRegistry& units = scope.units();
    const units::UnitId vu = item.table.valuesUnit;
    const units::QuantityId q = units.contains(vu) ? units.quantityOf(vu) : units::kUnitless;
    plan->out.unit = units.containsQuantity(q) ? units.quantity(q).base : units::kNoUnit;
    plan->out.dim = units.containsQuantity(q) ? units.quantity(q).dim : units::Dimension::none();
    plan->out.scaleToBase = 1.0;
    plan->out.integer = false;
    if (plan->out.kind != Kind::Scalar) {
        plan->out.kind = Kind::Series;
    }
    bool f64 = item.precision == "double";
    for (const PlanInput& pi : plan->inputs) {
        f64 = f64 || pi.f64;
    }
    plan->outDtype = f64 ? store::Dtype::F64 : store::Dtype::F32;
    Hash128Stream h; // 03 section 7.2: the table data is part of the definition
    h.updatePart("esses.maths.table-1");
    updateHash(h, plan->defHash);
    h.updateU64(vu);
    h.updateU64(item.table.x.breakpoints.size());
    for (const double b : item.table.x.breakpoints) {
        h.updatePart(std::to_string(b));
    }
    h.updateU64(item.table.values.size());
    for (const double v : item.table.values) {
        h.updatePart(std::to_string(v));
    }
    h.updateU64(static_cast<std::uint64_t>(item.table.interpolation));
    h.updateU64(static_cast<std::uint64_t>(item.table.outOfRange));
    plan->defHash = h.digest();
    return plan;
}

} // namespace graph

namespace {

using graph::MathNode;

struct SealArgs {
    ChannelKey key;
    Version version;
    Hash128 selection;
    std::string name;
    model::ChannelDescriptor descriptor;
    store::Dtype dtype = store::Dtype::F32;
    store::ChunkCache* cache = nullptr;
    TaskPool* pool = nullptr;
    std::string chunkKeyPrefix;
    Priority priority = Priority::Background;
};

using WholeFn = std::function<Result<analysis::UniformSignal>(const CancelToken&)>;

// Seals a whole-extent result into the ChunkCache (03 section 6.0 item 1) and returns its handle.
Result<ColumnHandle> sealColumn(const SealArgs& a, analysis::UniformSignal whole, WholeFn again,
                                Diagnostics diags, const CancelToken& token) {
    if (token.cancelled()) {
        return fail(ErrorCode::Cancelled, "cancelled after evaluating '" + a.name + "'");
    }
    if (!whole.valid.empty() && whole.valid.size() != whole.values.size()) {
        return fail(ErrorCode::Internal, "'" + a.name + "' returned a validity vector of the wrong size");
    }
    if (!whole.values.empty() && !whole.rate.valid()) {
        return fail(ErrorCode::Internal, "'" + a.name + "' returned samples without a rate");
    }
    auto plan = std::make_shared<PlanSnapshot>();
    plan->key = a.key;
    plan->version = a.version;
    plan->outDtype = a.dtype;
    plan->rate = whole.rate;
    plan->t0 = whole.t0;
    plan->size = static_cast<SampleIdx>(whole.values.size());
    plan->name = a.name;
    plan->evaluate = std::move(again);
    const Rate rate = plan->rate;
    const Micros t0 = plan->t0;
    const SampleIdx size = plan->size;
    auto provider = std::make_shared<PlanProvider>(std::move(plan), std::move(whole));
    store::ColumnOptions options;
    options.cache = a.cache;
    options.pool = a.pool;
    options.taskKeyPrefix = a.chunkKeyPrefix;
    store::Column column(std::move(provider), std::move(options));
    if (size > 0) {
        // Seal every chunk into the ChunkCache now (the 0.1 output is one eviction group).
        Result<store::ChunkHandle> sealed = column.require(0, size - 1, a.priority, token);
        if (!sealed) {
            return Failure{sealed.error()};
        }
    }
    ColumnHandle handle;
    handle.key = a.key;
    handle.version = a.version;
    handle.descriptor = a.descriptor;
    handle.descriptor.rate = rate;
    handle.descriptor.statusText.clear();
    handle.series = store::UniformSeries{rate, t0, std::move(column)};
    handle.diags = std::move(diags);
    handle.selectionKey = a.selection;
    return handle;
}

std::optional<ColumnHandle> recall(graph::EvalMemo& memo) {
    std::lock_guard lock(memo.mutex);
    return memo.handle;
}

Result<ColumnHandle> remember(graph::EvalMemo& memo, Result<ColumnHandle> r) {
    if (r) {
        std::lock_guard lock(memo.mutex);
        if (!memo.handle) {
            memo.handle = *r;
        } else {
            return *memo.handle; // a racing duplicate sealed first: one column per version
        }
    }
    return r;
}

Error upstreamError(const std::string& name, const Error& e) {
    Error out = e;
    if (out.code != ErrorCode::Cancelled) {
        out.message = "input '" + name + "': " + out.message;
    }
    return out;
}

// The whole-node task of a built-in output (M1, 03 section 9).
struct BuiltinTask {
    SealArgs seal;
    WholeExtentFn evaluate;
    std::vector<std::pair<std::string, Future<ColumnHandle>>> upstream;
    EvalContext ctx;
    std::shared_ptr<std::atomic<std::uint64_t>> evaluations;
    std::shared_ptr<graph::EvalMemo> memo;

    static Result<std::vector<analysis::UniformSignal>> readAll(std::span<const store::Series> series,
                                                                const CancelToken& token, Priority prio) {
        std::vector<analysis::UniformSignal> out;
        out.reserve(series.size());
        for (const store::Series& s : series) {
            Result<analysis::UniformSignal> signal = readWholeSignal(s, token, {}, prio);
            if (!signal) {
                return Failure{signal.error()};
            }
            out.push_back(std::move(signal).value());
        }
        return out;
    }

    Result<ColumnHandle> operator()(const CancelToken& token) const {
        if (std::optional<ColumnHandle> done = recall(*memo)) {
            return std::move(*done);
        }
        std::vector<store::Series> upstreamSeries;
        upstreamSeries.reserve(upstream.size());
        for (const auto& [upName, future] : upstream) {
            const Result<ColumnHandle>& r = future.get();
            if (!r) {
                return Failure{upstreamError(upName, r.error())};
            }
            upstreamSeries.push_back(r->series);
        }
        evaluations->fetch_add(1, std::memory_order_acq_rel);
        Result<std::vector<analysis::UniformSignal>> signals = readAll(upstreamSeries, token, seal.priority);
        if (!signals) {
            return Failure{signals.error()};
        }
        EvalContext c = ctx;
        c.cancel = token;
        Diagnostics diags;
        Result<analysis::UniformSignal> out = evaluate(c, *signals, diags);
        if (!out) {
            return Failure{out.error()};
        }
        WholeFn again = [fn = evaluate, baseCtx = ctx, upstreamSeries,
                         prio = seal.priority](const CancelToken& t) -> Result<analysis::UniformSignal> {
            Result<std::vector<analysis::UniformSignal>> ups = readAll(upstreamSeries, t, prio);
            if (!ups) {
                return Failure{ups.error()};
            }
            EvalContext repeat = baseCtx;
            repeat.cancel = t;
            Diagnostics ignored;
            return fn(repeat, *ups, ignored);
        };
        return remember(*memo,
                        sealColumn(seal, std::move(out).value(), std::move(again), std::move(diags), token));
    }
};

// One input of an item task: a logged series or an upstream node's sealed column.
struct InputSeries {
    ChannelKey key;
    std::string name;
    units::UnitId storedUnit = units::kNoUnit;
    store::Series series;
};

// Compiles the item and starts a PlanStream over the node's extent (D-191); the stream reads `inputs`
// through getChannel, so the evaluation shares them.
Result<std::unique_ptr<graph::NodeEvaluation>>
startItem(const Ast& ast, const Scope& scope, const MathItem& item, const MathsCompat& compat,
          const EvalContext& base, std::shared_ptr<const std::vector<InputSeries>> inputs, Priority priority,
          const CancelToken& token) {
    Diagnostics ignored; // the definition's diagnostics are the graph's; a repeat compile gives the same
    Result<Plan> plan = graph::buildPlan(ast, scope, item, compat, ignored);
    if (!plan) {
        return Failure{plan.error()};
    }
    auto evaluation = std::make_unique<graph::NodeEvaluation>();
    evaluation->plan = std::move(plan).value();
    EvalContext c = base;
    c.cancel = token;
    const units::UnitRegistry* units = base.units.get();
    c.getChannel = [inputs = std::move(inputs), units, token, priority](
                       const ChannelKey& key, units::UnitId unit, Rate rate) -> Result<MathInputGatherer> {
        for (const InputSeries& in : *inputs) {
            if (!(in.key == key)) {
                continue;
            }
            units::Converter conv;
            if (unit != in.storedUnit && unit != units::kNoUnit && in.storedUnit != units::kNoUnit &&
                units != nullptr) {
                Result<units::Converter> cv = units->converter(in.storedUnit, unit);
                if (!cv) {
                    return Failure{cv.error()};
                }
                conv = *cv;
            }
            return MathInputGatherer(in.series, rate, 0, store::Interp::Linear, conv, token, priority);
        }
        return fail(ErrorCode::NotFound, "no input '" + key.nameCf + "' for the maths evaluation");
    };
    evaluation->stream = std::make_unique<lang::PlanStream>(evaluation->plan, std::move(c));
    if (Result<void> r = evaluation->stream->start(); !r) {
        return Failure{r.error()};
    }
    return evaluation;
}

// Seals an expression node's column from its started evaluation (03 section 6.0 item 1, D-191): the
// column's chunks are produced and sealed in order while the stream runs.
Result<ColumnHandle> sealStream(const SealArgs& a, Rate rate, graph::NodeEvaluationFactory factory,
                                std::unique_ptr<graph::NodeEvaluation> first, Diagnostics diags,
                                const CancelToken& token) {
    if (token.cancelled()) {
        return fail(ErrorCode::Cancelled, "cancelled after evaluating '" + a.name + "'");
    }
    const Micros t0 = first->stream->t0();
    const auto size = static_cast<SampleIdx>(first->stream->size());
    graph::StreamPlanProvider::Info info;
    info.version = a.version;
    info.dtype = a.dtype;
    info.size = size;
    info.name = a.name;
    auto provider =
        std::make_shared<graph::StreamPlanProvider>(std::move(info), std::move(factory), std::move(first));
    store::ColumnOptions options;
    options.cache = a.cache;
    options.pool = a.pool;
    options.taskKeyPrefix = a.chunkKeyPrefix;
    store::Column column(std::move(provider), std::move(options));
    if (size > 0) {
        // Every chunk into the ChunkCache now (the 0.1 output is one eviction group). This task builds them
        // in order; a chunk-independent column also offers the others to idle workers, one priority band
        // below the node's so that they never run ahead of other nodes' tasks (this task takes back any
        // still queued when it gets there).
        const Priority helpers =
            a.priority == Priority::Interactive ? Priority::Visible : Priority::Background;
        Result<store::ChunkHandle> sealed = column.require(0, size - 1, helpers, token);
        if (!sealed) {
            return Failure{sealed.error()};
        }
    }
    ColumnHandle handle;
    handle.key = a.key;
    handle.version = a.version;
    handle.descriptor = a.descriptor;
    handle.descriptor.rate = rate;
    handle.descriptor.statusText.clear();
    handle.series = store::UniformSeries{rate, t0, std::move(column)};
    handle.diags = std::move(diags);
    handle.selectionKey = a.selection;
    return handle;
}

// The whole-node task of an item (03 sections 4.4, 6.0).
struct ItemTask {
    SealArgs seal;
    std::shared_ptr<const Ast> ast;
    std::shared_ptr<const graph::FrozenScope> scope;
    MathItem item;
    MathsCompat compat;
    Diagnostics warnings; // the definition's warnings and infos, reported with the result
    std::vector<InputSeries> logged;
    struct Upstream {
        ChannelKey key;
        std::string name;
        units::UnitId storedUnit = units::kNoUnit;
        Future<ColumnHandle> future;
    };
    std::vector<Upstream> upstream;
    EvalContext ctx;
    std::shared_ptr<std::atomic<std::uint64_t>> evaluations;
    std::shared_ptr<graph::EvalMemo> memo;

    Result<ColumnHandle> operator()(const CancelToken& token) const {
        if (std::optional<ColumnHandle> done = recall(*memo)) {
            return std::move(*done);
        }
        auto inputs = std::make_shared<std::vector<InputSeries>>(logged);
        for (const Upstream& u : upstream) {
            const Result<ColumnHandle>& r = u.future.get();
            if (!r) {
                return Failure{upstreamError(u.name, r.error())};
            }
            inputs->push_back(InputSeries{u.key, u.name, u.storedUnit, r->series});
        }
        evaluations->fetch_add(1, std::memory_order_acq_rel);
        graph::NodeEvaluationFactory factory =
            [tree = ast, frozen = scope, def = item, sw = compat, base = ctx,
             shared = std::shared_ptr<const std::vector<InputSeries>>(inputs),
             prio = seal.priority](const CancelToken& t) -> Result<std::unique_ptr<graph::NodeEvaluation>> {
            return startItem(*tree, *frozen, def, sw, base, shared, prio, t);
        };
        Result<std::unique_ptr<graph::NodeEvaluation>> first = factory(token);
        if (!first) {
            return Failure{first.error()};
        }
        return remember(*memo, sealStream(seal, ctx.resultRate, std::move(factory), std::move(first).value(),
                                          warnings, token));
    }
};

} // namespace

// ---- Impl -------------------------------------------------------------------------------------------------

std::string MathGraph::Impl::taskTextOf(const MathNode& n) const {
    std::string text = MathGraph::taskKeyPrefix(sourceId()) + graph::escapeSegment(layer) + "/" +
                       graph::escapeSegment(n.key.nameCf) + "/" + n.version.hex();
    if (n.selectionDependent) {
        text += "/" + selectionKeyOf(n).hex();
    }
    return text;
}

TaskKey MathGraph::Impl::taskKeyOf(const MathNode& n) const {
    return TaskKey{taskTextOf(n) + "/all"};
}

// The node's version (and selection key) is part of its chunk keys: a new version's column never joins or
// raises a chunk task of the superseded column (share keys coalesce by text).
std::string MathGraph::Impl::chunkKeyPrefix(const MathNode& n) const {
    std::string prefix = "chunk/" + sourceId().hex() + "/" + graph::escapeSegment(layer) + "/maths/" +
                         graph::escapeSegment(n.key.nameCf) + "/" + n.version.hex();
    if (n.selectionDependent) {
        prefix += "/" + selectionKeyOf(n).hex();
    }
    return prefix;
}

void MathGraph::Impl::cancelInflight(MathNode& n) const {
    if (!n.inflight.valid() || n.inflight.ready()) {
        return;
    }
    if (TaskPool* p = pool != nullptr ? pool : TaskPool::installed()) {
        // The superseded evaluation and its chunk tasks stop (their keys carry the old version); a late
        // completion is stale and ignored.
        std::string text = MathGraph::taskKeyPrefix(sourceId()) + graph::escapeSegment(layer) + "/" +
                           graph::escapeSegment(n.key.nameCf) + "/" + n.inflightVersion.hex();
        if (n.selectionDependent) {
            text += "/" + n.inflightSelection.hex();
        }
        p->cancelAll(text + "/all");
        std::string chunk = "chunk/" + sourceId().hex() + "/" + graph::escapeSegment(layer) + "/maths/" +
                            graph::escapeSegment(n.key.nameCf) + "/" + n.inflightVersion.hex() + "/";
        p->cancelAll(chunk);
    }
}

EvalContext MathGraph::Impl::evalContext(const MathNode& n) const {
    EvalContext ctx;
    ctx.source = source;
    ctx.layer = layer;
    ctx.units = units;
    ctx.ranges = ranges;
    const model::DataLayer* l = dataLayer();
    const Micros duration = l != nullptr ? l->duration : 0;
    // The math window (03 section 7.6): Main when it is in this source, else the earliest selected overlay in
    // it, else the whole layer.
    ctx.mathStartUs = 0;
    ctx.mathEndUs = duration;
    if (n.selectionDependent) {
        ctx.selection = selection;
        ctx.mainDist = mainDist;
        ctx.refDist = refDist;
        // The same window selectionKeyOf() hashes, so the cached column always matches what the task read.
        std::tie(ctx.mathStartUs, ctx.mathEndUs) = mathWindow();
    }
    ctx.resultRate = n.rate;
    ctx.resultFirst = 0;
    ctx.resultCount =
        duration > 0 && n.rate.valid() ? static_cast<std::uint64_t>(n.rate.indexAt(duration - 1, 0)) + 1 : 0;
    return ctx;
}

Future<ColumnHandle> MathGraph::Impl::require(MathNode& n, Priority priority, const CancelToken& cancel) {
    if (n.ignored) {
        return graph::readyFailure<ColumnHandle>(
            ErrorCode::Invalid, "'" + n.name + "' is ignored (W-DUP-IGNORED)", "W-DUP-IGNORED");
    }
    if (n.off != graph::Off::None) {
        return graph::readyFailure<ColumnHandle>(ErrorCode::Invalid,
                                                 "maths output '" + n.name + "' is disabled", "disabled");
    }
    if (n.defError) {
        return makeReadyFuture(Result<ColumnHandle>(Failure{*n.defError}));
    }
    const Hash128 sel = selectionKeyOf(n);
    if (n.handle && n.handle->version == n.version && n.handleSelection == sel) {
        return makeReadyFuture(Result<ColumnHandle>(*n.handle));
    }
    if (n.failure && n.failureSelection == sel) { // a deterministic failure stays until the version changes
        return makeReadyFuture(Result<ColumnHandle>(Failure{*n.failure}));
    }
    if (n.inflight.valid() && n.inflightVersion == n.version && n.inflightSelection == sel &&
        n.inflight.ready()) {
        const std::optional<Result<ColumnHandle>> done = n.inflight.tryGet();
        if (done && (done->ok() || done->error().code != ErrorCode::Cancelled)) {
            return n.inflight; // finished, not applied yet; a deterministic failure stays until a change
        }
    }
    TaskPool* p = pool != nullptr ? pool : TaskPool::installed();
    store::ChunkCache* c = cache != nullptr ? cache : store::ChunkCache::installed();
    if (p == nullptr || c == nullptr) {
        return graph::readyFailure<ColumnHandle>(ErrorCode::Invalid,
                                                 "no TaskPool or ChunkCache for maths evaluation");
    }
    SealArgs seal;
    seal.key = n.key;
    seal.version = n.version;
    seal.selection = sel;
    seal.name = n.name;
    seal.descriptor = n.descriptor;
    seal.dtype = n.dtype;
    seal.cache = c;
    seal.pool = p;
    seal.chunkKeyPrefix = chunkKeyPrefix(n);
    seal.priority = priority;
    if (!n.memo || n.memoVersion != n.version || n.memoSelection != sel) {
        n.memo = std::make_shared<graph::EvalMemo>();
        n.memoVersion = n.version;
        n.memoSelection = sel;
    }
    const std::string text = taskTextOf(n);
    Hash128Stream keyHash;
    keyHash.updatePart(text);
    const MathTaskKey taskKey{keyHash.digest(), MathTaskKey::kAll};

    Future<ColumnHandle> f;
    if (n.builtin) {
        BuiltinTask task;
        task.seal = std::move(seal);
        task.evaluate = n.def.evaluate;
        for (const std::size_t u : n.upstream) {
            task.upstream.emplace_back(nodes[u]->name, require(*nodes[u], priority, cancel));
        }
        task.ctx = evalContext(n);
        task.evaluations = evaluations;
        task.memo = n.memo;
        f = scheduler.submit(taskKey, text, priority, cancel, std::move(task));
    } else {
        ItemTask task;
        task.seal = std::move(seal);
        task.ast = n.ast;
        task.scope = n.scope;
        task.item = n.item;
        task.compat = compat;
        for (const Diagnostic& d : n.diags) {
            if (d.level != Diagnostic::Error) {
                task.warnings.push_back(d);
            }
        }
        for (const graph::InputRef& r : n.inputs) {
            if (r.fromNode) {
                task.upstream.push_back(ItemTask::Upstream{r.key, r.name, r.storedUnit,
                                                           require(*nodes[r.node], priority, cancel)});
            } else if (r.channel != nullptr) {
                task.logged.push_back(InputSeries{r.key, r.name, r.storedUnit, r.channel->series});
            }
        }
        task.ctx = evalContext(n);
        task.evaluations = evaluations;
        task.memo = n.memo;
        f = scheduler.submit(taskKey, text, priority, cancel, std::move(task));
    }
    n.inflight = f;
    n.inflightVersion = n.version;
    n.inflightSelection = sel;
    if (!f.ready()) {
        n.status.state = MathState::Evaluating;
        n.status.summary = "Calculating";
        n.status.progress = 0.0F;
    }
    return f;
}

} // namespace esses::maths
