// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MappingPanel (05 section 8.1, 06 section 13.8; AC-M2-11): the rows of a pending open, their editors and the
// Scan / Resolve / Save / Open Anyway / Cancel actions. See mapping_panel.hpp.

#include <esses/base/casefold.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/widgets/editors_data/mapping_panel.hpp>

#include <QAbstractTableModel>
#include <QComboBox>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QUndoStack>
#include <QVBoxLayout>
#include <algorithm>
#include <optional>
#include <utility>

namespace esses::widgets {

namespace {

using Match = formats::UnitMappingProfile::Match;

QString matchLabel(Match m) {
    switch (m) {
    case Match::NameOnly:
        return QObject::tr("Name only");
    case Match::UnitOnly:
        return QObject::tr("Unit only");
    case Match::NameAndUnit:
        break;
    }
    return QObject::tr("Name and unit");
}

std::optional<Match> parseMatchLabel(const QString& text) {
    for (const Match m : {Match::NameAndUnit, Match::NameOnly, Match::UnitOnly}) {
        if (text == matchLabel(m)) {
            return m;
        }
    }
    return std::nullopt;
}

bool sameKey(const formats::UnitMappingProfile::Rule& a, const formats::UnitMappingProfile::Rule& b) {
    return a.match == b.match && caseFold(a.name) == caseFold(b.name) && a.vendorUnit == b.vendorUnit;
}

formats::UnitMappingProfile::Rule ruleOf(const MappingPanel::Row& row) {
    formats::UnitMappingProfile::Rule r;
    r.match = row.match;
    if (row.match != Match::UnitOnly) {
        r.name = row.loggedName;
    }
    if (row.match != Match::NameOnly) {
        r.vendorUnit = row.vendorUnit;
    }
    r.quantity = row.quantity;
    r.unit = row.unit;
    return r;
}

// Combo-box editors for the cells whose model data carries choices (MappingPanel::ChoicesRole).
class ChoiceDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const override {
        const QStringList choices = index.data(MappingPanel::ChoicesRole).toStringList();
        if (choices.isEmpty()) {
            return QStyledItemDelegate::createEditor(parent, option, index);
        }
        auto* combo = new QComboBox(parent);
        combo->addItems(choices);
        return combo;
    }
    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        if (auto* combo = qobject_cast<QComboBox*>(editor)) {
            const int at = combo->findText(index.data(Qt::EditRole).toString());
            combo->setCurrentIndex(at < 0 ? 0 : at);
            return;
        }
        QStyledItemDelegate::setEditorData(editor, index);
    }
    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override {
        if (auto* combo = qobject_cast<QComboBox*>(editor)) {
            model->setData(index, combo->currentText(), Qt::EditRole);
            return;
        }
        QStyledItemDelegate::setModelData(editor, model, index);
    }
};

} // namespace

// The rows of the shown open; edits go through the panel's setters (one rule set for the table and the API).
class MappingRowsModel final : public QAbstractTableModel {
public:
    explicit MappingRowsModel(MappingPanel& panel) : QAbstractTableModel(&panel), panel_(panel) {}

    std::vector<MappingPanel::Row> rows;

