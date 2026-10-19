// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MessagesWindow's M4 budget members (messages_window.hpp, "M4 budget"; 07 section 7.1 row 1, D-011, D-206
// (d)-(e)). Root of the M4 budget package in src/docks/ (docs/milestones/M4.md section 3); the package also
// owns messages_window.cpp (the second link suffix, SHIFT for the alternate action, the Stats line's widget).

#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/memory_budget.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/widgets/docks/messages_window.hpp>

#include <QAction>
#include <QLabel>
#include <QMenu>
#include <QModelIndex>
#include <QSortFilterProxyModel>
#include <initializer_list>
#include <utility>

namespace esses::widgets {

bool MessagesWindow::invokeAltAction(const QModelIndex& index) {
    return runAction(index, true);
}

void MessagesWindow::showPerformance(const qt::PerformanceSnapshot& snapshot) {
    const QString text = qt::performanceDetails(snapshot);
    stats_->setText(text);
    stats_->setVisible(!text.isEmpty());
}

QString MessagesWindow::performanceText() const {
    return stats_->text();
}

QMenu* MessagesWindow::rowMenu(const QModelIndex& index) {
    delete rowMenu_;
    rowMenu_ = nullptr;
    if (!index.isValid()) {
        return nullptr;
    }
    const QModelIndex s = proxy_->mapToSource(index);
    if (!s.isValid()) {
        return nullptr;
    }
    const qt::Message& m = model_.at(s.row());
    if (m.action.isEmpty() && m.altAction.isEmpty()) {
        return nullptr;
    }
    rowMenu_ = new QMenu(this);
    rowMenu_->setObjectName(QStringLiteral("messagesRowMenu"));
    for (const auto& [id, args] :
         {std::pair{m.action, m.actionArgs}, std::pair{m.altAction, m.altActionArgs}}) {
        if (id.isEmpty()) {
            continue;
        }
        const qt::CommandSpec* spec = commands_.find(id);
        QAction* item = rowMenu_->addAction(spec != nullptr ? spec->title : id);
        item->setObjectName(id);
        item->setEnabled(commands_.isEnabled(id));
        connect(item, &QAction::triggered, this,
                [this, command = id, arguments = args] { commands_.invoke(command, arguments); });
    }
    return rowMenu_;
}

} // namespace esses::widgets
