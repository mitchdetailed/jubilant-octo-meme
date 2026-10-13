// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelsWindow, dock id "channels", C (06 sections 3.2 and 7): a search line edit (focused by
// view.channels_window) above a QTreeView over qt::ChannelsModel through a filter proxy. M1: plain substring
// filter (qt::ChannelsModel::filterAccepts: every whitespace-separated word is a case-insensitive substring
// of the name; the source and category rows of a match stay visible); the word-prefix search semantics arrive
// in M2. The first match is highlighted on every keystroke. Keyboard: DOWN or TAB from the search box moves
// into the list; ENTER (search box or list) requests the highlighted channel for the focused component
// (addChannelRequested(name, false)), CTRL+ENTER as a new panel; double-click / CTRL+double-click likewise;
// ESC returns focus (view.return_focus; the search text is kept). Drag carries MIME
// application/x-esses-channels = {"channels": [...], "newPanel": false} (the target reads CTRL at the drop);
// parseMime() reads it back for drop targets. The tree is expanded after every rebuild (the persisted
// expand state is M2).
#pragma once

#include <QStringList>
#include <QWidget>
#include <optional>

class QLineEdit;
class QMimeData;
class QSortFilterProxyModel;
class QTreeView;

namespace esses::qt {
class ChannelsModel;
class CommandRegistry;
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
    QTreeView* tree_ = nullptr;
};

} // namespace esses::widgets
