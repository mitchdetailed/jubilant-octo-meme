// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/base/casefold.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/maths/math_graph.hpp>
#include <esses/maths/plan_provider.hpp>
#include <esses/maths/whole_signal.hpp>
#include <esses/model/layer.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/column.hpp>
#include <esses/store/series.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace esses::maths {

namespace {

// Part of every defHash (03 section 7.2 engineVersion): bump when an evaluation rule changes.
constexpr std::string_view kEngineVersion = "esses_maths 0.1 whole-node M1";

struct Node {
    ChannelKey key;
    std::string moduleId;
    BuiltinOutput def;
    std::vector<ChannelKey> upstream;
    Hash128 defHash;
    Version version;
    MathStatus status;
    model::ChannelDescriptor descriptor; // rate and statusText are filled by MathGraph::descriptor
    std::optional<ColumnHandle> handle;  // evaluated for `version`
    std::optional<Error> failure;        // the applied evaluation failure of `version` (not Cancelled)
    Future<ColumnHandle> inflight;       // the evaluation for inflightVersion, until applied
    Version inflightVersion;
};

// '/' separates task-key segments and '%' escapes: percent-escape both in names.
std::string escapeSegment(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c == '%') {
            out += "%25";
        } else if (c == '/') {
            out += "%2F";
        } else {
            out += c;
        }
    }
    return out;
}

void updateHash(Hash128Stream& h, const Hash128& v) {
    h.updateU64(v.lo);
    h.updateU64(v.hi);
}

Hash128 defHashOf(const IBuiltinModule& module, const model::OpaqueJson& settings, const BuiltinOutput& o,
                  const MathsCompat& compat) {
    Hash128Stream h;
    h.updatePart("esses.maths.defHash-1");
    h.updatePart(kEngineVersion);
    h.updatePart("builtin");
    h.updatePart(module.id());
    h.updateU64(static_cast<std::uint64_t>(module.version()));
    h.updatePart(settings.compact());
    h.updatePart(o.name);
    h.updateU64(o.upstream.size());
    for (const std::string& up : o.upstream) {
        h.updatePart(caseFold(up));
    }
    h.updateU64(o.inputs.size());
    for (const std::string& in : o.inputs) {
        h.updatePart(caseFold(in));
    }
    h.updateU64(o.quantity);
    h.updateU64(o.resultUnit);
    h.updateU64(static_cast<std::uint64_t>(o.interp));
    h.updateU64(static_cast<std::uint64_t>(o.dtype));
    h.updateU64(compat.unaryMinusAbovePower ? 1u : 0u);
    return h.digest();
}

// What a node reads of one logged channel: its column identity and store version, unit and time base.
void updateChannel(Hash128Stream& h, const model::Channel& channel) {
    h.updatePart(channel.desc.id.str());
    h.updateU64(
        static_cast<std::uint64_t>(channel.status)); // an input in Error is rejected by the evaluation
    h.updateU64(channel.desc.unit);
    h.updateU64(static_cast<std::uint64_t>(channel.desc.dtype));
    h.updateU64(static_cast<std::uint64_t>(channel.desc.interp));
    h.updateU64(channel.series.index());
    if (const auto* u = std::get_if<store::UniformSeries>(&channel.series)) {
        h.updateU64(u->rate.num);
        h.updateU64(u->rate.den);
        h.updateU64(static_cast<std::uint64_t>(u->t0));
        h.updateU64(u->values.id());
        h.updateU64(u->values.version());
        h.updateU64(static_cast<std::uint64_t>(u->values.size()));
    } else if (const auto* t = std::get_if<store::TimestampedSeries>(&channel.series)) {
        h.updateU64(t->timesUs.id());
        h.updateU64(t->timesUs.version());
        h.updateU64(t->values.id());
        h.updateU64(t->values.version());
        h.updateU64(static_cast<std::uint64_t>(t->tMin));
        h.updateU64(static_cast<std::uint64_t>(t->tMax));
    }
}

