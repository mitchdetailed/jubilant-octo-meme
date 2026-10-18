// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathGraph's public members (03 section 7.1): the documents it holds (built-in registrations, global, local
// and transient modules), status application on the owning thread (03 section 7.3), descriptors (03
// section 8) and the shared helpers of src/graph. rebuild() is in graph_build.cpp, the evaluation tasks in
// graph_eval.cpp.

#include "graph_internal.hpp"

#include <esses/base/casefold.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/maths/math_graph.hpp>
#include <esses/model/layer.hpp>
#include <esses/store/series.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace esses::maths {

namespace graph {

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

void updateChannel(Hash128Stream& h, const model::Channel& channel) {
    h.updatePart(channel.desc.id.str());
    h.updateU64(static_cast<std::uint64_t>(channel.status)); // an input in Error is rejected by evaluation
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

MathStatus freshStatus(const Version& version, Off off, std::string_view offSummary) {
    MathStatus s;
    s.version = version;
    if (off == Off::None) {
        s.state = MathState::Pending;
        s.summary = "Pending";
    } else {
        s.state = MathState::Disabled;
        s.summary = offSummary.empty() ? std::string("Disabled") : std::string(offSummary);
    }
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
        if (const std::optional<TextSpan>& span = r.error().context.span) {
            d.span = *span;
        }
        s.state = MathState::Error;
        s.summary = d.message;
        s.diags.push_back(std::move(d));
    }
    return s;
}

MathStatus errorStatus(const Version& version, const Error& error, const Diagnostics& diags) {
    MathStatus s;
    s.version = version;
    s.state = MathState::Error;
    s.diags = diags;
    s.summary = error.message;
    for (const Diagnostic& d : diags) {
        if (d.level == Diagnostic::Error) {
            s.summary = d.message;
            break;
        }
    }
    if (!hasError(s.diags)) {
        Diagnostic d;
        const std::optional<std::string>& detail = error.context.detail;
        d.code = detail && isDiagnosticCode(detail) ? *detail : std::string("E-EVAL");
        d.level = Diagnostic::Error;
        d.message = error.message;
        s.diags.push_back(std::move(d));
    }
    return s;
}

Error definitionError(std::string_view code, const std::string& message) {
    ErrorContext c;
    c.detail = std::string(code);
    return Error{code == "E-CYCLE" ? ErrorCode::Cycle : ErrorCode::Invalid, message, std::move(c)};
}

std::string_view itemKindName(ItemKind k) noexcept {
    switch (k) {
    case ItemKind::Expression:
        return "expression";
    case ItemKind::Filter:
        return "filter";
    case ItemKind::ScaleOffset:
        return "scale_offset";
    case ItemKind::Table2D:
        return "table2d";
    case ItemKind::Table3D:
        return "table3d";
    case ItemKind::Builtin:
        return "builtin";
    case ItemKind::Plugin:
        return "plugin";
    case ItemKind::Regression:
        return "regression";
    }
    return "expression";
}

Rate highestRate(const model::DataLayer& layer) {
    Rate best{};
    for (const model::Channel& c : layer.channels) {
        const std::optional<Rate> r = nominalRate(c); // timestamped channels count too (03 section 6.4)
        if (r && r->valid() && (!best.valid() || best < *r)) {
            best = *r;
        }
    }
    return best;
}

std::optional<Rate> nominalRate(const model::Channel& c) {
    if (c.desc.rate) {
        return c.desc.rate;
    }
    if (const auto* u = std::get_if<store::UniformSeries>(&c.series)) {
        return u->rate;
    }
    if (const auto* ts = std::get_if<store::TimestampedSeries>(&c.series)) {
        return timestampedRate(*ts);
    }
    return Rate{};
}

Rate timestampedRate(const store::TimestampedSeries& ts) {
    // 03 section 6.4 rule 2 asks for round(1 / median interval) Hz. The rebuild runs on the owning thread,
    // which never reads samples (01 section 5.3), so the interval is the mean over the extent, from the first
    // and last timestamps; rounded to whole Hz like the median would be (D-185), and kept as a period below
    // 1 Hz.
    if (ts.size() < 2 || ts.tMax <= ts.tMin) {
        return Rate{};
    }
    const double interval = static_cast<double>(ts.tMax - ts.tMin) / static_cast<double>(ts.size() - 1);
    const double hz = 1e6 / interval;
    if (hz >= 1.0) {
        return Rate{static_cast<std::uint32_t>(std::llround(std::min(hz, 4e9))), 1};
    }
    return Rate::fromPeriodUs(static_cast<std::uint64_t>(std::llround(interval)));
}

units::UnitId baseUnitOf(units::UnitId u, const units::UnitRegistry& units) {
    if (u == units::kNoUnit || !units.contains(u)) {
        return units::kNoUnit;
    }
    return units.quantity(units.quantityOf(u)).base;
}

} // namespace graph

