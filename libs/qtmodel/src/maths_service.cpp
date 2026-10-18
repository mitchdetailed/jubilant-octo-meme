// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathsService (03 section 1, 06 section 10; D-016, D-022, D-139; AC-M1-10): one maths::MathGraph per
// (source, layer) with the built-in modules esses.corrected_speed_distance and esses.gps_corrections and the
// project's maths, prefetched at Background on registration. The graphs are mutated on the GUI thread only:
// every completion is posted through the Dispatcher and applied with MathGraph::applyCompletion (01 section
// 5.3 rule 10). The M2 members (modules, compat, resolver tables, local and transient modules, selection,
// invalidation, status rows, live checks) are in src/maths/maths_service_m2.cpp.

#include "maths/maths_service_impl.hpp"
#include "range_lookup.hpp"

#include <esses/base/casefold.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/qtmodel/dispatcher.hpp>
#include <esses/qtmodel/maths_service.hpp>

#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace esses::qt {

namespace {

bool isDone(maths::MathState s) noexcept {
    return s == maths::MathState::Ok || s == maths::MathState::Warning;
}

// The latitude and longitude channels the GPS corrections read (their default settings, 03 section 9.2) are
// both logged in `layer` (by name or logged name).
bool hasGpsInputs(const maths::GpsCorrectionsModule& gps, const model::DataLayer& layer) {
    const QJsonObject s =
        QJsonDocument::fromJson(QByteArray::fromStdString(gps.defaultSettings().compact())).object();
    const auto logged = [&layer](const QJsonValue& v) {
        const std::string name = v.toString().toStdString();
        return !name.empty() && (layer.find(name) != nullptr || layer.findLogged(name) != nullptr);
    };
    return logged(s.value(QStringLiteral("latitude"))) && logged(s.value(QStringLiteral("longitude")));
}

} // namespace

MathsService::MathsService(TaskPool& pool, Dispatcher& dispatcher,
                           std::shared_ptr<const units::UnitRegistry> units, QObject* parent)
    : QObject(parent), pool_(pool), dispatcher_(dispatcher), units_(std::move(units)),
      impl_(std::make_unique<Impl>()) {}

MathsService::~MathsService() {
    // Evaluations still queued for our sources are pointless now; running ones finish and their posted
    // completions are skipped (the Dispatcher context is gone).
    for (const auto& entry : impl_->sources) {
        pool_.cancelAll(maths::MathGraph::taskKeyPrefix(entry.first));
    }
}

Result<void> MathsService::loadSettingsSchema(const std::filesystem::path& dataDir) {
    const std::filesystem::path dir = maths::CorrectedSpeedDistanceModule::findSchemasDir(dataDir);
    if (dir.empty()) {
        return fail(ErrorCode::NotFound, "the corrected speed/distance settings schema was not found");
    }
    if (Result<void> r = impl_->module.loadSchema(dir); !r) {
        return r;
    }
    return impl_->gps.loadSchema(dir);
}

Result<void> MathsService::validateCorrectedSpeedSettings(const model::OpaqueJson& settings) const {
    return impl_->module.validateSettings(settings.isNull() ? impl_->module.defaultSettings() : settings);
}

const maths::CorrectedSpeedDistanceModule& MathsService::correctedSpeedModule() const {
    return impl_->module;
}

const maths::GpsCorrectionsModule& MathsService::gpsCorrectionsModule() const {
    return impl_->gps;
}

void MathsService::setCorrectedSpeedSettings(model::OpaqueJson settings) {
    if (settings == impl_->settings) {
        return;
    }
    impl_->settings = std::move(settings);
    for (const auto& entry : impl_->sources) {
        registerSource(entry.second);
    }
    for (const auto& entry : impl_->sources) {
        prefetchSource(entry.first);
    }
}

void MathsService::setCorrectedSpeedEnabled(bool enabled) {
    if (enabled == impl_->enabled) {
        return;
    }
    impl_->enabled = enabled;
    for (auto& [gk, graph] : impl_->graphs) {
        emitChanged(graph->setModuleEnabled(maths::CorrectedSpeedDistanceModule::kId, enabled));
    }
    for (const auto& entry : impl_->sources) {
        prefetchSource(entry.first);
    }
}

void MathsService::emitChanged(const std::vector<maths::ChannelKey>& keys) {
    for (const maths::ChannelKey& key : keys) {
        Q_EMIT statusChanged(key);
    }
}

