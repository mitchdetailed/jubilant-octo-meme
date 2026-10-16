// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathGraph internals (src/graph, 03 sections 4.3-4.4, 7.1-7.4, 8): the node record, the stored documents,
// the frozen per-node Scope a task compiles against, the definition of MathGraph::Impl and the helpers the
// graph sources share. Internal to esses_maths.
//
//   math_graph.cpp    the public members, status application, descriptors
//   graph_build.cpp   rebuild(): chains, name resolution (03 section 3.3), cycles, compile, versions
//   graph_eval.cpp    require(): the whole-node tasks of items and built-in outputs, sealing into the cache
#pragma once

#include <esses/analysis/distance_index.hpp>
#include <esses/analysis/range_tables.hpp>
#include <esses/base/future.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/maths/ast.hpp>
#include <esses/maths/builtin_module.hpp>
#include <esses/maths/function_registry.hpp>
#include <esses/maths/kernel.hpp>
#include <esses/maths/math_graph.hpp>
#include <esses/maths/maths_scheduler.hpp>
#include <esses/maths/plan.hpp>
#include <esses/maths/scope.hpp>
#include <esses/model/layer.hpp>
#include <esses/store/chunk_cache.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace esses::maths::graph {

// Part of every defHash (03 section 7.2 engineVersion): bump when a graph evaluation rule changes.
inline constexpr std::string_view kEngineVersion = "esses_maths 0.1 whole-node M2 graph";
// Deepest chain of maths items reading each other that a rebuild follows (E-EXPR-TOO-DEEP beyond it).
inline constexpr std::size_t kMaxDepth = 256;
// Columns a SelectionDependent node keeps for earlier selections (toggling overlays back is free, 03 7.2).
inline constexpr std::size_t kSelectionCacheSize = 8;

// Why a node is Disabled (03 section 7.3).
enum class Off : std::uint8_t { None, Item, Module, Condition, Missing };

// A data-backed Scope frozen for one requester: the entries its references resolved to at build time. Tasks
// compile against it on a worker (immutable after construction; it keeps the unit registry alive).
class FrozenScope final : public Scope {
public:
    explicit FrozenScope(std::shared_ptr<const units::UnitRegistry> units, bool dataBacked = true)
        : Scope(FunctionRegistry::builtin(), *units), units_(std::move(units)), dataBacked_(dataBacked) {}

    std::optional<ScopeEntry> lookup(std::string_view nameCf,
                                     const ChannelKey& /*requester*/) const override {
        const auto it = entries.find(std::string(nameCf));
        if (it == entries.end()) {
            return std::nullopt;
        }
        return it->second;
    }
    bool dataBacked() const noexcept override { return dataBacked_; }

    std::unordered_map<std::string, ScopeEntry> entries; // case-folded name -> entry

private:
    std::shared_ptr<const units::UnitRegistry> units_;
    bool dataBacked_ = true;
};

// compile() of an item's AST; for a table2d item whose plan does not end in the Table stage yet, the stage
// of src/kernels is appended after the axis plan (03 section 4.4: real output in the base unit of the values,
// F32 unless the precision or an input is double). Used by the rebuild (metadata) and by every evaluation
// task (a fresh Plan each time: kernels keep state).
Result<Plan> buildPlan(const Ast& ast, const Scope& scope, const MathItem& item, const MathsCompat& compat,
                       Diagnostics& diags);

// What one PlanInput key reads when the task runs.
struct InputRef {
    ChannelKey key;   // the PlanInput key (unique per target within the node)
    std::string name; // the target's name (error messages)
    bool fromNode = false;
    std::size_t node = 0;                      // fromNode: index into Impl::nodes
    const model::Channel* channel = nullptr;   // logged: points into Impl::source
    units::UnitId storedUnit = units::kNoUnit; // the unit the target's values are stored in
};

// A parsed text (expression, synthesised filter/scale_offset expression, table axis, condition), cached per
// compat switches and text so that a rebuild parses only what changed.
struct ParsedText {
    std::shared_ptr<const Ast> ast;
    Diagnostics diags;
    std::optional<Error> error;
    bool used = false; // touched by the current rebuild (unused entries are dropped after it)
};

// The sealed result of one (version, selection) of a node, shared by every task submitted for it. A task
// that finds it filled returns it without evaluating: a require() racing the completion of the node's task
// (the share key no longer coalesces once the task is done) costs no second evaluation.
struct EvalMemo {
    std::mutex mutex;
    std::optional<ColumnHandle> handle;
};

struct SelectionCached {
    Hash128 selection;
    ColumnHandle handle;
    MathStatus status;
};