using graph::MathNode;
using graph::Off;

// ---- Impl lookups -----------------------------------------------------------------------------------------

MathNode* MathGraph::Impl::head(const ChannelKey& key) const {
    if (key.src != sourceId() || key.layer != layer) {
        return nullptr;
    }
    const auto it = chains.find(key.nameCf);
    if (it == chains.end() || it->second.empty()) {
        return nullptr;
    }
    for (std::size_t k = it->second.size(); k-- > 0;) {
        MathNode* n = nodes[it->second[k]].get();
        if (n->active()) {
            return n;
        }
    }
    return nodes[it->second.back()].get();
}

MathNode* MathGraph::Impl::findNode(const NodeId& id) const {
    if (id.key.src != sourceId() || id.key.layer != layer) {
        return nullptr;
    }
    if (id.chainIndex == NodeId::kIgnored) {
        for (const auto& n : nodes) {
            if (n->ignored && n->key == id.key) {
                return n.get();
            }
        }
        return nullptr;
    }
    const auto it = chains.find(id.key.nameCf);
    if (it == chains.end() || id.chainIndex >= it->second.size()) {
        return nullptr;
    }
    return nodes[it->second[id.chainIndex]].get();
}

MathNode* MathGraph::Impl::findVersion(const ChannelKey& key, const Version& version) const {
    for (const auto& n : nodes) {
        if (!n->ignored && n->key == key && n->version == version) {
            return n.get();
        }
    }
    return nullptr;
}

NodeId MathGraph::Impl::idOf(const MathNode& n) const {
    return NodeId{n.key, n.ignored ? NodeId::kIgnored : n.chainIndex};
}

Hash128 MathGraph::Impl::selectionKeyOf(const MathNode& n) const {
    if (!n.selectionDependent) {
        return Hash128{};
    }
    Hash128Stream h;
    h.updatePart("esses.maths.selectionKey-2");
    graph::updateHash(h, selection.key);
    h.updateU64(distanceGeneration);
    // The math window the task reads (03 section 7.6), resolved against the current snapshot: an edit that
    // moves the window's lap re-keys the node even when MathSelection::key (range references) holds.
    const auto [start, end] = mathWindow();
    h.updateU64(static_cast<std::uint64_t>(start));
    h.updateU64(static_cast<std::uint64_t>(end));
    return h.digest();
}

std::pair<Micros, Micros> MathGraph::Impl::mathWindow() const {
    const model::DataLayer* l = dataLayer();
    const Micros duration = l != nullptr ? l->duration : 0;
    const model::SourceId src = sourceId();
    const auto here = [&](const model::RangeRef& ref) {
        return ref.src == src && model::canonicalLayerName(ref.layer) == model::canonicalLayerName(layer);
    };
    // Main when it is in this source and layer, else the earliest selected overlay in it, else the whole
    // layer [compat].
    if (here(selection.main.ref) && selection.main.end > selection.main.start) {
        return {selection.main.start, selection.main.end};
    }
    if (l != nullptr) {
        for (const model::RangeRef& ref : selection.overlays) {
            if (!here(ref)) {
                continue;
            }
            if (const model::RangeGroup* group = l->ranges.find(ref.groupPath)) {
                for (const model::Range& r : group->ranges) {
                    if (r.id == ref.range) {
                        return {r.start, r.end};
                    }
                }
            }
        }
    }
    return {0, duration};
}