model::ChannelDescriptor makeDescriptor(const model::SourceId& src, std::string_view layer,
                                        const BuiltinOutput& o, std::string_view moduleId) {
    model::ChannelDescriptor d;
    d.id = model::channelIdMathsModule(src, layer, o.name, moduleId);
    d.name = o.name;
    d.loggedName = o.name;
    d.unit = o.resultUnit;
    d.quantity = o.quantity;
    d.dtype = o.dtype;
    d.kind = o.dtype == store::Dtype::I32 ? model::SampleKind::Integer : model::SampleKind::Real;
    d.interp = o.interp;
    d.dps = o.display.dps;
    d.colourIndex = o.display.colourIndex;
    d.source = model::ChannelSource::Maths;
    return d;
}

MathStatus freshStatus(const Version& version, bool enabled) {
    MathStatus s;
    s.version = version;
    s.state = enabled ? MathState::Pending : MathState::Disabled;
    s.summary = enabled ? "Pending" : "Disabled";
    return s;
}

bool isDiagnosticCode(const std::optional<std::string>& detail) {
    if (!detail || detail->size() < 3 || !(detail->starts_with("E-"))) {
        return false;
    }
    return std::all_of(detail->begin(), detail->end(),
                       [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'; });
}

MathStatus statusFromResult(const Version& version, const Result<ColumnHandle>& r) {
    MathStatus s;
    s.version = version;
    if (r) {
        s.diags = r->diags;
        std::vector<const Diagnostic*> warnings;
        for (const Diagnostic& d : s.diags) {
            if (d.level != Diagnostic::Info) {
                warnings.push_back(&d);
            }
        }
        if (warnings.empty()) {
            s.state = MathState::Ok;
            s.summary = "OK";
        } else {
            s.state = MathState::Warning;
            s.summary = warnings.front()->message;
            if (warnings.size() > 1) {
                s.summary += " (+" + std::to_string(warnings.size() - 1) + " more)";
            }
        }
        s.progress = 1.0F;
    } else if (r.error().code == ErrorCode::Cancelled) {
        s.state = MathState::Pending;
        s.summary = "Pending";
    } else {
        Diagnostic d;
        const std::optional<std::string>& detail = r.error().context.detail;
        d.code = detail && isDiagnosticCode(detail) ? *detail : std::string("E-EVAL");
        d.level = Diagnostic::Error;
        d.message = r.error().message;
        s.state = MathState::Error;
        s.summary = d.message;
        s.diags.push_back(std::move(d));
    }
    return s;
}

template <class T> Future<T> readyFailure(ErrorCode code, std::string message, std::string detail = {}) {
    ErrorContext c;
    if (!detail.empty()) {
        c.detail = std::move(detail);
    }
    return makeReadyFuture(Result<T>(fail(code, std::move(message), std::move(c))));
}

// The whole-node evaluation task of one output (03 section 6.0): a value snapshot of the dependency cone
// (01 section 5.3 rule 9); it never touches the MathGraph.
struct EvalTask {
    ChannelKey key;
    Version version;
    std::string name;
    model::ChannelDescriptor descriptor;
    store::Dtype dtype = store::Dtype::F32;
    WholeExtentFn evaluate;
    std::vector<std::pair<std::string, Future<ColumnHandle>>> upstream;
    EvalContext ctx;
    store::ChunkCache* cache = nullptr;
    TaskPool* pool = nullptr;
    std::string chunkKeyPrefix;
    Priority priority = Priority::Background;

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
        // Upstream nodes: their tasks were submitted (or joined) by require(); get() runs a still-queued one
        // inline and waits only for a running one (Rule W).
        std::vector<store::Series> upstreamSeries;
        upstreamSeries.reserve(upstream.size());
        for (const auto& [upName, future] : upstream) {
            const Result<ColumnHandle>& r = future.get();
            if (!r) {
                Error e = r.error();
                if (e.code != ErrorCode::Cancelled) {
                    e.message = "input '" + upName + "': " + e.message;
                }
                return Failure{std::move(e)};
            }
            upstreamSeries.push_back(r->series);
        }
        Result<std::vector<analysis::UniformSignal>> signals = readAll(upstreamSeries, token, priority);
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
        if (token.cancelled()) {
            return fail(ErrorCode::Cancelled, "cancelled after evaluating '" + name + "'");
        }
        analysis::UniformSignal& whole = *out;
        if (!whole.valid.empty() && whole.valid.size() != whole.values.size()) {
            return fail(ErrorCode::Internal, "'" + name + "' returned a validity vector of the wrong size");
        }
        if (!whole.values.empty() && !whole.rate.valid()) {
            return fail(ErrorCode::Internal, "'" + name + "' returned samples without a rate");
        }

        auto plan = std::make_shared<PlanSnapshot>();
        plan->key = key;
        plan->version = version;
        plan->outDtype = dtype;
        plan->rate = whole.rate;
        plan->t0 = whole.t0;
        plan->size = static_cast<SampleIdx>(whole.values.size());
        plan->name = name;
        // Recomputation after the extent was released (03 section 6.2 Whole: the eviction group is rebuilt by
        // one evaluation): the same definition over the same stored upstream columns.
        plan->evaluate = [fn = evaluate, baseCtx = ctx, upstreamSeries,
                          prio = priority](const CancelToken& t) -> Result<analysis::UniformSignal> {
            Result<std::vector<analysis::UniformSignal>> ups = readAll(upstreamSeries, t, prio);
            if (!ups) {
                return Failure{ups.error()};
            }
            EvalContext again = baseCtx;
            again.cancel = t;
            Diagnostics ignored;
            return fn(again, *ups, ignored);
        };
        const Rate rate = plan->rate;
        const Micros t0 = plan->t0;
        const SampleIdx size = plan->size;
        auto provider = std::make_shared<PlanProvider>(std::move(plan), std::move(whole));

        store::ColumnOptions options;
        options.cache = cache;
        options.pool = pool;
        options.taskKeyPrefix = chunkKeyPrefix;
        store::Column column(std::move(provider), std::move(options));
        if (size > 0) {
            // Seal every chunk into the ChunkCache now (the 0.1 output is one eviction group).
            Result<store::ChunkHandle> sealed = column.require(0, size - 1, priority, token);
            if (!sealed) {
                return Failure{sealed.error()};
            }
        }
        ColumnHandle handle;
        handle.key = key;
        handle.version = version;
        handle.descriptor = descriptor;
        handle.descriptor.rate = rate;
        handle.series = store::UniformSeries{rate, t0, std::move(column)};
        handle.diags = std::move(diags);
        return handle;
    }
};

} // namespace

