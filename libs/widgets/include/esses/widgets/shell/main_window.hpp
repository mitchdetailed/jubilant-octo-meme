// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MainWindow (06 sections 2.1-2.2, 3.4, 4.1, 18; 07 section 4.1: menus and toolbar with open, save,
// time/distance, overlays, single colour; AC-M1-01 walkthrough). Owns the workspace's esses_qtmodel objects
// (SettingsStore, ProjectSession, DataSelectionModel, WorkspaceViewModel, one WorksheetViewModel and
// WorksheetContext per worksheet of the active workbook, MathsService, DistanceService, RenderService,
// SourceLoader, CommandRegistry, KeymapBinder, MessagesModel and the dock item models) on top of the process
// AppRuntime, wired as libs/qtmodel/README.md "Wiring a workspace" describes, and the shell widgets: the
// workbook selector and worksheet tab bar above the WorksheetCanvas (central widget), the DockHost with the
// M1 docks (Data F8, Channels C, Values V, Messages), CommandActions (menus and the "Main" toolbar) and the
// KeyEventRouter. Status bar: cursor time, distance, lap, delta time, delta distance of the active worksheet
// (WorksheetViewModel::cursorInfo) and transient hints (5 s).
// Commands registered here (06 section 14.4 "Views and windows", hotkey batch 1): view.data_window (F8),
// view.channels_window (C), view.values_window (V), view.return_focus (ESC), view.worksheet_n (1-9, 0),
// view.prev_worksheet / view.next_worksheet (F11 / F12), view.messages_window, view.reset_docks,
// file.open_log, file.save_project, file.open_project; the data/zoom/pan/cursor/datum rows through
// qt::registerSelectionCommands / registerViewCommands (data.overlays_toggle becomes checkable "Overlays"
// for the toolbar); the Data window registers its row commands. Every other keymap command is registered
// disabled ("planned for <milestone>" / "reserved" tool tip) so that menus, --dump-commands and the binder
// see the whole table (06 section 14.4).
// Session restore (06 sections 3.4 and 18 step 4): restoreLayout() reads the layout-1 document (window
// geometry, the active workbook's perspective, else "_default", the active worksheet, Values "All channels")
// and saveLayout() writes it on workbook switch, Reset Dock Windows, project change and exit (closeEvent or
// the destructor). The document is <project>/.local/layout.json, or <config>/layout.json without a project.
// A perspective that cannot be restored resets the docks with an info row in Messages.
//
// M2 wiring (libs/qtmodel/README.md "Wiring a workspace"; 06 sections 1, 2.1-2.2, 13.7, 16, 18): the
// session's documents reach MathsService and SourceLoader (connectProjectSession), the selection reaches the
// maths (connectMathsSelection), the user's text profiles are registered, a LapStretchService serves the
// toolbar's data.lap_stretch_toggle, edit.undo / edit.redo (Edit menu and toolbar, "Undo <text>") act on the
// session's QUndoStack, component edits from the canvas are WorkbookCommands and every document change (undo,
// redo, recovery) re-syncs the workbook selector, the worksheet tabs and the shown worksheet; tools.options
// opens the OptionsDialog (window-modal, geometry kept in layout.json); the SchemeSelector sits on the
// toolbar when Options > Colours turns it on, a scheme change repaints every worksheet and restyles the
// application palette, and "follow OS light/dark" follows QStyleHints. Opening a project with a journal shows
// the RecoveryDialog (AC-M2-06) and only then starts SessionRestore (the -l logs, else the previous data,
// then the selection; 06 section 18 steps 3-6). Leaving a project (exit, another project) saves it when it is
// dirty and options.general.saveOnExit is on, else asks Save / Discard / Cancel (setExitPrompt replaces the
// question); an exit with only view-state changes writes them (the last selection, the Data-window filters).
// The M2 editors (06 sections 10 and 13; libs/widgets/README.md "M2 maths editors" and "M2 data editors"):
// the MappingPanel sits above the worksheet area and shows itself while an open waits for it (the loader's
// Mapping-panel hand-off is on, AC-M2-11); the Channel Status dock (tools.channel_status CTRL+U,
// view.channel_status); the Maths Editor (tools.maths_editor CTRL+M, [module, item] opens it at the item),
// the Channel Editor (tools.channel_editor CTRL+E) with its Aliases and Mappings editors (tools.aliases
// [alias], tools.mappings [logged name]), the Details editor (tools.details CTRL+D) and the Lap Editor
// (tools.lap_editor), each a modeless dialog created on first use with its geometry in layout.json;
// cursor.insert_marker (SHIFT+F7, a beacon at the main cursor) and the nine channel.* rows of
// ActiveChannelCommands (hints to the status bar; the editors' messages to Messages, origin Maths).
//
// M3 wiring (07 section 6.1; libs/widgets/README.md "M3 properties and shell"; src/shell/main_window_m3.cpp):
// the ZoomSummaryBar between the workbook row and the worksheet tabs; the Track Map dock (TrackMapWindow,
// AutoHide on the right, its context the active worksheet's); the AnimationController (data.animate A, the
// toolbar toggle, Data > Animation Rate, options.animation.rate); the CommandPalette (view.command_palette
// CTRL+SHIFT+P, with the '>' worksheet, '@' channel and '#' range lists); the Layout Editor (layout.editor
// CTRL+F7, ui.layoutLocked locks the canvas), the Select Worksheet dialog (F7), the Corrected Speed dialog,
// the Track Editor, the Units list, full screen; the component rows (properties F5 / CTRL+N, cut/copy/paste/
// delete/select all through ComponentClipboard and the component undo commands, focus next/previous, L K G S
// ALT+n Y and the graph toggles through genericComponentEdit unless the component handles them, maximise,
// z order, Export Data, Print to Clipboard); the offset rows (O, CTRL+SHIFT and ALT+SHIFT arrows) with the
// WorksheetViewModel's offset commit as an undoable OffsetRangeCommand; cursor.add_annotation; the file rows
// (Open Log with the Details pane, Close All, New/Close Project, Import/Export Data). Questions go through
// Prompts (tests replace them). Commands a component must implement itself (vertical zoom, the track view,
// the graph's value copy) are offered to the focused component (ComponentWidget::handleCommand) and stay
// planned.
//
// M4 wiring (07 section 7.1; D-206; docs/milestones/M4.md sections 2-3): one ShellExtension per M4 package
// (shell_extension.hpp; BudgetShell, ReloadShell, HiDpiShell, FollowupsShell, AboutShell in
// src/shell/main_window_m4_<package>.cpp) reached through the hook of main_window_m4.cpp; this header,
// main_window.cpp, main_window_m3.cpp and main_window_impl.hpp are not edited by the packages.
//
// Placement note: 01 section 2.1 and the outline put MainWindow in esses_app; 06 section 1 splits it
// ("MainWindow building blocks" in esses_widgets, the assembly in esses_app) and 07 section 4.1 lists it
// under esses_widgets. The class lives here so the tests/ui walkthrough can construct it; apps/app only
// parses the arguments, creates the runtime and shows the window.
#pragma once

