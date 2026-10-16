// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The component types of this build (04 section 7.4 table): the ten MVP types (M1: time_distance,
// values_list; registered by the M3 contract: outing, scatter, histogram, channel_report, time_report,
// track_position, gauge_numeric, gauge_bar). Nothing registers itself statically. The owners of the M3 types
// implement their classes in place and never need to edit this file.

#include <esses/widgets/components/bar_gauge.hpp>
#include <esses/widgets/components/channel_report.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/components/histogram.hpp>
#include <esses/widgets/components/numeric_gauge.hpp>
#include <esses/widgets/components/outing_graph.hpp>
#include <esses/widgets/components/scatter_plot.hpp>
#include <esses/widgets/components/time_distance_graph.hpp>
#include <esses/widgets/components/time_report.hpp>
#include <esses/widgets/components/track_position.hpp>
#include <esses/widgets/components/values_list.hpp>

namespace esses::widgets {

namespace {

template <class Component> void registerComponent() {
    ComponentWidget::registerType(
        QString::fromLatin1(Component::kType),
        [](const project::ComponentSpec& spec, WorksheetContext& ctx, QWidget* parent) {
            return static_cast<ComponentWidget*>(new Component(spec, ctx, parent));
        });
}

} // namespace

void registerBuiltinComponents() {
    registerComponent<TimeDistanceGraph>();
    registerComponent<ValuesList>();
    registerComponent<OutingGraph>();
    registerComponent<ScatterPlot>();
    registerComponent<Histogram>();
    registerComponent<ChannelReport>();
    registerComponent<TimeReport>();
    registerComponent<TrackPosition>();
    registerComponent<NumericGauge>();
    registerComponent<BarGauge>();
}

} // namespace esses::widgets
