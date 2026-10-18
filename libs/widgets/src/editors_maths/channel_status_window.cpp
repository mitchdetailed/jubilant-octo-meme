// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelStatusWindow (06 section 10, AC-M2-12): the filtered, sortable Channel Status table, the trace
// detail pane, Copy Table and the double-click routing to the editors.

#include <esses/qtmodel/channel_status_model.hpp>
#include <esses/qtmodel/channels_model.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/widgets/editors_maths/channel_status_window.hpp>

#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStringList>
#include <QTableView>
#include <QVBoxLayout>
#include <QtGlobal>

namespace esses::widgets {

namespace {

using StatusModel = qt::ChannelStatusModel;

// The filter box and the "errors only" toggle over the Channel Status rows, with the status styling.
class StatusProxy final : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void setFilter(const QString& query, bool errorsOnly) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        // invalidateRowsFilter() is deprecated for Qt 6.13 (and warned about from 6.12).
        beginFilterChange();
        query_ = query.trimmed();
        errorsOnly_ = errorsOnly;
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
        query_ = query.trimmed();
        errorsOnly_ = errorsOnly;
        invalidateRowsFilter();
#endif
    }

    bool filterAcceptsRow(int row, const QModelIndex& parent) const override {
        const QAbstractItemModel* m = sourceModel();
        const QModelIndex first = m->index(row, 0, parent);
        if (errorsOnly_ && !first.data(StatusModel::ErrorRole).toBool()) {
            return false;
        }
        if (query_.isEmpty()) {
            return true;
        }
        const QString channel = m->index(row, StatusModel::ChannelColumn, parent).data().toString();
        if (qt::ChannelsModel::filterAccepts(channel, query_)) {
            return true;
        }
        for (const int col : {StatusModel::SourceColumn, StatusModel::KindColumn, StatusModel::StatusColumn,
                              StatusModel::ResolvedColumn}) {
            if (m->index(row, col, parent).data().toString().contains(query_, Qt::CaseInsensitive)) {
                return true;
            }
        }
        return false;
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (role == Qt::ForegroundRole || role == Qt::FontRole) {
            const QModelIndex statusIndex = index.siblingAtColumn(StatusModel::StatusColumn);
            const QString status = QSortFilterProxyModel::data(statusIndex, Qt::DisplayRole).toString();
            if (role == Qt::ForegroundRole) {
                if (QSortFilterProxyModel::data(index, StatusModel::ErrorRole).toBool()) {
                    return QColor(0xD3, 0x2F, 0x2F);
                }
                if (status.startsWith(QStringLiteral("Warning"))) {
                    return QColor(0xC7, 0x7C, 0x02);
                }
                if (status == QStringLiteral("(missing)")) {
                    return QColor(0x80, 0x80, 0x80);
                }
            } else if (status == QStringLiteral("Pending") ||
                       status.startsWith(QStringLiteral("Calculating"))) {
                QFont f;
                f.setItalic(true);
                return f;
            }
        }
        return QSortFilterProxyModel::data(index, role);
    }

protected:
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override {
        if (left.column() == StatusModel::RateColumn) {
            // "50 Hz" sorts by its number; empty cells first.
            const auto hz = [](const QModelIndex& i) {
                const QString t = i.data().toString().section(QLatin1Char(' '), 0, 0);
                bool ok = false;
                const double v = t.toDouble(&ok);
                return ok ? v : -1.0;
            };
            return hz(left) < hz(right);
        }
        return QString::localeAwareCompare(left.data().toString(), right.data().toString()) < 0;
    }

private:
    QString query_;
    bool errorsOnly_ = false;
};

// The logged name of a mapping row: the first quoted name of "Resolved to" ('TPS' -> mapping -> 'Throttle').
QString firstQuoted(const QString& text) {
    const auto open = text.indexOf(QLatin1Char('\''));
    const auto close = open < 0 ? -1 : text.indexOf(QLatin1Char('\''), open + 1);
    return close > open ? text.mid(open + 1, close - open - 1) : QString();
}

QString flat(QString cell) {
    cell.replace(QLatin1Char('\t'), QLatin1Char(' '));
    cell.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return cell;
}

} // namespace

