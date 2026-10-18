// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DetailsEditor (06 section 13.4): the details grid over the open sources, typed cell edits as
// SetDetailCommands, Restore Original, sort and Copy as TSV. See details_editor.hpp.

#include <esses/base/casefold.hpp>
#include <esses/model/source.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/widgets/editors_data/details_editor.hpp>

#include <QAbstractTableModel>
#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QUndoStack>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace esses::widgets {

namespace {

constexpr int kSortRole = Qt::UserRole + 2;

QString sourceLabel(const model::DataSource& src) {
    if (const model::Detail* d = model::findDetail(src.details, "File Name")) {
        if (const auto* s = std::get_if<std::string>(&d->value)) {
            return QString::fromStdString(*s);
        }
    }
    return QString::fromStdWString(src.prov.path.filename().wstring());
}

QIcon editedDot() {
    QPixmap pm(10, 10);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QApplication::palette().color(QPalette::Highlight));
    p.drawEllipse(QRectF(2.0, 2.0, 6.0, 6.0));
    p.end();
    return QIcon(pm);
}

QString singleLine(QString text) {
    text.replace(QLatin1Char('\t'), QLatin1Char(' '));
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    return text;
}

} // namespace

// Rows = detail ids (profile order first), column 0 = the id, column k = source k - 1.
class DetailsModel final : public QAbstractTableModel {
public:
    DetailsModel(DetailsEditor& editor, qt::ProjectSession& session, qt::SourceLoader& loader)
        : QAbstractTableModel(&editor), editor_(editor), session_(session), loader_(loader) {}

    std::vector<std::string> ids;
    std::vector<std::shared_ptr<const model::DataSource>> sources;

    void rebuild() {
        beginResetModel();
        sources = loader_.sources();
        ids.clear();
        std::set<std::string> seen;
        const auto add = [this, &seen](const std::string& id) {
            if (seen.insert(caseFold(id)).second) {
                ids.push_back(id);
            }
        };
        for (const project::ProfileDetail& d : session_.profile().details) {
            add(d.id);
        }
        for (const auto& src : sources) {
            for (const model::Detail& d : src->details) {
                const model::StandardDetail* standard = model::findStandardDetail(d.id);
                if (standard == nullptr || !standard->derived) {
                    add(d.id);
                }
            }
        }
        endResetModel();
    }

    const model::DataSource* source(int index) const {
        return index >= 0 && index < static_cast<int>(sources.size())
                   ? sources[static_cast<std::size_t>(index)].get()
                   : nullptr;
    }
    const model::Detail* detail(int row, int sourceIndex) const {
        const model::DataSource* src = source(sourceIndex);
        if (src == nullptr || row < 0 || row >= static_cast<int>(ids.size())) {
            return nullptr;
        }
        return model::findDetail(src->details, ids[static_cast<std::size_t>(row)]);
    }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : static_cast<int>(ids.size());
    }
    int columnCount(const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : static_cast<int>(sources.size()) + 1;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return QAbstractTableModel::headerData(section, orientation, role);
        }
        if (section == 0) {
            return tr("Detail");
        }
        const model::DataSource* src = source(section - 1);
        return src == nullptr ? QVariant() : QVariant(sourceLabel(*src));
    }

    Qt::ItemFlags flags(const QModelIndex& index) const override {
        Qt::ItemFlags f = QAbstractTableModel::flags(index);
        if (index.isValid() && index.column() > 0) {
            f |= Qt::ItemIsEditable;
        }
        return f;
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() >= static_cast<int>(ids.size())) {
            return {};
        }
        const QString id = QString::fromStdString(ids[static_cast<std::size_t>(index.row())]);
        if (index.column() == 0) {
            if (role == Qt::DisplayRole || role == Qt::EditRole || role == kSortRole) {
                return id;
            }
            return {};
        }
        const model::Detail* d = detail(index.row(), index.column() - 1);
        const bool edited = d != nullptr && d->prov == model::Provenance::User;
        switch (role) {
        case Qt::DisplayRole:
        case Qt::EditRole:
            return d == nullptr ? QString() : DetailsEditor::valueText(d->value);
        case kSortRole:
            if (d != nullptr) {
                if (const auto* q = std::get_if<model::Quantityd>(&d->value)) {
                    return q->v;
                }
                return DetailsEditor::valueText(d->value);
            }
            return QString();
        case DetailsEditor::EditedRole:
            return edited;
        case Qt::DecorationRole:
            if (edited) {
                static const QIcon dot = editedDot();
                return dot;
            }
            return {};
        case Qt::ToolTipRole:
            if (edited) {
                return d->original ? tr("Edited; original: %1").arg(DetailsEditor::valueText(*d->original))
                                   : tr("Edited; the file has no value");
            }
            return {};
        default:
            return {};
        }
    }

    bool setData(const QModelIndex& index, const QVariant& value, int role) override {
        if (role != Qt::EditRole || !index.isValid() || index.column() == 0 ||
            index.row() >= static_cast<int>(ids.size())) {
            return false;
        }
        return editor_.setCell(QString::fromStdString(ids[static_cast<std::size_t>(index.row())]),
                               index.column() - 1, value.toString());
    }