void MathGraph::Impl::apply(MathNode& n, const Version& version, const Result<ColumnHandle>& result) {
    if (version != n.version || !n.active() || n.defError) {
        return; // stale: the node changed after the evaluation was scheduled
    }
    const Result<ColumnHandle>* r = &result;
    std::optional<Result<ColumnHandle>> latest;
    Hash128 resultSelection = selectionKeyOf(n);
    if (n.inflight.valid() && n.inflightVersion == version) {
        resultSelection = n.inflightSelection;
        if (n.inflight.ready()) {
            latest = n.inflight.tryGet(); // the newest evaluation of this version is authoritative
            n.inflight = {};
            if (latest) {
                r = &*latest;
            }
        } else if (!result.ok() && result.error().code == ErrorCode::Cancelled) {
            return; // an earlier, cancelled evaluation; a newer one is still running
        }
    }
    if (*r) {
        resultSelection = (*r)->selectionKey;
    }
    const Hash128 current = selectionKeyOf(n);
    if (resultSelection != current) {
        // A SelectionDependent column of an earlier selection: kept for a later switch back (03 7.2).
        if (*r && n.selectionDependent) {
            auto& stash = n.selectionCache;
            stash.erase(std::remove_if(
                            stash.begin(), stash.end(),
                            [&](const graph::SelectionCached& c) { return c.selection == resultSelection; }),
                        stash.end());
            stash.push_back(
                graph::SelectionCached{resultSelection, **r, graph::statusFromResult(version, *r)});
            if (stash.size() > graph::kSelectionCacheSize) {
                stash.erase(stash.begin());
            }
        }
        return;
    }
    if (*r) {
        n.handle = **r;
        n.handleSelection = current;
        n.failure.reset();
    } else if (r->error().code != ErrorCode::Cancelled) {
        n.failure = r->error();
        n.failureSelection = current;
    }
    n.status = graph::statusFromResult(version, *r);
    n.status.resolvedInput = n.resolvedInput;
}

// ---- MathGraph --------------------------------------------------------------------------------------------

MathGraph::MathGraph(std::shared_ptr<const model::DataSource> source, std::string layer,
                     std::shared_ptr<const units::UnitRegistry> units, MathsCompat compat, TaskPool* pool,
                     store::ChunkCache* cache)
    : impl_(std::make_unique<Impl>(pool)) {
    impl_->source = std::move(source);
    impl_->layer = layer.empty() ? std::string(model::kNormalLayer) : std::move(layer); // canonical name
    impl_->units = std::move(units);
    impl_->compat = std::move(compat);
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

    std::vector<ChannelKey> keys;
    keys.reserve(outs.size());
    for (const BuiltinOutput& o : outs) {
        keys.push_back(ChannelKey::make(impl_->sourceId(), impl_->layer, o.name));
    }
    graph::BuiltinReg reg;
    reg.module = &module;
    reg.id = moduleId;
    reg.settings = effective;
    reg.outputs = std::move(outs);
    const auto it = std::find_if(impl_->builtins.begin(), impl_->builtins.end(),
                                 [&](const graph::BuiltinReg& r) { return r.id == moduleId; });
    if (it != impl_->builtins.end()) {
        *it = std::move(reg);
    } else {
        impl_->builtins.push_back(std::move(reg));
    }
    static_cast<void>(impl_->rebuild());
    return keys;
}

