// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathsService's M2 members (07 section 5.1, 03 sections 3.3, 4.2-4.3, 7 and 11, 06 section 10): the
// project's global modules, compat switches, mappings and aliases, each source's local (sidecar) and
// transient modules defined into every graph (MathGraph::setModules rebuilds only what changed, so only the
// edited cones become Pending, AC-M2-04), Recalculate, the selection SelectionDependent nodes key on, the
// Channel Status rows and the Maths Editor's live check against the Main source's scope.

#include "../range_lookup.hpp"
#include "maths_service_impl.hpp"

#include <esses/base/casefold.hpp>
#include <esses/base/hash.hpp>
#include <esses/maths/function_registry.hpp>
#include <esses/maths/math_module_io.hpp>
#include <esses/maths/parser.hpp>
#include <esses/maths/scope.hpp>
#include <esses/maths/type_checker.hpp>
#include <esses/model/detail.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/maths_service.hpp>

#include <algorithm>
#include <bit>
#include <utility>
#include <variant>

namespace esses::qt {

namespace {

// The Scope of a live check (03 section 11 item 2): the Main source's layer as its graph sees it. Order of
// 03 section 3.3: a maths output (the chain head) first, then the project's mappings and aliases and the
// logged channels through model::ChannelResolver, then the source's details, then the constants of the
// enabled global modules in project order.
class LiveScope final : public maths::Scope {
public:
    LiveScope(const maths::FunctionRegistry& functions, const units::UnitRegistry& units,
              const maths::MathGraph& graph, const model::DataSource& source, const model::DataLayer& layer,
              const model::ChannelResolver& resolver, const std::vector<maths::MathModule>& globals)
        : Scope(functions, units), units_(units), graph_(graph), source_(source), layer_(layer),
          resolver_(resolver), globals_(globals) {}

    std::optional<maths::ScopeEntry> lookup(std::string_view nameCf,
                                            const maths::ChannelKey& /*requester*/) const override {
        if (const std::optional<maths::ChannelKey> key = graph_.find(nameCf)) {
            const maths::MathStatus st = graph_.status(*key);
            if (st.state != maths::MathState::Disabled) {
                maths::ScopeEntry e;
                e.what = maths::ScopeEntry::Channel;
                e.type.kind = maths::Kind::Series;
                e.key = *key;
                if (const std::optional<model::ChannelDescriptor> d = graph_.descriptor(*key)) {
                    e.type.unit = d->unit;
                    if (d->rate && d->rate->valid()) {
                        e.type.rate = *d->rate;
                    }
                    e.interp = d->interp;
                    e.dtype = d->dtype;
                    e.resolvedName = d->name;
                }
                e.trace = graph_.trace(*key);
                return e;
            }
        }
        const model::ChannelResolver::Resolution r = resolver_.resolve(nameCf, layer_, source_.id);
        if (r.channel != nullptr) {
            const model::ChannelDescriptor& d = r.alias ? *r.alias : r.channel->desc;
            maths::ScopeEntry e;
            e.what = maths::ScopeEntry::Channel;
            e.type.kind = maths::Kind::Series;
            e.type.unit = r.channel->desc.unit;
            if (r.channel->desc.rate && r.channel->desc.rate->valid()) {
                e.type.rate = *r.channel->desc.rate;
            }
            e.type.integer = r.channel->desc.kind == model::SampleKind::Integer ||
                             r.channel->desc.kind == model::SampleKind::Enum;
            e.interp = r.channel->desc.interp;
            e.dtype = r.channel->desc.dtype;
            e.key = maths::ChannelKey::make(source_.id, layer_.name, r.channel->desc.name);
            e.resolvedName = d.name;
            e.trace = r.trace;
            return e;
        }
        for (const model::Detail& d : source_.details) {
            if (caseFold(d.id) == nameCf) {
                return detailEntry(d);
            }
        }
        for (const maths::MathModule& m : globals_) {
            if (!m.enabled) {
                continue;
            }
            for (const maths::MathConstant& c : m.constants) {
                if (c.enabled && caseFold(c.name) == nameCf) {
                    return constantEntry(c);
                }
            }
        }
        return std::nullopt;
    }
    bool dataBacked() const noexcept override { return true; }

private:
    units::UnitId baseOf(units::UnitId u) const {
        if (u == units::kNoUnit || !units_.contains(u)) {
            return units::kNoUnit;
        }
        return units_.quantity(units_.quantityOf(u)).base;
    }

