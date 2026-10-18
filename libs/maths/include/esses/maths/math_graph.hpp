// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathGraph: one per (DataSource, layer), mutated on its owning thread only (the GUI thread, or the CLI main
// thread; 03 section 7.1, 01 section 5.3 rule 10). It holds the maths of one layer: the built-in modules
// (defineBuiltin), the project's global modules in project order, the source's local (sidecar) module and its
// transient module (setModules, define), and evaluates their outputs into the ChunkCache.
//
// Shadowing chains (03 section 4.3, D-022): every output name N has a chain
//     logged N -> built-in outputs named N -> local items named N -> global items named N (module order)
//              -> transient items named N
// The first enabled item per name in a module wins; a later one is ignored with W-DUP-IGNORED (status
// Warning, never evaluated). A disabled element (item, module, false condition, built-in without its inputs)
// stays in the chain with status Disabled and is skipped: consumers read the last enabled element (the chain
// head), and a self-maths element (a filter or scale_offset item, an expression that reads its own name, a
// built-in output named like one of its inputs) reads the enabled element before it, else the logged channel.
// References resolve through the Scope order of 03 section 3.3: mappings, aliases (W-ALIAS-FALLBACK past a
// missing candidate or a maths candidate in Error), the chain head, the logged channel (canonical name, then
// logged name), a numeric or text detail, a constant (own module, local, other enabled global modules in
// project order, W-CONST-AMBIGUOUS). Cycles give every node on them E-CYCLE with the path ("A -> B -> A").
// Every structural change rebuilds the chains and recompiles only items whose definition or resolved inputs
// changed; nodes keep their evaluated column while their version holds.
//
// require() never blocks: it returns a Future that resolves on a worker (share key
// maths/<src>/<layer>/<channelKey>/<version>[/<selectionKey>]/all, 03 section 7.4, through MathsScheduler);
// the caller posts completions to the owning thread, which applies them with applyCompletion() (status
// transitions Pending -> Evaluating -> Ok/Warning or Error, 03 section 7.3). applyReadyCompletions() applies
// every finished evaluation at once (the CLI after waiting, or a host that polls).
//
// Evaluation (0.1 whole-node path, 03 section 6.0, D-139): one TaskPool task per node evaluates the whole
// layer extent (items: compile + evaluateWhole; built-in outputs: their WholeExtentFn) and seals it into
// ChunkCache chunks through a Column over a PlanProvider; parallelism is across nodes only. A node's task
// waits for its upstream nodes' tasks (submitted, or joined through their share keys, by require() on the
// owning thread; Future::get runs a still-queued upstream task inline, Rule W) and reads their sealed
// columns, so a downstream output depends only on the stored upstream values. Tasks capture immutable
// snapshots only (01 section 5.3 rule 9): the AST, a frozen copy of the node's resolved scope, the item, the
// source snapshot and the upstream Futures.
//
// Versions and cache keys (03 section 7.2): version = xxh3(defHash, enable state, chain index, sorted
// upstream versions, the identity, store version, unit and time base of exactly the logged columns read, the
// range-group versions the plan reads, the values of the details and constants read, the layer extent and
// result rate); defHash covers the item definition, the module precision, every MathsCompat switch and the
// engine version. DataSource::version is never part of it (D-140). SelectionDependent nodes (ungated stat_*,
// variance_* and everything downstream of them) cache one column per selectionKey.
#pragma once

#include <esses/analysis/distance_index.hpp>
#include <esses/base/cancel.hpp>
#include <esses/base/future.hpp>
#include <esses/base/priority.hpp>
#include <esses/base/result.hpp>
#include <esses/maths/builtin_module.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/maths/diagnostics.hpp>
#include <esses/maths/math_module.hpp>
#include <esses/maths/math_selection.hpp>
#include <esses/maths/math_status.hpp>
#include <esses/maths/maths_compat.hpp>
#include <esses/model/channel.hpp>
#include <esses/model/opaque_json.hpp>
#include <esses/model/resolver.hpp>
#include <esses/model/source.hpp>
#include <esses/units/unit_registry.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
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

// A node of the graph (03 section 4.3 rule 9): an output key and its position in the shadowing chain of that
// name (0 = the first maths element after the logged channel). The public ChannelKey resolves to the chain
// head. A NodeId stays valid until the next structural change (define, remove, setModules, defineBuiltin).
struct NodeId {
    static constexpr std::uint32_t kIgnored = std::numeric_limits<std::uint32_t>::max(); // W-DUP-IGNORED
    ChannelKey key;
    std::uint32_t chainIndex = 0;
    friend bool operator==(const NodeId&, const NodeId&) = default;
};

// One scheduled evaluation: what prefetch() returns per output, so the host can post each completion.
struct MathRequest {
    ChannelKey key;
    Version version;
    Future<ColumnHandle> future;
};

// What one name referenced by a node resolved to (Channel Status "Resolved to", the Maths Editor).
struct MathInput {
    enum class Kind : std::uint8_t { Logged, Maths, Detail, Constant, Missing };
    std::string name; // as written in the expression
    Kind kind = Kind::Missing;
    std::optional<NodeId> node; // Maths: the chain element read
    model::ResolutionTrace trace;
};

