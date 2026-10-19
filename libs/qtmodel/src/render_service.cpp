// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// RenderService (04 sections 1 item 2 and 6.3, 01 section 5.4 rule 2; AC-M1-05): replace-key build submission
// over the TaskPool with Dispatcher delivery, and the preparation of plot::ComponentBuildInputs.

#include "channel_resolution.hpp"
#include "range_lookup.hpp"

#include <esses/analysis/stat_cache.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/model/resolver.hpp>
#include <esses/plotcore/component_bodies.hpp>
#include <esses/plotcore/display_units.hpp>
#include <esses/plotcore/time_distance_builder.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/dispatcher.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/track_map_service.hpp>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <atomic>
#include <map>
#include <utility>

namespace esses::qt {

namespace {

constexpr std::string_view kTimeDistance = "time_distance";
constexpr std::string_view kValuesList = "values_list";

} // namespace

// ---- channel resolution ---------------------------------------------------------------------------------

std::vector<ChannelRequest> componentChannels(const project::ComponentSpec& spec) {
    std::vector<ChannelRequest> out;
    if (spec.type == kTimeDistance) {
        Result<plot::TimeDistanceBody> body = plot::TimeDistanceBody::parse(spec.body);
        if (!body) {
            return out;
        }
        for (const plot::TdGroup& g : body->groups) {
            for (const plot::TdTrace& t : g.traces) {
                out.push_back(ChannelRequest{t.channel, t.displayUnit, t.dps});
            }
        }
        return out;
    }
    if (spec.type == kValuesList && spec.body.isObject()) {
        const QJsonObject body =
            QJsonDocument::fromJson(QByteArray::fromStdString(spec.body.compact())).object();
        for (const QJsonValue& row : body.value(QStringLiteral("channels")).toArray()) {
            const QJsonObject o = row.toObject();
            ChannelRequest r;
            r.name = o.value(QStringLiteral("channel")).toString().toStdString();
            r.displayUnit = o.value(QStringLiteral("displayUnit")).toString().toStdString();
            if (o.value(QStringLiteral("dps")).isDouble()) {
                r.dps = o.value(QStringLiteral("dps")).toInt();
            }
            if (!r.name.empty()) {
                out.push_back(std::move(r));
            }
        }
        return out;
    }
    // The M3 types: each body lists its channels in builder order (plot::componentChannelUses).
    for (plot::ChannelUse& use : plot::componentChannelUses(spec)) {
        out.push_back(ChannelRequest{std::move(use.name), std::move(use.displayUnit), use.dps});
    }
    return out;
}

namespace detail {

plot::ResolvedChannel resolveChannel(MathsService& maths, const model::DataSource& source,
                                     std::string_view layer, const ChannelRequest& request,
                                     Priority priority) {
    plot::ResolvedChannel rc;
    rc.name = request.name;
    const std::string canonical(model::canonicalLayerName(layer));
    if (maths.isMathsChannel(source.id, canonical, request.name)) {
        const maths::ChannelKey key = maths::ChannelKey::make(source.id, canonical, request.name);
        rc.descriptor = maths.descriptor(key);
        const maths::MathState st = maths.status(key).state;
        if (st == maths::MathState::Error || st == maths::MathState::Disabled) {
            rc.status = model::ChannelStatus::Error;
        } else if (std::optional<maths::ColumnHandle> handle = maths.require(key, priority)) {
            rc.descriptor = handle->descriptor;
            rc.series = handle->series;
            rc.status = model::ChannelStatus::Ok;
        } else {
            rc.status = model::ChannelStatus::Pending;
        }
    } else if (const model::DataLayer* l = layerOf(source, canonical)) {
        const model::ChannelResolver resolver({}, {}, maths.units().get());
        const model::ChannelResolver::Resolution r = resolver.resolve(request.name, *l, source.id);
        if (r.channel != nullptr) {
            rc.descriptor = r.alias ? *r.alias : r.channel->desc;
            rc.series = r.channel->series;
            rc.status = r.channel->status;
        }
    }
    if (rc.descriptor) {
        rc.dps = request.dps.value_or(rc.descriptor->dps);
        const units::UnitRegistry* registry = maths.units().get();
        const units::UnitId base = rc.descriptor->unit;
        if (registry != nullptr && registry->contains(base)) {
            rc.displayUnit = registry->unit(base).label();
            rc.displayUnitId = base;
            bool named = false;
            if (!request.displayUnit.empty()) {
                if (const auto id = registry->find(request.displayUnit, registry->quantityOf(base))) {
                    rc.toDisplay = registry->fromBase(*id);
                    rc.displayUnit = registry->unit(*id).label();
                    rc.displayUnitId = *id;
                    named = true;
                }
            }
            // No unit of the channel's quantity named: its quantity's default display unit (D-211 (a): a
            // fraction stored in "ratio" shows in %).
            const units::UnitId shown = plot::defaultDisplayUnit(*registry, base);
            if (!named && shown != base) {
                if (Result<units::Converter> c = registry->converter(base, shown)) {
                    rc.toDisplay = *c;
                    rc.displayUnit = registry->unit(shown).label();
                    rc.displayUnitId = shown;
                }
            }
        }
    } else {
        rc.dps = request.dps.value_or(2);
    }
    return rc;
}

} // namespace detail

// ---- RenderService --------------------------------------------------------------------------------------

struct RenderService::Impl {
    struct Submission {
        std::shared_ptr<std::atomic<bool>> started;
        CancelSource cancel;
    };
    std::shared_ptr<std::atomic<int>> running = std::make_shared<std::atomic<int>>(0);
    std::map<std::string, Submission> latest;               // per replace key: the newest submission
    mutable std::shared_ptr<analysis::StatCache> statCache; // the workspace's (ReportModel), on first use
    QPointer<TrackMapService> trackMaps;
};

RenderService::RenderService(TaskPool& pool, Dispatcher& dispatcher, DataSelectionModel& selection,
                             MathsService& maths, DistanceService& distance, QObject* parent)
    : QObject(parent), pool_(pool), dispatcher_(dispatcher), selection_(selection), maths_(maths),
      distance_(distance), impl_(std::make_unique<Impl>()) {}

RenderService::~RenderService() {
    for (auto& entry : impl_->latest) {
        entry.second.cancel.cancel(); // queued builds of this service are pointless now
    }
}

void RenderService::submitBuild(const QString& worksheetId, const QString& componentId, Priority priority,
                                BuildFn build, QObject* context, OnGui onGui) {
    const std::string key = "trace/" + worksheetId.toStdString() + "/" + componentId.toStdString();
    Impl::Submission submission{std::make_shared<std::atomic<bool>>(false), CancelSource{}};
    impl_->latest[key] = submission;
    Future<plot::ComponentBuild> future =
        pool_.submit(priority, TaskKey{key}, submission.cancel.token(),
                     [build = std::move(build), running = impl_->running, started = submission.started](
                         const CancelToken& cancel) -> Result<plot::ComponentBuild> {
                         started->store(true);
                         running->fetch_add(1);
                         struct Done {
                             std::atomic<int>& n;
                             ~Done() { n.fetch_sub(1); }
                         } done{*running};
                         return build(cancel);
                     });
    Dispatcher* dispatcher = &dispatcher_; // runtime-owned (01 section 5.3 rule 11): the posting context
    QPointer<QObject> guard(context);      // made on the GUI thread; checked there
    future.onReady([dispatcher, guard, onGui = std::move(onGui)](const Result<plot::ComponentBuild>& result) {
        if (!result && result.error().code == ErrorCode::Cancelled) {
            return; // a replaced pending build: its result is dropped (01 section 5.4 rule 2)
        }
        dispatcher->post(dispatcher, [guard, onGui, result]() mutable {
            if (!guard.isNull()) {
                onGui(std::move(result));
            }
        });
    });
}

void RenderService::cancelPending(const QString& worksheetId, const QString& componentId) {
    const std::string key = "trace/" + worksheetId.toStdString() + "/" + componentId.toStdString();
    const auto it = impl_->latest.find(key);
    if (it == impl_->latest.end()) {
        return;
    }
    if (!it->second.started->load()) {
        it->second.cancel.cancel(); // still queued: resolves Cancelled without running
    }
    impl_->latest.erase(it);
}

int RenderService::runningBuilds() const noexcept {
    return impl_->running->load();
}

std::shared_ptr<analysis::StatCache> RenderService::statCache() const {
    if (impl_->statCache == nullptr) {
        impl_->statCache = std::make_shared<analysis::StatCache>();
    }
    return impl_->statCache;
}

void RenderService::setTrackMaps(TrackMapService* maps) {
    impl_->trackMaps = maps;
}

TrackMapService* RenderService::trackMaps() const {
    return impl_->trackMaps.data();
}

plot::ComponentBuildInputs
RenderService::prepareInputs(const project::ComponentSpec& spec, const plot::ViewState& view,
                             const plot::SelectionSnapshot& selection, const plot::LayoutContext& layout,
                             std::shared_ptr<const project::Scheme> scheme,
                             std::shared_ptr<const plot::ITextMetrics> metrics) const {
    plot::ComponentBuildInputs inputs;
    inputs.spec = spec;
    inputs.view = view;
    inputs.selection = selection;
    inputs.layout = layout;
    inputs.scheme = std::move(scheme);
    inputs.metrics = std::move(metrics);
    inputs.units = maths_.units();
    inputs.pool = &pool_;
    if (selection.main.range.v.isNil()) {
        return inputs;
    }
    if (TrackMapService* maps = impl_->trackMaps.data()) {
        if (const auto mainSource = selection_.source(selection.main.src)) {
            inputs.trackMap = maps->mapFor(*mainSource); // the Main source's venue (04 section 14 item 1)
        }
    }
    const std::vector<ChannelRequest> channels = componentChannels(spec);
    for (const plot::Slot& slot : selection.visibleSlots()) {
        const auto src = selection_.source(slot.range.src);
        const model::Range* range = src == nullptr ? nullptr : detail::findRange(*src, slot.range);
        const model::DataLayer* layer = src == nullptr ? nullptr : detail::layerOf(*src, slot.range.layer);
        if (range == nullptr || layer == nullptr) {
            continue;
        }
        plot::SlotData data;
        data.slot = slot;
        data.source = src;
        data.layer = std::string(model::canonicalLayerName(slot.range.layer));
        data.range = detail::geomOf(*range);
        data.outing = range->kind == model::RangeKind::Outing ? plot::OutingGeom{range->start, range->end}
                                                              : detail::outingAt(*layer, range->start);
        data.chain = distance_.chain(slot.range, Priority::Visible);
        for (const ChannelRequest& c : channels) {
            data.channels.push_back(detail::resolveChannel(maths_, *src, data.layer, c, Priority::Visible));
        }
        inputs.slots.push_back(std::move(data));
    }
    return inputs;
}

} // namespace esses::qt