    maths::ScopeEntry detailEntry(const model::Detail& d) const {
        maths::ScopeEntry e;
        e.resolvedName = d.id;
        if (const auto* text = std::get_if<std::string>(&d.value)) {
            e.what = maths::ScopeEntry::DetailText;
            e.type.kind = maths::Kind::Text;
            e.text = *text;
        } else if (const auto* dt = std::get_if<model::DateTime>(&d.value)) {
            e.what = maths::ScopeEntry::DetailText;
            e.type.kind = maths::Kind::Text;
            e.text = dt->iso();
        } else if (const auto* q = std::get_if<model::Quantityd>(&d.value)) {
            e.what = maths::ScopeEntry::DetailNumber;
            e.type.kind = maths::Kind::Scalar;
            e.type.unit = baseOf(q->unit);
            e.number = model::toBase(*q, units_).value_or(q->v);
        }
        return e;
    }

    maths::ScopeEntry constantEntry(const maths::MathConstant& c) const {
        maths::ScopeEntry e;
        e.what = maths::ScopeEntry::Constant;
        e.type.kind = maths::Kind::Scalar;
        e.resolvedName = c.name;
        if (units_.contains(c.value.unit)) {
            e.type.unit = baseOf(c.value.unit);
            e.number = units_.toBase(c.value.unit)(c.value.v);
        } else {
            e.number = c.value.v;
        }
        return e;
    }