private:
    DetailsEditor& editor_;
    qt::ProjectSession& session_;
    qt::SourceLoader& loader_;
};

DetailsEditor::DetailsEditor(qt::ProjectSession& session, qt::SourceLoader& loader, QWidget* parent)
    : QDialog(parent), session_(session), loader_(loader) {
    setWindowTitle(tr("Details"));
    setObjectName(QStringLiteral("detailsEditor"));
    setModal(false);
    model_ = new DetailsModel(*this, session_, loader_);
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setSortRole(kSortRole);
    proxy_->setSortCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setSortLocaleAware(true);
    grid_ = new QTableView(this);
    grid_->setObjectName(QStringLiteral("detailsGrid"));
    grid_->setAccessibleName(tr("Details of the open sources"));
    grid_->setModel(proxy_);
    grid_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    grid_->setSelectionMode(QAbstractItemView::SingleSelection);
    grid_->verticalHeader()->setVisible(false);
    grid_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    grid_->horizontalHeader()->setStretchLastSection(true);
    grid_->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder); // profile order until a click
    grid_->setSortingEnabled(true);
    grid_->setContextMenuPolicy(Qt::CustomContextMenu);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("detailsStatus"));
    status_->setWordWrap(true);
    restoreButton_ = new QPushButton(tr("Restore Original"), this);
    restoreButton_->setObjectName(QStringLiteral("restoreOriginal"));
    auto* copy = new QPushButton(tr("Copy"), this);
    copy->setObjectName(QStringLiteral("copyDetails"));
    copy->setToolTip(tr("Copy the grid as tab-separated text"));
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(restoreButton_);
    buttons->addWidget(copy);
    buttons->addStretch(1);
    buttons->addWidget(close);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(grid_, 1);
    layout->addWidget(status_);
    layout->addLayout(buttons);

    connect(close, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(copy, &QPushButton::clicked, this, [this] { copyAsTsv(); });
    const auto currentCell = [this]() -> std::pair<QString, int> {
        const QModelIndex i = proxy_->mapToSource(grid_->currentIndex());
        if (!i.isValid() || i.column() == 0) {
            return {QString(), -1};
        }
        return {model_->index(i.row(), 0).data().toString(), i.column() - 1};
    };
    connect(restoreButton_, &QPushButton::clicked, this, [this, currentCell] {
        const auto [id, column] = currentCell();
        static_cast<void>(restoreOriginal(id, column));
    });
    const auto updateButtons = [this, currentCell] {
        const auto [id, column] = currentCell();
        restoreButton_->setEnabled(column >= 0 && isEdited(id, column));
    };
    connect(grid_->selectionModel(), &QItemSelectionModel::currentChanged, this, updateButtons);
    connect(grid_, &QWidget::customContextMenuRequested, this, &DetailsEditor::showContextMenu);
    connect(&loader_, &qt::SourceLoader::sourcePublished, this, [this] { refresh(); });
    connect(&loader_, &qt::SourceLoader::sourceReplaced, this, [this] { refresh(); });
    connect(&loader_, &qt::SourceLoader::sourceClosed, this, [this] { refresh(); });
    connect(model_, &QAbstractItemModel::modelReset, this, updateButtons);
    refresh();
    // ENTER belongs to the grid (edit, commit), never to a button of the dialog.
    for (QPushButton* b : findChildren<QPushButton*>()) {
        b->setAutoDefault(false);
        b->setDefault(false);
    }
    resize(640, 420);
}

DetailsEditor::~DetailsEditor() = default;

void DetailsEditor::refresh() {
    const QModelIndex current = proxy_->mapToSource(grid_->currentIndex());
    const QString keepId = current.isValid() ? model_->index(current.row(), 0).data().toString() : QString();
    const int keepColumn = current.isValid() ? current.column() : -1;
    model_->rebuild();
    if (!keepId.isEmpty()) {
        const QStringList ids = detailIds();
        const int row = static_cast<int>(ids.indexOf(keepId));
        if (row >= 0 && keepColumn < model_->columnCount()) {
            grid_->setCurrentIndex(proxy_->mapFromSource(model_->index(row, keepColumn)));
        }
    }
}

