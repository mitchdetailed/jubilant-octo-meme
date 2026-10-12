// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/data_window_model.hpp>
#include <esses/widgets/docks/data_window.hpp>
#include <esses/widgets/shell/command_actions.hpp>

#include <QAction>
#include <QColor>
#include <QFont>
#include <QHeaderView>
#include <QIdentityProxyModel>
#include <QMenu>
#include <QTreeView>
#include <QVBoxLayout>
#include <utility>

namespace esses::widgets {

namespace {

constexpr int kMainSlot = 100; // DataWindowModel::SlotRole value of the Main range

QColor toColor(project::Rgba c) {
    return QColor(project::red(c), project::green(c), project::blue(c), project::alpha(c));
}

// The Qt-Core-only model carries slots and trust as roles; this proxy turns them into fonts, colours,
// alignment and tool tips (06 sections 5.3, 6 and 20).
class SlotStyleProxy final : public QIdentityProxyModel {
public:
    using QIdentityProxyModel::QIdentityProxyModel;
    std::shared_ptr<const project::Scheme> scheme;

    QVariant data(const QModelIndex& index, int role) const override {
        using M = qt::DataWindowModel;
        switch (role) {
        case Qt::FontRole: {
            if (QIdentityProxyModel::data(index, M::UntrustedRole).toBool()) {
                QFont f;
                f.setItalic(true);
                return f;
            }
            return QIdentityProxyModel::data(index, role);
        }
        case Qt::ForegroundRole: {
            const int slot = QIdentityProxyModel::data(index, M::SlotRole).toInt();
            const bool untrusted = QIdentityProxyModel::data(index, M::UntrustedRole).toBool();
            switch (index.column()) {
            case M::MainCol:
                if (slot == kMainSlot) {
                    return toColor(scheme ? scheme->role("cursorMain", 0xE01010FF) : 0xE01010FF);
                }
                break;
            case M::Overlay0Col:
                if (slot == 0) {
                    return toColor(scheme ? scheme->role("overlay0", 0x808080FF) : 0x808080FF);
                }
                break;
            case M::OverlayCol:
                if (slot > 0 && slot != kMainSlot && scheme) {
                    return toColor(scheme->overlayColour(slot));
                }
                break;
            default:
                if (untrusted) {
                    return toColor(scheme ? scheme->role("dataForeground2", 0x808080FF) : 0x808080FF);
                }
                break;
            }
            return QIdentityProxyModel::data(index, role);
        }
        case Qt::TextAlignmentRole:
            switch (index.column()) {
            case M::MainCol:
            case M::Overlay0Col:
            case M::OverlayCol:
            case M::LocalMathsCol:
                return QVariant(static_cast<int>(Qt::AlignCenter));
            case M::TimeCol:
            case M::DeltaFastestCol:
            case M::OffsetCol:
                return QVariant((Qt::AlignRight | Qt::AlignVCenter).toInt());
            default:
                return QIdentityProxyModel::data(index, role);
            }
        case Qt::ToolTipRole: {
            const QVariant own = QIdentityProxyModel::data(index, role);
            if (own.isValid()) {
                return own;
            }
            const QVariant slotValue = QIdentityProxyModel::data(index, M::SlotRole);
            if (!slotValue.isValid()) {
                return {};
            }
            const int slot = slotValue.toInt();
            QString text = slot == kMainSlot ? QStringLiteral("Main")
                           : slot == 0       ? QStringLiteral("Overlay 0")
                           : slot > 0        ? QStringLiteral("Overlay %1").arg(slot)
                                             : QString();
            if (QIdentityProxyModel::data(index, M::UntrustedRole).toBool()) {
                text += text.isEmpty() ? QStringLiteral("Untrusted") : QStringLiteral(", untrusted");
            }
            return text.isEmpty() ? QVariant() : QVariant(text);
        }
        default:
            return QIdentityProxyModel::data(index, role);
        }
    }
};

} // namespace

DataWindow::DataWindow(qt::DataSelectionModel& selection, qt::CommandRegistry& commands, QWidget* parent)
    : QWidget(parent), selection_(selection), commands_(commands) {
    model_ = new qt::DataWindowModel(selection_, this);
    proxy_ = new SlotStyleProxy(this);
    proxy_->setSourceModel(model_);
    tree_ = new QTreeView(this);
    tree_->setObjectName(QStringLiteral("dataTree"));
    tree_->setAccessibleName(tr("Data window ranges"));
    tree_->setModel(proxy_);
    tree_->setTreePosition(qt::DataWindowModel::NameCol);
    tree_->setUniformRowHeights(true);
    tree_->setAllColumnsShowFocus(true);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setExpandsOnDoubleClick(false);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    QHeaderView* header = tree_->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::ResizeToContents);
    header->setSectionResizeMode(qt::DataWindowModel::NameCol, QHeaderView::Stretch);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tree_);
    setFocusProxy(tree_);

    closeSource_ = [this](const model::SourceId& id) { selection_.closeSource(id); };
    connect(tree_, &QTreeView::clicked, this, &DataWindow::onClicked);
    connect(tree_, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        if (index.column() > qt::DataWindowModel::OverlayCol &&
            index.column() != qt::DataWindowModel::TrustedCol) {
            if (const auto r = model_->rangeAt(proxy_->mapToSource(index))) {
                selection_.setMain(*r);
            }
        }
    });
    connect(tree_, &QWidget::customContextMenuRequested, this, &DataWindow::showContextMenu);
    connect(model_, &QAbstractItemModel::modelReset, this, &DataWindow::onModelReset);
    connect(&selection_, &qt::DataSelectionModel::changed, this,
            [this](const plot::SelectionSnapshot&, qt::DataSelectionModel::Changes what) {
                if (what.testFlag(qt::DataSelectionModel::Change::Main)) {
                    followMain();
                }
            });
    registerCommands();
    onModelReset();
}