    void reset(std::vector<MappingPanel::Row> next) {
        beginResetModel();
        rows = std::move(next);
        endResetModel();
    }
    void rowChanged(int row) { Q_EMIT dataChanged(index(row, 0), index(row, MappingPanel::ColumnCount - 1)); }
    bool valid(int row) const { return row >= 0 && row < static_cast<int>(rows.size()); }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows.size());
    }
    int columnCount(const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : MappingPanel::ColumnCount;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return QAbstractTableModel::headerData(section, orientation, role);
        }
        switch (section) {
        case MappingPanel::VendorNameCol:
            return tr("Vendor name");
        case MappingPanel::VendorUnitCol:
            return tr("Vendor unit");
        case MappingPanel::SamplesCol:
            return tr("Sample values");
        case MappingPanel::AnalysisNameCol:
            return tr("Analysis name");
        case MappingPanel::QuantityCol:
            return tr("Quantity");
        case MappingPanel::UnitCol:
            return tr("Unit");
        case MappingPanel::RuleCol:
            return tr("Rule");
        case MappingPanel::FutureCol:
            return tr("Apply to future files");
        case MappingPanel::StatusCol:
            return tr("Status");
        default:
            return {};
        }
    }

    Qt::ItemFlags flags(const QModelIndex& index) const override {
        Qt::ItemFlags f = QAbstractTableModel::flags(index);
        switch (index.column()) {
        case MappingPanel::AnalysisNameCol:
        case MappingPanel::QuantityCol:
        case MappingPanel::UnitCol:
        case MappingPanel::RuleCol:
            f |= Qt::ItemIsEditable;
            break;
        case MappingPanel::FutureCol:
            f |= Qt::ItemIsUserCheckable;
            break;
        default:
            break;
        }
        return f;
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || !valid(index.row())) {
            return {};
        }
        const MappingPanel::Row& r = rows[static_cast<std::size_t>(index.row())];
        const int col = index.column();
        if (role == Qt::CheckStateRole) {
            return col == MappingPanel::FutureCol ? QVariant(r.applyToFuture ? Qt::Checked : Qt::Unchecked)
                                                  : QVariant();
        }
        if (role == MappingPanel::ChoicesRole) {
            switch (col) {
            case MappingPanel::QuantityCol:
                return panel_.quantityChoices();
            case MappingPanel::UnitCol:
                return panel_.unitChoices(QString::fromStdString(r.quantity));
            case MappingPanel::RuleCol: {
                QStringList labels;
                for (const Match m : {Match::NameAndUnit, Match::NameOnly, Match::UnitOnly}) {
                    if (m == Match::NameOnly || !r.vendorUnit.empty()) {
                        labels << matchLabel(m);
                    }
                }
                return labels;
            }
            default:
                return {};
            }
        }
        if (role == Qt::FontRole && col == MappingPanel::StatusCol && !r.complete()) {
            QFont f;
            f.setBold(true);
            return f;
        }
        if (role == Qt::ToolTipRole) {
            if (col == MappingPanel::AnalysisNameCol) {
                return tr("The name the channel is analysed under (a project mapping); empty keeps \"%1\".")
                    .arg(QString::fromStdString(r.loggedName));
            }
            if (col == MappingPanel::FutureCol) {
                return tr("Write this answer into the project's unit map, so later files apply it silently.");
            }
            return {};
        }
        if (role != Qt::DisplayRole && role != Qt::EditRole) {
            return {};
        }
        switch (col) {
        case MappingPanel::VendorNameCol:
            return QString::fromStdString(r.loggedName);
        case MappingPanel::VendorUnitCol:
            return QString::fromStdString(r.vendorUnit);
        case MappingPanel::SamplesCol: {
            QStringList values;
            for (const double v : r.samples) {
                values << QString::number(v, 'g', 6);
            }
            return values.join(QStringLiteral(", "));
        }
        case MappingPanel::AnalysisNameCol:
            return QString::fromStdString(r.analysisName);
        case MappingPanel::QuantityCol:
            return QString::fromStdString(r.quantity);
        case MappingPanel::UnitCol:
            return QString::fromStdString(r.unit);
        case MappingPanel::RuleCol:
            return matchLabel(r.match);
        case MappingPanel::StatusCol:
            if (!r.complete()) {
                return r.quantity.empty() ? tr("Choose a quantity and unit") : tr("Choose a unit");
            }
            return r.origin.isEmpty() ? tr("Resolved") : tr("Resolved (%1)").arg(r.origin);
        default:
            return {};
        }
    }

    bool setData(const QModelIndex& index, const QVariant& value, int role) override {
        if (!index.isValid() || !valid(index.row())) {
            return false;
        }
        const int row = index.row();
        if (role == Qt::CheckStateRole && index.column() == MappingPanel::FutureCol) {
            panel_.setApplyToFuture(row, value.toInt() == Qt::Checked);
            return true;
        }
        if (role != Qt::EditRole) {
            return false;
        }
        switch (index.column()) {
        case MappingPanel::AnalysisNameCol:
            panel_.setAnalysisName(row, value.toString());
            return true;
        case MappingPanel::QuantityCol:
            return panel_.setQuantity(row, value.toString());
        case MappingPanel::UnitCol:
            return panel_.setUnit(row, value.toString());
        case MappingPanel::RuleCol: {
            const auto m = parseMatchLabel(value.toString());
            return m.has_value() && panel_.setMatch(row, *m);
        }
        default:
            return false;
        }
    }

private:
    MappingPanel& panel_;
};