    const units::UnitRegistry& units_;
    const maths::MathGraph& graph_;
    const model::DataSource& source_;
    const model::DataLayer& layer_;
    const model::ChannelResolver& resolver_;
    const std::vector<maths::MathModule>& globals_;
};

void hashRef(Hash128Stream& h, const model::RangeRef& r) {
    h.updatePart(r.src.hex());
    h.updatePart(model::canonicalLayerName(r.layer));
    h.updatePart(r.groupPath);
    h.updatePart(r.range.str());
}

void hashGeom(Hash128Stream& h, const maths::MathRangeGeom& g) {
    hashRef(h, g.ref);
    h.updateU64(static_cast<std::uint64_t>(g.start));
    h.updateU64(static_cast<std::uint64_t>(g.end));
    h.updateU64(static_cast<std::uint64_t>(g.offsetUs));
    h.updateU64(std::bit_cast<std::uint64_t>(g.offsetM));
    h.updateU64(std::bit_cast<std::uint64_t>(g.stretch));
}

} // namespace

// ---- modules
// ------------------------------------------------------------------------------------------------

void MathsService::applyModules(const model::SourceId& source) {
    const auto src = this->source(source);
    if (src == nullptr) {
        return;
    }
    const auto override = impl_->localOverride.find(source);
    const auto transient = impl_->transient.find(source);
    for (auto& [gk, graph] : impl_->graphs) {
        if (gk.first != source) {
            continue;
        }
        // The local module in effect: setLocalModule's, else the sidecar's localMaths of this layer.
        std::optional<maths::MathModule> local;
        if (override != impl_->localOverride.end() && override->second) {
            local = *override->second;
        } else if (const std::optional<JsonDoc>& doc = src->sidecar.layer(gk.second).localMaths;
                   doc && units_ != nullptr) {
            std::vector<std::string> log;
            Result<maths::MathModule> parsed = maths::MathModuleIo::fromJson(*doc, *units_, log);
            if (parsed) {
                local = std::move(parsed).value();
            } else {
                Q_EMIT moduleMessage(source, QStringLiteral("E-LOCAL-MATHS"),
                                     tr("The local maths of the sidecar cannot be read: %1")
                                         .arg(QString::fromStdString(parsed.error().toString())));
            }
        }
        if (local) {
            local->origin = maths::MathModule::Origin::Local;
            if (local->id.empty()) {
                local->id = std::string(maths::MathModule::kLocalId);
            }
        }
        const maths::MathModule* transientModule =
            transient != impl_->transient.end() ? &transient->second : nullptr;
        auto& current = impl_->local[gk];
        current = std::move(local);
        emitChanged(graph->setModules(impl_->globals, current ? &*current : nullptr, transientModule));
    }
}

void MathsService::setModules(std::vector<maths::MathModule> globalInOrder) {
    for (maths::MathModule& m : globalInOrder) {
        m.origin = maths::MathModule::Origin::Global;
    }
    if (globalInOrder == impl_->globals) {
        return;
    }
    impl_->globals = std::move(globalInOrder);
    for (const model::SourceId& id : impl_->order) {
        applyModules(id);
    }
}

const std::vector<maths::MathModule>& MathsService::modules() const {
    return impl_->globals;
}

void MathsService::setCompat(maths::MathsCompat compat) {
    if (compat == impl_->compat) {
        return;
    }
    impl_->compat = std::move(compat);
    for (auto& [gk, graph] : impl_->graphs) {
        emitChanged(graph->setCompat(impl_->compat));
    }
}

const maths::MathsCompat& MathsService::compat() const {
    return impl_->compat;
}

void MathsService::setResolverTables(model::MappingTable mappings, model::AliasTable aliases) {
    impl_->mappings = std::move(mappings);
    impl_->aliases = std::move(aliases);
    for (auto& [gk, graph] : impl_->graphs) {
        emitChanged(graph->setResolverTables(impl_->mappings, impl_->aliases));
    }
}

void MathsService::setLocalModule(const model::SourceId& source, std::optional<maths::MathModule> local) {
    if (local) {
        local->origin = maths::MathModule::Origin::Local;
        impl_->localOverride[source] = std::move(local);
    } else {
        impl_->localOverride.erase(source);
    }
    applyModules(source);
}

void MathsService::setTransientModule(const model::SourceId& source,
                                      std::optional<maths::MathModule> transient) {
    if (transient) {
        transient->origin = maths::MathModule::Origin::Transient;
        transient->id = std::string(maths::MathModule::kTransientId);
        impl_->transient[source] = std::move(*transient);
    } else if (impl_->transient.erase(source) == 0) {
        return;
    }
    applyModules(source);
}

void MathsService::invalidate(std::string_view moduleId) {
    for (auto& [gk, graph] : impl_->graphs) {
        emitChanged(graph->invalidate(moduleId));
    }
    // The built-in outputs are prefetched again (D-016); items are evaluated again on demand.
    for (const model::SourceId& id : impl_->order) {
        prefetchSource(id);
    }
}

// ---- selection (03 sections 7.2, 7.5, 7.6)
// ------------------------------------------------------------------

void MathsService::setSelection(const plot::SelectionSnapshot& snapshot) {
    const auto geomOf = [this](const model::RangeRef& ref) {
        maths::MathRangeGeom g;
        g.ref = ref;
        const auto src = this->source(ref.src);
        if (const model::Range* r = src == nullptr ? nullptr : detail::findRange(*src, ref)) {
            g.start = r->start;
            g.end = r->end;
            g.offsetUs = r->offsetUs;
            g.offsetM = r->offsetM;
            g.stretch = r->stretch.value_or(1.0);
        }
        return g;
    };
    maths::MathSelection sel;
    sel.main = geomOf(snapshot.main);
    if (const std::optional<model::RangeRef> ref = snapshot.referenceOverlay()) {
        sel.reference = geomOf(*ref);
    }
    if (snapshot.overlay0) {
        sel.overlays.push_back(*snapshot.overlay0);
    }
    sel.overlays.insert(sel.overlays.end(), snapshot.overlays.begin(), snapshot.overlays.end());
    Hash128Stream h;
    h.updatePart("esses.qtmodel.mathSelection-1");
    hashGeom(h, sel.main);
    h.updateU64(sel.reference ? 1 : 0);
    if (sel.reference) {
        hashGeom(h, *sel.reference);
    }
    for (const model::RangeRef& r : sel.overlays) {
        hashRef(h, r);
    }
    sel.key = h.digest();
    if (sel.key == impl_->selection.key) {
        return;
    }
    impl_->selection = std::move(sel);
    for (auto& [gk, graph] : impl_->graphs) {
        emitChanged(graph->setSelection(impl_->selection));
    }
}

void MathsService::setSelectionDistances(std::shared_ptr<const analysis::DistanceIndex> main,
                                         std::shared_ptr<const analysis::DistanceIndex> reference) {
    if (main == impl_->mainDist && reference == impl_->refDist) {
        return;
    }
    impl_->mainDist = std::move(main);
    impl_->refDist = std::move(reference);
    for (auto& [gk, graph] : impl_->graphs) {
        emitChanged(graph->setDistanceIndexes(impl_->mainDist, impl_->refDist));
    }
}

const maths::MathSelection& MathsService::mathSelection() const {
    return impl_->selection;
}

// ---- Channel Status rows and the live check
// -----------------------------------------------------------------

std::vector<MathsService::StatusRow> MathsService::statusRows() const {
    std::vector<StatusRow> rows;
    for (const model::SourceId& id : impl_->order) {
        const auto src = source(id);
        if (src == nullptr) {
            continue;
        }
        for (const model::DataLayer& layer : src->layers) {
            const maths::MathGraph* graph = impl_->graph(id, layer.name);
            if (graph == nullptr) {
                continue;
            }
            for (const maths::MathNodeInfo& info : graph->nodes()) {
                StatusRow row;
                row.source = id;
                row.layer = std::string(model::canonicalLayerName(layer.name));
                row.key = info.id.key;
                row.name = info.name;
                row.moduleId = info.moduleId;
                row.kind = info.kind;
                row.status = info.status;
                row.trace = graph->trace(info.id.key);
                if (info.head) {
                    row.descriptor = graph->descriptor(info.id.key);
                }
                rows.push_back(std::move(row));
            }
        }
    }
    return rows;
}

maths::Diagnostics MathsService::check(std::string_view expression,
                                       const std::optional<model::SourceId>& source) const {
    maths::Diagnostics diags;
    Result<maths::Ast> ast = maths::Parser::parse(expression, impl_->compat, diags);
    if (!ast.ok() || units_ == nullptr) {
        return diags;
    }
    const std::shared_ptr<const model::DataSource> src = source ? this->source(*source) : nullptr;
    const model::DataLayer* layer = src == nullptr ? nullptr : detail::layerOf(*src, model::kNormalLayer);
    const maths::MathGraph* graph = src == nullptr ? nullptr : impl_->graph(src->id, model::kNormalLayer);
    if (layer == nullptr || graph == nullptr) {
        const maths::Scope scope(functions(), *units_); // data-less: unknown names are I-UNRESOLVED
        static_cast<void>(maths::TypeChecker::check(*ast, scope, diags));
        return diags;
    }
    const model::ChannelResolver resolver(impl_->mappings, impl_->aliases, units_.get());
    const LiveScope scope(functions(), *units_, *graph, *src, *layer, resolver, impl_->globals);
    static_cast<void>(maths::TypeChecker::check(*ast, scope, diags));
    return diags;
}

const maths::FunctionRegistry& MathsService::functions() const {
    return maths::FunctionRegistry::builtin();
}

// ---- selection wiring
// ---------------------------------------------------------------------------------------

void connectMathsSelection(DataSelectionModel& selection, MathsService& maths, DistanceService* distance) {
    const auto apply = [&selection, &maths, distance] {
        maths.setSelection(selection.snapshot());
        if (distance == nullptr) {
            return;
        }
        const plot::SelectionSnapshot& s = selection.snapshot();
        std::shared_ptr<const analysis::DistanceIndex> main =
            selection.hasMain() ? distance->index(s.main, Priority::Visible) : nullptr;
        const std::optional<model::RangeRef> ref = s.referenceOverlay();
        std::shared_ptr<const analysis::DistanceIndex> reference =
            ref ? distance->index(*ref, Priority::Visible) : nullptr;
        maths.setSelectionDistances(std::move(main), std::move(reference));
    };
    QObject::connect(&selection, &DataSelectionModel::changed, &maths,
                     [apply](const plot::SelectionSnapshot&, DataSelectionModel::Changes) { apply(); });
    QObject::connect(&selection, &DataSelectionModel::sourcesChanged, &maths, [apply] { apply(); });
    if (distance != nullptr) {
        QObject::connect(distance, &DistanceService::distanceReady, &maths,
                         [apply](const model::SourceId&) { apply(); });
    }
    apply();
}

} // namespace esses::qt