#include <esses/base/paths.hpp>

#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QComboBox;
class QDialog;
class QLabel;
class QTabBar;
class QToolBar;

namespace esses::model {
struct DataSource;
} // namespace esses::model

namespace esses::qt {
class AppRuntime;
class CommandRegistry;
class DataSelectionModel;
class DistanceService;
class KeymapBinder;
class LapStretchService;
class MathsService;
class MessagesModel;
class ProjectSession;
class RenderService;
class SessionRestore;
class SettingsStore;
class SourceLoader;
class TrackMapService;
class WorksheetViewModel;
class WorkspaceViewModel;
} // namespace esses::qt

namespace esses::widgets {

class ActiveChannelCommands;
class AnimationController;
class CommandPalette;
class CorrectedSpeedDialog;
class LayoutEditor;
class OpenLogDialog;
class SelectWorksheetDialog;
class TrackEditor;
class TrackMapWindow;
class UnitsListDialog;
class ZoomSummaryBar;
class ChannelEditor;
class ChannelStatusWindow;
class ChannelsWindow;
class CommandActions;
class DataWindow;
class DetailsEditor;
class DockHost;
class KeyEventRouter;
class LapEditor;
class MappingPanel;
class MathsEditor;
class MessagesWindow;
class OptionsDialog;
class RecoveryDialog;
class SchemeSelector;
class ShellExtension;
class ValuesWindow;
class WorksheetCanvas;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(qt::AppRuntime& runtime, Paths paths, QWidget* parent = nullptr);
    ~MainWindow() override;

