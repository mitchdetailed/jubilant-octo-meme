// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The data/zoom/pan/cursor/datum commands of hotkey batch 1 (06 section 14.4, data/keymaps/default.json).

#include <esses/qtmodel/builtin_commands.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/lap_stretch_service.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>

#include <QPointer>
#include <QUndoStack>
#include <memory>
#include <utility>

namespace esses::qt {

namespace {

CommandSpec spec(const char* id, const char* title, const char* menu, CommandContext context,
                 QStringList keys) {
    CommandSpec s;
    s.id = QString::fromLatin1(id);
    s.title = QString::fromLatin1(title);
    s.menuPath = QString::fromLatin1(menu);
    s.context = context;
    s.defaultKeys = std::move(keys);
    return s;
}

} // namespace

void registerSelectionCommands(CommandRegistry& registry, DataSelectionModel& selection) {
    const QPointer<DataSelectionModel> sel(&selection);
    const auto has = [sel] { return !sel.isNull() && sel->hasMain(); };
    const auto add = [&](CommandSpec s, std::function<void(DataSelectionModel&)> fn,
                         std::function<bool()> enabled = {}) {
        s.invoke = [sel, fn = std::move(fn)](const QVariantList&) {
            if (!sel.isNull()) {
                fn(*sel);
            }
        };
        s.enabled = enabled ? std::move(enabled) : has;
        registry.add(std::move(s));
    };
    add(spec("data.overlays_toggle", "Hide Overlays", "Data", CommandContext::Global, {QStringLiteral("F4")}),
        [](DataSelectionModel& m) { m.toggleOverlaysHidden(); });
    add(spec("data.overlays_clear", "Clear Overlays", "Data", CommandContext::Global,
             {QStringLiteral("Shift+F4")}),
        [](DataSelectionModel& m) { m.clearOverlays(); });
    {
        CommandSpec s = spec("data.single_colour_main", "Single Colour Main", "Data",
                             CommandContext::Worksheet, {QStringLiteral("J")});
        s.checkable = true;
        s.checked = [sel] { return !sel.isNull() && sel->snapshot().singleColourMain; };
        add(std::move(s),
            [](DataSelectionModel& m) { m.setSingleColourMain(!m.snapshot().singleColourMain); });
    }
    add(spec("data.next_range", "Next Range", "Data", CommandContext::Worksheet, {QStringLiteral("N")}),
        [](DataSelectionModel& m) { m.nextRange(); });
    add(spec("data.prev_range", "Previous Range", "Data", CommandContext::Worksheet, {QStringLiteral("P")}),
        [](DataSelectionModel& m) { m.prevRange(); });
    add(
        spec("data.select_fastest", "Select Fastest Lap", "Data", CommandContext::Global,
             {QStringLiteral("Ctrl+F")}),
        [](DataSelectionModel& m) { m.selectFastest(); },
        [sel] { return !sel.isNull() && !sel->sources().empty(); });
    add(
        spec("data.select_all_fast", "Select All Fast Laps", "Data", CommandContext::Global,
             {QStringLiteral("Ctrl+Alt+F")}),
        [](DataSelectionModel& m) { m.selectAllFast(); },
        [sel] { return !sel.isNull() && !sel->sources().empty(); });
    add(spec("data.swap_main_overlay0", "Swap Main and Overlay 0", "Data", CommandContext::Worksheet,
             {QStringLiteral("Q")}),
        [](DataSelectionModel& m) { m.swapMainOverlay0(); });
    add(spec("data.filter_all_sources", "Apply Filter to All Sources", "Data", CommandContext::Global, {}),
        [](DataSelectionModel& m) { m.applyFilterToAllSources(); });
}

void registerViewCommands(CommandRegistry& registry, std::function<WorksheetViewModel*()> activeWorksheet) {
    auto active = std::make_shared<std::function<WorksheetViewModel*()>>(std::move(activeWorksheet));
    const auto enabled = [active] { return (*active) && (*active)() != nullptr; };
    const auto add = [&](CommandSpec s, std::function<void(WorksheetViewModel&)> fn) {
        s.invoke = [active, fn = std::move(fn)](const QVariantList&) {
            if (*active) {
                if (WorksheetViewModel* w = (*active)()) {
                    fn(*w);
                }
            }
        };
        s.enabled = enabled;
        registry.add(std::move(s));
    };
    using C = CommandContext;
    {
        CommandSpec s =
            spec("data.toggle_domain", "Time / Distance", "Data", C::Global, {QStringLiteral("F9")});
        s.checkable = true;
        s.checked = [active] {
            WorksheetViewModel* w = *active ? (*active)() : nullptr;
            return w != nullptr && w->domain() == plot::Domain::Distance;
        };
        add(std::move(s), [](WorksheetViewModel& w) { w.toggleDomain(); });
    }
    add(spec("zoom.in", "Zoom In", "Component/Zoom", C::Graph, {QStringLiteral("Up")}),
        [](WorksheetViewModel& w) { w.zoomIn(); });
    add(spec("zoom.out", "Zoom Out", "Component/Zoom", C::Graph, {QStringLiteral("Down")}),
        [](WorksheetViewModel& w) { w.zoomOut(); });
    add(spec("zoom.full", "Zoom Full Outing", "Component/Zoom", C::Global, {QStringLiteral("F2")}),
        [](WorksheetViewModel& w) { w.zoomFull(); });
    add(spec("zoom.default_range", "Zoom to Main Range", "Component/Zoom", C::Graph, {QStringLiteral("W")}),
        [](WorksheetViewModel& w) { w.zoomToMainRange(); });
    add(spec("zoom.datum_main", "Zoom to Datum and Cursor", "Component/Zoom", C::Graph,
             {QStringLiteral("Z")}),
        [](WorksheetViewModel& w) { w.zoomToDatumMain(); });
    add(spec("pan.left", "Pan Left", "Component/Pan", C::Graph, {QStringLiteral("Shift+Left")}),
        [](WorksheetViewModel& w) { w.panBy(-0.10); });
    add(spec("pan.right", "Pan Right", "Component/Pan", C::Graph, {QStringLiteral("Shift+Right")}),
        [](WorksheetViewModel& w) { w.panBy(0.10); });
    add(spec("pan.back", "Page Back", "Component/Pan", C::Graph, {QStringLiteral("B")}),
        [](WorksheetViewModel& w) { w.panBy(-0.90); });
    add(spec("pan.forward", "Page Forward", "Component/Pan", C::Graph, {QStringLiteral("F")}),
        [](WorksheetViewModel& w) { w.panBy(0.90); });
    add(spec("pan.centre_cursor", "Centre on Cursor", "Component/Pan", C::Graph, {QStringLiteral("H")}),
        [](WorksheetViewModel& w) { w.centreOnCursor(); });
    add(spec("cursor.left", "Cursor Left", "Component/Cursor", C::Graph, {QStringLiteral("Left")}),
        [](WorksheetViewModel& w) { w.stepCursor(-1); });
    add(spec("cursor.right", "Cursor Right", "Component/Cursor", C::Graph, {QStringLiteral("Right")}),
        [](WorksheetViewModel& w) { w.stepCursor(1); });
    add(spec("cursor.left_fast", "Cursor Left Fast", "Component/Cursor", C::Graph,
             {QStringLiteral("Ctrl+Left")}),
        [](WorksheetViewModel& w) { w.stepCursor(-10); });
    add(spec("cursor.right_fast", "Cursor Right Fast", "Component/Cursor", C::Graph,
             {QStringLiteral("Ctrl+Right")}),
        [](WorksheetViewModel& w) { w.stepCursor(10); });
    add(spec("cursor.home", "Cursor to Left Edge", "Component/Cursor", C::Graph, {QStringLiteral("Home")}),
        [](WorksheetViewModel& w) { w.cursorHome(); });
    add(spec("cursor.end", "Cursor to Right Edge", "Component/Cursor", C::Graph, {QStringLiteral("End")}),
        [](WorksheetViewModel& w) { w.cursorEnd(); });
    add(spec("datum.toggle", "Datum On/Off", "Component/Datum", C::Graph, {QStringLiteral("D")}),
        [](WorksheetViewModel& w) { w.toggleDatum(); });
    add(spec("datum.place", "Place Datum", "Component/Datum", C::Graph, {QStringLiteral("Space")}),
        [](WorksheetViewModel& w) { w.placeDatum(); });
    add(spec("datum.swap", "Swap Datum and Cursor", "Component/Datum", C::Graph, {QStringLiteral("X")}),
        [](WorksheetViewModel& w) { w.swapDatum(); });
}

void registerEditCommands(CommandRegistry& registry, QUndoStack& undoStack) {
    const QPointer<QUndoStack> stack(&undoStack);
    CommandSpec undo = spec("edit.undo", "Undo", "Edit", CommandContext::Global, {QStringLiteral("Ctrl+Z")});
    undo.invoke = [stack](const QVariantList&) {
        if (!stack.isNull()) {
            stack->undo();
        }
    };
    undo.enabled = [stack] { return !stack.isNull() && stack->canUndo(); };
    registry.add(std::move(undo));
    CommandSpec redo = spec("edit.redo", "Redo", "Edit", CommandContext::Global, {QStringLiteral("Ctrl+Y")});
    redo.invoke = [stack](const QVariantList&) {
        if (!stack.isNull()) {
            stack->redo();
        }
    };
    redo.enabled = [stack] { return !stack.isNull() && stack->canRedo(); };
    registry.add(std::move(redo));
}

void registerLapStretchCommand(CommandRegistry& registry, LapStretchService& stretch) {
    const QPointer<LapStretchService> service(&stretch);
    CommandSpec s =
        spec("data.lap_stretch_toggle", "Lap Distance Stretching", "Data", CommandContext::Global, {});
    s.checkable = true;
    s.checked = [service] { return !service.isNull() && service->enabled(); };
    s.enabled = [service] { return !service.isNull(); };
    s.invoke = [service](const QVariantList&) {
        if (!service.isNull()) {
            service->toggle();
        }
    };
    registry.add(std::move(s));
}

} // namespace esses::qt
