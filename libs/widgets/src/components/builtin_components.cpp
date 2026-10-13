// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The component types of this build (04 section 7.4 table; M1: time_distance, values_list). Add a line per
// new component type; nothing registers itself statically.

#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/components/time_distance_graph.hpp>
#include <esses/widgets/components/values_list.hpp>

namespace esses::widgets {

void registerBuiltinComponents() {
    ComponentWidget::registerType(
        QString::fromLatin1(TimeDistanceGraph::kType),
        [](const project::ComponentSpec& spec, WorksheetContext& ctx, QWidget* parent) {
            return static_cast<ComponentWidget*>(new TimeDistanceGraph(spec, ctx, parent));
        });
    ComponentWidget::registerType(
        QString::fromLatin1(ValuesList::kType),
        [](const project::ComponentSpec& spec, WorksheetContext& ctx, QWidget* parent) {
            return static_cast<ComponentWidget*>(new ValuesList(spec, ctx, parent));
        });
}

} // namespace esses::widgets
