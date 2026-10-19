// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MainWindow's private state (main_window.cpp, main_window_m3.cpp): the workspace's qtmodel objects, the
// shell widgets and, since M3, the summary bar, palette, animation, Track Map dock and the M3 dialogs; since
// M4 the list of the packages' shell extensions (main_window_m4.cpp, D-206). Internal to src/shell/; the M4
// packages read it through ShellExtension::impl() (with `auto&`: the type is private to MainWindow) and never
// edit it.
#pragma once

#include <esses/base/paths.hpp>
#include <esses/qtmodel/channel_status_model.hpp>
#include <esses/qtmodel/lap_stretch_service.hpp>
#include <esses/qtmodel/qtmodel.hpp>
#include <esses/qtmodel/session_restore.hpp>
#include <esses/qtmodel/track_map_service.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/components/track_editor.hpp>
#include <esses/widgets/components/track_map_window.hpp>
#include <esses/widgets/docks/channels_window.hpp>
#include <esses/widgets/properties_shell/animation_controller.hpp>
#include <esses/widgets/properties_shell/command_palette.hpp>
#include <esses/widgets/properties_shell/corrected_speed_dialog.hpp>
#include <esses/widgets/properties_shell/layout_editor.hpp>
#include <esses/widgets/properties_shell/shell_dialogs.hpp>
#include <esses/widgets/properties_shell/zoom_summary_bar.hpp>
#include <esses/widgets/shell/layout_file.hpp>
#include <esses/widgets/shell/main_window.hpp>

#include <QByteArray>
#include <QDropEvent>
#include <QLabel>
#include <QMenu>
#include <QMetaObject>
#include <QMimeData>
#include <QPointer>
#include <QString>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace esses::widgets {

class ActiveChannelCommands;
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
class ValuesWindow;
class WorksheetCanvas;

namespace shell_detail {

// Drops of application/x-esses-channels on a component that does not take drops itself: the shell adds the
// channels through ComponentWidget::addChannel (06 section 7 item 5; CTRL at the drop = new panel).
class ChannelDropFilter final : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        auto* component = qobject_cast<ComponentWidget*>(watched);
        if (component == nullptr || !component->acceptsChannelDrop()) {
            return false;
        }
        switch (event->type()) {
        case QEvent::DragEnter:
        case QEvent::DragMove: {
            auto* e = static_cast<QDropEvent*>(event);
            if (e->mimeData() != nullptr && ChannelsWindow::parseMime(*e->mimeData())) {
                e->setDropAction(Qt::CopyAction);
                e->accept();
                return true;
            }
            return false;
        }
        case QEvent::Drop: {
            auto* e = static_cast<QDropEvent*>(event);
            const auto drop =
                e->mimeData() != nullptr ? ChannelsWindow::parseMime(*e->mimeData()) : std::nullopt;
            if (!drop) {
                return false;
            }
            const bool newPanel = drop->newPanel || e->modifiers().testFlag(Qt::ControlModifier);
            for (qsizetype i = 0; i < drop->channels.size(); ++i) {
                component->addChannel(drop->channels[i], newPanel && i == 0);
            }
            e->setDropAction(Qt::CopyAction);
            e->accept();
            return true;
        }
        default:
            return false;
        }
    }
};

} // namespace shell_detail

struct MainWindow::Impl {
    Impl(qt::AppRuntime& rt, Paths p); // main_window.cpp

    qt::AppRuntime& runtime;
    Paths paths;
    QString unitsError;
    std::shared_ptr<const units::UnitRegistry> units;
    std::unique_ptr<qt::SettingsStore> settings;
    std::unique_ptr<qt::ProjectSession> session;
    std::unique_ptr<qt::DataSelectionModel> selection;
    std::unique_ptr<qt::WorkspaceViewModel> workspace;
    std::unique_ptr<qt::MathsService> maths;
    std::unique_ptr<qt::DistanceService> distance;
    // M3: the venues' track maps (declared before the render service and the loader that hold it).
    std::unique_ptr<qt::TrackMapService> trackMaps;
    std::unique_ptr<qt::RenderService> render;
    std::unique_ptr<qt::SourceLoader> loader;
    std::unique_ptr<qt::CommandRegistry> commands;
    std::unique_ptr<qt::KeymapBinder> keymap;
    std::unique_ptr<qt::MessagesModel> messages;
    std::unique_ptr<qt::ChannelsModel> channelsModel;
    std::unique_ptr<qt::ValuesModel> valuesModel;
    // M2: declared after the loader, the distance service and the selection they reference.
    std::unique_ptr<qt::LapStretchService> stretch;
    std::unique_ptr<qt::SessionRestore> restore;
    std::unique_ptr<qt::ChannelStatusModel> statusModel; // behind the Channel Status dock
    // One per worksheet of the active workbook; contexts reference the view models (declared after them, so
    // destroyed first).
    std::vector<std::unique_ptr<qt::WorksheetViewModel>> worksheets;
    std::vector<std::unique_ptr<WorksheetContext>> contexts;
    int activeWorksheet = -1;
    Uuid shownWorkbook; // the workbook whose worksheets are the tabs (syncDocuments)
    LayoutFile layout;
    bool layoutSaved = false;
    bool saveLayoutOnExit = true;