namespace {

// The module document `m`, created empty when absent.
MathModule& documentIn(std::optional<MathModule>& m) {
    return m ? *m : m.emplace();
}

// The module document that holds item node `n`, or nullptr when it has none.
MathModule* documentOf(MathGraph::Impl& impl, const MathNode& n) {
    if (n.origin == MathModule::Origin::Local) {
        return impl.local ? &*impl.local : nullptr;
    }
    if (n.origin == MathModule::Origin::Transient) {
        return impl.transient ? &*impl.transient : nullptr;
    }
    if (n.origin == MathModule::Origin::Global && n.moduleSlot < impl.globals.size()) {
        return &impl.globals[n.moduleSlot];
    }
    return nullptr;
}

} // namespace

NodeId MathGraph::define(const MathModule& module, const MathItem& item, Diagnostics& diags) {
    if (item.name.empty()) {
        Diagnostic d;
        d.code = "E-NAME-EMPTY";
        d.level = Diagnostic::Error;
        d.message = "a maths item needs an output name";
        diags.push_back(std::move(d));
        return {};
    }
    MathModule* doc = nullptr;
    std::size_t slot = 0;
    switch (module.origin) {
    case MathModule::Origin::Builtin:
        return {};
    case MathModule::Origin::Local:
        doc = &documentIn(impl_->local);
        break;
    case MathModule::Origin::Transient:
        doc = &documentIn(impl_->transient);
        break;
    case MathModule::Origin::Global: {
        const auto it = std::find_if(impl_->globals.begin(), impl_->globals.end(),
                                     [&](const MathModule& m) { return m.id == module.id; });
        if (it == impl_->globals.end()) {
            impl_->globals.emplace_back();
            slot = impl_->globals.size() - 1;
        } else {
            slot = static_cast<std::size_t>(it - impl_->globals.begin());
        }
        doc = &impl_->globals[slot];
        break;
    }
    }
    doc->id = module.id;
    doc->enabled = module.enabled;
    doc->condition = module.condition;
    doc->precision = module.precision;
    doc->constants = module.constants;
    doc->enumerations = module.enumerations;
    doc->unknown = module.unknown;
    doc->origin = module.origin;
    const std::string nameCf = caseFold(item.name);
    std::size_t index = doc->items.size();
    for (std::size_t i = 0; i < doc->items.size(); ++i) {
        if (caseFold(doc->items[i].name) == nameCf && doc->items[i].kind == item.kind) {
            index = i;
            break;
        }
    }
    if (index == doc->items.size()) {
        for (std::size_t i = 0; i < doc->items.size(); ++i) {
            if (caseFold(doc->items[i].name) == nameCf) {
                index = i;
                break;
            }
        }
    }
    if (index == doc->items.size()) {
        doc->items.push_back(item);
    } else {
        doc->items[index] = item;
    }
    static_cast<void>(impl_->rebuild());
    for (const auto& n : impl_->nodes) {
        if (!n->builtin && n->origin == module.origin && n->itemIndex == index &&
            (module.origin != MathModule::Origin::Global || n->moduleSlot == slot)) {
            diags.insert(diags.end(), n->diags.begin(), n->diags.end());
            return impl_->idOf(*n);
        }
    }
    return {};
}

void MathGraph::remove(const NodeId& node) {
    MathNode* n = impl_->findNode(node);
    if (n == nullptr) {
        return;
    }
    if (n->builtin) {
        if (n->builtinReg < impl_->builtins.size()) {
            impl_->builtins.erase(impl_->builtins.begin() + static_cast<std::ptrdiff_t>(n->builtinReg));
        }
    } else {
        MathModule* doc = documentOf(*impl_, *n);
        if (doc == nullptr || n->itemIndex >= doc->items.size()) {
            return;
        }
        doc->items.erase(doc->items.begin() + static_cast<std::ptrdiff_t>(n->itemIndex));
    }
    static_cast<void>(impl_->rebuild());
}