struct MathGraph::Impl {
    std::shared_ptr<const model::DataSource> source;
    std::string layer;
    std::shared_ptr<const units::UnitRegistry> units;
    MathsCompat compat;
    TaskPool* pool = nullptr;
    store::ChunkCache* cache = nullptr;
    std::vector<std::unique_ptr<Node>> nodes; // definition order
    std::unordered_set<std::string> disabledModules;

    model::SourceId sourceId() const { return source != nullptr ? source->id : model::SourceId{}; }

    Node* find(const ChannelKey& key) const {
        for (const auto& n : nodes) {
            if (n->key == key) {
                return n.get();
            }
        }
        return nullptr;
    }

    bool moduleEnabled(std::string_view id) const { return disabledModules.count(std::string(id)) == 0; }
    bool enabled(const Node& n) const { return moduleEnabled(n.moduleId); }

    std::string keySegments(const Node& n) const {
        return MathGraph::taskKeyPrefix(sourceId()) + escapeSegment(layer) + "/" +
               escapeSegment(n.key.nameCf);
    }
    TaskKey taskKeyOf(const Node& n) const {
        return TaskKey{keySegments(n) + "/" + n.version.hex() + "/all"};
    }
    // The node's version is part of its chunk keys: a new version's column never joins or raises a chunk
    // task of the superseded column (share keys coalesce by text).
    std::string chunkKeyPrefix(const Node& n) const {
        return "chunk/" + sourceId().hex() + "/" + escapeSegment(layer) + "/maths/" +
               escapeSegment(n.key.nameCf) + "/" + n.version.hex();
    }

