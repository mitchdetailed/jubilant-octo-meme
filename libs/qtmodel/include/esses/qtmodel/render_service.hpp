// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// RenderService (04 sections 1 item 2 and 6.3, 06 section 1; 01 section 5.4 rule 2): the only way widgets
// reach the TaskPool. submitBuild() uses the replace key trace/<worksheetId>/<componentId>: at most one
// running and one pending build per key; a new submit replaces the pending one (its result is dropped) and
// never cancels the running one; the result is delivered on the GUI thread through Dispatcher::post(context,
// ...), also when superseded (an interim frame, 04 section 6.3 step 3). prepareInputs() turns a component
// spec plus the current view and selection into the value-type plot::ComponentBuildInputs a worker receives:
// channel names resolved per slot (logged channels through the source's ChannelResolver, Corr Speed / Corr
// Dist and other maths outputs through MathsService, Pending when not yet evaluated), display converters, the
// slot geometry and DistanceChain (DistanceService). esses-render and the render-parity test use the same
// preparation (AC-M1-05). GUI thread only.
// M1 details: the replace-key semantics are the TaskPool's (trace/ keys: one running, one pending; the
// replaced pending build resolves Cancelled and is not delivered); every other result, superseded or failed,
// reaches onGui while `context` lives. Channels are resolved per slot through the source's ChannelResolver
// (no project mappings or aliases before M2) or MathsService for maths outputs (Pending until evaluated,
// requested at Visible); the display unit is the component's trace/row displayUnit when it names a unit of
// the channel's quantity, else the stored (base) unit (M4, D-211 (a): else the quantity's default display
// unit, plot::defaultDisplayUnit: "%" for a fraction stored in "ratio"). componentChannels() lists a
// component's channels (time_distance traces, values_list rows).
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/priority.hpp>
#include <esses/base/result.hpp>
#include <esses/plotcore/component_build.hpp>
#include <esses/plotcore/selection_snapshot.hpp>
#include <esses/plotcore/text_metrics.hpp>
#include <esses/plotcore/view_state.hpp>
#include <esses/project/component_spec.hpp>
#include <esses/project/scheme.hpp>

#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace esses {
class TaskPool;
} // namespace esses

namespace esses::analysis {
class StatCache;
} // namespace esses::analysis

namespace esses::qt {

class DataSelectionModel;
class Dispatcher;
class DistanceService;
class MathsService;
class TrackMapService;

// One channel a component shows: the name as referenced, the persisted display unit and decimals.
struct ChannelRequest {
    std::string name;
    std::string displayUnit; // "" = the channel's
    std::optional<int> dps;
};
// The channels of a component in display order: time_distance traces (TimeDistanceBody::channelNames order),
// values_list rows; every other type through plot::componentChannelUses (M3: the order its builders read
// SlotData::channels in); empty for an unknown type.
std::vector<ChannelRequest> componentChannels(const project::ComponentSpec& spec);

class RenderService final : public QObject {
    Q_OBJECT
public:
    using BuildFn = std::function<Result<plot::ComponentBuild>(const CancelToken& cancel)>;
    using OnGui = std::function<void(Result<plot::ComponentBuild> result)>;

    RenderService(TaskPool& pool, Dispatcher& dispatcher, DataSelectionModel& selection, MathsService& maths,
                  DistanceService& distance, QObject* parent = nullptr);
    ~RenderService() override;

    // Replace-key submission; `onGui` runs on the GUI thread while `context` is alive.
    void submitBuild(const QString& worksheetId, const QString& componentId, Priority priority, BuildFn build,
                     QObject* context, OnGui onGui);
    // Drops the pending build of a component (the running one completes and is discarded by the caller).
    void cancelPending(const QString& worksheetId, const QString& componentId);
    int runningBuilds() const noexcept;

    plot::ComponentBuildInputs prepareInputs(const project::ComponentSpec& spec, const plot::ViewState& view,
                                             const plot::SelectionSnapshot& selection,
                                             const plot::LayoutContext& layout,
                                             std::shared_ptr<const project::Scheme> scheme,
                                             std::shared_ptr<const plot::ITextMetrics> metrics) const;

    // M3 (the reports and track work, D-199): the pool and the Dispatcher a component's own qtmodel object
    // runs on (ReportModel), the workspace's StatCache shared by every ReportModel (created on first use),
    // and the track maps prepareInputs binds: ComponentBuildInputs::trackMap is the map of the Main source's
    // venue (TrackMapService::mapFor) when a service is set, else null.
    TaskPool& pool() const noexcept { return pool_; }
    Dispatcher& dispatcher() const noexcept { return dispatcher_; }
    std::shared_ptr<analysis::StatCache> statCache() const;
    void setTrackMaps(TrackMapService* maps);
    TrackMapService* trackMaps() const;

private:
    struct Impl;
    TaskPool& pool_;
    Dispatcher& dispatcher_;
    DataSelectionModel& selection_;
    MathsService& maths_;
    DistanceService& distance_;
    std::unique_ptr<Impl> impl_;
};

} // namespace esses::qt