void MathGraph::setEnabled(const NodeId& node, bool enabled) {
    MathNode* n = impl_->findNode(node);
    if (n == nullptr) {
        return;
    }
    if (n->builtin) {
        static_cast<void>(setModuleEnabled(n->moduleId, enabled));
        return;
    }
    MathModule* doc = documentOf(*impl_, *n);
    if (doc == nullptr || n->itemIndex >= doc->items.size() || doc->items[n->itemIndex].enabled == enabled) {
        return;
    }
    doc->items[n->itemIndex].enabled = enabled;
    static_cast<void>(impl_->rebuild());
}

std::vector<ChannelKey> MathGraph::setModules(std::span<const MathModule> globalInOrder,
                                              const MathModule* local, const MathModule* transient) {
    impl_->globals.assign(globalInOrder.begin(), globalInOrder.end());
    for (MathModule& m : impl_->globals) {
        m.origin = MathModule::Origin::Global;
    }
    impl_->local.reset();
    if (local != nullptr) {
        MathModule copy = *local;
        copy.origin = MathModule::Origin::Local;
        impl_->local = std::move(copy);
    }
    impl_->transient.reset();
    if (transient != nullptr) {
        MathModule copy = *transient;
        copy.origin = MathModule::Origin::Transient;
        impl_->transient = std::move(copy);
    }
    return impl_->rebuild();
}

std::vector<ChannelKey> MathGraph::setResolverTables(model::MappingTable mappings,
                                                     model::AliasTable aliases) {
    impl_->mappings = std::move(mappings);
    impl_->aliases = std::move(aliases);
    return impl_->rebuild();
}

std::vector<ChannelKey> MathGraph::setCompat(MathsCompat compat) {
    if (compat == impl_->compat) {
        return {};
    }
    impl_->compat = std::move(compat);
    impl_->astCache.clear();
    return impl_->rebuild();
}

namespace {

// Re-keys every SelectionDependent node from `before` to its current selection key.
std::vector<ChannelKey> rekey(MathGraph::Impl& g, const std::vector<Hash128>& before) {
    std::vector<ChannelKey> changed;
    for (std::size_t i = 0; i < g.nodes.size(); ++i) {
        MathNode& n = *g.nodes[i];
        if (!n.selectionDependent || !n.active() || n.defError) {
            continue;
        }
        const Hash128 now = g.selectionKeyOf(n);
        if (now == before[i]) {
            continue;
        }
        if (n.handle && n.handleSelection == before[i]) {
            n.selectionCache.push_back(graph::SelectionCached{before[i], *n.handle, n.status});
            if (n.selectionCache.size() > graph::kSelectionCacheSize) {
                n.selectionCache.erase(n.selectionCache.begin());
            }
        }
        n.handle.reset();
        const auto hit = std::find_if(n.selectionCache.begin(), n.selectionCache.end(),
                                      [&](const graph::SelectionCached& c) { return c.selection == now; });
        if (hit != n.selectionCache.end()) {
            n.handle = hit->handle;
            n.handleSelection = now;
            n.status = hit->status;
            n.selectionCache.erase(hit);
        } else {
            n.status = graph::freshStatus(n.version, Off::None, {});
        }
        if (n.failure && n.failureSelection != now) {
            n.failure.reset();
        }
        if (std::find(changed.begin(), changed.end(), n.key) == changed.end()) {
            changed.push_back(n.key);
        }
    }
    return changed;
}

std::vector<Hash128> selectionKeys(const MathGraph::Impl& g) {
    std::vector<Hash128> keys;
    keys.reserve(g.nodes.size());
    for (const auto& n : g.nodes) {
        keys.push_back(g.selectionKeyOf(*n));
    }
    return keys;
}

} // namespace

std::vector<ChannelKey> MathGraph::setSelection(const MathSelection& selection) {
    const std::vector<Hash128> before = selectionKeys(*impl_);
    impl_->selection = selection;
    return rekey(*impl_, before);
}

std::vector<ChannelKey>
MathGraph::setDistanceIndexes(std::shared_ptr<const analysis::DistanceIndex> main,
                              std::shared_ptr<const analysis::DistanceIndex> reference) {
    if (main == impl_->mainDist && reference == impl_->refDist) {
        return {};
    }
    const std::vector<Hash128> before = selectionKeys(*impl_);
    impl_->mainDist = std::move(main);
    impl_->refDist = std::move(reference);
    ++impl_->distanceGeneration;
    return rekey(*impl_, before);
}

