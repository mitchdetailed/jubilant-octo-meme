// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SelectWorksheetDialog, UnitsListDialog and OpenLogDialog (06 sections 2.2, 4.1, 14.4). Owner:
// src/properties_shell/.

#include <esses/formats/registry.hpp>
#include <esses/model/detail.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/units/unit_registry.hpp>
#include <esses/widgets/properties_shell/shell_dialogs.hpp>

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace esses::widgets {

namespace {

constexpr int kBookRole = Qt::UserRole;
constexpr int kSheetRole = Qt::UserRole + 1;

} // namespace

// ---- SelectWorksheetDialog --------------------------------------------------------------------------------

SelectWorksheetDialog::SelectWorksheetDialog(const qt::ProjectSession& session, int activeWorkbook,
                                             int activeWorksheet, QWidget* parent)
    : QDialog(parent), session_(session), activeWorkbook_(activeWorkbook), activeWorksheet_(activeWorksheet) {
    setObjectName(QStringLiteral("selectWorksheetDialog"));
    setWindowTitle(tr("Select Worksheet"));
    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("filter"));
    filter_->setPlaceholderText(tr("Type to filter"));
    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("worksheets"));
    tree_->setHeaderHidden(true);
    tree_->setFocusPolicy(Qt::NoFocus);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* column = new QVBoxLayout(this);
    column->addWidget(filter_);
    column->addWidget(tree_, 1);
    column->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { choose(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(filter_, &QLineEdit::textChanged, this, [this] { rebuild(); });
    connect(tree_, &QTreeWidget::itemActivated, this, [this] { choose(); });
    filter_->installEventFilter(this);
    resize(360, 420);
    rebuild();
}

SelectWorksheetDialog::~SelectWorksheetDialog() = default;

void SelectWorksheetDialog::setFilter(const QString& text) {
    filter_->setText(text);
}

void SelectWorksheetDialog::rebuild() {
    tree_->clear();
    const QString f = filter_->text().trimmed();
    const auto& books = session_.workbooks();
    QTreeWidgetItem* preselect = nullptr;
    QTreeWidgetItem* first = nullptr;
    for (std::size_t b = 0; b < books.size(); ++b) {
        const QString bookName = QString::fromStdString(books[b].name);
        QTreeWidgetItem* bookItem = nullptr;
        for (std::size_t s = 0; s < books[b].worksheets.size(); ++s) {
            const QString sheetName = QString::fromStdString(books[b].worksheets[s].name);
            if (!f.isEmpty() && !sheetName.contains(f, Qt::CaseInsensitive) &&
                !bookName.contains(f, Qt::CaseInsensitive)) {
                continue;
            }
            if (bookItem == nullptr) {
                bookItem = new QTreeWidgetItem(tree_, {bookName});
                bookItem->setFlags(Qt::ItemIsEnabled);
                bookItem->setExpanded(true);
            }
            auto* item = new QTreeWidgetItem(bookItem, {sheetName});
            item->setData(0, kBookRole, static_cast<int>(b));
            item->setData(0, kSheetRole, static_cast<int>(s));
            if (first == nullptr) {
                first = item;
            }
            if (static_cast<int>(b) == activeWorkbook_ && static_cast<int>(s) == activeWorksheet_) {
                preselect = item;
            }
        }
    }
    tree_->expandAll();
    tree_->setCurrentItem(preselect != nullptr ? preselect : first);
}

std::pair<int, int> SelectWorksheetDialog::current() const {
    const QTreeWidgetItem* item = tree_->currentItem();
    if (item == nullptr || item->parent() == nullptr) {
        return {-1, -1};
    }
    return {item->data(0, kBookRole).toInt(), item->data(0, kSheetRole).toInt()};
}

bool SelectWorksheetDialog::choose() {
    const auto [b, s] = current();
    if (b < 0 || s < 0) {
        return false;
    }
    accept();
    Q_EMIT chosen(b, s);
    return true;
}

bool SelectWorksheetDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == filter_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) {
            // Move among the worksheet rows (workbook rows are headings).
            QTreeWidgetItem* item = tree_->currentItem();
            QTreeWidgetItem* next = item;
            do {
                next = next == nullptr
                           ? nullptr
                           : (key->key() == Qt::Key_Up ? tree_->itemAbove(next) : tree_->itemBelow(next));
            } while (next != nullptr && next->parent() == nullptr);
            if (next != nullptr) {
                tree_->setCurrentItem(next);
            }
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            choose();
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

// ---- UnitsListDialog --------------------------------------------------------------------------------------

UnitsListDialog::UnitsListDialog(std::shared_ptr<const units::UnitRegistry> units, QWidget* parent)
    : QDialog(parent), units_(std::move(units)) {
    setObjectName(QStringLiteral("unitsListDialog"));
    setWindowTitle(tr("Units"));
    setModal(false);
    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("filter"));
    filter_->setPlaceholderText(tr("Filter by quantity, symbol or alias"));
    table_ = new QTableWidget(this);
    table_->setObjectName(QStringLiteral("units"));
    table_->setColumnCount(5);
    table_->setHorizontalHeaderLabels(
        {tr("Quantity"), tr("Symbol"), tr("Shown as"), tr("Scale to base"), tr("Aliases")});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    if (units_ != nullptr) {
        const auto list = units_->units();
        table_->setRowCount(static_cast<int>(list.size()));
        int row = 0;
        for (const units::Unit& u : list) {
            QStringList aliases;
            for (const std::string& a : u.aliases) {
                aliases << QString::fromStdString(a);
            }
            const QString quantity = units_->containsQuantity(u.qty)
                                         ? QString::fromStdString(units_->quantity(u.qty).name)
                                         : QString();
            const QString scale =
                u.offset == 0.0 ? QString::number(u.scalar, 'g', 10)
                                : tr("%1, offset %2").arg(u.scalar, 0, 'g', 10).arg(u.offset, 0, 'g', 10);
            const QStringList cells{
                quantity, QString::fromStdString(u.symbol),
                QString::fromUtf8(u.label().data(), static_cast<qsizetype>(u.label().size())), scale,
                aliases.join(QStringLiteral(", "))};
            for (int c = 0; c < cells.size(); ++c) {
                table_->setItem(row, c, new QTableWidgetItem(cells[c]));
            }
            ++row;
        }
    }
    table_->resizeColumnsToContents();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* column = new QVBoxLayout(this);
    column->addWidget(filter_);
    column->addWidget(table_, 1);
    column->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(filter_, &QLineEdit::textChanged, this, &UnitsListDialog::setFilter);
    resize(640, 480);
}