MappingPanel::MappingPanel(qt::SourceLoader& loader, qt::ProjectSession& session,
                           std::shared_ptr<const units::UnitRegistry> units, QWidget* parent)
    : QWidget(parent), loader_(loader), session_(session), units_(std::move(units)) {
    if (units_ == nullptr) {
        units_ = session_.units();
    }
    setObjectName(QStringLiteral("mappingPanel"));
    rows_ = new MappingRowsModel(*this);
    bar_ = new QLabel(this);
    bar_->setObjectName(QStringLiteral("mappingBar"));
    bar_->setWordWrap(true);
    table_ = new QTableView(this);
    table_->setObjectName(QStringLiteral("mappingTable"));
    table_->setAccessibleName(tr("Channels that need units"));
    table_->setModel(rows_);
    table_->setItemDelegate(new ChoiceDelegate(table_));
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                            QAbstractItemView::SelectedClicked);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setVisible(false);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("mappingStatus"));
    status_->setWordWrap(true);

    const auto button = [this](const QString& text, const char* name, const QString& tip) {
        auto* b = new QPushButton(text, this);
        b->setObjectName(QString::fromLatin1(name));
        b->setToolTip(tip);
        return b;
    };
    scanButton_ = button(tr("Scan"), "mappingScan",
                         tr("Look the rows up again in the project's unit map and the units table."));
    resolveButton_ = button(tr("Resolve"), "mappingResolve",
                            tr("Open the file with the chosen quantities, units and names."));
    saveButton_ = button(tr("Save"), "mappingSave",
                         tr("Save the answers in the project's unit map for this importer."));
    anywayButton_ = button(tr("Open Anyway"), "mappingOpenAnyway",
                           tr("Open the file; the channels without a unit open unitless."));
    cancelButton_ = button(tr("Cancel"), "mappingCancel", tr("Do not open the file."));
    connect(scanButton_, &QPushButton::clicked, this, [this] { scan(); });
    connect(resolveButton_, &QPushButton::clicked, this, [this] { static_cast<void>(resolve()); });
    connect(saveButton_, &QPushButton::clicked, this, [this] { static_cast<void>(save()); });
    connect(anywayButton_, &QPushButton::clicked, this, [this] { openAnyway(); });
    connect(cancelButton_, &QPushButton::clicked, this, [this] { cancel(); });

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(scanButton_);
    buttons->addStretch(1);
    buttons->addWidget(saveButton_);
    buttons->addWidget(resolveButton_);
    buttons->addWidget(anywayButton_);
    buttons->addWidget(cancelButton_);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(bar_);
    layout->addWidget(table_, 1);
    layout->addWidget(status_);
    layout->addLayout(buttons);

    connect(&loader_, &qt::SourceLoader::mappingRequired, this,
            [this](int id, const formats::PendingResolution& pending) { enqueue(id, pending); });
    refresh();
}

MappingPanel::~MappingPanel() = default;

const std::vector<MappingPanel::Row>& MappingPanel::rows() const noexcept {
    return rows_->rows;
}

void MappingPanel::showPending(int pendingId, const formats::PendingResolution& pending) {
    enqueue(pendingId, pending);
}

void MappingPanel::enqueue(int pendingId, const formats::PendingResolution& pending) {
    if (pendingId < 0 || pendingId == pendingId_ ||
        std::any_of(
            queue_.begin(), queue_.end(), [pendingId](const auto& q) { return q.first == pendingId; })) {
        return;
    }
    if (pendingId_ < 0) {
        present(pendingId, pending);
        return;
    }
    queue_.emplace_back(pendingId, pending);
    refresh();
    Q_EMIT pendingChanged(true);
}

void MappingPanel::present(int pendingId, formats::PendingResolution pending) {
    pendingId_ = pendingId;
    pending_ = std::move(pending);
    std::vector<Row> next;
    next.reserve(pending_.rows.size());
    for (const formats::UnresolvedRow& u : pending_.rows) {
        Row r;
        r.loggedName = u.loggedName;
        r.vendorUnit = u.vendorUnit;
        r.samples = u.sampleValues;
        r.match = u.vendorUnit.empty() ? Match::NameOnly : Match::NameAndUnit;
        next.push_back(std::move(r));
    }
    rows_->reset(std::move(next));
    error_.clear();
    scan();
    Q_EMIT pendingChanged(true);
}