void MathsService::registerSource(const std::shared_ptr<const model::DataSource>& source) {
    for (const model::DataLayer& layer : source->layers) {
        const std::string name(model::canonicalLayerName(layer.name));
        auto& slot = impl_->graphs[mathsdetail::GraphKey{source->id, name}];
        if (slot == nullptr) {
            slot = std::make_unique<maths::MathGraph>(source, name, units_, impl_->compat, &pool_);
            // The project's names (mappings, aliases) before any definition resolves against them.
            static_cast<void>(slot->setResolverTables(impl_->mappings, impl_->aliases));
        }
        // The built-in modules first in every shadowing chain (D-171): corrected speed and distance, then the
        // GPS corrections' zero-fix step (always on from M2, 03 section 9.2) for a layer that logs both GPS
        // inputs (a layer without them would only list two Disabled outputs).
        std::vector<std::pair<const maths::IBuiltinModule*, model::OpaqueJson>> builtins{
            {&impl_->module, impl_->settings}};
        if (hasGpsInputs(impl_->gps, layer)) {
            builtins.emplace_back(&impl_->gps, model::OpaqueJson{});
        }
        for (const auto& [module, settings] : builtins) {
            maths::Diagnostics diags;
            Result<std::vector<maths::ChannelKey>> keys = slot->defineBuiltin(*module, settings, diags);
            if (!keys) {
                const maths::Diagnostic* first = diags.empty() ? nullptr : &diags.front();
                Q_EMIT moduleMessage(
                    source->id, QString::fromStdString(first != nullptr ? first->code : "E-EVAL"),
                    QString::fromStdString(
                        first != nullptr ? first->message + (first->hint.empty() ? std::string()
                                                                                 : " (" + first->hint + ")")
                                         : keys.error().message));
                continue;
            }
            if (module == &impl_->module && !impl_->enabled) {
                slot->setModuleEnabled(maths::CorrectedSpeedDistanceModule::kId, false);
            }
            emitChanged(*keys);
        }
    }
    applyModules(source->id);
}

void MathsService::prefetchSource(const model::SourceId& source) {
    for (auto& [gk, graph] : impl_->graphs) {
        if (gk.first != source) {
            continue;
        }
        for (maths::MathRequest& req : graph->prefetch(Priority::Background)) {
            watch(req.key, req.version, std::move(req.future));
        }
    }
}

void MathsService::addSource(std::shared_ptr<const model::DataSource> source) {
    if (source == nullptr) {
        return;
    }
    if (impl_->sources.count(source->id) != 0) {
        replaceSource(std::move(source));
        return;
    }
    impl_->sources[source->id] = source;
    impl_->order.push_back(source->id);
    registerSource(source);
    prefetchSource(source->id);
}

void MathsService::replaceSource(std::shared_ptr<const model::DataSource> source) {
    if (source == nullptr) {
        return;
    }
    const auto it = impl_->sources.find(source->id);
    if (it == impl_->sources.end()) {
        addSource(std::move(source));
        return;
    }
    it->second = source;
    for (auto& [gk, graph] : impl_->graphs) {
        if (gk.first != source->id) {
            continue;
        }
        emitChanged(graph->setSource(source)); // re-versioned (03 section 7.2): Pending again
    }
    applyModules(source->id); // the sidecar's local maths may have changed with the snapshot
    prefetchSource(source->id);
}

void MathsService::removeSource(const model::SourceId& source) {
    if (impl_->sources.erase(source) == 0) {
        return;
    }
    impl_->order.erase(std::remove(impl_->order.begin(), impl_->order.end(), source), impl_->order.end());
    impl_->localOverride.erase(source);
    impl_->transient.erase(source); // a transient module is dropped on close (03 section 4.2)
    pool_.cancelAll(maths::MathGraph::taskKeyPrefix(source));
    for (auto it = impl_->graphs.begin(); it != impl_->graphs.end();) {
        it = it->first.first == source ? impl_->graphs.erase(it) : std::next(it);
    }
    for (auto it = impl_->local.begin(); it != impl_->local.end();) {
        it = it->first.first == source ? impl_->local.erase(it) : std::next(it);
    }
    for (auto it = impl_->inflight.begin(); it != impl_->inflight.end();) {
        it = it->key.src == source ? impl_->inflight.erase(it) : std::next(it);
    }
    for (auto it = impl_->announced.begin(); it != impl_->announced.end();) {
        it = it->key.src == source ? impl_->announced.erase(it) : std::next(it);
    }
}

void MathsService::watch(const maths::ChannelKey& key, const maths::Version& version,
                         Future<maths::ColumnHandle> future) {
    if (!future.valid() || !impl_->inflight.insert(mathsdetail::InflightKey{key, version}).second) {
        return; // one posted completion per (key, version)
    }
    // The Dispatcher is runtime-owned and outlives every pool task (01 section 5.3 rule 11), so it is the
    // posting context; the guard is made here on the GUI thread and checked there (a continuation may run
    // while this service is being destroyed, e.g. from cancelAll in the destructor).
    Dispatcher* dispatcher = &dispatcher_;
    const QPointer<MathsService> guard(this);
    future.onReady([dispatcher, guard, key, version](const Result<maths::ColumnHandle>& result) {
        dispatcher->post(dispatcher, [guard, key, version, result] {
            if (MathsService* self = guard.data()) {
                self->complete(key, version, result);
            }
        });
    });
}

