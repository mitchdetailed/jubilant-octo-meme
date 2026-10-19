// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/data_window_model.hpp>
#include <esses/qtmodel/source_reload.hpp>
#include <esses/widgets/docks/data_window.hpp>
#include <esses/widgets/shell/command_actions.hpp>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QColor>
#include <QDialogButtonBox>
#include <QFont>
#include <QHeaderView>
#include <QIcon>
#include <QIdentityProxyModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QPushButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <memory>
#include <utility>

namespace esses::widgets {

namespace {

constexpr int kMainSlot = 100; // DataWindowModel::SlotRole value of the Main range
constexpr const char* kReloadCommand = "file.reload_source"; // qt::kReloadSourceCommand (06 section 14.4)

QColor toColor(project::Rgba c) {
    return QColor(project::red(c), project::green(c), project::blue(c), project::alpha(c));
}

// The stale badge of 06 section 6 item 1 (M4 reload): an amber disc with an exclamation mark, so that the
// shape, not the colour alone, carries it (06 section 20 item 4; the tool tip says what it means). Drawn
// once.
QIcon staleIcon() {
    static const QIcon icon = [] {
        QPixmap pm(12, 12);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xE0, 0xA0, 0x00));
        p.drawEllipse(QRectF(0.5, 0.5, 11.0, 11.0));
        p.setBrush(QColor(0x20, 0x20, 0x20));
        p.drawRect(QRectF(5.0, 2.5, 2.0, 5.0));
        p.drawRect(QRectF(5.0, 8.5, 2.0, 1.5));
        p.end();
        return QIcon(pm);
    }();
    return icon;
}

// The filter icon of 06 section 6 (a funnel in the text colour), drawn once.
QIcon filterIcon() {
    static const QIcon icon = [] {
        QPixmap pm(12, 12);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QApplication::palette().color(QPalette::Text));
        const QPolygonF funnel({QPointF(1.0, 1.5), QPointF(11.0, 1.5), QPointF(7.0, 6.0), QPointF(7.0, 11.0),
                                QPointF(5.0, 10.0), QPointF(5.0, 6.0)});
        p.drawPolygon(funnel);
        p.end();
        return QIcon(pm);
    }();
    return icon;
}

QString filterTitle(qt::FilterSpec::Kind kind) {
    using K = qt::FilterSpec::Kind;
    switch (kind) {
    case K::All:
        return QObject::tr("All");
    case K::Selected:
        return QObject::tr("Selected");
    case K::Fastest:
        return QObject::tr("Fastest");
    case K::Fastest3:
        return QObject::tr("Fastest 3");
    case K::WithinOnePercent:
        return QObject::tr("Within 1 % of Fastest");
    case K::LapNumbers:
        break;
    }
    return QObject::tr("Lap Numbers");
}

// The Qt-Core-only model carries slots and trust as roles; this proxy turns them into fonts, colours,
// alignment and tool tips (06 sections 5.3, 6 and 20).
class SlotStyleProxy final : public QIdentityProxyModel {
public:
    using QIdentityProxyModel::QIdentityProxyModel;
    std::shared_ptr<const project::Scheme> scheme;
    const qt::DataSelectionModel* selection = nullptr;
    const qt::DataWindowModel* rows = nullptr;

    // The filter of a source row (index.parent() invalid) that hides ranges, else nullopt.
    std::optional<qt::FilterSpec> sourceFilter(const QModelIndex& index) const {
        if (selection == nullptr || rows == nullptr || index.parent().isValid()) {
            return std::nullopt;
        }
        const auto src = rows->sourceAt(mapToSource(index));
        if (!src) {
            return std::nullopt;
        }
        const qt::FilterSpec f = selection->filter(*src);
        return f.kind == qt::FilterSpec::Kind::All ? std::nullopt : std::optional<qt::FilterSpec>(f);
    }