void MappingPanel::finish() {
    pendingId_ = -1;
    pending_ = {};
    rows_->reset({});
    while (!queue_.empty()) {
        auto [id, pending] = std::move(queue_.front());
        queue_.pop_front();
        if (loader_.pendingMapping(id) != nullptr) { // still waiting (not cancelled elsewhere)
            present(id, std::move(pending));
            return;
        }
    }
    refresh();
    Q_EMIT pendingChanged(false);
}

void MappingPanel::refresh() {
    const bool shown = pendingId_ >= 0;
    if (!shown) {
        bar_->setText(tr("No file is waiting for units."));
    } else {
        const int open = static_cast<int>(std::count_if(rows_->rows.begin(), rows_->rows.end(),
                                                        [](const Row& r) { return !r.complete(); }));
        const QString file = QFileInfo(QString::fromStdString(pending_.path)).fileName();
        QString text =
            open == 0 ? tr("\"%1\": every channel has a quantity and unit; Resolve opens the file.").arg(file)
            : open == 1 ? tr("\"%1\": 1 channel needs a quantity and unit before the file opens.").arg(file)
                        : tr("\"%1\": %2 channels need a quantity and unit before the file opens.")
                              .arg(file)
                              .arg(open);
        if (queue_.size() == 1) {
            text += QLatin1Char(' ') + tr("1 more file waiting.");
        } else if (!queue_.empty()) {
            text += QLatin1Char(' ') + tr("%1 more files waiting.").arg(static_cast<int>(queue_.size()));
        }
        bar_->setText(text);
    }
    status_->setText(error_);
    for (QPushButton* b : {scanButton_, resolveButton_, saveButton_, anywayButton_, cancelButton_}) {
        b->setEnabled(shown);
    }
}

bool MappingPanel::fail(const QString& reason) {
    error_ = reason;
    refresh();
    Q_EMIT hint(reason);
    return false;
}

// ---- row edits --------------------------------------------------------------------------------------------

bool MappingPanel::setQuantity(int row, const QString& quantity) {
    if (!rows_->valid(row)) {
        return false;
    }
    Row& r = rows_->rows[static_cast<std::size_t>(row)];
    if (quantity.trimmed().isEmpty()) {
        r.quantity.clear();
        r.unit.clear();
        r.origin.clear();
        rows_->rowChanged(row);
        refresh();
        return true;
    }
    const auto q = units_->quantityByName(quantity.trimmed().toStdString());
    if (!q || *q == units::kUnitless) {
        return fail(tr("\"%1\" is not a quantity of the units table.").arg(quantity));
    }
    r.quantity = units_->quantity(*q).name;
    if (!r.unit.empty()) {
        const auto u = units_->find(r.unit);
        if (!u || units_->quantityOf(*u) != *q) {
            r.unit.clear();
        }
    }
    if (r.unit.empty() && !r.vendorUnit.empty()) {
        if (const auto u = units_->find(r.vendorUnit, *q)) { // the file's spelling within the quantity
            r.unit = units_->unit(*u).symbol;
        }
    }
    r.origin.clear();
    error_.clear();
    rows_->rowChanged(row);
    refresh();
    return true;
}

bool MappingPanel::setUnit(int row, const QString& unit) {
    if (!rows_->valid(row)) {
        return false;
    }
    Row& r = rows_->rows[static_cast<std::size_t>(row)];
    if (unit.trimmed().isEmpty()) {
        r.unit.clear();
        r.origin.clear();
        rows_->rowChanged(row);
        refresh();
        return true;
    }
    const auto u = units_->find(unit.trimmed().toStdString());
    if (!u || *u == units::kNoUnit) {
        return fail(tr("\"%1\" is not a unit of the units table.").arg(unit));
    }
    const units::QuantityId q = units_->quantityOf(*u);
    if (!r.quantity.empty() && units_->quantity(q).name != r.quantity) {
        return fail(tr("\"%1\" is not a unit of %2.").arg(unit, QString::fromStdString(r.quantity)));
    }
    r.quantity = units_->quantity(q).name;
    r.unit = units_->unit(*u).symbol;
    r.origin.clear();
    error_.clear();
    rows_->rowChanged(row);
    refresh();
    return true;
}

bool MappingPanel::setMatch(int row, Match match) {
    if (!rows_->valid(row)) {
        return false;
    }
    Row& r = rows_->rows[static_cast<std::size_t>(row)];
    if (match != Match::NameOnly && r.vendorUnit.empty()) {
        return fail(tr("\"%1\" has no vendor unit to match.").arg(QString::fromStdString(r.loggedName)));
    }
    r.match = match;
    error_.clear();
    rows_->rowChanged(row);
    refresh();
    return true;
}