void MathsService::complete(const maths::ChannelKey& key, const maths::Version& version,
                            const Result<maths::ColumnHandle>& result) {
    impl_->inflight.erase(mathsdetail::InflightKey{key, version});
    if (maths::MathGraph* graph = impl_->graph(key.src, key.layer)) {
        applyResult(*graph, key, version, result);
    }
}

void MathsService::applyResult(maths::MathGraph& graph, const maths::ChannelKey& key,
                               const maths::Version& version, const Result<maths::ColumnHandle>& result) {
    graph.applyCompletion(key, version, result);
    // The upstream nodes this evaluation required finished before it: their states move on too (no node is
    // left showing Evaluating after its column is sealed).
    std::vector<maths::ChannelKey> upstream = graph.applyReadyCompletions();
    upstream.erase(std::remove(upstream.begin(), upstream.end(), key), upstream.end());
    const maths::MathStatus st = graph.status(key);
    Q_EMIT statusChanged(key);
    emitChanged(upstream);
    // channelReady once per (output, version), whichever completion applied it first.
    const auto announce = [this, &graph](const maths::ChannelKey& k) {
        if (isDone(graph.status(k).state) &&
            impl_->announced.insert(mathsdetail::InflightKey{k, graph.version(k)}).second) {
            Q_EMIT channelReady(k);
        }
    };
    if (isDone(st.state)) {
        announce(key);
    } else if (st.state == maths::MathState::Error &&
               impl_->reported.insert(mathsdetail::InflightKey{key, version}).second) {
        const std::string code = st.diags.empty() ? std::string("E-EVAL") : st.diags.front().code;
        Q_EMIT moduleMessage(key.src, QString::fromStdString(code), QString::fromStdString(st.summary));
    }
    for (const maths::ChannelKey& k : upstream) {
        announce(k);
    }
}

std::optional<maths::ColumnHandle> MathsService::require(const maths::ChannelKey& key, Priority priority) {
    maths::MathGraph* graph = impl_->graph(key.src, key.layer);
    if (graph == nullptr) {
        return std::nullopt;
    }
    const maths::MathStatus before = graph->status(key);
    if (before.state == maths::MathState::Disabled) {
        return std::nullopt;
    }
    const maths::Version version = graph->version(key);
    Future<maths::ColumnHandle> future = graph->require(key, maths::SampleRange::all(), priority, {});
    if (future.ready()) {
        std::optional<Result<maths::ColumnHandle>> r = future.tryGet();
        if (r && r->ok()) {
            if (!isDone(before.state)) {
                // Evaluated but not applied yet (an idle worker can finish before this check): apply now,
                // with the upstream nodes it read; the posted completion (if any) is then a repeat.
                applyResult(*graph, key, version, *r);
            }
            return std::move(*r).value();
        }
        if (before.state == maths::MathState::Error) {
            return std::nullopt; // a memoised failure, already applied (03 section 7.3)
        }
    }
    if (graph->status(key).state != before.state) {
        Q_EMIT statusChanged(key); // Pending -> Evaluating (03 section 7.3)
    }
    watch(key, version, std::move(future));
    return std::nullopt;
}

bool MathsService::isMathsChannel(const model::SourceId& source, std::string_view layer,
                                  std::string_view name) const {
    const maths::MathGraph* graph = impl_->graph(source, layer);
    return graph != nullptr && graph->find(name).has_value();
}

maths::MathStatus MathsService::status(const maths::ChannelKey& key) const {
    if (const maths::MathGraph* graph = impl_->graph(key.src, key.layer)) {
        return graph->status(key);
    }
    maths::MathStatus st;
    st.state = maths::MathState::Error;
    st.summary = "not a maths channel";
    maths::Diagnostic d;
    d.code = "E-NOT-MATHS";
    d.message = st.summary;
    st.diags.push_back(std::move(d));
    return st;
}

std::optional<model::ChannelDescriptor> MathsService::descriptor(const maths::ChannelKey& key) const {
    const maths::MathGraph* graph = impl_->graph(key.src, key.layer);
    return graph == nullptr ? std::nullopt : graph->descriptor(key);
}

std::vector<maths::ChannelKey> MathsService::outputs(const model::SourceId& source,
                                                     std::string_view layer) const {
    const maths::MathGraph* graph = impl_->graph(source, layer);
    return graph == nullptr ? std::vector<maths::ChannelKey>{} : graph->outputs();
}

std::shared_ptr<const model::DataSource> MathsService::source(const model::SourceId& id) const {
    const auto it = impl_->sources.find(id);
    return it == impl_->sources.end() ? nullptr : it->second;
}

std::vector<model::SourceId> MathsService::sources() const {
    return impl_->order;
}

} // namespace esses::qt