ChannelStatusWindow::ChannelStatusWindow(qt::ChannelStatusModel& model, qt::CommandRegistry& commands,
                                         QWidget* parent)
    : QWidget(parent), model_(model), commands_(commands) {
    setAccessibleName(tr("Channel Status"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    auto* bar = new QHBoxLayout;
    filter_ = new QLineEdit(this);
    filter_->setPlaceholderText(tr("Filter"));
    filter_->setClearButtonEnabled(true);
    filter_->setAccessibleName(tr("Channel Status filter"));
    errorsBox_ = new QCheckBox(tr("Errors only"), this);
    auto* copy = new QPushButton(tr("Copy Table"), this);
    copy->setAutoDefault(false);
    bar->addWidget(filter_, 1);
    bar->addWidget(errorsBox_);
    bar->addWidget(copy);
    layout->addLayout(bar);

    auto* split = new QSplitter(Qt::Vertical, this);
    layout->addWidget(split, 1);
    auto* proxy = new StatusProxy(this);
    proxy->setSourceModel(&model_);
    proxy_ = proxy;
    table_ = new QTableView(split);
    table_->setModel(proxy_);
    table_->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder); // model order until the user sorts
    table_->setSortingEnabled(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setWordWrap(false);
    table_->setAccessibleName(tr("Channel Status table"));
    detail_ = new QPlainTextEdit(split);
    detail_->setReadOnly(true);
    detail_->setAccessibleName(tr("Resolution trace"));
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 1);

    connect(filter_, &QLineEdit::textChanged, this, [this, proxy](const QString& text) {
        rememberCurrent();
        proxy->setFilter(text, errorsOnly_);
        restoreCurrent();
    });
    connect(errorsBox_, &QCheckBox::toggled, this, [this](bool on) { setErrorsOnly(on); });
    connect(copy, &QPushButton::clicked, this, [this] { copyTable(); });
    connect(table_, &QTableView::doubleClicked, this,
            [this](const QModelIndex& index) { static_cast<void>(openRow(index.row())); });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this] {
        rememberCurrent();
        updateDetail();
    });
    connect(&model_, &QAbstractItemModel::modelAboutToBeReset, this, [this] { rememberCurrent(); });
    connect(&model_, &QAbstractItemModel::modelReset, this, [this] { restoreCurrent(); });
    connect(&model_, &QAbstractItemModel::dataChanged, this, [this] { updateDetail(); });
    table_->installEventFilter(this);
    updateDetail();
}

ChannelStatusWindow::~ChannelStatusWindow() {
    // ~QWidget deletes the table after this: its selection signals must not reach the half-destroyed window.
    for (QObject* child : findChildren<QObject*>()) {
        child->disconnect(this);
    }
}

void ChannelStatusWindow::setErrorsOnly(bool on) {
    errorsOnly_ = on;
    if (errorsBox_->isChecked() != on) {
        const QSignalBlocker block(errorsBox_);
        errorsBox_->setChecked(on);
    }
    rememberCurrent();
    static_cast<StatusProxy*>(proxy_)->setFilter(filter_->text(), on);
    restoreCurrent();
}

int ChannelStatusWindow::currentSourceRow() const {
    const QModelIndex current = table_->currentIndex();
    return current.isValid() ? proxy_->mapToSource(current).row() : -1;
}

bool ChannelStatusWindow::focusChannel(const maths::ChannelKey& key) {
    int row = model_.rowOf(key);
    if (row < 0) {
        model_.refresh(); // a status change may still be coalescing
        row = model_.rowOf(key);
    }
    if (row < 0) {
        return false;
    }
    QModelIndex index = proxy_->mapFromSource(model_.index(row, StatusModel::ChannelColumn));
    if (!index.isValid()) {
        filter_->clear();
        setErrorsOnly(false);
        index = proxy_->mapFromSource(model_.index(row, StatusModel::ChannelColumn));
    }
    if (!index.isValid()) {
        return false;
    }
    table_->setCurrentIndex(index);
    table_->selectRow(index.row());
    table_->scrollTo(index);
    table_->setFocus(Qt::OtherFocusReason);
    updateDetail();
    return true;
}

bool ChannelStatusWindow::selectRow(const QString& channel, const QString& kind) {
    for (int r = 0; r < proxy_->rowCount(); ++r) {
        const QModelIndex c = proxy_->index(r, StatusModel::ChannelColumn);
        const QString k = proxy_->index(r, StatusModel::KindColumn).data().toString();
        if (c.data().toString().compare(channel, Qt::CaseInsensitive) == 0 &&
            (kind.isEmpty() || k.compare(kind, Qt::CaseInsensitive) == 0)) {
            table_->setCurrentIndex(c);
            table_->selectRow(r);
            table_->scrollTo(c);
            updateDetail();
            return true;
        }
    }
    return false;
}