    // 03 section 7.2: exactly what the node reads; DataSource::version is never part of it.
    Version versionOf(const Node& n, std::unordered_map<const Node*, Version>& memo) const {
        if (const auto it = memo.find(&n); it != memo.end()) {
            return it->second;
        }
        Hash128Stream h;
        h.updatePart("esses.maths.version-1");
        updateHash(h, n.defHash);
        h.updateU64(1u); // item enabled (per-item enable arrives with define(MathItem) in M2)
        h.updateU64(moduleEnabled(n.moduleId) ? 1u : 0u);
        std::vector<Version> ups;
        for (const ChannelKey& k : n.upstream) {
            const Node* u = find(k);
            ups.push_back(u != nullptr ? versionOf(*u, memo) : Version{});
        }
        std::sort(ups.begin(), ups.end());
        h.updateU64(ups.size());
        for (const Version& v : ups) {
            updateHash(h, v);
        }
        const model::DataLayer* l = source != nullptr ? source->layer(layer) : nullptr;
        h.updateU64(l != nullptr ? 1u : 0u);
        if (l != nullptr) {
            h.updateU64(static_cast<std::uint64_t>(l->duration));
        }
        h.updateU64(n.def.inputs.size());
        for (const std::string& input : n.def.inputs) {
            h.updatePart(caseFold(input));
            // Resolved as the evaluation resolves it (analysis' loadSpeed: the channel name, then the logged
            // name), so an input matched by its logged name only is part of the version too.
            const model::Channel* channel = nullptr;
            if (l != nullptr) {
                channel = l->find(input);
                if (channel == nullptr) {
                    channel = l->findLogged(input);
                }
            }
            h.updateU64(channel != nullptr ? 1u : 0u);
            if (channel != nullptr) {
                updateChannel(h, *channel);
            }
        }
        const Version v = h.digest();
        memo.emplace(&n, v);
        return v;
    }

    // Re-hashes every node; a node whose version changed drops its column and evaluation and starts over.
    std::vector<ChannelKey> rehash() {
        std::unordered_map<const Node*, Version> memo;
        std::vector<ChannelKey> changed;
        for (const auto& n : nodes) {
            const Version v = versionOf(*n, memo);
            if (v != n->version) {
                // The superseded version's evaluation and its chunk tasks stop (their keys carry the old
                // version); their late completion is stale and ignored.
                if (n->inflight.valid() && !n->inflight.ready()) {
                    if (TaskPool* p = pool != nullptr ? pool : TaskPool::installed()) {
                        p->cancelAll(taskKeyOf(*n).text);
                        p->cancelAll(chunkKeyPrefix(*n) + "/");
                    }
                }
                n->version = v;
                n->handle.reset();
                n->failure.reset();
                n->inflight = {};
                n->inflightVersion = {};
                n->status = freshStatus(v, enabled(*n));
                changed.push_back(n->key);
            }
        }
        return changed;
    }

    EvalContext evalContext() const {
        EvalContext ctx;
        ctx.source = source;
        ctx.layer = layer;
        const model::DataLayer* l = source != nullptr ? source->layer(layer) : nullptr;
        ctx.mathStartUs = 0;
        ctx.mathEndUs = l != nullptr ? l->duration : 0; // no selection: the whole layer (03 section 7.6)
        ctx.units = units;
        return ctx;
    }

    Future<ColumnHandle> require(Node& n, Priority priority, const CancelToken& cancel) {
        if (!enabled(n)) {
            return readyFailure<ColumnHandle>(ErrorCode::Invalid,
                                              "maths output '" + n.def.name + "' is disabled", "disabled");
        }
        if (n.handle && n.handle->version == n.version) {
            return makeReadyFuture(Result<ColumnHandle>(*n.handle));
        }
        if (n.failure) { // a deterministic failure stays until the version changes
            return makeReadyFuture(Result<ColumnHandle>(Failure{*n.failure}));
        }
        if (n.inflight.valid() && n.inflightVersion == n.version && n.inflight.ready()) {
            const std::optional<Result<ColumnHandle>> done = n.inflight.tryGet();
            if (done && (done->ok() || done->error().code != ErrorCode::Cancelled)) {
                return n.inflight; // finished, not applied yet; a deterministic failure stays until a change
            }
        }
        TaskPool* p = pool != nullptr ? pool : TaskPool::installed();
        store::ChunkCache* c = cache != nullptr ? cache : store::ChunkCache::installed();
        if (p == nullptr || c == nullptr) {
            return readyFailure<ColumnHandle>(ErrorCode::Invalid,
                                              "no TaskPool or ChunkCache for maths evaluation");
        }
        EvalTask task;
        task.key = n.key;
        task.version = n.version;
        task.name = n.def.name;
        task.descriptor = n.descriptor;
        task.dtype = n.def.dtype;
        task.evaluate = n.def.evaluate;
        for (const ChannelKey& k : n.upstream) {
            Node* u = find(k);
            task.upstream.emplace_back(
                u != nullptr ? u->def.name : k.nameCf,
                u != nullptr
                    ? require(*u, priority, cancel)
                    : readyFailure<ColumnHandle>(ErrorCode::NotFound, "no maths output " + k.nameCf));
        }
        task.ctx = evalContext();
        task.cache = c;
        task.pool = p;
        task.chunkKeyPrefix = chunkKeyPrefix(n);
        task.priority = priority;
        Future<ColumnHandle> f = p->submit(priority, taskKeyOf(n), cancel, std::move(task));
        n.inflight = f;
        n.inflightVersion = n.version;
        if (!f.ready()) {
            n.status.state = MathState::Evaluating;
            n.status.summary = "Calculating";
            n.status.progress = 0.0F;
        }
        return f;
    }
};