QStringList DetailsEditor::detailIds() const {
    QStringList out;
    for (const std::string& id : model_->ids) {
        out << QString::fromStdString(id);
    }
    return out;
}

int DetailsEditor::sourceCount() const {
    return static_cast<int>(model_->sources.size());
}

QString DetailsEditor::cellText(const QString& detailId, int sourceIndex) const {
    const model::DataSource* src = model_->source(sourceIndex);
    const model::Detail* d =
        src == nullptr ? nullptr : model::findDetail(src->details, detailId.toStdString());
    return d == nullptr ? QString() : valueText(d->value);
}

bool DetailsEditor::isEdited(const QString& detailId, int sourceIndex) const {
    const model::DataSource* src = model_->source(sourceIndex);
    const model::Detail* d =
        src == nullptr ? nullptr : model::findDetail(src->details, detailId.toStdString());
    return d != nullptr && d->prov == model::Provenance::User;
}

bool DetailsEditor::fail(const QString& reason) {
    error_ = reason;
    status_->setText(error_);
    return false;
}

bool DetailsEditor::setCell(const QString& detailId, int sourceIndex, const QString& text) {
    const model::DataSource* src = model_->source(sourceIndex);
    if (src == nullptr || detailId.isEmpty()) {
        return fail(tr("No such cell."));
    }
    const std::string id = detailId.toStdString();
    const model::Detail* existing = model::findDetail(src->details, id);
    const project::ProfileDetail* profile = session_.profile().findDetail(id);
    const model::StandardDetail* standard = model::findStandardDetail(id);
    // The cell's type: the value's, else the profile's, else the standard detail's, else text.
    model::DetailType type = model::DetailType::Text;
    std::string symbol;
    if (existing != nullptr) {
        if (const auto* q = std::get_if<model::Quantityd>(&existing->value)) {
            type = model::DetailType::Number;
            symbol = q->symbol;
        } else if (std::holds_alternative<model::DateTime>(existing->value)) {
            type = model::DetailType::DateTime;
        }
    } else if (profile != nullptr) {
        type = profile->type == "number" ? model::DetailType::Number : model::DetailType::Text;
        symbol = profile->unit;
    } else if (standard != nullptr) {
        type = standard->type;
    }
    if (type == model::DetailType::Number && symbol.empty() && profile != nullptr) {
        symbol = profile->unit;
    }
    QString reason;
    const std::shared_ptr<const units::UnitRegistry> registry = session_.units();
    std::optional<model::DetailValue> value = parseValue(type, text, symbol, registry.get(), &reason);
    if (!value) {
        return fail(reason);
    }
    if (existing != nullptr && existing->value == *value) {
        return fail(tr("\"%1\" is unchanged.").arg(detailId));
    }
    const std::string canonicalId = existing != nullptr ? existing->id : id;
    const QString before = existing != nullptr ? valueText(existing->value) : QString();
    const model::SourceId sourceId = src->id;
    session_.undoStack().push(new qt::SetDetailCommand(loader_, *src, std::string(model::kNormalLayer),
                                                       canonicalId, std::move(value)));
    error_.clear();
    status_->clear();
    if (caseFold(canonicalId) == caseFold("Venue")) {
        Q_EMIT venueRenamed(sourceId, before, text);
    }
    return true;
}

bool DetailsEditor::restoreOriginal(const QString& detailId, int sourceIndex) {
    const model::DataSource* src = model_->source(sourceIndex);
    const model::Detail* d =
        src == nullptr ? nullptr : model::findDetail(src->details, detailId.toStdString());
    if (d == nullptr || d->prov != model::Provenance::User) {
        return fail(tr("\"%1\" holds the value of the file.").arg(detailId));
    }
    const std::string id = d->id;
    const QString before = valueText(d->value);
    const QString after = d->original ? valueText(*d->original) : QString();
    const model::SourceId sourceId = src->id;
    session_.undoStack().push(
        new qt::SetDetailCommand(loader_, *src, std::string(model::kNormalLayer), id, std::nullopt));
    error_.clear();
    status_->clear();
    if (caseFold(id) == caseFold("Venue")) {
        Q_EMIT venueRenamed(sourceId, before, after);
    }
    return true;
}

