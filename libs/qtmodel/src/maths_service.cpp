// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathsService (03 section 1, 06 section 10; D-016, D-022, D-139; AC-M1-10): one maths::MathGraph per
// (source, layer) with the built-in module esses.corrected_speed_distance, prefetched at Background on
// registration. The graphs are mutated on the GUI thread only: every completion is posted through the
// Dispatcher and applied with MathGraph::applyCompletion (01 section 5.3 rule 10).

#include "range_lookup.hpp"

#include <esses/base/casefold.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/maths/corrected_speed_distance.hpp>
#include <esses/maths/math_graph.hpp>
#include <esses/qtmodel/dispatcher.hpp>
#include <esses/qtmodel/maths_service.hpp>

#include <QPointer>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace esses::qt {

namespace {

using GraphKey = std::pair<model::SourceId, std::string>; // (source, canonical layer)

struct InflightKey {
    maths::ChannelKey key;
    maths::Version version;
    friend bool operator<(const InflightKey& a, const InflightKey& b) {
        if (a.key.src != b.key.src) {
            return a.key.src < b.key.src;
        }
        if (a.key.layer != b.key.layer) {
            return a.key.layer < b.key.layer;
        }
        if (a.key.nameCf != b.key.nameCf) {
            return a.key.nameCf < b.key.nameCf;
        }
        return a.version < b.version;
    }
};

bool isDone(maths::MathState s) noexcept {
    return s == maths::MathState::Ok || s == maths::MathState::Warning;
}

} // namespace

struct MathsService::Impl {
    maths::CorrectedSpeedDistanceModule module;
    model::OpaqueJson settings; // null: the module defaults
    bool enabled = true;
    std::map<model::SourceId, std::shared_ptr<const model::DataSource>> sources;
    std::map<GraphKey, std::unique_ptr<maths::MathGraph>> graphs;
    std::set<InflightKey> inflight;
    std::set<InflightKey> reported; // errors already sent to Messages (once per version)

    maths::MathGraph* graph(const model::SourceId& src, std::string_view layer) {
        const auto it = graphs.find(GraphKey{src, std::string(model::canonicalLayerName(layer))});
        return it == graphs.end() ? nullptr : it->second.get();
    }
    const maths::MathGraph* graph(const model::SourceId& src, std::string_view layer) const {
        const auto it = graphs.find(GraphKey{src, std::string(model::canonicalLayerName(layer))});
        return it == graphs.end() ? nullptr : it->second.get();
    }
};

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
    return impl_->module.loadSchema(dir);
}

Result<void> MathsService::validateCorrectedSpeedSettings(const model::OpaqueJson& settings) const {
    return impl_->module.validateSettings(settings.isNull() ? impl_->module.defaultSettings() : settings);
}

const maths::CorrectedSpeedDistanceModule& MathsService::correctedSpeedModule() const {
    return impl_->module;
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
        for (const maths::ChannelKey& key :
             graph->setModuleEnabled(maths::CorrectedSpeedDistanceModule::kId, enabled)) {
            Q_EMIT statusChanged(key);
        }
    }
    for (const auto& entry : impl_->sources) {
        prefetchSource(entry.first);
    }
}

void MathsService::registerSource(const std::shared_ptr<const model::DataSource>& source) {
    for (const model::DataLayer& layer : source->layers) {
        const std::string name(model::canonicalLayerName(layer.name));
        auto& slot = impl_->graphs[GraphKey{source->id, name}];
        if (slot == nullptr) {
            slot = std::make_unique<maths::MathGraph>(source, name, units_, maths::MathsCompat{}, &pool_);
        }
        maths::Diagnostics diags;
        Result<std::vector<maths::ChannelKey>> keys =
            slot->defineBuiltin(impl_->module, impl_->settings, diags);
        if (!keys) {
            const maths::Diagnostic* first = diags.empty() ? nullptr : &diags.front();
            Q_EMIT moduleMessage(
                source->id, QString::fromStdString(first != nullptr ? first->code : "E-EVAL"),
                QString::fromStdString(
                    first != nullptr
                        ? first->message + (first->hint.empty() ? std::string() : " (" + first->hint + ")")
                        : keys.error().message));
            continue;
        }
        if (!impl_->enabled) {
            slot->setModuleEnabled(maths::CorrectedSpeedDistanceModule::kId, false);
        }
        for (const maths::ChannelKey& key : *keys) {
            Q_EMIT statusChanged(key);
        }
    }
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
        for (const maths::ChannelKey& key : graph->setSource(source)) {
            Q_EMIT statusChanged(key); // re-versioned (03 section 7.2): Pending again
        }
    }
    prefetchSource(source->id);
}

void MathsService::removeSource(const model::SourceId& source) {
    if (impl_->sources.erase(source) == 0) {
        return;
    }
    pool_.cancelAll(maths::MathGraph::taskKeyPrefix(source));
    for (auto it = impl_->graphs.begin(); it != impl_->graphs.end();) {
        it = it->first.first == source ? impl_->graphs.erase(it) : std::next(it);
    }
    for (auto it = impl_->inflight.begin(); it != impl_->inflight.end();) {
        it = it->key.src == source ? impl_->inflight.erase(it) : std::next(it);
    }
}

void MathsService::setSelection(const plot::SelectionSnapshot&) {
    // No SelectionDependent node exists in M1 (03 section 7.5 arrives with the language in M2).
}

void MathsService::watch(const maths::ChannelKey& key, const maths::Version& version,
                         Future<maths::ColumnHandle> future) {
    if (!future.valid() || !impl_->inflight.insert(InflightKey{key, version}).second) {
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
    impl_->inflight.erase(InflightKey{key, version});
    maths::MathGraph* graph = impl_->graph(key.src, key.layer);
    if (graph == nullptr) {
        return;
    }
    graph->applyCompletion(key, version, result);
    const maths::MathStatus st = graph->status(key);
    Q_EMIT statusChanged(key);
    if (isDone(st.state)) {
        Q_EMIT channelReady(key);
    } else if (st.state == maths::MathState::Error &&
               impl_->reported.insert(InflightKey{key, version}).second) {
        const std::string code = st.diags.empty() ? std::string("E-EVAL") : st.diags.front().code;
        Q_EMIT moduleMessage(key.src, QString::fromStdString(code), QString::fromStdString(st.summary));
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
                // Evaluated but not applied yet: apply now; the posted completion (if any) is then a repeat.
                graph->applyCompletion(key, version, *r);
            }
            return std::move(*r).value();
        }
        if (before.state == maths::MathState::Error) {
            return std::nullopt; // a memoised failure, already applied (03 section 7.3)
        }
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

} // namespace esses::qt