UnitsListDialog::~UnitsListDialog() = default;

void UnitsListDialog::setFilter(const QString& text) {
    if (filter_->text() != text) {
        filter_->setText(text); // comes back here through textChanged
        return;
    }
    const QString f = text.trimmed();
    for (int r = 0; r < table_->rowCount(); ++r) {
        bool match = f.isEmpty();
        for (int c = 0; !match && c < table_->columnCount(); ++c) {
            const QTableWidgetItem* item = table_->item(r, c);
            match = item != nullptr && item->text().contains(f, Qt::CaseInsensitive);
        }
        table_->setRowHidden(r, !match);
    }
}

int UnitsListDialog::visibleRows() const {
    int n = 0;
    for (int r = 0; r < table_->rowCount(); ++r) {
        n += table_->isRowHidden(r) ? 0 : 1;
    }
    return n;
}

// ---- OpenLogDialog ----------------------------------------------------------------------------------------

OpenLogDialog::OpenLogDialog(const formats::ImporterRegistry* registry, const QString& folder,
                             QWidget* parent)
    : QFileDialog(parent, tr("Open Log File"), folder), registry_(registry) {
    setObjectName(QStringLiteral("openLogDialog"));
    setOption(QFileDialog::DontUseNativeDialog, true); // the Details pane needs Qt's own dialog
    setFileMode(QFileDialog::ExistingFiles);
    setAcceptMode(QFileDialog::AcceptOpen);
    details_ = new QPlainTextEdit(this);
    details_->setObjectName(QStringLiteral("details"));
    details_->setReadOnly(true);
    details_->setPlaceholderText(tr("Details of the highlighted file"));
    details_->setMinimumWidth(220);
    if (auto* grid = qobject_cast<QGridLayout*>(layout())) {
        grid->addWidget(new QLabel(tr("Details"), this), 0, grid->columnCount());
        grid->addWidget(details_, 1, grid->columnCount() - 1, grid->rowCount() - 1, 1);
    }
    connect(this, &QFileDialog::currentChanged, this, &OpenLogDialog::showDetails);
}

OpenLogDialog::~OpenLogDialog() = default;

QString OpenLogDialog::details() const {
    return details_->toPlainText();
}

void OpenLogDialog::showDetails(const QString& path) {
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isFile()) {
        details_->clear();
        return;
    }
    QStringList lines;
    lines << tr("%1 (%2 KiB)").arg(info.fileName()).arg(QLocale().toString(info.size() / 1024));
    if (registry_ == nullptr) {
        details_->setPlainText(lines.join(QLatin1Char('\n')));
        return;
    }
    // The probe reads the file's header only (no bulk decode, 05 section 1.1).
    const Result<std::pair<const formats::IImporter*, formats::ProbeResult>> probed =
        registry_->probe(std::filesystem::path(path.toStdWString()));
    if (!probed) {
        lines << tr("Not a log this version opens: %1").arg(QString::fromStdString(probed.error().message));
        details_->setPlainText(lines.join(QLatin1Char('\n')));
        return;
    }
    const formats::ProbeResult& p = probed->second;
    lines << tr("Format: %1").arg(QString::fromStdString(p.importerId));
    if (!p.layers.empty()) {
        const formats::LayerInfo& layer = p.layers.front();
        lines << tr("Duration: %1 s").arg(static_cast<double>(layer.durationUs) / 1e6, 0, 'f', 1);
        lines << tr("Channels: %1").arg(layer.channels.size());
        lines << tr("Markers: %1").arg(layer.markers.size());
    }
    for (const model::Detail& d : p.details) {
        const std::string value = model::detailValueText(d.value);
        if (!value.empty()) {
            lines << QStringLiteral("%1: %2").arg(QString::fromStdString(d.id),
                                                  QString::fromStdString(value));
        }
    }
    details_->setPlainText(lines.join(QLatin1Char('\n')));
}

} // namespace esses::widgets