struct MathNode {
    // ---- identity (stable across rebuilds) and place in the chains
    std::string identity;
    ChannelKey key;
    std::string name;
    std::string moduleId;
    MathModule::Origin origin = MathModule::Origin::Global;
    ItemKind kind = ItemKind::Expression;
    bool builtin = false;
    std::size_t builtinReg = 0; // index into Impl::builtins
    std::size_t moduleSlot = 0; // Global: index into Impl::globals
    std::size_t itemIndex = 0;  // index into the module's items or the registration's outputs
    std::uint32_t chainIndex = 0;
    bool ignored = false; // W-DUP-IGNORED
    Off off = Off::None;
    std::string offSummary;

    // ---- definition (rebuild)
    MathItem item;     // the effective item (module precision applied)
    BuiltinOutput def; // built-in outputs
    std::shared_ptr<const Ast> ast;
    std::shared_ptr<const FrozenScope> scope;
    Diagnostics diags;             // definition diagnostics (parse, check, compile, cycle, duplicate)
    std::optional<Error> defError; // the node cannot be evaluated
    bool onCycle = false;
    std::string cyclePath; // "A -> B -> A" from this node
    bool selfMaths = false;
    ChannelKey resolvedInput; // self-maths: the key of the element read (MathStatus::resolvedInput)
    std::string sourceText;   // the expression compiled (synthesised for filter and scale_offset)
    std::string settingsText; // built-in outputs: the canonical settings they were expanded from
    // The compile result of `compileSig` (reused while the signature holds).
    bool compiled = false;
    Diagnostics compileDiags;
    std::optional<Error> compileError;
    std::vector<ChannelKey> planKeys;
    std::vector<MathInput> inputsInfo;
    std::vector<InputRef> inputs;      // the PlanInput keys (items) and their targets
    std::vector<std::size_t> upstream; // nodes read (items: maths targets; built-ins: own module upstream)
    std::vector<const model::Channel*> loggedRead; // logged channels read (version)
    std::vector<std::string> rangeGroups;
    Hash128 scopeHash;  // the values of the details and constants read, the resolved inputs
    Hash128 compileSig; // definition + scope: an unchanged signature reuses the previous compile
    Hash128 defHash;
    std::optional<Rate> planRate; // the plan's own rate (explicit or from its series inputs)
    Rate rate;                    // the result rate (planRate, else the layer's highest logged rate)
    units::UnitId storedUnit = units::kNoUnit;
    store::Dtype dtype = store::Dtype::F32;
    bool integer = false;
    Kind outKind = Kind::Series; // the plan's result kind (LapScalar and Scalar results step, D-185)
    bool checkOnly = false;      // a graph without data: checked, not compiled (names are I-UNRESOLVED)
    bool ownSelectionDependent = false;
    bool selectionDependent = false; // own or inherited from an upstream node
    model::ChannelDescriptor descriptor;
    Version version;
    std::uint8_t buildState = 0; // 0 not built, 1 building (on the stack), 2 built

    // ---- runtime (kept across rebuilds while the version holds)
    MathStatus status;
    std::optional<ColumnHandle> handle; // evaluated for `version` and `handleSelection`
    Hash128 handleSelection;
    std::vector<SelectionCached> selectionCache; // SelectionDependent: columns of earlier selections
    std::optional<Error> failure;                // the applied failure of `version` (not Cancelled)
    Hash128 failureSelection;
    Future<ColumnHandle> inflight; // the evaluation for inflightVersion / inflightSelection, until applied
    Version inflightVersion;
    Hash128 inflightSelection;
    std::shared_ptr<EvalMemo> memo; // of memoVersion / memoSelection
    Version memoVersion;
    Hash128 memoSelection;

    bool active() const noexcept { return !ignored && off == Off::None; }
};

// One defineBuiltin registration.
struct BuiltinReg {
    const IBuiltinModule* module = nullptr;
    std::string id;
    model::OpaqueJson settings;         // as registered (project settings)
    std::vector<BuiltinOutput> outputs; // the expansion of `settings`
    // The expansion of an override (a builtin item's settings) for this source, by its canonical text.
    std::string overrideText;
    std::optional<Result<std::vector<BuiltinOutput>>> overrideOutputs;
    Diagnostics overrideDiags;
};

