// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelsWindow, dock id "channels", C (06 sections 3.2 and 7): a search line edit (focused by
// view.channels_window) above a QTreeView over qt::ChannelsModel through a filter proxy. Search (06 section 7
// item 3, D-175, M2): qt::ChannelsModel::filterAccepts on every keystroke, the query and the names split into
// words on white space, '_', '-', '/', brackets and parentheses, every query word a case-insensitive prefix
// of a distinct name word in order ("W S FL" finds "Wheel Speed FL", "temp (ecu)" "Water Temp (ECU)"); loose
// matching (Options > General, ProjectSession::looseChannelSearch, followed through followSession) makes each
// word a substring, still in order. The source and category rows of a match stay visible; a query that
// matches nothing says so under the box. The first match is highlighted on every keystroke. Keyboard: DOWN or
// TAB from the search box moves into the list; ENTER (search box or list) requests the highlighted channel
// for the focused component (addChannelRequested(name, false)), CTRL+ENTER as a new panel; double-click /
// CTRL+double-click likewise; ESC returns focus (view.return_focus; the search text is kept). Drag carries
// MIME application/x-esses-channels = {"channels": [...], "newPanel": false} (the target reads CTRL at the
// drop); parseMime() reads it back for drop targets. The tree is expanded after every rebuild (the persisted
// expand state of 06 section 7 item 1 is not built yet).
#pragma once

#include <QStringList>
#include <QWidget>
#include <optional>

class QLabel;
class QLineEdit;
class QMimeData;
class QSortFilterProxyModel;
class QTreeView;

namespace esses::qt {
class ChannelsModel;
class CommandRegistry;
class ProjectSession;
} // namespace esses::qt

namespace esses::widgets {

class ChannelsWindow final : public QWidget {
    Q_OBJECT
public:
    static constexpr const char* kDockId = "channels";
    static constexpr const char* kMimeType = "application/x-esses-channels";

    struct ChannelDrop {
        QStringList channels;
        bool newPanel = false;
    };
    // The payload of a channel drag, or nullopt when the MIME data carries none.
    static std::optional<ChannelDrop> parseMime(const QMimeData& mime);

    ChannelsWindow(qt::ChannelsModel& model, qt::CommandRegistry& commands, QWidget* parent = nullptr);
    ~ChannelsWindow() override;

    QLineEdit& search() noexcept { return *search_; }
    QTreeView& tree() noexcept { return *tree_; }
    QSortFilterProxyModel& filterModel() noexcept { return *proxy_; }
    // The channel name of the highlighted row, or empty.
    QString currentChannel() const;
    // The channel names currently shown by the filter (depth-first, tests and the first-match rule).
    QStringList visibleChannels() const;
    // Loose name matching (a substring per word instead of a word prefix); re-filters at once.
    void setLooseMatch(bool loose);
    bool looseMatch() const noexcept { return loose_; }
    // Follows the project's options.general.looseChannelSearch (now, on every document change and on open).
    void followSession(qt::ProjectSession& session);
    // The "nothing matches" note under the search box (hidden while something matches or the box is empty).
    QLabel& noMatch() noexcept { return *noMatch_; }

Q_SIGNALS:
    // ENTER / double-click on a channel (CTRL: new panel); MainWindow routes it to the focused component.
    void addChannelRequested(const QString& name, bool newPanel);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applyFilter(const QString& text);
    void highlightFirstMatch();
    void request(bool newPanel);

    qt::ChannelsModel& model_;
    qt::CommandRegistry& commands_;
    QSortFilterProxyModel* proxy_ = nullptr;
    QLineEdit* search_ = nullptr;
    QLabel* noMatch_ = nullptr;
    QTreeView* tree_ = nullptr;
    bool loose_ = false;
};

} // namespace esses::widgets