MathGraph::MathGraph(std::shared_ptr<const model::DataSource> source, std::string layer,
                     std::shared_ptr<const units::UnitRegistry> units, MathsCompat compat, TaskPool* pool,
                     store::ChunkCache* cache)
    : impl_(std::make_unique<Impl>()) {
    impl_->source = std::move(source);
    impl_->layer = layer.empty() ? std::string(model::kNormalLayer) : std::move(layer); // canonical name
    impl_->units = std::move(units);
    impl_->compat = compat;
    impl_->pool = pool;
    impl_->cache = cache;
}

MathGraph::~MathGraph() = default;
MathGraph::MathGraph(MathGraph&&) noexcept = default;
MathGraph& MathGraph::operator=(MathGraph&&) noexcept = default;

Result<std::vector<ChannelKey>> MathGraph::defineBuiltin(const IBuiltinModule& module,
                                                         const model::OpaqueJson& settings,
                                                         Diagnostics& diags) {
    if (impl_->units == nullptr) {
        return fail(ErrorCode::Invalid, "the maths graph has no unit registry");
    }
    const model::OpaqueJson effective = settings.isNull() ? module.defaultSettings() : settings;
    Result<std::vector<BuiltinOutput>> expanded = module.expand(effective, *impl_->units, diags);
    if (!expanded) {
        return Failure{expanded.error()};
    }
    std::vector<BuiltinOutput>& outs = *expanded;
    const std::string moduleId = module.id();
    const model::SourceId src = impl_->sourceId();

    auto reject = [&](ErrorCode code, std::string_view diagCode, std::string message) -> Failure {
        Diagnostic d;
        d.code = std::string(diagCode);
        d.level = Diagnostic::Error;
        d.message = message;
        diags.push_back(std::move(d));
        ErrorContext c;
        c.detail = std::string(diagCode);
        return fail(code, std::move(message), std::move(c));
    };

    std::unordered_map<std::string, std::size_t> byName; // case-folded name -> output index
    std::vector<ChannelKey> keys;
    for (std::size_t i = 0; i < outs.size(); ++i) {
        const BuiltinOutput& o = outs[i];
        if (o.name.empty()) {
            return reject(ErrorCode::Invalid, "E-DUPLICATE-OUTPUT",
                          "module " + moduleId + " has an unnamed output");
        }
        if (!byName.emplace(caseFold(o.name), i).second) {
            return reject(ErrorCode::Invalid, "E-DUPLICATE-OUTPUT",
                          "module " + moduleId + " defines '" + o.name + "' twice");
        }
        if (!o.evaluate) {
            return reject(ErrorCode::Invalid, "E-EVAL",
                          "output '" + o.name + "' of module " + moduleId + " has no evaluate function");
        }
        ChannelKey key = ChannelKey::make(src, impl_->layer, o.name);
        if (const Node* other = impl_->find(key); other != nullptr && other->moduleId != moduleId) {
            return reject(ErrorCode::Invalid, "E-DUPLICATE-OUTPUT",
                          "'" + o.name + "' is already defined by module " + other->moduleId);
        }
        keys.push_back(std::move(key));
    }
    std::vector<std::vector<std::size_t>> edges(outs.size());
    for (std::size_t i = 0; i < outs.size(); ++i) {
        for (const std::string& up : outs[i].upstream) {
            const auto it = byName.find(caseFold(up));
            if (it == byName.end()) {
                std::string message = "output '" + outs[i].name + "' reads '";
                message.append(up).append("', which module ").append(moduleId).append(" does not define");
                return reject(ErrorCode::Invalid, "E-UNKNOWN-UPSTREAM", std::move(message));
            }
            edges[i].push_back(it->second);
        }
    }
    // Cycle check (depth-first, three colours).
    std::vector<std::uint8_t> colour(outs.size(), 0);
    std::function<bool(std::size_t)> cyclic = [&](std::size_t i) {
        colour[i] = 1;
        for (const std::size_t j : edges[i]) {
            if (colour[j] == 1 || (colour[j] == 0 && cyclic(j))) {
                return true;
            }
        }
        colour[i] = 2;
        return false;
    };
    for (std::size_t i = 0; i < outs.size(); ++i) {
        if (colour[i] == 0 && cyclic(i)) {
            return reject(ErrorCode::Cycle, "E-CYCLE",
                          "the outputs of module " + moduleId + " read each other in a cycle");
        }
    }

    // Replace this module's nodes, keeping the state of outputs that remain (their version decides below).
    std::unordered_map<std::string, std::unique_ptr<Node>> previous; // nameCf -> node
    std::size_t insertAt = impl_->nodes.size();
    for (std::size_t i = 0; i < impl_->nodes.size();) {
        if (impl_->nodes[i]->moduleId == moduleId) {
            insertAt = std::min(insertAt, i);
            previous.emplace(impl_->nodes[i]->key.nameCf, std::move(impl_->nodes[i]));
            impl_->nodes.erase(impl_->nodes.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    std::vector<std::unique_ptr<Node>> fresh;
    for (std::size_t i = 0; i < outs.size(); ++i) {
        std::unique_ptr<Node> n;
        if (auto it = previous.find(keys[i].nameCf); it != previous.end()) {
            n = std::move(it->second);
        } else {
            n = std::make_unique<Node>();
            n->status = freshStatus(Version{}, impl_->moduleEnabled(moduleId));
        }
        n->key = keys[i];
        n->moduleId = moduleId;
        n->defHash = defHashOf(module, effective, outs[i], impl_->compat);
        n->descriptor = makeDescriptor(src, impl_->layer, outs[i], moduleId);
        n->upstream.clear();
        for (const std::string& up : outs[i].upstream) {
            n->upstream.push_back(ChannelKey::make(src, impl_->layer, up));
        }
        n->def = std::move(outs[i]);
        fresh.push_back(std::move(n));
    }
    impl_->nodes.insert(impl_->nodes.begin() + static_cast<std::ptrdiff_t>(insertAt),
                        std::make_move_iterator(fresh.begin()), std::make_move_iterator(fresh.end()));
    static_cast<void>(impl_->rehash());
    return keys;
}

std::vector<ChannelKey> MathGraph::setModuleEnabled(std::string_view moduleId, bool enabled) {
    if (enabled) {
        impl_->disabledModules.erase(std::string(moduleId));
    } else {
        impl_->disabledModules.insert(std::string(moduleId));
    }
    return impl_->rehash();
}

std::vector<ChannelKey> MathGraph::setSource(std::shared_ptr<const model::DataSource> source) {
    impl_->source = std::move(source);
    return impl_->rehash();
}

Version MathGraph::version(const ChannelKey& key) const {
    const Node* n = impl_->find(key);
    return n != nullptr ? n->version : Version{};
}

Future<ColumnHandle> MathGraph::require(const ChannelKey& key, SampleRange /*range*/, Priority priority,
                                        const CancelToken& cancel) {
    Node* n = impl_->find(key);
    if (n == nullptr) {
        return readyFailure<ColumnHandle>(ErrorCode::NotFound,
                                          "no maths output '" + key.nameCf + "' in layer " + impl_->layer);
    }
    return impl_->require(*n, priority, cancel);
}

std::vector<MathRequest> MathGraph::prefetch(Priority priority, const CancelToken& cancel) {
    std::vector<MathRequest> out;
    for (const auto& n : impl_->nodes) {
        if (impl_->enabled(*n)) {
            out.push_back(MathRequest{n->key, n->version, impl_->require(*n, priority, cancel)});
        }
    }
    return out;
}

void MathGraph::applyCompletion(const ChannelKey& key, const Version& version,
                                const Result<ColumnHandle>& result) {
    Node* n = impl_->find(key);
    if (n == nullptr || version != n->version || !impl_->enabled(*n)) {
        return; // stale: the node changed (or went away) after the evaluation was scheduled
    }
    const Result<ColumnHandle>* r = &result;
    std::optional<Result<ColumnHandle>> latest;
    if (n->inflight.valid() && n->inflightVersion == version) {
        if (n->inflight.ready()) {
            latest = n->inflight.tryGet(); // the newest evaluation of this version is authoritative
            n->inflight = {};
            if (latest) {
                r = &*latest;
            }
        } else if (!result.ok() && result.error().code == ErrorCode::Cancelled) {
            return; // an earlier, cancelled evaluation; a newer one is still running
        }
    }
    if (*r) {
        n->handle = **r;
        n->failure.reset();
    } else if (r->error().code != ErrorCode::Cancelled) {
        n->failure = r->error();
    }
    n->status = statusFromResult(version, *r);
}

std::vector<ChannelKey> MathGraph::applyReadyCompletions() {
    std::vector<ChannelKey> applied;
    for (const auto& n : impl_->nodes) {
        if (n->inflight.valid() && n->inflight.ready()) {
            const std::optional<Result<ColumnHandle>> r = n->inflight.tryGet();
            if (!r) {
                continue; // not reachable: ready() above
            }
            const ChannelKey key = n->key;
            applyCompletion(key, n->inflightVersion, *r);
            applied.push_back(key);
        }
    }
    return applied;
}

MathStatus MathGraph::status(const ChannelKey& key) const {
    if (const Node* n = impl_->find(key); n != nullptr) {
        return n->status;
    }
    MathStatus s;
    s.state = MathState::Error;
    s.summary = "'" + key.nameCf + "' is not a maths output of layer " + impl_->layer;
    Diagnostic d;
    d.code = "E-NOT-MATHS";
    d.level = Diagnostic::Error;
    d.message = s.summary;
    s.diags.push_back(std::move(d));
    return s;
}

std::vector<ChannelKey> MathGraph::consumersOf(const ChannelKey& key) const {
    std::vector<ChannelKey> out;
    for (const auto& n : impl_->nodes) {
        if (std::find(n->upstream.begin(), n->upstream.end(), key) != n->upstream.end()) {
            out.push_back(n->key);
        }
    }
    return out;
}

std::vector<ChannelKey> MathGraph::outputs() const {
    std::vector<ChannelKey> out;
    out.reserve(impl_->nodes.size());
    for (const auto& n : impl_->nodes) {
        out.push_back(n->key);
    }
    return out;
}

std::optional<model::ChannelDescriptor> MathGraph::descriptor(const ChannelKey& key) const {
    const Node* n = impl_->find(key);
    if (n == nullptr) {
        return std::nullopt;
    }
    model::ChannelDescriptor d = n->descriptor;
    if (n->handle && n->handle->version == n->version) {
        d.rate = n->handle->descriptor.rate;
    }
    switch (toChannelStatus(n->status.state)) {
    case model::ChannelStatus::Ok:
        d.statusText.clear();
        break;
    case model::ChannelStatus::Pending:
        d.statusText = "Pending";
        break;
    case model::ChannelStatus::Error:
        d.statusText = "Error: " + n->status.summary;
        break;
    }
    return d;
}

std::optional<ChannelKey> MathGraph::find(std::string_view name) const {
    ChannelKey key = ChannelKey::make(impl_->sourceId(), impl_->layer, name);
    if (impl_->find(key) != nullptr) {
        return key;
    }
    return std::nullopt;
}

TaskKey MathGraph::taskKey(const ChannelKey& key) const {
    const Node* n = impl_->find(key);
    return n != nullptr ? impl_->taskKeyOf(*n) : TaskKey{};
}

std::string MathGraph::taskKeyPrefix(const model::SourceId& source) {
    return "maths/" + source.hex() + "/";
}

const std::shared_ptr<const model::DataSource>& MathGraph::source() const noexcept {
    return impl_->source;
}

const std::string& MathGraph::layer() const noexcept {
    return impl_->layer;
}

} // namespace esses::maths