model::ResolutionTrace MathGraph::trace(const ChannelKey& key) const {
    const MathNode* n = impl_->head(key);
    if (n == nullptr) {
        return {};
    }
    if (n->selfMaths) {
        for (const MathInput& in : n->inputsInfo) {
            if (caseFold(in.name) == n->key.nameCf) {
                return in.trace;
            }
        }
    }
    model::ResolutionTrace t;
    t.requested = n->name;
    t.steps.push_back(
        model::ResolutionStep{model::ResolutionStep::Kind::CaseFold, n->name, n->name, "exact"});
    t.result = n->descriptor.id;
    if (impl_->units != nullptr && impl_->units->contains(n->descriptor.unit)) {
        t.unitLabel = impl_->units->unit(n->descriptor.unit).symbol;
    }
    return t;
}

std::vector<ChannelKey> MathGraph::setModuleEnabled(std::string_view moduleId, bool enabled) {
    const std::string id(moduleId);
    const bool was = impl_->moduleEnabled(id);
    if (was == enabled) {
        return {};
    }
    if (enabled) {
        impl_->disabledModules.erase(id);
    } else {
        impl_->disabledModules.insert(id);
    }
    return impl_->rebuild();
}

std::vector<ChannelKey> MathGraph::setSource(std::shared_ptr<const model::DataSource> source) {
    impl_->source = std::move(source);
    std::vector<ChannelKey> changed = impl_->rebuild();
    // A SelectionDependent node whose column was evaluated over another math window (a marker edit moved the
    // window's lap) goes back to Pending even though its version holds.
    std::vector<Hash128> evaluatedFor;
    evaluatedFor.reserve(impl_->nodes.size());
    for (const auto& n : impl_->nodes) {
        evaluatedFor.push_back(n->handle ? n->handleSelection : impl_->selectionKeyOf(*n));
    }
    for (const ChannelKey& k : rekey(*impl_, evaluatedFor)) {
        if (std::find(changed.begin(), changed.end(), k) == changed.end()) {
            changed.push_back(k);
        }
    }
    return changed;
}

std::vector<ChannelKey> MathGraph::invalidate(std::string_view moduleId) {
    std::vector<ChannelKey> out;
    for (const auto& n : impl_->nodes) {
        if (n->ignored || n->moduleId != moduleId) {
            continue;
        }
        impl_->cancelInflight(*n);
        n->handle.reset();
        n->failure.reset();
        n->selectionCache.clear();
        n->inflight = {};
        n->inflightVersion = {};
        n->memo.reset();
        if (n->active() && !n->defError) {
            n->status = graph::freshStatus(n->version, Off::None, {});
        }
        if (std::find(out.begin(), out.end(), n->key) == out.end()) {
            out.push_back(n->key);
        }
    }
    return out;
}

Version MathGraph::version(const ChannelKey& key) const {
    const MathNode* n = impl_->head(key);
    return n != nullptr ? n->version : Version{};
}

Future<ColumnHandle> MathGraph::require(const ChannelKey& key, SampleRange /*range*/, Priority priority,
                                        const CancelToken& cancel) {
    MathNode* n = impl_->head(key);
    if (n == nullptr) {
        return graph::readyFailure<ColumnHandle>(ErrorCode::NotFound, "no maths output '" + key.nameCf +
                                                                          "' in layer " + impl_->layer);
    }
    return impl_->require(*n, priority, cancel);
}

std::vector<MathRequest> MathGraph::prefetch(Priority priority, const CancelToken& cancel) {
    std::vector<MathRequest> out;
    for (const auto& n : impl_->nodes) {
        if (n->builtin && n->active()) {
            out.push_back(MathRequest{n->key, n->version, impl_->require(*n, priority, cancel)});
        }
    }
    return out;
}

