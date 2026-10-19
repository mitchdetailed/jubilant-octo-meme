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
#include <QPainter>
#include <QPalette>
#include <QStyle>
#include <QStyledItemDelegate>
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

// A cell is one line of text, drawn at its place with QPainter::drawText(QPoint, ...) over the style's item
// panel: the style's CE_ItemViewItem lays every cell's text out (QTextLayout) and measures it twice, which
// was most of a repaint on a cursor move (07 section 12 "Values window refresh, 250 channels <= 2 ms"; D-217
// (k)). Selection, focus, alignment, the pending colour and the elision stay the standard item's.
class ValuesDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QWidget* widget = opt.widget;
        QStyle* style = widget != nullptr ? widget->style() : QApplication::style();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, widget);
        const bool selected = (opt.state & QStyle::State_Selected) != 0;
        const QPalette::ColorGroup group = (opt.state & QStyle::State_Enabled) == 0  ? QPalette::Disabled
                                           : (opt.state & QStyle::State_Active) != 0 ? QPalette::Normal
                                                                                     : QPalette::Inactive;
        if (!opt.text.isEmpty()) {
            const int margin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, &opt, widget) + 1;
            const QRect textRect = opt.rect.adjusted(margin, 0, -margin, 0);
            const QFontMetrics& metrics = opt.fontMetrics;
            QString shown = opt.text;
            int width = metrics.horizontalAdvance(shown);
            if (width > textRect.width()) {
                shown = metrics.elidedText(shown, opt.textElideMode, textRect.width());
                width = metrics.horizontalAdvance(shown);
            }
            const Qt::Alignment h =
                QStyle::visualAlignment(opt.direction, opt.displayAlignment) & Qt::AlignHorizontal_Mask;
            const int x = (h & Qt::AlignRight) != 0     ? textRect.right() + 1 - width
                          : (h & Qt::AlignHCenter) != 0 ? textRect.left() + (textRect.width() - width) / 2
                                                        : textRect.left();
            const int y = textRect.top() + (textRect.height() - metrics.height()) / 2 + metrics.ascent();
            painter->save();
            painter->setFont(opt.font);
            painter->setPen(opt.palette.color(group, selected ? QPalette::HighlightedText : QPalette::Text));
            painter->drawText(QPoint(x, y), shown);
            painter->restore();
        }
        if ((opt.state & QStyle::State_HasFocus) != 0) {
            QStyleOptionFocusRect focus;
            focus.QStyleOption::operator=(opt);
            focus.rect = style->subElementRect(QStyle::SE_ItemViewItemFocusRect, &opt, widget);
            focus.state |= QStyle::State_KeyboardFocusChange | QStyle::State_Item;
            focus.backgroundColor =
                opt.palette.color(group, selected ? QPalette::Highlight : QPalette::Window);
            style->drawPrimitive(QStyle::PE_FrameFocusRect, &focus, painter, widget);
        }
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
    table_->setItemDelegate(new ValuesDelegate(table_));
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(qt::ValuesModel::NameCol, QHeaderView::Stretch);
    // The value columns fit what is on screen, not all 250 rows: a cursor move then reads only the visible
    // rows (ValuesModel computes a row when it is asked for; 07 section 12, D-217).
    table_->horizontalHeader()->setResizeContentsPrecision(0);
    table_->setWordWrap(false); // one line a value, as the Values List component
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
