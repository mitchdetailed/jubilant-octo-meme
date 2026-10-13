// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ValuesWindow, dock id "values", V toggles (06 sections 3.2 and 8, D-109 values rule; AC-M1-01: a cursor
// drag updates the rows for Main and Overlay 1): a QTableView over qt::ValuesModel (Name, Main, reference
// overlay, Delta, Unit). Rows are the focused component's channelNames() (the last focused component's list
// stays when none is focused) or every channel of the Main source with the "All channels" toggle (persisted
// per user by the shell in layout.json docks.values.allChannels). The values follow the active worksheet's
// view (setWorksheet: domain, cursor; the shell calls it on every worksheet switch). Pending cells ("…") are
// greyed. Context menu: Copy Value, Copy Row ("name<TAB>value<TAB>unit"); the property and maths entries
// arrive with their dialogs.
#pragma once

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QIdentityProxyModel;
class QTableView;

namespace esses::qt {
class ValuesModel;
class WorksheetViewModel;
} // namespace esses::qt

namespace esses::widgets {

class ComponentWidget;

class ValuesWindow final : public QWidget {
    Q_OBJECT
public:
    static constexpr const char* kDockId = "values";

    explicit ValuesWindow(qt::ValuesModel& model, QWidget* parent = nullptr);
    ~ValuesWindow() override;

    QTableView& table() noexcept { return *table_; }
    qt::ValuesModel& model() noexcept { return model_; }
    QCheckBox& allChannelsToggle() noexcept { return *allChannels_; }
    bool allChannels() const;
    void setAllChannels(bool on);
    // The worksheet whose view (domain, cursor) the values follow; null = none (empty value cells).
    void setWorksheet(qt::WorksheetViewModel* worksheet);
    qt::WorksheetViewModel* worksheet() const;
    // The component whose channels are listed (the last focused one), or null.
    ComponentWidget* followedComponent() const;
    // Copy Row text of a row: "name<TAB>value<TAB>unit".
    QString rowText(int row) const;

public Q_SLOTS:
    void followComponent(esses::widgets::ComponentWidget* component); // focused component changed
    // Re-reads the followed component's channel list (after a channel was added to it).
    void refreshChannels();

Q_SIGNALS:
    void allChannelsChanged(bool on);

private:
    void showContextMenu(const QPoint& pos);

    qt::ValuesModel& model_;
    QIdentityProxyModel* proxy_ = nullptr;
    QTableView* table_ = nullptr;
    QCheckBox* allChannels_ = nullptr;
    QPointer<qt::WorksheetViewModel> worksheet_;
    QPointer<ComponentWidget> followed_;
};

} // namespace esses::widgets