QString ChannelStatusWindow::tableText() const {
    QStringList lines;
    QStringList header;
    for (int c = 0; c < proxy_->columnCount(); ++c) {
        header << flat(proxy_->headerData(c, Qt::Horizontal).toString());
    }
    lines << header.join(QLatin1Char('\t'));
    for (int r = 0; r < proxy_->rowCount(); ++r) {
        QStringList cells;
        for (int c = 0; c < proxy_->columnCount(); ++c) {
            cells << flat(proxy_->index(r, c).data().toString());
        }
        lines << cells.join(QLatin1Char('\t'));
    }
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

void ChannelStatusWindow::copyTable() const {
    if (QClipboard* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(tableText());
    }
}

bool ChannelStatusWindow::openRow(int row) {
    const QModelIndex index = proxy_->mapToSource(proxy_->index(row, 0));
    if (!index.isValid() || index.row() >= static_cast<int>(model_.rows().size())) {
        return false;
    }
    const StatusModel::Row& r = model_.rows()[static_cast<std::size_t>(index.row())];
    const auto invoke = [this](const QString& id, const QVariantList& args) {
        const qt::CommandSpec* spec = commands_.find(id);
        if (spec != nullptr && spec->invoke && commands_.isEnabled(id)) {
            static_cast<void>(commands_.invoke(id, args));
        }
    };
    if (r.kind == QStringLiteral("Alias")) {
        Q_EMIT openItemRequested(QStringLiteral("alias"), r.channel, QString());
        invoke(QStringLiteral("tools.aliases"), {r.channel});
        return true;
    }
    if (r.kind == QStringLiteral("Mapping")) {
        const QString logged = firstQuoted(r.resolved);
        Q_EMIT openItemRequested(QStringLiteral("mapping"), logged, QString());
        invoke(QStringLiteral("tools.mappings"), {logged});
        return true;
    }
    if (r.kind == QStringLiteral("Reference") || r.kind == QStringLiteral("Built-in") || r.module.isEmpty()) {
        return false; // a missing reference or a built-in output has no maths item to open
    }
    Q_EMIT openItemRequested(QStringLiteral("maths"), r.module, r.channel);
    invoke(QStringLiteral("tools.maths_editor"), {r.module, r.channel});
    return true;
}

bool ChannelStatusWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == table_ && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            key->modifiers() == Qt::NoModifier && table_->currentIndex().isValid()) {
            static_cast<void>(openRow(table_->currentIndex().row()));
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ChannelStatusWindow::rememberCurrent() {
    const int row = currentSourceRow();
    if (row >= 0 && row < static_cast<int>(model_.rows().size())) {
        currentId_ = model_.rows()[static_cast<std::size_t>(row)].id;
    }
}

void ChannelStatusWindow::restoreCurrent() {
    if (!currentId_.isEmpty()) {
        const auto& rows = model_.rows();
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].id != currentId_) {
                continue;
            }
            const QModelIndex index = proxy_->mapFromSource(model_.index(static_cast<int>(i), 0));
            if (index.isValid()) {
                const QSignalBlocker block(table_->selectionModel());
                table_->setCurrentIndex(index);
                table_->selectRow(index.row());
            }
            break;
        }
    }
    updateDetail();
}

void ChannelStatusWindow::updateDetail() {
    const int row = currentSourceRow();
    if (row < 0 || row >= static_cast<int>(model_.rows().size())) {
        detail_->setPlainText(model_.rows().empty() ? tr("No maths items, aliases or mappings.") : QString());
        return;
    }
    const StatusModel::Row& r = model_.rows()[static_cast<std::size_t>(row)];
    QStringList lines;
    lines << tr("%1 (%2, %3)").arg(r.channel, r.kind, r.source);
    if (!r.module.isEmpty()) {
        lines << tr("Module: %1").arg(r.module);
    }
    lines << tr("Status: %1").arg(r.status);
    lines << tr("Resolved to: %1").arg(r.resolved);
    if (!r.rate.isEmpty() || !r.unit.isEmpty()) {
        lines << tr("Rate: %1   Unit: %2").arg(r.rate, r.unit);
    }
    if (!r.trace.isEmpty()) {
        lines << QString() << tr("Resolution:") << r.trace;
    }
    detail_->setPlainText(lines.join(QLatin1Char('\n')));
}

} // namespace esses::widgets