void MappingPanel::setAnalysisName(int row, const QString& name) {
    if (!rows_->valid(row)) {
        return;
    }
    rows_->rows[static_cast<std::size_t>(row)].analysisName = name.trimmed().toStdString();
    rows_->rowChanged(row);
}

void MappingPanel::setApplyToFuture(int row, bool on) {
    if (!rows_->valid(row)) {
        return;
    }
    rows_->rows[static_cast<std::size_t>(row)].applyToFuture = on;
    rows_->rowChanged(row);
}

// ---- choices ----------------------------------------------------------------------------------------------

QStringList MappingPanel::quantityChoices() const {
    QStringList out;
    for (const units::Quantity& q : units_->quantities()) {
        if (q.id != units::kUnitless) {
            out << QString::fromStdString(q.name);
        }
    }
    return out;
}

QStringList MappingPanel::unitChoices(const QString& quantity) const {
    QStringList out;
    const auto add = [&out](const units::Unit& u) {
        if (u.id != units::kNoUnit && !u.isCompat()) {
            out << QString::fromStdString(u.symbol);
        }
    };
    if (quantity.isEmpty()) {
        for (const units::Unit& u : units_->units()) {
            add(u);
        }
        return out;
    }
    if (const auto q = units_->quantityByName(quantity.toStdString())) {
        for (const units::UnitId id : units_->unitsOf(*q)) {
            add(units_->unit(id));
        }
    }
    return out;
}

// ---- resolution -------------------------------------------------------------------------------------------

std::string MappingPanel::importerStem() const {
    return pendingId_ < 0 ? std::string() : formats::UnitMappingProfile::fileStem(pending_.importerId);
}

formats::UnitMappingProfile MappingPanel::existingMap() const {
    const auto& maps = session_.unitMaps();
    if (const auto it = maps.find(importerStem()); it != maps.end()) {
        return it->second;
    }
    formats::UnitMappingProfile empty;
    empty.importer = pending_.importerId;
    return empty;
}

formats::UnitMappingProfile MappingPanel::mergedMap(bool futureOnly) const {
    formats::UnitMappingProfile map = existingMap();
    if (map.importer.empty()) {
        map.importer = pending_.importerId;
    }
    for (const Row& r : rows_->rows) {
        if (!r.complete() || (futureOnly && !r.applyToFuture)) {
            continue;
        }
        formats::UnitMappingProfile::Rule rule = ruleOf(r);
        const auto same = std::find_if(map.rules.begin(), map.rules.end(),
                                       [&rule](const auto& e) { return sameKey(e, rule); });
        if (same == map.rules.end()) {
            map.rules.push_back(std::move(rule));
        } else {
            *same = std::move(rule);
        }
    }
    return map;
}

void MappingPanel::scan() {
    if (pendingId_ < 0) {
        refresh();
        return;
    }
    const formats::UnitMappingProfile map = existingMap();
    for (std::size_t i = 0; i < rows_->rows.size(); ++i) {
        Row& r = rows_->rows[i];
        if (const auto res = map.resolve(r.loggedName, r.vendorUnit, *units_)) {
            r.quantity = units_->quantity(res->quantity).name;
            r.unit = units_->unit(res->unit).symbol;
            if (res->rule != nullptr) {
                r.match = res->rule->match;
            }
            if (r.analysisName.empty() && res->analysisName) {
                r.analysisName = *res->analysisName;
            }
            r.origin = tr("unit map");
        } else if (const std::optional<units::UnitId> u =
                       r.vendorUnit.empty() ? std::optional<units::UnitId>() : units_->find(r.vendorUnit);
                   u && *u != units::kNoUnit) {
            r.quantity = units_->quantity(units_->quantityOf(*u)).name;
            r.unit = units_->unit(*u).symbol;
            r.origin = tr("units table");
        } else if (r.quantity.empty() && i < pending_.rows.size() && pending_.rows[i].inferredQuantity) {
            if (const auto q = units_->quantityByName(*pending_.rows[i].inferredQuantity);
                q && *q != units::kUnitless) {
                r.quantity = units_->quantity(*q).name;
                r.origin = tr("inferred");
            }
        }
        rows_->rowChanged(static_cast<int>(i));
    }
    error_.clear();
    refresh();
}

