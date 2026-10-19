// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Internal helpers of the components in src/components/plots/ (not installed): the Export Data runner (a pool
// task through RenderService, awaited with a local event loop so that the GUI thread keeps processing events
// and never blocks, 01 section 5.3), the inputs of an export range (04 section 7.2 item 8), CSV fields,
// slot labels, the quantity of a channel in the Main source (like-units grouping), the wheel's notch counting
// and the command ids every drawn component answers.
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/plotcore/component_build.hpp>
#include <esses/units/unit_registry.hpp>
#include <esses/widgets/components/component_widget.hpp>

#include <QByteArray>
#include <QString>
#include <QVariantList>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace esses::widgets::plots {

// Command ids (06 section 14.4) the plots components handle in handleCommand.
inline const QString kCycleLayout = QStringLiteral("component.cycle_layout");
inline const QString kCycleStyle = QStringLiteral("component.cycle_style");
inline const QString kLegend = QStringLiteral("component.legend");
inline const QString kColourLegend = QStringLiteral("component.colour_legend");
inline const QString kColourBand = QStringLiteral("component.colour_band");
inline const QString kCursorStyle = QStringLiteral("cursor.style");
inline const QString kCursorLeft = QStringLiteral("cursor.left");
inline const QString kCursorRight = QStringLiteral("cursor.right");
inline const QString kCursorLeftFast = QStringLiteral("cursor.left_fast");
inline const QString kCursorRightFast = QStringLiteral("cursor.right_fast");
inline const QString kCopyEquation = QStringLiteral("component.copy_equation");

// The band index of component.colour_band's args ([n]); -1 when absent or not an integer.
int bandArg(const QVariantList& args);

// Runs `work` on the TaskPool (Priority::Background, a key of its own) and waits for its result with a local
// QEventLoop: the GUI thread keeps processing events (no Future::get), and the loop also ends when the
// application quits. nullopt when the work failed or never completed.
std::optional<QByteArray> runOffGui(WorksheetContext& context, const QString& key,
                                    std::function<std::optional<QByteArray>(const CancelToken&)> work);

// The inputs of an export (04 section 7.2 item 8): the current zoom, the Main range padded by 2 s, or the
// whole Main outing, as the view's zoom of `inputs`.
plot::ComponentBuildInputs exportInputs(plot::ComponentBuildInputs inputs, ExportOptions::Range range);

// A CSV number (locale independent, up to 12 significant digits; "" for a non-finite value) and a quoted
// CSV text field.
QByteArray csvNumber(double v);
QByteArray csvText(const QString& text);
// "Main", "Overlay 0", "Overlay n" for a slot.
QString slotLabel(const plot::SlotData& slot);

// The quantity of `channel` in the Main source of the context (logged channels through the source's
// ChannelResolver, maths outputs through MathsService); nullopt when it does not resolve.
std::optional<units::QuantityId> quantityOf(WorksheetContext& context, const std::string& channel);

// Whole notches of a wheel event: high-resolution wheels add up to 120 per notch; a change of direction
// starts again from zero.
int wheelNotches(int& remainder, int angleDelta);

// The component's zoom (the worksheet's, or its own when links.zoom is off; D-211 (f)) scaled by `factor`
// about its cursor (a summary component's wheel, zoom.wheel).
void zoomAboutCursor(ComponentWidget& component, const plot::ComponentBuildInputs& inputs, double factor);

// What the legacy CSV's metadata names besides the Main source's details (05 section 6 writer item 1): the
// worksheet the export comes from (its "Worksheet" field).
struct LegacyExportInfo {
    std::string worksheet;
};

// Worker (M4, D-211 (d); 04 section 7.2 item 8): the Main slot's `channels` (indexes into
// SlotData::channels; repeats and channels that cannot be read are left out) over the view's zoom of `inputs`
// in the legacy CSV dialect of esses-cli export --format motec.csv (formats::makeLegacyCsvWriter: the
// metadata lines with the Main source's details, the export range and worksheet, names and units rows, every
// channel resampled onto one grid at the highest uniform rate capped at 1 kHz by its own interpolation, Time
// from 0 at the start of the span, values in each channel's display unit, CRLF, every field quoted). The
// writer writes a file, so the bytes pass through a temporary file that is removed again. nullopt when no
// channel can be read, the span is empty, or the work is cancelled or fails.
std::optional<QByteArray> legacyCsv(const plot::ComponentBuildInputs& inputs,
                                    const std::vector<std::size_t>& channels, const LegacyExportInfo& info,
                                    const CancelToken& cancel);

} // namespace esses::widgets::plots