    QVariant data(const QModelIndex& index, int role) const override {
        using M = qt::DataWindowModel;
        // M4 reload: a stale source row carries the badge, before the filter icon when it has both; the stale
        // line of its tool tip is the model's.
        if (index.column() == M::NameCol && role == Qt::DecorationRole && !index.parent().isValid() &&
            QIdentityProxyModel::data(index, M::StaleRole).toBool()) {
            if (!sourceFilter(index)) {
                return staleIcon();
            }
            QPixmap both(26, 12);
            both.fill(Qt::transparent);
            QPainter p(&both);
            p.drawPixmap(0, 0, staleIcon().pixmap(12, 12));
            p.drawPixmap(14, 0, filterIcon().pixmap(12, 12));
            p.end();
            return QIcon(both);
        }
        if (index.column() == M::NameCol && (role == Qt::DecorationRole || role == Qt::ToolTipRole)) {
            // Filters (06 section 5.4): a row kept only by its slot, and a filtered source, carry the icon.
            if (QIdentityProxyModel::data(index, M::FilteredInRole).toBool()) {
                return role == Qt::DecorationRole
                           ? QVariant(filterIcon())
                           : QVariant(
                                 QObject::tr("Shown because it holds a selection slot; the filter hides it"));
            }
            if (const auto f = sourceFilter(index)) {
                if (role == Qt::DecorationRole) {
                    return filterIcon();
                }
                const QString own = QIdentityProxyModel::data(index, role).toString();
                QString text = QObject::tr("Filter: %1").arg(filterTitle(f->kind));
                if (f->kind == qt::FilterSpec::Kind::LapNumbers) {
                    text += QStringLiteral(" (%1)").arg(QString::fromStdString(f->laps));
                }
                return own.isEmpty() ? text : own + QLatin1Char('\n') + text;
            }
        }
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
    auto* styled = new SlotStyleProxy(this);
    styled->selection = &selection_;
    styled->rows = model_;
    proxy_ = styled;
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
    if (const std::unique_ptr<QMenu> menu = contextMenu(index)) {
        menu->exec(tree_->viewport()->mapToGlobal(pos));
    }
}

std::unique_ptr<QMenu> DataWindow::contextMenu(const QModelIndex& index) {
    if (!index.isValid()) {
        return nullptr;
    }
    tree_->setCurrentIndex(index);
    auto menu = std::make_unique<QMenu>(this);
    const auto shared = [this](const char* id) -> QAction* {
        return actions_.isNull() ? nullptr : actions_->action(QString::fromLatin1(id));
    };
    for (const char* id : {"data.select_main", "data.toggle_overlay0", "data.toggle_overlay"}) {
        if (QAction* a = shared(id)) {
            menu->addAction(a);
        }
    }
    if (!menu->isEmpty()) {
        menu->addSeparator();
    }
    if (const auto src = highlightedSource()) {
        menu->addMenu(filterMenu(*src, menu.get()));
        if (QAction* all = shared("data.filter_all_sources")) {
            menu->addAction(all);
        } else {
            connect(menu->addAction(tr("Apply Filter To All Sources")), &QAction::triggered, this,
                    [this] { selection_.applyFilterToAllSources(); });
        }
        menu->addSeparator();
    }
    const QModelIndex trust = index.siblingAtColumn(qt::DataWindowModel::TrustedCol);
    const QVariant checked = proxy_->data(trust, Qt::CheckStateRole);
    if (checked.isValid()) {
        QAction* trusted = menu->addAction(tr("Trusted"));
        trusted->setCheckable(true);
        trusted->setChecked(checked.toInt() == Qt::Checked);
        connect(trusted, &QAction::toggled, this, [this, trust](bool on) {
            proxy_->setData(trust, on ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
        });
        menu->addSeparator();
    }
    if (!actions_.isNull()) {
        actions_->refresh();
    }
    bool tools = false;
    for (const char* id : {"tools.lap_editor", "tools.details"}) {
        if (QAction* a = shared(id); a != nullptr && a->isEnabled()) {
            menu->addAction(a);
            tools = true;
        }
    }
    if (tools) {
        menu->addSeparator();
    }
    // Reload File (06 section 6; M4 reload): file.reload_source on this row's source, enabled when it is
    // stale.
    if (const auto src = highlightedSource();
        src && commands_.find(QString::fromLatin1(kReloadCommand)) != nullptr) {
        QAction* reload = menu->addAction(tr("Reload File"));
        reload->setObjectName(QStringLiteral("dataReloadFile"));
        reload->setEnabled(model_->isStale(*src));
        connect(reload, &QAction::triggered, this, [this, arg = qt::sourceIdArgument(*src)] {
            static_cast<void>(commands_.invoke(QString::fromLatin1(kReloadCommand), {arg}));
        });
    }
    if (QAction* a = shared("data.close_file")) {
        menu->addAction(a);
    }
    return menu;
}

QMenu* DataWindow::filterMenu(const model::SourceId& source, QWidget* parent) {
    using K = qt::FilterSpec::Kind;
    auto* menu = new QMenu(tr("Filters"), parent);
    menu->setObjectName(QStringLiteral("dataFilters"));
    auto* group = new QActionGroup(menu);
    group->setExclusive(true);
    const qt::FilterSpec current = selection_.filter(source);
    for (const K kind : {K::All, K::Selected, K::Fastest, K::Fastest3, K::WithinOnePercent, K::LapNumbers}) {
        QAction* a = menu->addAction(kind == K::LapNumbers ? tr("Lap Numbers...") : filterTitle(kind));
        a->setCheckable(true);
        a->setChecked(current.kind == kind);
        const std::string_view name = qt::filterKindName(kind);
        a->setData(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
        group->addAction(a);
        if (kind == K::LapNumbers) {
            connect(a, &QAction::triggered, this, [this, source] { editLapNumbers(source); });
        } else {
            connect(a, &QAction::triggered, this,
                    [this, source, kind] { selection_.setFilter(source, qt::FilterSpec{kind, {}}); });
        }
    }
    return menu;
}

LapFilterEditor* DataWindow::editLapNumbers(const model::SourceId& source) {
    if (!lapEditor_.isNull() && lapEditor_->source() != source) {
        lapEditor_->close();
        lapEditor_ = nullptr;
    }
    if (lapEditor_.isNull()) {
        lapEditor_ = new LapFilterEditor(selection_, source, this);
        lapEditor_->setAttribute(Qt::WA_DeleteOnClose);
    }
    lapEditor_->show();
    lapEditor_->raise();
    lapEditor_->activateWindow();
    return lapEditor_.data();
}

// ---- LapFilterEditor ------------------------------------------------------------------------------------

LapFilterEditor::LapFilterEditor(qt::DataSelectionModel& selection, model::SourceId source, QWidget* parent)
    : QDialog(parent), selection_(selection), source_(source) {
    setModal(false);
    setObjectName(QStringLiteral("lapFilterEditor"));
    setWindowTitle(tr("Lap Number Filter"));
    auto* help = new QLabel(tr("Lap numbers and ranges, for example 3, 5-7"), this);
    text_ = new QLineEdit(this);
    text_->setObjectName(QStringLiteral("lapNumbers"));
    text_->setAccessibleName(tr("Lap numbers"));
    const qt::FilterSpec current = selection_.filter(source_);
    if (current.kind == qt::FilterSpec::Kind::LapNumbers) {
        text_->setText(QString::fromStdString(current.laps));
    }
    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("lapNumbersError"));
    error_->setWordWrap(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(help);
    layout->addWidget(text_);
    layout->addWidget(error_);
    layout->addWidget(buttons);
    connect(text_, &QLineEdit::textChanged, this, [this](const QString& t) {
        error_->setText(QString::fromStdString(qt::DataSelectionModel::lapNumbersError(t.toStdString())));
    });
    // ENTER in the text presses Apply (the default button), never Cancel.
    QPushButton* applyButton = buttons->button(QDialogButtonBox::Apply);
    applyButton->setDefault(true);
    buttons->button(QDialogButtonBox::Cancel)->setAutoDefault(false);
    connect(applyButton, &QPushButton::clicked, this, [this] { static_cast<void>(apply()); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // The model refuses text that does not parse (06 section 5.4): the previous filter stays, the error
    // shows.
    connect(&selection_, &qt::DataSelectionModel::filterRejected, this,
            [this](const model::SourceId& id, const QString& message) {
                if (id == source_) {
                    rejected_ = true;
                    error_->setText(message);
                }
            });
}

LapFilterEditor::~LapFilterEditor() = default;

bool LapFilterEditor::apply() {
    rejected_ = false;
    selection_.setFilter(source_,
                         qt::FilterSpec{qt::FilterSpec::Kind::LapNumbers, text_->text().toStdString()});
    if (rejected_) {
        return false;
    }
    error_->clear();
    accept();
    return true;
}

} // namespace esses::widgets
