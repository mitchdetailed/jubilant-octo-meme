// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelStatusWindow, dock id "channelstatus" (CTRL+U; 06 sections 3.2 and 10, D-013, D-021, AC-M2-12): the
// table of qt::ChannelStatusModel (Source, Channel, Kind, Status, Resolved to, Rate, Unit; sortable) with a
// filter box (the Channels-window word-prefix search on the channel, or a substring of the source, kind,
// status or "Resolved to" text), an "Errors only" toggle (ChannelStatusModel::ErrorRole), Copy Table (the
// visible rows as tab-separated text with a header line) and a detail pane below the table with the full
// ResolutionTrace of the current row, one step per line (mapping, alias, case-fold, logged name, missing).
// Error rows are red, warnings amber, pending rows italic; a missing reference shows "(missing)" and is never
// filtered away by the model. The current row survives the model's coalesced refreshes (by row identity).
// Double-click (or ENTER) on a row opens its editor: a maths item or constant -> openItemRequested("maths",
// module id, name) and the command tools.maths_editor with [module id, name]; an alias -> ("alias", name)
// and tools.aliases [name]; a mapping -> ("mapping", logged name) and tools.mappings [logged name] (a command
// is invoked only when it is registered with a handler and enabled). channel.status (CTRL+SHIFT+S) shows the
// active channel's row through focusChannel().
#pragma once

#include <esses/maths/channel_key.hpp>

#include <QString>
#include <QWidget>

class QCheckBox;
class QLineEdit;
class QModelIndex;
class QPlainTextEdit;
class QSortFilterProxyModel;
class QTableView;

namespace esses::qt {
class ChannelStatusModel;
class CommandRegistry;
} // namespace esses::qt

namespace esses::widgets {

class ChannelStatusWindow final : public QWidget {
    Q_OBJECT
public:
    static constexpr const char* kDockId = "channelstatus";

    ChannelStatusWindow(qt::ChannelStatusModel& model, qt::CommandRegistry& commands,
                        QWidget* parent = nullptr);
    ~ChannelStatusWindow() override;

    QTableView& table() noexcept { return *table_; }
    QLineEdit& filterEdit() noexcept { return *filter_; }
    QCheckBox& errorsOnlyBox() noexcept { return *errorsBox_; }
    QPlainTextEdit& detailPane() noexcept { return *detail_; }
    QSortFilterProxyModel& filterModel() noexcept { return *proxy_; }
    void setErrorsOnly(bool on);
    bool errorsOnly() const noexcept { return errorsOnly_; }
    // Selects and shows the row of `key` (clearing a filter that hides it); false when it has none.
    bool focusChannel(const maths::ChannelKey& key);
    // Selects the first visible row whose Channel (and Kind, when given) matches case-insensitively.
    bool selectRow(const QString& channel, const QString& kind = {});
    // The source row of the current row, or -1.
    int currentSourceRow() const;
    // The visible rows as tab-separated text with a header line (Copy Table).
    QString tableText() const;
    // Copy Table: tableText() on the clipboard.
    void copyTable() const;
    // The action of a double-click on visible row `row` (openItemRequested and the editor command); false
    // when the row has no editor (a built-in output or a missing reference).
    bool openRow(int row);

Q_SIGNALS:
    // kind: "maths" (moduleId, item), "alias" (alias name), "mapping" (logged name).
    void openItemRequested(const QString& kind, const QString& moduleOrName, const QString& item);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void updateDetail();
    void rememberCurrent();
    void restoreCurrent();

    qt::ChannelStatusModel& model_;
    qt::CommandRegistry& commands_;
    QSortFilterProxyModel* proxy_ = nullptr;
    QTableView* table_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QCheckBox* errorsBox_ = nullptr;
    QPlainTextEdit* detail_ = nullptr;
    QString currentId_; // ChannelStatusModel::Row::id of the current row across resets
    bool errorsOnly_ = false;
};

} // namespace esses::widgets