QString DetailsEditor::tableText() const {
    QStringList lines;
    QStringList header;
    for (int c = 0; c < proxy_->columnCount(); ++c) {
        header << singleLine(proxy_->headerData(c, Qt::Horizontal).toString());
    }
    lines << header.join(QLatin1Char('\t'));
    for (int r = 0; r < proxy_->rowCount(); ++r) {
        QStringList cells;
        for (int c = 0; c < proxy_->columnCount(); ++c) {
            cells << singleLine(proxy_->index(r, c).data().toString());
        }
        lines << cells.join(QLatin1Char('\t'));
    }
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

void DetailsEditor::copyAsTsv() const {
    if (QClipboard* clipboard = QApplication::clipboard()) {
        clipboard->setText(tableText());
    }
}

void DetailsEditor::showContextMenu(const QPoint& pos) {
    const QModelIndex at = proxy_->mapToSource(grid_->indexAt(pos));
    QMenu menu(this);
    QAction* restore = menu.addAction(tr("Restore Original"));
    const QString id = at.isValid() ? model_->index(at.row(), 0).data().toString() : QString();
    const int column = at.isValid() ? at.column() - 1 : -1;
    restore->setEnabled(column >= 0 && isEdited(id, column));
    connect(restore, &QAction::triggered, this,
            [this, id, column] { static_cast<void>(restoreOriginal(id, column)); });
    connect(menu.addAction(tr("Copy as TSV")), &QAction::triggered, this, [this] { copyAsTsv(); });
    connect(menu.addAction(tr("Profile Order")), &QAction::triggered, this,
            [this] { grid_->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder); });
    menu.exec(grid_->viewport()->mapToGlobal(pos));
}

// ---- values
// ------------------------------------------------------------------------------------------------

QString DetailsEditor::valueText(const model::DetailValue& value) {
    if (const auto* s = std::get_if<std::string>(&value)) {
        return QString::fromStdString(*s);
    }
    if (const auto* q = std::get_if<model::Quantityd>(&value)) {
        // Every significant digit (06 section 13.4 item 1: numbers are not rounded on reopen).
        const QString number = QString::number(q->v, 'g', QLocale::FloatingPointShortest);
        return q->symbol.empty() ? number : number + QLatin1Char(' ') + QString::fromStdString(q->symbol);
    }
    return QString::fromStdString(std::get<model::DateTime>(value).iso());
}

std::optional<model::DetailValue> DetailsEditor::parseValue(model::DetailType type, const QString& text,
                                                            const std::string& defaultSymbol,
                                                            const units::UnitRegistry* units,
                                                            QString* error) {
    const auto refuse = [error](const QString& reason) -> std::optional<model::DetailValue> {
        if (error != nullptr) {
            *error = reason;
        }
        return std::nullopt;
    };
    switch (type) {
    case model::DetailType::Text:
        return model::DetailValue(text.toStdString()); // empty strings are allowed
    case model::DetailType::DateTime: {
        if (const auto dt = model::DateTime::parseIso(text.trimmed().toStdString())) {
            return model::DetailValue(*dt);
        }
        return refuse(tr("\"%1\" is not a date and time; write it as 2026-10-12T14:05:00+02:00.").arg(text));
    }
    case model::DetailType::Number:
        break;
    }
    static const QRegularExpression number(
        QStringLiteral(R"(^\s*([+-]?(?:\d+(?:\.(\d*))?|\.(\d+))(?:[eE][+-]?\d+)?)\s*(.*?)\s*$)"));
    const QRegularExpressionMatch m = number.match(text);
    if (!m.hasMatch()) {
        return refuse(
            tr("\"%1\" is not a number (Restore Original brings back the file's value).").arg(text));
    }
    bool ok = false;
    const double v = m.captured(1).toDouble(&ok);
    if (!ok || !std::isfinite(v)) {
        return refuse(tr("\"%1\" is not a number.").arg(text));
    }
    model::Quantityd q;
    q.v = v;
    // The decimals as typed, so the value shows as entered elsewhere too (it is never rounded on reopen).
    const QString decimals = m.captured(2).isEmpty() ? m.captured(3) : m.captured(2);
    q.dps = static_cast<int>(std::min<qsizetype>(decimals.size(), 9));
    const QString typedUnit = m.captured(4);
    q.symbol = typedUnit.isEmpty() ? defaultSymbol : typedUnit.toStdString();
    if (units != nullptr && !q.symbol.empty()) {
        const auto id = units->find(q.symbol);
        if (!id) {
            return refuse(
                tr("\"%1\" is not a unit of the units table.").arg(QString::fromStdString(q.symbol)));
        }
        if (!typedUnit.isEmpty() && !defaultSymbol.empty()) {
            if (const auto expected = units->find(defaultSymbol);
                expected && units->quantityOf(*expected) != units->quantityOf(*id)) {
                return refuse(tr("\"%1\" is not a unit of %2.")
                                  .arg(typedUnit, QString::fromStdString(
                                                      units->quantity(units->quantityOf(*expected)).name)));
            }
        }
        q.unit = *id;
        q.symbol = units->unit(*id).symbol;
    }
    return model::DetailValue(q);
}

} // namespace esses::widgets