    // -l <log>: asynchronous; the source appears in the Data window when published. Relative paths resolve
    // against the current directory.
    void openLog(const std::filesystem::path& path);
    // -w <project>: the project folder or its project.json. False (and an Error row in Messages) when it
    // cannot be opened, or when leaving the open project was cancelled; the previous state stays. Once any
    // recovery prompt is answered, `logs` (the -l arguments) are opened, else the project's previous data
    // when options.general.loadPreviousData is on, and the last selection is restored (SessionRestore).
    bool openProject(const std::filesystem::path& folder, std::vector<std::filesystem::path> logs = {});
    // True when the last openProject() returned false because leaving the open project was cancelled (not
    // because the project could not be opened); a forwarded launch then drops its logs (D-217).
    bool projectOpenCancelled() const;

    // The answer to "Save changes to the project?" when a dirty project is left (exit or another project)
    // with options.general.saveOnExit off (or its save failed). The default asks with a QMessageBox.
    enum class ExitAnswer : int { Save, Discard, Cancel };
    void setExitPrompt(std::function<ExitAnswer()> prompt);
    // Saves or asks as above; false when the user cancelled (the project stays open).
    bool confirmLeaveProject();

    // Session restore of the layout document (constructor and openProject call it).
    void restoreLayout();
    // Writes the layout document now; false (and a Warning row) when it cannot be written.
    bool saveLayout();
    // Whether closing the window or destroying it writes the layout (default true; `esses --dump-commands`
    // turns it off).
    void setSaveLayoutOnExit(bool on);
    std::filesystem::path layoutFilePath() const;
    // Reset Dock Windows (06 section 3.5): default docks, the active workbook's perspective dropped.
    void resetDocks();

    // Worksheets of the active workbook (06 section 4.1).
    int worksheetCount() const;
    int activeWorksheetIndex() const;
    void selectWorksheet(int index);
    void selectWorkbook(int index);
    // view.return_focus: focus back to the remembered component (or the canvas).
    void returnFocus();

    qt::ProjectSession& session();
    qt::SettingsStore& settings();
    qt::DataSelectionModel& selection();
    qt::WorkspaceViewModel& workspace();
    qt::WorksheetViewModel* activeWorksheet();
    qt::MathsService& maths();
    qt::DistanceService& distance();
    qt::RenderService& render();
    qt::SourceLoader& loader();
    qt::CommandRegistry& commands();
    qt::KeymapBinder& keymap();
    qt::MessagesModel& messages();
    DockHost& docks();
    WorksheetCanvas& canvas();
    CommandActions& actions();
    KeyEventRouter& router();
    DataWindow& dataWindow();
    ChannelsWindow& channelsWindow();
    ValuesWindow& valuesWindow();
    MessagesWindow& messagesWindow();
    QTabBar& worksheetTabs();
    QComboBox& workbookSelector();
    QToolBar& mainToolBar();
    // M2: the lap-stretch service, the toolbar's scheme selector, the Options dialog (created on first use),
    // the recovery prompt while it is open (else nullptr) and the session restore of steps 5-6.
    qt::LapStretchService& lapStretch();
    SchemeSelector& schemeSelector();
    OptionsDialog& optionsDialog();
    RecoveryDialog* recoveryDialog() const;
    qt::SessionRestore& sessionRestore();
    // M2 editors: the Mapping panel and the Channel Status window exist with the window; the dialogs are
    // created on first use (by their command or by these accessors) and stay until the window closes.
    MappingPanel& mappingPanel();
    ChannelStatusWindow& channelStatusWindow();
    MathsEditor& mathsEditor();
    ChannelEditor& channelEditor();
    LapEditor& lapEditor();
    DetailsEditor& detailsEditor();
    ActiveChannelCommands& activeChannelCommands();
    // Status-bar fields "t", "d", "lap", "dt", "dd".
    QLabel* statusField(const QString& name) const;