// '/' separates task-key segments and '%' escapes: percent-escape both in names.
std::string escapeSegment(std::string_view s);
void updateHash(Hash128Stream& h, const Hash128& v);
// What a node reads of one logged channel: its column identity and store version, unit and time base.
void updateChannel(Hash128Stream& h, const model::Channel& channel);
MathStatus freshStatus(const Version& version, Off off, std::string_view offSummary);
MathStatus statusFromResult(const Version& version, const Result<ColumnHandle>& r);
MathStatus errorStatus(const Version& version, const Error& error, const Diagnostics& diags);
Error definitionError(std::string_view code, const std::string& message);
bool isDiagnosticCode(const std::optional<std::string>& detail);
std::string_view itemKindName(ItemKind k) noexcept;
// The layer's highest logged rate (03 section 6.4 rule 3), nominalRate() of every channel; Rate{} when none.
Rate highestRate(const model::DataLayer& layer);
// Nominal rate of a channel (03 section 6.4 rule 2): the descriptor's, else the series', else for a
// timestamped series timestampedRate().
std::optional<Rate> nominalRate(const model::Channel& c);
// 1 / mean interval of a timestamped series rounded to whole Hz (a period below 1 Hz); Rate{} when it has
// fewer than two samples (D-185).
Rate timestampedRate(const store::TimestampedSeries& ts);
units::UnitId baseUnitOf(units::UnitId u, const units::UnitRegistry& units);

template <class T> Future<T> readyFailure(ErrorCode code, std::string message, std::string detail = {}) {
    ErrorContext c;
    if (!detail.empty()) {
        c.detail = std::move(detail);
    }
    return makeReadyFuture(Result<T>(fail(code, std::move(message), std::move(c))));
}

} // namespace esses::maths::graph

namespace esses::maths {

struct MathGraph::Impl {
    std::shared_ptr<const model::DataSource> source;
    std::string layer;
    std::shared_ptr<const units::UnitRegistry> units;
    MathsCompat compat;
    TaskPool* pool = nullptr;
    store::ChunkCache* cache = nullptr;
    MathsScheduler scheduler;

    // ---- the documents
    std::vector<graph::BuiltinReg> builtins; // registration order
    std::vector<MathModule> globals;         // project order
    std::optional<MathModule> local;
    std::optional<MathModule> transient;
    std::unordered_set<std::string> disabledModules;
    model::MappingTable mappings;
    model::AliasTable aliases;
    MathSelection selection;
    std::shared_ptr<const analysis::DistanceIndex> mainDist;
    std::shared_ptr<const analysis::DistanceIndex> refDist;
    std::uint64_t distanceGeneration = 0;

    // ---- derived by rebuild()
    std::vector<std::unique_ptr<graph::MathNode>> nodes;              // chain-building order
    std::unordered_map<std::string, std::vector<std::size_t>> chains; // nameCf -> elements (not ignored)
    std::vector<std::string> outputOrder;                             // nameCf in first-definition order
    std::shared_ptr<const analysis::RangeTables> ranges;              // of the current source snapshot
    const model::DataSource* rangesFor = nullptr;
    std::unordered_map<std::string, graph::ParsedText> astCache; // compat + text -> parsed text

    std::shared_ptr<std::atomic<std::uint64_t>> evaluations = std::make_shared<std::atomic<std::uint64_t>>(0);

    explicit Impl(TaskPool* p) : pool(p), scheduler(p) {}

    model::SourceId sourceId() const { return source != nullptr ? source->id : model::SourceId{}; }
    const model::DataLayer* dataLayer() const { return source != nullptr ? source->layer(layer) : nullptr; }
    bool moduleEnabled(std::string_view id) const { return disabledModules.count(std::string(id)) == 0; }

    // The chain head of `key`: the last enabled element, else the last element; nullptr when none.
    graph::MathNode* head(const ChannelKey& key) const;
    graph::MathNode* findNode(const NodeId& id) const;
    // The node with `key` whose version (or in-flight version) is `version`.
    graph::MathNode* findVersion(const ChannelKey& key, const Version& version) const;
    NodeId idOf(const graph::MathNode& n) const;
    // The selection key a SelectionDependent node caches under (zero for every other node): the selection's
    // key, the DistanceIndex generation and the resolved math window.
    Hash128 selectionKeyOf(const graph::MathNode& n) const;
    // The math window of this source and layer (03 section 7.6): [start, end) of Main when it is here, else
    // of the earliest selected overlay here, else the whole layer.
    std::pair<Micros, Micros> mathWindow() const;

    // graph_build.cpp: rebuilds chains, resolution, compile metadata and versions from the documents;
    // carries the runtime state of every node whose identity and version hold. Returns the keys whose
    // version changed, new and removed keys included, in node order.
    std::vector<ChannelKey> rebuild();

    // graph_eval.cpp
    Future<ColumnHandle> require(graph::MathNode& n, Priority priority, const CancelToken& cancel);
    TaskKey taskKeyOf(const graph::MathNode& n) const;
    std::string taskTextOf(const graph::MathNode& n) const; // the key without "/all"
    std::string chunkKeyPrefix(const graph::MathNode& n) const;
    EvalContext evalContext(const graph::MathNode& n) const;
    void cancelInflight(graph::MathNode& n) const;
    // Applies one completion to `n` (owning thread).
    void apply(graph::MathNode& n, const Version& version, const Result<ColumnHandle>& result);
};

} // namespace esses::maths