// One maths item or built-in output (06 section 10 Channel Status rows, MathsService::statusRows).
struct MathNodeInfo {
    NodeId id;
    std::string name;     // output name as defined
    std::string moduleId; // "user", "local", "~transient", "esses.corrected_speed_distance", ...
    MathModule::Origin origin = MathModule::Origin::Global;
    ItemKind kind = ItemKind::Expression; // Builtin for the outputs of defineBuiltin
    bool head = false;                    // what consumers of the name read
    bool selfMaths = false;               // reads the chain element before it
    bool selectionDependent = false;
    MathStatus status;
    std::vector<MathInput> inputs; // every referenced name, in first-reference order
};

class MathGraph {
public:
    // `pool` nullptr: TaskPool::installed() at require time (the runtime's pool, 01 section 5.3 rule 11);
    // `cache` nullptr: ChunkCache::installed() at require time. require() fails with Invalid when neither is
    // available. `units` is needed by defineBuiltin and to compile items (Invalid / Error without it).
    MathGraph(std::shared_ptr<const model::DataSource> source, std::string layer,
              std::shared_ptr<const units::UnitRegistry> units, MathsCompat compat = {},
              TaskPool* pool = nullptr, store::ChunkCache* cache = nullptr);
    ~MathGraph();
    MathGraph(const MathGraph&) = delete;
    MathGraph& operator=(const MathGraph&) = delete;
    MathGraph(MathGraph&&) noexcept;
    MathGraph& operator=(MathGraph&&) noexcept;

    // Registers (or re-registers after a settings change) a built-in module's outputs (03 section 9; D-016).
    // Null settings: module.defaultSettings(). `module` must outlive the graph (or its next registration): a
    // `builtin` item of the local or a global module with the module's id re-expands it with that item's
    // settings for this source. Returns the output keys in the module's order; a new node, or one whose
    // version changed, becomes Pending (Disabled while the module is disabled); an unchanged output keeps its
    // evaluated column. Outputs the module no longer produces are removed. Outputs of different modules with
    // one name form a shadowing chain in registration order. Errors (nothing changes): the module's expand()
    // error; Invalid for an output name that is empty or repeated within the module (E-DUPLICATE-OUTPUT), for
    // an upstream name that is not an output of the module (E-UNKNOWN-UPSTREAM) and for a missing evaluate
    // function; Cycle for an upstream cycle (E-CYCLE).
    Result<std::vector<ChannelKey>> defineBuiltin(const IBuiltinModule& module,
                                                  const model::OpaqueJson& settings, Diagnostics& diags);

    // (Re)defines one item of `module` (03 section 7.1): the graph's copy of the module (matched by id and
    // origin; an unknown global module is appended to the project order, a local or transient one becomes
    // the layer's local or transient module) takes `module`'s constants, enumerations, condition, enable
    // flag and precision, and `item` replaces its first item of the same (case-folded) name or is appended.
    // Parse, type and compile errors become the node's status (Error) and are added to `diags` with the
    // item's other diagnostics; the node always exists. Returns the item's node (chainIndex kIgnored for a
    // definition ignored with W-DUP-IGNORED; a default NodeId for an item without a name or a Builtin-origin
    // module).
    NodeId define(const MathModule& module, const MathItem& item, Diagnostics& diags);
    // Removes an item from the graph's copy of its module; a built-in output removes its module's
    // registration.
    void remove(const NodeId& node);
    // Enables or disables an item in the graph's copy of its module (a built-in output: its module, as
    // setModuleEnabled).
    void setEnabled(const NodeId& node, bool enabled);
    // Replaces every global, local and transient module (null: none) and rebuilds the shadowing chains;
    // returns the keys whose version changed (removed outputs included). Built-in modules registered with
    // defineBuiltin stay.
    std::vector<ChannelKey> setModules(std::span<const MathModule> globalInOrder, const MathModule* local,
                                       const MathModule* transient);
    // The project's mappings and aliases (02 section 4.6, 03 section 3.3 steps 1-2) for name resolution;
    // returns the keys whose version changed.
    std::vector<ChannelKey> setResolverTables(model::MappingTable mappings, model::AliasTable aliases);
    // The compatibility switches (options.maths.compat; part of every defHash). Returns the re-versioned
    // keys.
    std::vector<ChannelKey> setCompat(MathsCompat compat);
    // SelectionDependent nodes re-key (03 sections 7.2, 7.5, 7.6): returns the keys whose cache key changed.
    // A node that already holds a column for the new selection key comes back with it, without evaluating.
    std::vector<ChannelKey> setSelection(const MathSelection& selection);
    // The DistanceIndexes of the Main range and the reference overlay (EvalContext::mainDist, refDist; may be
    // null) for variance_*; SelectionDependent nodes re-key. Returns their keys.
    std::vector<ChannelKey> setDistanceIndexes(std::shared_ptr<const analysis::DistanceIndex> main,
                                               std::shared_ptr<const analysis::DistanceIndex> reference);
    // How the output's name resolved (Channel Status "Resolved to"): for a self-maths head, the resolution of
    // its input; otherwise the output itself. An empty trace for an unknown key.
    model::ResolutionTrace trace(const ChannelKey& key) const;

