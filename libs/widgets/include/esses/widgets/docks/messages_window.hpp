// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MessagesWindow, dock id "messages" (06 sections 3.2 and 11; 07 section 4.1: Import Log and budget
// warnings): a list over qt::MessagesModel (newest last) with severity filter buttons (Errors, Warnings,
// Info, each with its count), a text filter (text, source and code, case-insensitive), Clear, Copy (the
// visible rows) and Save As (.log text: "<ISO time>\t<severity>\t<origin>\t<source>\t<code>\t<text>"). Rows
// read "hh:mm:ss  Origin  source: text"; a row with an action shows its command title as a link-coloured
// suffix and runs it on double-click or ENTER (invokeAction). The shell auto-shows the dock on the first
// Error of a session (MessagesModel::firstError) unless the user turned that off.
// M1 simplification: the Import Log of each file is a run of rows with that file as source (the collapsible
// per-file group arrives with the M2 importers).
#pragma once

#include <QWidget>

class QLineEdit;
class QListView;
class QModelIndex;
class QSortFilterProxyModel;
class QToolButton;

namespace esses::qt {
class CommandRegistry;
class MessagesModel;
} // namespace esses::qt

namespace esses::widgets {

class MessagesWindow final : public QWidget {
    Q_OBJECT
public:
    static constexpr const char* kDockId = "messages";
    enum class Severity : int { Info = 0, Warning = 1, Error = 2 }; // qt::Message::Severity values

    MessagesWindow(qt::MessagesModel& model, qt::CommandRegistry& commands, QWidget* parent = nullptr);
    ~MessagesWindow() override;

    QListView& list() noexcept { return *list_; }
    QSortFilterProxyModel& filterModel() noexcept { return *proxy_; }
    QLineEdit& filterEdit() noexcept { return *filter_; }
    void setSeverityShown(Severity severity, bool shown);
    bool severityShown(Severity severity) const;
    int visibleCount() const;
    // The visible rows as log text (Copy, Save As).
    QString logText() const;
    bool saveAs(const QString& path, QString* error = nullptr) const;
    // Runs the action of a row of the list (proxy index); false when it has none or it is disabled.
    bool invokeAction(const QModelIndex& index);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void updateCounts();

    qt::MessagesModel& model_;
    qt::CommandRegistry& commands_;
    QSortFilterProxyModel* proxy_ = nullptr;
    QListView* list_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QToolButton* severityButtons_[3] = {nullptr, nullptr, nullptr};
};

} // namespace esses::widgets