DataWindow::~DataWindow() = default;

void DataWindow::setScheme(std::shared_ptr<const project::Scheme> scheme) {
    static_cast<SlotStyleProxy*>(proxy_)->scheme = std::move(scheme);
    tree_->viewport()->update();
}

void DataWindow::setActions(CommandActions* actions) {
    actions_ = actions;
}

void DataWindow::setCloseSource(std::function<void(const model::SourceId&)> close) {
    if (close) {
        closeSource_ = std::move(close);
    }
}

void DataWindow::registerCommands() {
    const QPointer<DataWindow> self(this);
    const auto add = [&](const char* id, const char* title, QStringList keys,
                         std::function<void(DataWindow&)> run,
                         std::function<bool(const DataWindow&)> enabled) {
        qt::CommandSpec s;
        s.id = QString::fromLatin1(id);
        s.title = QString::fromLatin1(title);
        s.context = qt::CommandContext::DataWindow;
        s.defaultKeys = std::move(keys);
        s.invoke = [self, run = std::move(run)](const QVariantList&) {
            if (!self.isNull()) {
                run(*self);
            }
        };
        s.enabled = [self, enabled = std::move(enabled)] { return !self.isNull() && enabled(*self); };
        commands_.add(std::move(s));
    };
    const auto hasRange = [](const DataWindow& w) { return w.highlightedRange().has_value(); };
    add(
        "data.select_main", "Set Main", {QStringLiteral("Return"), QStringLiteral("Enter")},
        [](DataWindow& w) {
            if (const auto r = w.highlightedRange()) {
                w.selection_.setMain(*r);
            }
        },
        hasRange);
    add(
        "data.toggle_overlay0", "Toggle Overlay 0",
        {QStringLiteral("Ctrl+Return"), QStringLiteral("Ctrl+Enter")},
        [](DataWindow& w) {
            if (const auto r = w.highlightedRange()) {
                w.selection_.toggleOverlay0(*r);
            }
        },
        hasRange);
    add(
        "data.toggle_overlay", "Toggle Overlay", {QStringLiteral("Alt+Return"), QStringLiteral("Alt+Enter")},
        [](DataWindow& w) {
            if (const auto r = w.highlightedRange()) {
                w.selection_.toggleOverlay(*r);
            }
        },
        hasRange);
    // DELETE on a source row closes it with no prompt; on a range row it is a no-op (06 section 6).
    add(
        "data.close_file", "Close File", {QStringLiteral("Delete")},
        [](DataWindow& w) {
            if (w.highlightedIsSource()) {
                if (const auto s = w.highlightedSource()) {
                    w.closeSource_(*s);
                }
            }
        },
        [](const DataWindow& w) { return w.highlightedIsSource(); });
}