void MathGraph::applyCompletion(const ChannelKey& key, const Version& version,
                                const Result<ColumnHandle>& result) {
    if (MathNode* n = impl_->findVersion(key, version)) {
        impl_->apply(*n, version, result);
    }
}

std::vector<ChannelKey> MathGraph::applyReadyCompletions() {
    std::vector<ChannelKey> applied;
    for (const auto& n : impl_->nodes) {
        if (n->inflight.valid() && n->inflight.ready()) {
            const std::optional<Result<ColumnHandle>> r = n->inflight.tryGet();
            if (!r) {
                continue; // not reachable: ready() above
            }
            const Version v = n->inflightVersion;
            impl_->apply(*n, v, *r);
            n->inflight = {};
            if (std::find(applied.begin(), applied.end(), n->key) == applied.end()) {
                applied.push_back(n->key);
            }
        }
    }
    return applied;
}

MathStatus MathGraph::status(const ChannelKey& key) const {
    if (const MathNode* n = impl_->head(key); n != nullptr) {
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

MathStatus MathGraph::status(const NodeId& node) const {
    if (const MathNode* n = impl_->findNode(node); n != nullptr) {
        return n->status;
    }
    return status(ChannelKey{});
}

std::vector<MathNodeInfo> MathGraph::nodes() const {
    std::vector<MathNodeInfo> out;
    out.reserve(impl_->nodes.size());
    for (const auto& n : impl_->nodes) {
        MathNodeInfo info;
        info.id = impl_->idOf(*n);
        info.name = n->name;
        info.moduleId = n->moduleId;
        info.origin = n->origin;
        info.kind = n->kind;
        info.head = !n->ignored && impl_->head(n->key) == n.get();
        info.selfMaths = n->selfMaths;
        info.selectionDependent = n->selectionDependent;
        info.status = n->status;
        info.inputs = n->inputsInfo;
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<ChannelKey> MathGraph::consumersOf(const ChannelKey& key) const {
    std::vector<ChannelKey> out;
    for (const auto& n : impl_->nodes) {
        if (n->ignored || n->key == key) {
            continue;
        }
        for (const std::size_t u : n->upstream) {
            if (u < impl_->nodes.size() && impl_->nodes[u]->key == key) {
                if (std::find(out.begin(), out.end(), n->key) == out.end()) {
                    out.push_back(n->key);
                }
                break;
            }
        }
    }
    return out;
}

std::vector<ChannelKey> MathGraph::outputs() const {
    std::vector<ChannelKey> out;
    out.reserve(impl_->outputOrder.size());
    for (const std::string& nameCf : impl_->outputOrder) {
        const auto it = impl_->chains.find(nameCf);
        if (it != impl_->chains.end() && !it->second.empty()) {
            out.push_back(impl_->nodes[it->second.front()]->key);
        }
    }
    return out;
}

std::optional<model::ChannelDescriptor> MathGraph::descriptor(const ChannelKey& key) const {
    const MathNode* n = impl_->head(key);
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
    const std::string nameCf = caseFold(name);
    const auto it = impl_->chains.find(nameCf);
    if (it == impl_->chains.end() || it->second.empty()) {
        return std::nullopt;
    }
    return impl_->nodes[it->second.front()]->key;
}

TaskKey MathGraph::taskKey(const ChannelKey& key) const {
    const MathNode* n = impl_->head(key);
    return n != nullptr ? impl_->taskKeyOf(*n) : TaskKey{};
}

std::string MathGraph::taskKeyPrefix(const model::SourceId& source) {
    return "maths/" + source.hex() + "/";
}

std::uint64_t MathGraph::evaluationCount() const noexcept {
    return impl_->evaluations->load(std::memory_order_acquire);
}

const std::shared_ptr<const model::DataSource>& MathGraph::source() const noexcept {
    return impl_->source;
}

const std::string& MathGraph::layer() const noexcept {
    return impl_->layer;
}

} // namespace esses::maths