    QWidget* central = nullptr;
    QComboBox* workbookCombo = nullptr;
    QTabBar* tabs = nullptr;
    WorksheetCanvas* canvas = nullptr;
    DockHost* docks = nullptr;
    CommandActions* actions = nullptr;
    KeyEventRouter* router = nullptr;
    QToolBar* toolBar = nullptr;
    DataWindow* dataWindow = nullptr;
    ChannelsWindow* channelsWindow = nullptr;
    ValuesWindow* valuesWindow = nullptr;
    MessagesWindow* messagesWindow = nullptr;
    shell_detail::ChannelDropFilter* dropFilter = nullptr;
    std::map<QString, QLabel*> status;
    QMetaObject::Connection cursorInfo;
    QPointer<ComponentWidget> lastComponent; // view.return_focus target
    std::shared_ptr<ILogSink> messagesSink;  // this window's Messages log sink (removed with the window)
    // M2
    SchemeSelector* schemeSelector = nullptr; // on the toolbar (ui.schemeSelectorOnToolbar)
    QAction* schemeAction = nullptr;
    QAction* schemeSeparator = nullptr;
    QPointer<OptionsDialog> options;
    QPointer<RecoveryDialog> recovery;
    std::vector<std::filesystem::path> restoreLogs; // -l logs of the project being opened (SessionRestore)
    bool restoreQueued = false;                     // SessionRestore waits for the recovery answer
    bool recoveryDeferred = false;                  // "Later": the journal stays until the next open
    bool opening = false;                           // inside openProject: syncDocuments waits for its rebuild
    std::function<ExitAnswer()> exitPrompt;
    // M2 editors: the panel and the Channel Status window live with the window, the dialogs from first use.
    MappingPanel* mappingPanel = nullptr;
    ChannelStatusWindow* statusWindow = nullptr;
    ActiveChannelCommands* active = nullptr;
    QPointer<MathsEditor> mathsEditor;
    QPointer<ChannelEditor> channelEditor;
    QPointer<LapEditor> lapEditor;
    QPointer<DetailsEditor> detailsEditor;
    // M3 (main_window_m3.cpp)
    ZoomSummaryBar* zoomBar = nullptr;
    CommandPalette* palette = nullptr;
    AnimationController* animation = nullptr;
    TrackMapWindow* trackMap = nullptr;
    QPointer<LayoutEditor> layoutEditor;
    QPointer<CorrectedSpeedDialog> correctedSpeed;
    QPointer<TrackEditor> trackEditor;
    QMetaObject::Connection trackSpanFollow; // the active worksheet's changes -> the editor's cursor span
    std::set<std::string> autoTrackVenues;   // venues whose map auto-generation was started this session
    QPointer<UnitsListDialog> unitsList;
    QPointer<SelectWorksheetDialog> selectWorksheet;
    QPointer<OpenLogDialog> openLog;
    QMenu* rateMenu = nullptr;
    QLabel* fullScreenHint = nullptr;
    QByteArray fullScreenPerspective; // the docks before full screen
    bool fullScreen = false;
    Prompts prompts;
    QString lastHint;
    std::map<std::string, int> pasteCounts; // worksheet id + clipboard text hash -> pastes so far
    // M4 (main_window_m4.cpp, D-206): the packages' shell extensions in makeM4Extensions order (children of
    // the window, deleted by destroyM4Extensions before any widget or model).
    std::vector<ShellExtension*> extensions;
};

} // namespace esses::widgets