formats::MappingDecision MappingPanel::decision() const {
    formats::MappingDecision d;
    if (pendingId_ < 0) {
        return d;
    }
    const bool anyComplete =
        std::any_of(rows_->rows.begin(), rows_->rows.end(), [](const Row& r) { return r.complete(); });
    formats::UnitMappingProfile map = mergedMap(false);
    if (anyComplete || !map.rules.empty()) {
        d.unitMap = std::move(map);
    }
    for (const Row& r : rows_->rows) {
        if (!r.analysisName.empty() && caseFold(r.analysisName) != caseFold(r.loggedName)) {
            d.mappings.push_back(model::MappingRow{r.loggedName, r.analysisName, std::nullopt, false});
        }
    }
    return d;
}

void MappingPanel::submit(formats::MappingDecision d, const formats::UnitMappingProfile* toSave) {
    const int id = pendingId_;
    if (loader_.pendingMapping(id) == nullptr) { // dropped elsewhere (another panel, a closed loader)
        fail(tr("The open is no longer waiting."));
        finish();
        return;
    }
    if (toSave == nullptr) {
        d.saveUnitMap = false;
        qt::applyMappingDecision(session_, loader_, id, std::move(d));
    } else if (d.unitMap && d.unitMap->rules == toSave->rules) {
        d.saveUnitMap = true;
        qt::applyMappingDecision(session_, loader_, id, std::move(d));
    } else {
        // Only the rows ticked "Apply to future files" go into the unit map; the open uses them all.
        QUndoStack& stack = session_.undoStack();
        stack.beginMacro(tr("Resolve mapping"));
        stack.push(new qt::SetUnitMapCommand(session_, importerStem(), *toSave));
        d.saveUnitMap = false;
        qt::applyMappingDecision(session_, loader_, id, std::move(d));
        stack.endMacro();
    }
    error_.clear();
    finish();
}

bool MappingPanel::resolve() {
    if (pendingId_ < 0) {
        return fail(tr("No file is waiting for units."));
    }
    for (const Row& r : rows_->rows) {
        if (!r.complete()) {
            return fail(tr("\"%1\" has no quantity and unit: choose them, or use Open Anyway.")
                            .arg(QString::fromStdString(r.loggedName)));
        }
    }
    // The rows ticked "Apply to future files" change the importer's unit map (nothing when they add nothing).
    const formats::UnitMappingProfile future = mergedMap(true);
    const bool known = session_.unitMaps().count(importerStem()) != 0;
    const bool saveFuture = !future.rules.empty() && (!known || future.rules != existingMap().rules);
    submit(decision(), saveFuture ? &future : nullptr);
    return true;
}

bool MappingPanel::save(QString* error) {
    const auto refuse = [this, error](const QString& reason) {
        if (error != nullptr) {
            *error = reason;
        }
        return fail(reason);
    };
    if (pendingId_ < 0) {
        return refuse(tr("Nothing to save: no file is waiting for units."));
    }
    if (std::none_of(rows_->rows.begin(), rows_->rows.end(), [](const Row& r) { return r.complete(); })) {
        return refuse(tr("No row has a quantity and unit to save."));
    }
    for (std::size_t i = 0; i < rows_->rows.size(); ++i) {
        if (rows_->rows[i].complete()) {
            rows_->rows[i].applyToFuture = true;
            rows_->rowChanged(static_cast<int>(i));
        }
    }
    const formats::UnitMappingProfile merged = mergedMap(false);
    const std::string stem = importerStem();
    const bool known = session_.unitMaps().count(stem) != 0;
    if (!known || merged.rules != existingMap().rules) {
        session_.undoStack().push(new qt::SetUnitMapCommand(session_, stem, merged));
    }
    if (error != nullptr) {
        error->clear();
    }
    scan(); // the rows now come from the unit map
    Q_EMIT hint(tr("Unit map \"%1\" saved in the project.").arg(QString::fromStdString(stem)));
    return true;
}

void MappingPanel::openAnyway() {
    if (pendingId_ < 0) {
        return;
    }
    formats::MappingDecision d = decision();
    d.openAnyway = true;
    submit(std::move(d), nullptr);
}

void MappingPanel::cancel() {
    if (pendingId_ < 0) {
        return;
    }
    loader_.cancelMapping(pendingId_);
    error_.clear();
    finish();
}

} // namespace esses::widgets