    // ---- M3 (main_window_m3.cpp) ------------------------------------------------------------------------
    // The questions of the file and project rows (tests replace them; the defaults are Qt's dialogs). A
    // function left empty keeps the current one.
    struct Prompts {
        // A file to open (save = false) or write; "" = cancelled.
        std::function<QString(const QString& title, const QString& filter, bool save)> file;
        // A folder; "" = cancelled.
        std::function<QString(const QString& title)> folder;
        // A line of text; nullopt = cancelled.
        std::function<std::optional<QString>(const QString& title, const QString& label,
                                             const QString& initial)>
            text;
        // A choice among `items`; nullopt = cancelled.
        std::function<std::optional<int>(const QString& title, const QStringList& items)> choose;
    };
    void setPrompts(Prompts prompts);
    ZoomSummaryBar& zoomSummaryBar();
    // The venues' track maps (RenderService, SourceLoader, the Track Editor and the Track Map dock use it).
    qt::TrackMapService& trackMaps();
    CommandPalette& commandPalette();
    AnimationController& animation();
    TrackMapWindow& trackMapWindow();
    // Created on first use, kept until the window closes.
    LayoutEditor& layoutEditor();
    CorrectedSpeedDialog& correctedSpeedDialog();
    TrackEditor& trackEditor();
    UnitsListDialog& unitsList();
    // The dialogs open while their command runs (nullptr when closed).
    SelectWorksheetDialog* selectWorksheetDialog() const;
    OpenLogDialog* openLogDialog() const;
    // view.fullscreen: the window full screen with the docks hidden (and back).
    bool fullScreenMode() const;
    void setFullScreenMode(bool on);
    // component.copy / cut / paste / delete on the canvas's selection (else its focused component); false
    // when refused (layout locked, nothing selected, nothing on the clipboard).
    bool copyComponents();
    bool cutComponents();
    bool pasteComponents();
    bool deleteComponents();
    // The last status-bar hint of a shell command ("" when none since the last call of clearHint).
    QString lastHint() const;
    void clearHint();

Q_SIGNALS:
    void sourceOpened(const QString& path);
    void worksheetActivated(int index);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    struct Impl;
    void buildWidgets();
    void registerShellCommands();
    void registerPlannedCommands();
    void wireModels();
    void rebuildWorksheets();
    void refreshWorkbookSelector();
    void updateTitle();
    // M2
    void syncDocuments();
    void applyScheme();
    void updateEditCommands();
    void showRecoveryPrompt();
    void startSessionRestore();
    void showOptions();
    void registerEditorCommands();
    void showEditor(QDialog& dialog, const char* geometryKey);
    // M3 (main_window_m3.cpp)
    void buildM3Widgets(QWidget* central);
    void buildM3Menus();
    void registerM3Commands();
    void followWorksheetM3(int index);
    void syncLayoutLock();
    // M3 fix pass (D-203 (u)): the Track Editor's cursor span in track metres, and the auto-generation of an
    // unknown venue's map when a log opens (06 section 9, section 13.5 items 4 and 6).
    void syncTrackEditorSpan();
    void autoGenerateTrackMap(const std::shared_ptr<const model::DataSource>& source, int attempt);
    void hint(const QString& text);
    void openLogDialogNow();
    bool newProjectNow();
    bool closeProjectNow();
    // M4 (main_window_m4.cpp; D-206, docs/milestones/M4.md section 2): the M4 packages' shell extensions
    // (shell_extension.hpp), created before buildWidgets and called at the fixed points listed there. The
    // packages wire the shell through them and never edit this header.
    friend class ShellExtension;
    void createM4Extensions();
    void forEachM4Extension(const std::function<void(ShellExtension&)>& run);
    void destroyM4Extensions();
    std::unique_ptr<Impl> impl_;
};

} // namespace esses::widgets