    // Disabled status for the module's outputs (their versions change, so cached columns are dropped); true
    // brings them back as Pending. Applies to built-in and item modules. Returns the keys whose version
    // changed.
    std::vector<ChannelKey> setModuleEnabled(std::string_view moduleId, bool enabled);
    // A new snapshot of the same source: conditions, resolution and versions are rebuilt from exactly what
    // each node reads (03 section 7.2, D-140); nodes whose version changed become Pending and drop their
    // column. Returns their keys.
    std::vector<ChannelKey> setSource(std::shared_ptr<const model::DataSource> source);
    // "Recalculate" (MathsService::invalidate): every node of the module drops its column and failure and
    // becomes Pending (versions unchanged; running evaluations of it are cancelled). Returns their keys.
    std::vector<ChannelKey> invalidate(std::string_view moduleId);

    // The head's version (03 section 7.2); the zero Version for an unknown key.
    Version version(const ChannelKey& key) const;
    // NotFound for an unknown key, Invalid for a disabled one, the definition error (Invalid, Cycle) for an
    // item that cannot be evaluated (ready Futures); never blocks (the GUI thread may call it, 01 section
    // 5.3 rule 4). The cached column when the version (and selection key) holds; otherwise one task per node
    // at `priority` (a repeated require joins the queued or running task, registers `cancel` as one more
    // requester and raises its priority), after requiring the upstream nodes. The 0.1 path always evaluates
    // the whole extent; `range` is accepted for the v1.0 chunk path.
    Future<ColumnHandle> require(const ChannelKey& key, SampleRange range, Priority priority,
                                 const CancelToken& cancel);
    // The Background prefetch hook (D-016, AC-M1-10): require() of every enabled built-in output at
    // `priority`; outputs already evaluated come back as ready Futures. The host posts each completion.
    std::vector<MathRequest> prefetch(Priority priority = Priority::Background,
                                      const CancelToken& cancel = {});
    // Applies a posted completion on the owning thread (03 section 7.3 transitions): ignored when no node
    // of `key` has `version` any more; Ok/Warning with the column cached, Pending after Cancelled, Error
    // otherwise (the error's context.detail when it is a diagnostic code such as E-CSD-NO-SPEED, else
    // E-EVAL).
    void applyCompletion(const ChannelKey& key, const Version& version, const Result<ColumnHandle>& result);
    // Applies every in-flight evaluation whose Future is ready; returns the keys whose status was applied.
    std::vector<ChannelKey> applyReadyCompletions();

    // The head's status; an unknown key reports Error E-NOT-MATHS.
    MathStatus status(const ChannelKey& key) const;
    // One node's status (an ignored duplicate: Warning W-DUP-IGNORED); E-NOT-MATHS for an unknown node.
    MathStatus status(const NodeId& node) const;
    // Every item and built-in output, chain heads and shadowed elements alike, in chain-building order
    // (built-in, local, global in project order, transient), ignored duplicates included.
    std::vector<MathNodeInfo> nodes() const;
    // Direct consumers: the outputs (other than `key`) with a node that reads an element of `key`'s chain,
    // in definition order.
    std::vector<ChannelKey> consumersOf(const ChannelKey& key) const;
    // Every output key in definition order.
    std::vector<ChannelKey> outputs() const;
    // The model::ChannelDescriptor of an output's head (03 section 8), or nullopt: id channelIdMathsModule
    // (channelIdMathsLocal for local maths), source Maths, unit = the unit the values are stored in (the base
    // unit of the declared result unit; a self-maths item keeps the shadowed channel's unit and display
    // settings), statusText from the status ("", "Pending", "Error: <summary>"), rate once known.
    std::optional<model::ChannelDescriptor> descriptor(const ChannelKey& key) const;
    // The key of an output by name (case-folded), or nullopt.
    std::optional<ChannelKey> find(std::string_view name) const;
    // The share key of the head's evaluation task for its current version (and selection key)
    // (maths/<src hex>/<layer>/<name>/<version hex>[/<selection hex>]/all; '/' and '%' in names are
    // percent-escaped).
    TaskKey taskKey(const ChannelKey& key) const;
    // "maths/<src hex>/": TaskPool::cancelAll(prefix) stops every evaluation of the source.
    static std::string taskKeyPrefix(const model::SourceId& source);
    // Whole-node evaluations started by this graph's tasks since construction (tests and benchmarks: the
    // invalidation cone of AC-M2-04).
    std::uint64_t evaluationCount() const noexcept;

    const std::shared_ptr<const model::DataSource>& source() const noexcept;
    const std::string& layer() const noexcept;

    struct Impl; // opaque (src/graph/graph_internal.hpp); named publicly so the graph's sources can share it

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace esses::maths
