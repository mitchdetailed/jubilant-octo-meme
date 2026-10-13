// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/qtmodel/values_model.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/docks/values_window.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QHeaderView>
#include <QIdentityProxyModel>
#include <QMenu>
#include <QPalette>
#include <QTableView>
#include <QVBoxLayout>

namespace esses::widgets {

namespace {

const QString kPending = QStringLiteral("…");

// Alignment and the pending colour of the Qt-Core-only ValuesModel.
class ValuesStyleProxy final : public QIdentityProxyModel {
public:
    using QIdentityProxyModel::QIdentityProxyModel;

    QVariant data(const QModelIndex& index, int role) const override {
        using M = qt::ValuesModel;
        if (role == Qt::TextAlignmentRole) {
            if (index.column() == M::MainCol || index.column() == M::ReferenceCol ||
                index.column() == M::DeltaCol) {
                return QVariant((Qt::AlignRight | Qt::AlignVCenter).toInt());
            }
        } else if (role == Qt::ForegroundRole) {
            if (QIdentityProxyModel::data(index, Qt::DisplayRole).toString() == kPending) {
                return QApplication::palette().color(QPalette::Disabled, QPalette::Text);
            }
        }
        return QIdentityProxyModel::data(index, role);
    }
};

} // namespace

ValuesWindow::ValuesWindow(qt::ValuesModel& model, QWidget* parent) : QWidget(parent), model_(model) {
    allChannels_ = new QCheckBox(tr("All channels"), this);
    allChannels_->setObjectName(QStringLiteral("valuesAllChannels"));
    allChannels_->setChecked(model_.allChannels());
    proxy_ = new ValuesStyleProxy(this);
    proxy_->setSourceModel(&model_);
    table_ = new QTableView(this);
    table_->setObjectName(QStringLiteral("valuesTable"));
    table_->setAccessibleName(tr("Values at the cursor"));
    table_->setModel(proxy_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(qt::ValuesModel::NameCol, QHeaderView::Stretch);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(allChannels_);
    layout->addWidget(table_);
    setFocusProxy(table_);
    connect(allChannels_, &QCheckBox::toggled, this, [this](bool on) {
        if (model_.allChannels() != on) {
            model_.setAllChannels(on);
            Q_EMIT allChannelsChanged(on);
        }
    });
    connect(table_, &QWidget::customContextMenuRequested, this, &ValuesWindow::showContextMenu);
}

ValuesWindow::~ValuesWindow() = default;

bool ValuesWindow::allChannels() const {
    return model_.allChannels();
}

void ValuesWindow::setAllChannels(bool on) {
    allChannels_->setChecked(on); // toggled() updates the model and announces the change
    if (model_.allChannels() != on) {
        model_.setAllChannels(on);
    }
}

void ValuesWindow::setWorksheet(qt::WorksheetViewModel* worksheet) {
    worksheet_ = worksheet;
    model_.setWorksheet(worksheet);
}

qt::WorksheetViewModel* ValuesWindow::worksheet() const {
    return worksheet_.data();
}

ComponentWidget* ValuesWindow::followedComponent() const {
    return followed_.data();
}

void ValuesWindow::followComponent(ComponentWidget* component) {
    if (component == nullptr) {
        return; // the last focused component's list stays (06 section 8)
    }
    followed_ = component;
    model_.setChannels(component->channelNames());
}

void ValuesWindow::refreshChannels() {
    if (!followed_.isNull()) {
        model_.setChannels(followed_->channelNames());
    }
}

QString ValuesWindow::rowText(int row) const {
    using M = qt::ValuesModel;
    return model_.cellText(row, M::NameCol) + QLatin1Char('\t') + model_.cellText(row, M::MainCol) +
           QLatin1Char('\t') + model_.cellText(row, M::UnitCol);
}

void ValuesWindow::showContextMenu(const QPoint& pos) {
    const QModelIndex index = table_->indexAt(pos);
    if (!index.isValid()) {
        return;
    }
    const int row = index.row();
    QMenu menu(this);
    QAction* copyValue = menu.addAction(tr("Copy Value"));
    QAction* copyRow = menu.addAction(tr("Copy Row"));
    connect(copyValue, &QAction::triggered, this, [this, index] {
        QApplication::clipboard()->setText(proxy_->data(index, Qt::DisplayRole).toString());
    });
    connect(copyRow, &QAction::triggered, this,
            [this, row] { QApplication::clipboard()->setText(rowText(row)); });
    menu.exec(table_->viewport()->mapToGlobal(pos));
}

} // namespace esses::widgets