std::optional<model::RangeRef> DataWindow::highlightedRange() const {
    const QModelIndex current = tree_->currentIndex();
    if (!current.isValid()) {
        return std::nullopt;
    }
    return model_->rangeAt(proxy_->mapToSource(current));
}

std::optional<model::SourceId> DataWindow::highlightedSource() const {
    const QModelIndex current = tree_->currentIndex();
    if (!current.isValid()) {
        return std::nullopt;
    }
    return model_->sourceAt(proxy_->mapToSource(current));
}

bool DataWindow::highlightedIsSource() const {
    const QModelIndex current = tree_->currentIndex();
    return current.isValid() && !current.parent().isValid() && highlightedSource().has_value();
}

void DataWindow::highlight(const model::RangeRef& range) {
    // Column M: scrolling to it keeps the slot columns in view in a narrow dock (rows are selected whole).
    const QModelIndex index =
        proxy_->mapFromSource(model_->indexOf(range)).siblingAtColumn(qt::DataWindowModel::MainCol);
    if (!index.isValid()) {
        return;
    }
    tree_->setCurrentIndex(index);
    tree_->scrollTo(index);
}

void DataWindow::followMain() {
    if (selection_.hasMain()) {
        highlight(selection_.snapshot().main);
    }
}

void DataWindow::onModelReset() {
    tree_->expandAll();
    followMain();
}

void DataWindow::onClicked(const QModelIndex& index) {
    const auto r = model_->rangeAt(proxy_->mapToSource(index));
    if (!r) {
        return;
    }
    switch (index.column()) {
    case qt::DataWindowModel::MainCol:
        selection_.setMain(*r);
        break;
    case qt::DataWindowModel::Overlay0Col:
        selection_.toggleOverlay0(*r);
        break;
    case qt::DataWindowModel::OverlayCol:
        selection_.toggleOverlay(*r);
        break;
    default:
        break;
    }
}

void DataWindow::showContextMenu(const QPoint& pos) {
    const QModelIndex index = tree_->indexAt(pos);
    if (!index.isValid() || actions_.isNull()) {
        return;
    }
    tree_->setCurrentIndex(index);
    QMenu menu(this);
    for (const char* id : {"data.select_main", "data.toggle_overlay0", "data.toggle_overlay"}) {
        if (QAction* a = actions_->action(QString::fromLatin1(id))) {
            menu.addAction(a);
        }
    }
    menu.addSeparator();
    const QModelIndex trust = index.siblingAtColumn(qt::DataWindowModel::TrustedCol);
    const QVariant checked = proxy_->data(trust, Qt::CheckStateRole);
    if (checked.isValid()) {
        QAction* trusted = menu.addAction(tr("Trusted"));
        trusted->setCheckable(true);
        trusted->setChecked(checked.toInt() == Qt::Checked);
        connect(trusted, &QAction::toggled, this, [this, trust](bool on) {
            proxy_->setData(trust, on ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
        });
        menu.addSeparator();
    }
    if (QAction* a = actions_->action(QStringLiteral("data.close_file"))) {
        menu.addAction(a);
    }
    actions_->refresh();
    menu.exec(tree_->viewport()->mapToGlobal(pos));
}

} // namespace esses::widgets
