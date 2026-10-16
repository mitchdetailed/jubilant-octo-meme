// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The generic behaviour of the component rows that edit a component's spec (06 section 14.4 "Components
// general" and "Graph display", 04 section 7.2 item 4; AC-M3-01): what L, K, G, S, ALT+n, Y, E, SHIFT+E, F3
// and M do when the focused component does not handle the command itself (ComponentWidget::handleCommand).
// Each is one spec edit (the component's specEditRequested, so one undo step), computed here as a pure
// function:
//   component.legend         legend.channels on/off (every type)
//   component.colour_legend  legend.colour on/off (scatter, histogram, track_position)
//   component.cycle_layout   time_distance/outing display.overlapMode panels <-> overlapped; scatter layout
//                            overlapped -> tiled -> average; histogram layout overlapped <-> tiled;
//                            channel_report layout channelsInRows <-> channelsInColumns
//   component.cycle_style    time_distance/outing display.traceStyle line <-> points; scatter style points ->
//                            pointsTrend -> trend; histogram style bar -> line -> text
//   component.colour_band n  time_report bands[n].visible; gauge_numeric/gauge_bar colourBands[n].visible
//                            (a band beyond the list does nothing)
//   cursor.style             time_distance/outing display.cursorStyle line -> crosshair -> none; scatter
//                            cursorStyle cross -> ring -> none
//   graph.status_errors      time_distance/outing display.showStatusStrip
//   graph.device_events      time_distance/outing display.showDeviceEvents
//   graph.variance           time_distance display.variance.show
//   graph.measurements       time_distance display.key.stats
// An absent member counts as the builders' default (plotcore TdDisplay, ScatterBody, HistogramBody,
// ReportSpec). New public names (D-200). Owner: src/properties_shell/.
#pragma once

#include <esses/project/component_spec.hpp>

#include <QString>
#include <QVariantList>
#include <optional>

namespace esses::widgets {

// The spec after `commandId` (with its keymap arguments), or nullopt when the command does not apply to the
// component's type (or changes nothing: a colour band beyond the list). `undoText` names the edit.
std::optional<project::ComponentSpec> genericComponentEdit(const project::ComponentSpec& spec,
                                                           const QString& commandId, const QVariantList& args,
                                                           QString* undoText = nullptr);
// True when genericComponentEdit knows the command for the type (whatever the arguments).
bool hasGenericComponentEdit(const QString& type, const QString& commandId);

} // namespace esses::widgets
