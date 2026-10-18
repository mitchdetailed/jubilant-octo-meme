// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ActiveChannelCommands (06 section 14.4 "Active channel", 03 sections 4.2 and 11 item 10, D-015, D-016):
// the handlers of the component-context hotkeys that act on the focused component's active channel (the one
// marked with the square; the first visible trace when the component names none). registerCommands() adds
// the M2 rows of data/keymaps/default.json with their default keys:
//   channel.next_active, prev_active (PgDown, PgUp): the next / previous visible trace, wrapping (spec edit);
//   channel.hide (Ctrl+H): the active trace becomes invisible, the square moves to the next visible trace;
//   channel.remove (Ctrl+Delete): the active trace leaves the component, an emptied group goes too;
//   channel.edit_maths (Ctrl+Shift+M): the Maths Editor at the item that defines the channel in the Main
//     source; a logged or built-in channel only gets a hint (no expression is created);
//   channel.status (Ctrl+Shift+S): the Channel Status dock with the channel's row selected (maths items,
//     aliases and mappings have rows); else a hint;
//   channel.filter (Ctrl+Shift+F): a Filter item (points or seconds) shadowing the channel in the Main
//     source's transient maths: never saved, gone when the source closes or reloads;
//   channel.scale_offset (Ctrl+Shift+O): a Scale & Offset item in the Main source's local maths (sidecar),
//     DPS preserved;
//   channel.zero_at_cursor (Ctrl+Shift+Z): the same item with offset = -(the displayed value at the cursor)
//     in the display unit, applied as a delta, so the channel reads 0 there in its display unit (0 deg C,
//     not -273.15); a second zero adjusts the existing item.
// Spec edits go out as the component's specEditRequested (the canvas turns them into undo commands); local
// maths edits are SetLocalMathsCommand on the project's undo stack. The shell supplies the focused component,
// the active worksheet and the Maths Editor and Channel Status windows through Host, and shows hint() in the
// status bar. The Filter and Scale & Offset dialogs are replaceable prompts (tests). GUI thread only.
#pragma once

#include <esses/maths/math_module.hpp>
#include <esses/model/ids.hpp>

#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <map>
#include <optional>
#include <utility>

namespace esses::qt {
class CommandRegistry;
class DataSelectionModel;
class MathsService;
class ProjectSession;
class SourceLoader;
class WorksheetViewModel;
} // namespace esses::qt

namespace esses::widgets {

class ChannelStatusWindow;
class ComponentWidget;
class MathsEditor;

class ActiveChannelCommands final : public QObject {
    Q_OBJECT
public:
    struct Host {
        std::function<ComponentWidget*()> focusedComponent;       // the worksheet's focused component
        std::function<qt::WorksheetViewModel*()> activeWorksheet; // its cursor (zero at cursor)
        std::function<MathsEditor*()> mathsEditor;           // shows the Maths Editor (created on demand)
        std::function<ChannelStatusWindow*()> channelStatus; // shows and focuses the Channel Status dock
    };
    struct FilterChoice {
        std::optional<int> points;     // an odd number of samples is centred (03 section 5.2)
        std::optional<double> seconds; // or a time window
    };
    struct ScaleOffsetChoice {
        double scale = 1.0;
        double offset = 0.0;
        QString offsetUnit; // "" = the channel's stored unit
    };
    using FilterPrompt = std::function<std::optional<FilterChoice>(const QString& channel)>;
    using ScaleOffsetPrompt =
        std::function<std::optional<ScaleOffsetChoice>(const QString& channel, const QString& displayUnit)>;

    ActiveChannelCommands(qt::ProjectSession& session, qt::SourceLoader& loader, qt::MathsService& maths,
                          qt::DataSelectionModel& selection, Host host, QObject* parent = nullptr);
    ~ActiveChannelCommands() override;

    // The command ids registerCommands() adds.
    static QStringList commandIds();
    void registerCommands(qt::CommandRegistry& registry);
    void setFilterPrompt(FilterPrompt prompt) { filterPrompt_ = std::move(prompt); }
    void setScaleOffsetPrompt(ScaleOffsetPrompt prompt) { scaleOffsetPrompt_ = std::move(prompt); }

    // The focused component's active channel, or nullopt.
    std::optional<QString> activeChannel() const;
    // The handlers (false when nothing happened; a hint says why where the user needs to know).
    bool nextActive(int step);
    bool hideActive();
    bool removeActive();
    bool editMaths();
    bool showStatus();
    bool filterActive();
    bool scaleOffsetActive();
    bool zeroAtCursor();

    // The transient module of a source (the filters created here), or nullopt.
    std::optional<maths::MathModule> transientModule(const model::SourceId& source) const;
    // The last hint ("" after a command that needed none).
    QString lastHint() const { return lastHint_; }

Q_SIGNALS:
    // A status-bar hint (06 section 14.4: a logged channel for CTRL+SHIFT+M / S, a value not available yet).
    void hint(const QString& text);

private:
    struct Target;
    std::optional<Target> target() const;
    bool fail(const QString& text);
    bool writeLocalItem(const Target& t, const std::function<void(maths::MathItem&, bool existed)>& edit,
                        const QString& undoText);

    qt::ProjectSession& session_;
    qt::SourceLoader& loader_;
    qt::MathsService& maths_;
    qt::DataSelectionModel& selection_;
    Host host_;
    FilterPrompt filterPrompt_;
    ScaleOffsetPrompt scaleOffsetPrompt_;
    std::map<model::SourceId, maths::MathModule> transient_;
    QString lastHint_;
};

} // namespace esses::widgets
