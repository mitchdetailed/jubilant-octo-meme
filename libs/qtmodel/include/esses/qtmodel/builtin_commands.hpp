// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The esses_qtmodel facade commands (06 section 14.1: "esses_qtmodel facades (data, tools)"; 06 section 14.4
// rows; hotkey batch 1 of 07 section 4.1). Added in M1 so that the shell, the walkthrough and the keymap Qt
// Tests (AC-M1-06) drive one set of handlers:
//   registerSelectionCommands  data.overlays_toggle (F4), data.overlays_clear (Shift+F4),
//                              data.single_colour_main (J), data.next_range (N), data.prev_range (P),
//                              data.select_fastest (Ctrl+F), data.select_all_fast (Ctrl+Alt+F),
//                              data.swap_main_overlay0 (Q), data.filter_all_sources
//   registerViewCommands       data.toggle_domain (F9) and the zoom.*, pan.*, cursor.* and datum.* rows of
//                              batch 1, acting on the active WorksheetViewModel
//   registerEditCommands       edit.undo (Ctrl+Z), edit.redo (Ctrl+Y) on the project's QUndoStack (M2, D-032;
//                              the Edit-menu QActions are esses_widgets', D-161)
//   registerLapStretchCommand  data.lap_stretch_toggle (checkable; LapStretchService, M2, D-016)
// Ids, contexts and default keys are those of data/keymaps/default.json. The Data-window row commands
// (data.select_main, data.toggle_overlay0, data.toggle_overlay, data.close_file) act on the highlighted row
// and are registered by the Data window (esses_widgets); file.*, view.* and layout.* by the application.
#pragma once

#include <functional>

class QUndoStack;

namespace esses::qt {

class CommandRegistry;
class DataSelectionModel;
class LapStretchService;
class WorksheetViewModel;

void registerSelectionCommands(CommandRegistry& registry, DataSelectionModel& selection);
// `activeWorksheet` returns the active worksheet's view model, or nullptr (commands are then disabled).
void registerViewCommands(CommandRegistry& registry, std::function<WorksheetViewModel*()> activeWorksheet);
// Enabled while the stack can undo / redo.
void registerEditCommands(CommandRegistry& registry, QUndoStack& undoStack);
void registerLapStretchCommand(CommandRegistry& registry, LapStretchService& stretch);

} // namespace esses::qt
