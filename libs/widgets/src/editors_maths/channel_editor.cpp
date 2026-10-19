// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelEditor (06 section 13.1, D-010, D-013): the channel table of every open source with the project's
// overrides applied, single-channel edits as SetOverrideCommand / ClearOverrideCommand, the unit rule of 06
// section 13.1 item 6 at edit time, and the Aliases / Mappings editors.

#include <esses/base/casefold.hpp>
#include <esses/base/utf8.hpp>
#include <esses/model/channel.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/source.hpp>
#include <esses/plotcore/display_units.hpp>
#include <esses/project/channel_overrides.hpp>
#include <esses/qtmodel/channels_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/store/types.hpp>
#include <esses/units/unit_registry.hpp>
#include <esses/widgets/editors_data/channel_editor.hpp>
#include <esses/widgets/editors_maths/channel_documents_dialogs.hpp>

#include <QAbstractTableModel>
#include <QColor>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QtGlobal>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace esses::widgets {

namespace {

using Col = ChannelEditor::Column;
constexpr int kSortRole = Qt::UserRole;
constexpr int kQuantityRole = Qt::UserRole + 1;
constexpr std::array<const char*, 4> kInterpNames{"Linear", "Closest", "Previous", "Next"};

QString qs(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

QString categoryName(model::ChannelSource s) {
    switch (s) {
    case model::ChannelSource::Logged:
        return ChannelEditor::tr("Logged");
    case model::ChannelSource::Maths:
        return ChannelEditor::tr("Maths");
    case model::ChannelSource::Detail:
        return ChannelEditor::tr("Detail");
    case model::ChannelSource::Alias:
        return ChannelEditor::tr("Alias");
    case model::ChannelSource::Mapped:
        return ChannelEditor::tr("Mapped");
    case model::ChannelSource::Constant:
        return ChannelEditor::tr("Constant");
    case model::ChannelSource::Setup:
        return ChannelEditor::tr("Setup");
    }
    return ChannelEditor::tr("Logged");
}

// One channel of one open source.
struct ChannelRow {
    model::SourceId source;
    QString sourceName;
    std::string name;
    QString logged;
    QString category;
    QString type;
    std::optional<double> rateHz;
    units::UnitId unit = units::kNoUnit; // stored (base) unit
    bool unitKnown = true;               // false for a maths output that has no descriptor yet
    int dps = 2;
    int colour = -1;
    store::Interp interp = store::Interp::Linear;
};

// The effective display settings of a row: the override where it applies, else the channel's own.
struct Effective {
    units::UnitId displayUnit = units::kNoUnit;
    std::optional<double> min;
    std::optional<double> max;
    int dps = 2;
    int colour = -1;
    QString interp;
    bool overridden = false;
};

class ChannelTableModel final : public QAbstractTableModel {
public:
    using Editor = std::function<bool(const ChannelRow& row, int column, const QVariant& value)>;

    ChannelTableModel(qt::ProjectSession& session, std::shared_ptr<const units::UnitRegistry> units,
                      QObject* parent)
        : QAbstractTableModel(parent), session_(session), units_(std::move(units)) {}

    void setEditor(Editor editor) { editor_ = std::move(editor); }

    void setRows(std::vector<ChannelRow> rows) {
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
    }
    const std::vector<ChannelRow>& rows() const noexcept { return rows_; }

    void overridesChanged() {
        if (!rows_.empty()) {
            Q_EMIT dataChanged(index(0, 0), index(static_cast<int>(rows_.size()) - 1, Col::ColumnCount - 1));
        }
    }

    Effective effective(const ChannelRow& r) const {
        Effective e;
        // Without an override, the unit the components show the channel in (D-211 (a): a fraction stored in
        // "ratio" shows in %).
        e.displayUnit = r.unitKnown && units_ != nullptr ? plot::defaultDisplayUnit(*units_, r.unit) : r.unit;
        e.dps = r.dps;
        e.colour = r.colour;
        e.interp = qs(store::interpName(r.interp));
        const project::ChannelOverrides::Entry* o = session_.overrides().find(r.name);
        if (o == nullptr) {
            return e;
        }
        e.overridden = true;
        const units::UnitRegistry* reg = units_.get();
        if (o->displayUnit && reg != nullptr) {
            // A display unit of another family than this file's channel does not apply here (D-010).
            if (const auto u = reg->find(*o->displayUnit);
                u && (!r.unitKnown || reg->converter(r.unit, *u))) {
                e.displayUnit = *u;
            }
        }
        const auto convert = [&](const std::optional<model::Quantityd>& q) -> std::optional<double> {
            if (!q || reg == nullptr) {
                return std::nullopt;
            }
            units::UnitId from = q->unit;
            if ((from == units::kNoUnit || !reg->contains(from)) && !q->symbol.empty()) {
                from = reg->find(q->symbol).value_or(units::kNoUnit);
            }
            if (const Result<units::Converter> c = reg->converter(from, e.displayUnit)) {
                return c.value()(q->v);
            }
            return q->v;
        };
        e.min = convert(o->scaleMin);
        e.max = convert(o->scaleMax);
        if (o->dps) {
            e.dps = *o->dps;
        }
        if (o->colourIndex) {
            e.colour = *o->colourIndex;
        }
        if (o->interp) {
            e.interp = qs(*o->interp);
        }
        return e;
    }

    QString unitSymbol(units::UnitId id) const {
        if (units_ == nullptr || id == units::kNoUnit || !units_->contains(id)) {
            return {};
        }
        return qs(units_->unit(id).symbol);
    }

    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }
    int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : Col::ColumnCount;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        static const std::array<const char*, Col::ColumnCount> kHeaders{
            "Source", "Name", "Logged name", "Category", "Rate",   "Type",    "Unit",
            "Min",    "Max",  "DPS",         "Colour",   "Interp", "Override"};
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 ||
            section >= Col::ColumnCount) {
            return {};
        }
        return ChannelEditor::tr(kHeaders[static_cast<std::size_t>(section)]);
    }

    Qt::ItemFlags flags(const QModelIndex& index) const override {
        Qt::ItemFlags f = QAbstractTableModel::flags(index);
        switch (index.column()) {
        case Col::UnitCol:
        case Col::MinCol:
        case Col::MaxCol:
        case Col::DpsCol:
        case Col::ColourCol:
        case Col::InterpCol:
            f |= Qt::ItemIsEditable;
            break;
        default:
            break;
        }
        return f;
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() >= static_cast<int>(rows_.size())) {
            return {};
        }
        const ChannelRow& r = rows_[static_cast<std::size_t>(index.row())];
        if (role == kQuantityRole) {
            return r.unitKnown && units_ != nullptr ? QVariant::fromValue<uint>(units_->quantityOf(r.unit))
                                                    : QVariant();
        }
        const Effective e = effective(r);
        const auto number = [&e](const std::optional<double>& v) {
            return v ? QString::number(*v, 'f', std::clamp(e.dps, 0, 9)) : QString();
        };
        switch (role) {
        case Qt::DisplayRole:
        case Qt::EditRole:
            switch (index.column()) {
            case Col::SourceCol:
                return r.sourceName;
            case Col::NameCol:
                return qs(r.name);
            case Col::LoggedCol:
                return r.logged;
            case Col::CategoryCol:
                return r.category;
            case Col::RateCol:
                return r.rateHz ? QString::number(*r.rateHz, 'g', 6) + QStringLiteral(" Hz") : QString();
            case Col::TypeCol:
                return r.type;
            case Col::UnitCol:
                return r.unitKnown ? unitSymbol(e.displayUnit) : QString();
            case Col::MinCol:
                return role == Qt::EditRole && e.min ? QLocale::c().toString(*e.min, 'g', 12) : number(e.min);
            case Col::MaxCol:
                return role == Qt::EditRole && e.max ? QLocale::c().toString(*e.max, 'g', 12) : number(e.max);
            case Col::DpsCol:
                return e.dps;
            case Col::ColourCol:
                return role == Qt::EditRole
                           ? QVariant(e.colour)
                           : (e.colour < 0 ? ChannelEditor::tr("Auto") : QString::number(e.colour));
            case Col::InterpCol:
                return e.interp;
            case Col::OverrideCol:
                return e.overridden ? QStringLiteral("●") : QString();
            default:
                return {};
            }
        case kSortRole:
            switch (index.column()) {
            case Col::RateCol:
                return r.rateHz.value_or(-1.0);
            case Col::MinCol:
                return e.min.value_or(-1.0e300);
            case Col::MaxCol:
                return e.max.value_or(-1.0e300);
            case Col::DpsCol:
                return e.dps;
            case Col::ColourCol:
                return e.colour;
            default:
                return data(index, Qt::DisplayRole);
            }
        case Qt::ToolTipRole:
            if (index.column() == Col::OverrideCol && e.overridden) {
                return ChannelEditor::tr("Display settings from channels/overrides.json");
            }
            return {};
        default:
            return {};
        }
    }

    bool setData(const QModelIndex& index, const QVariant& value, int role) override {
        if (role != Qt::EditRole || !index.isValid() || index.row() >= static_cast<int>(rows_.size()) ||
            !editor_) {
            return false;
        }
        // The editor pushes a command; documentsChanged refreshes every row.
        return editor_(rows_[static_cast<std::size_t>(index.row())], index.column(), value);
    }

private:
    qt::ProjectSession& session_;
    std::shared_ptr<const units::UnitRegistry> units_;
    std::vector<ChannelRow> rows_;
    Editor editor_;
};

class ChannelProxy final : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    std::function<bool()> loose;

    void setQuery(const QString& query) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        beginFilterChange();
        query_ = query.trimmed();
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
        query_ = query.trimmed();
        invalidateRowsFilter();
#endif
    }

    bool filterAcceptsRow(int row, const QModelIndex& parent) const override {
        if (query_.isEmpty()) {
            return true;
        }
        const QString name = sourceModel()->index(row, Col::NameCol, parent).data().toString();
        return qt::ChannelsModel::filterAccepts(name, query_, loose && loose());
    }

private:
    QString query_;
};

// The Unit cell edits through a combo of the channel's quantity's units, the Interp cell through the four
// policies; the other cells use the default editors.
class ChannelDelegate final : public QStyledItemDelegate {
public:
    ChannelDelegate(std::shared_ptr<const units::UnitRegistry> units, QObject* parent)
        : QStyledItemDelegate(parent), units_(std::move(units)) {}

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const override {
        if (index.column() == Col::UnitCol) {
            auto* box = new QComboBox(parent);
            const QVariant quantity = index.data(kQuantityRole);
            if (quantity.isValid() && units_ != nullptr) {
                const auto qid = static_cast<units::QuantityId>(quantity.toUInt());
                for (const units::UnitId u : units_->unitsOf(qid)) {
                    if (!units_->unit(u).isCompat()) {
                        box->addItem(qs(units_->unit(u).symbol));
                    }
                }
            }
            return box;
        }
        if (index.column() == Col::InterpCol) {
            auto* box = new QComboBox(parent);
            for (const char* name : kInterpNames) {
                box->addItem(QString::fromLatin1(name));
            }
            return box;
        }
        return QStyledItemDelegate::createEditor(parent, option, index);
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        if (auto* box = qobject_cast<QComboBox*>(editor)) {
            const int at = box->findText(index.data(Qt::EditRole).toString());
            box->setCurrentIndex(std::max(at, 0));
            return;
        }
        QStyledItemDelegate::setEditorData(editor, index);
    }

    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override {
        if (auto* box = qobject_cast<QComboBox*>(editor)) {
            model->setData(index, box->currentText(), Qt::EditRole);
            return;
        }
        QStyledItemDelegate::setModelData(editor, model, index);
    }

private:
    std::shared_ptr<const units::UnitRegistry> units_;
};

} // namespace

// ---- Impl -------------------------------------------------------------------------------------------------

struct ChannelEditor::Impl {
    explicit Impl(ChannelEditor& editor) : q(editor) {}

    ChannelEditor& q;
    ChannelTableModel* model = nullptr;
    ChannelProxy* proxy = nullptr;
    QLineEdit* filter = nullptr;
    QLabel* message = nullptr;
    QTimer* refreshTimer = nullptr;
    QPointer<AliasesDialog> aliases;
    QPointer<MappingsDialog> mappings;
    QString refusal;

    std::shared_ptr<const units::UnitRegistry> units() const {
        if (q.maths_ != nullptr && q.maths_->units() != nullptr) {
            return q.maths_->units();
        }
        return q.session_.units();
    }

    std::vector<ChannelRow> buildRows() const {
        std::vector<ChannelRow> rows;
        const auto reg = units();
        for (const auto& src : q.loader_.sources()) {
            const model::DataLayer* layer = src->layer(model::kNormalLayer);
            if (layer == nullptr) {
                continue;
            }
            const std::string file = pathToUtf8(src->prov.path.filename());
            const QString sourceName = file.empty() ? qs(src->id.hex().substr(0, 8)) : qs(file);
            std::set<std::string> seen;
            const auto add = [&](const model::ChannelDescriptor& d, bool unitKnown) {
                ChannelRow r;
                r.source = src->id;
                r.sourceName = sourceName;
                r.name = d.name;
                r.logged = d.loggedName != d.name ? qs(d.loggedName) : QString();
                r.category = categoryName(d.source);
                r.type = qs(store::dtypeName(d.dtype)) + QLatin1Char(' ') + qs(store::sampleKindName(d.kind));
                if (d.rate && d.rate->valid()) {
                    r.rateHz = d.rate->hz();
                }
                r.unit = d.unit;
                r.unitKnown = unitKnown;
                r.dps = d.dps;
                r.colour = d.colourIndex;
                r.interp = d.interp;
                rows.push_back(std::move(r));
            };
            for (const model::Channel& c : layer->channels) {
                if (seen.insert(caseFold(c.desc.name)).second) {
                    add(c.desc, true);
                }
            }
            if (q.maths_ != nullptr) {
                for (const maths::ChannelKey& key : q.maths_->outputs(src->id, model::kNormalLayer)) {
                    const std::optional<model::ChannelDescriptor> d = q.maths_->descriptor(key);
                    if (!d || !seen.insert(caseFold(d->name)).second) {
                        continue; // a self-maths output is the logged channel's row
                    }
                    model::ChannelDescriptor desc = *d;
                    desc.source = model::ChannelSource::Maths;
                    add(desc,
                        reg != nullptr && (desc.unit != units::kNoUnit || desc.quantity == units::kUnitless));
                }
            }
        }
        return rows;
    }

    // Every open file carrying `channel` whose channel cannot show `unit` (another quantity family).
    QStringList conflicts(const std::string& channel, units::UnitId unit) const {
        QStringList files;
        const auto reg = units();
        if (reg == nullptr) {
            return files;
        }
        const std::string cf = caseFold(channel);
        for (const ChannelRow& r : model->rows()) {
            if (caseFold(r.name) != cf || !r.unitKnown) {
                continue;
            }
            if (!reg->converter(r.unit, unit) && !files.contains(r.sourceName)) {
                files << r.sourceName;
            }
        }
        return files;
    }

    // The row of the first source that carries `channel`, or nullptr.
    const ChannelRow* rowOf(const std::string& channel) const {
        const std::string cf = caseFold(channel);
        for (const ChannelRow& r : model->rows()) {
            if (caseFold(r.name) == cf) {
                return &r;
            }
        }
        return nullptr;
    }

    project::ChannelOverrides::Entry entryOf(const std::string& channel) const {
        const project::ChannelOverrides::Entry* o = q.session_.overrides().find(channel);
        return o != nullptr ? *o : project::ChannelOverrides::Entry{};
    }

    bool refuse(const QString& reason, QString* out) {
        refusal = reason;
        message->setText(reason);
        message->setVisible(!reason.isEmpty());
        if (out != nullptr) {
            *out = reason;
        }
        Q_EMIT q.editRefused(reason);
        return false;
    }

    bool push(const std::string& channel, const project::ChannelOverrides::Entry& entry) {
        refusal.clear();
        message->hide();
        const project::ChannelOverrides::Entry* current = q.session_.overrides().find(channel);
        if ((current == nullptr && entry.empty()) || (current != nullptr && *current == entry)) {
            return true;
        }
        if (entry.empty()) {
            q.session_.undoStack().push(new qt::ClearOverrideCommand(q.session_, channel));
        } else {
            q.session_.undoStack().push(new qt::SetOverrideCommand(q.session_, channel, entry));
        }
        return true;
    }

    // The display unit an edit of `channel` is expressed in (the effective one of its first row).
    units::UnitId displayUnitOf(const std::string& channel) const {
        const ChannelRow* r = rowOf(channel);
        return r == nullptr ? units::kNoUnit : model->effective(*r).displayUnit;
    }

    bool edit(const ChannelRow& row, int column, const QVariant& value) {
        const QString channel = qs(row.name);
        switch (column) {
        case Col::UnitCol:
            return q.setDisplayUnit(channel, value.toString());
        case Col::MinCol:
        case Col::MaxCol: {
            const QString text = value.toString().trimmed();
            if (text.isEmpty()) {
                return q.setLimit(channel, column == Col::MaxCol, std::nullopt);
            }
            bool ok = false;
            double v = QLocale::c().toDouble(text, &ok);
            if (!ok) {
                v = QLocale().toDouble(text, &ok);
            }
            if (!ok || !std::isfinite(v)) {
                return refuse(ChannelEditor::tr("'%1' is not a number.").arg(text), nullptr);
            }
            return q.setLimit(channel, column == Col::MaxCol, v);
        }
        case Col::DpsCol:
            return q.setDps(channel, value.toInt());
        case Col::ColourCol: {
            const int c = value.toInt();
            return q.setColourIndex(channel, c < 0 ? std::nullopt : std::optional<int>(c));
        }
        case Col::InterpCol:
            return q.setInterp(channel, value.toString());
        default:
            return false;
        }
    }
};

ChannelEditor::ChannelEditor(qt::ProjectSession& session, qt::SourceLoader& loader, qt::MathsService* maths,
                             QWidget* parent)
    : QDialog(parent), d_(std::make_unique<Impl>(*this)), session_(session), loader_(loader), maths_(maths) {
    setWindowTitle(tr("Channel Editor"));
    setModal(false);
    setSizeGripEnabled(true);
    resize(1100, 560);
    auto* layout = new QVBoxLayout(this);
    d_->filter = new QLineEdit(this);
    d_->filter->setPlaceholderText(tr("Search channels"));
    d_->filter->setClearButtonEnabled(true);
    d_->filter->setProperty("esses.textEntry", true);
    d_->filter->setAccessibleName(tr("Channel search"));
    layout->addWidget(d_->filter);

    d_->model = new ChannelTableModel(session_, d_->units(), this);
    d_->model->setEditor([this](const ChannelRow& row, int column, const QVariant& value) {
        return d_->edit(row, column, value);
    });
    d_->proxy = new ChannelProxy(this);
    d_->proxy->loose = [this] { return session_.looseChannelSearch(); };
    d_->proxy->setSourceModel(d_->model);
    d_->proxy->setSortRole(kSortRole);
    d_->proxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    table_ = new QTableView(this);
    table_->setModel(d_->proxy);
    table_->setItemDelegate(new ChannelDelegate(d_->units(), table_));
    table_->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
    table_->setSortingEnabled(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection); // multi-select edit is v1.0
    table_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                            QAbstractItemView::SelectedClicked);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setAccessibleName(tr("Channels"));
    layout->addWidget(table_, 1);

    d_->message = new QLabel(this);
    d_->message->setWordWrap(true);
    QPalette pal = d_->message->palette();
    pal.setColor(QPalette::WindowText, QColor(0xD3, 0x2F, 0x2F));
    d_->message->setPalette(pal);
    d_->message->hide();
    layout->addWidget(d_->message);

    auto* buttons = new QHBoxLayout;
    const auto make = [this](const QString& text) {
        auto* b = new QPushButton(text, this);
        b->setAutoDefault(false);
        return b;
    };
    auto* clear = make(tr("Clear override"));
    auto* aliasesButton = make(tr("Aliases..."));
    auto* mappingsButton = make(tr("Mappings..."));
    auto* close = make(tr("Close"));
    buttons->addWidget(clear);
    buttons->addWidget(aliasesButton);
    buttons->addWidget(mappingsButton);
    buttons->addStretch(1);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    d_->refreshTimer = new QTimer(this);
    d_->refreshTimer->setSingleShot(true);
    d_->refreshTimer->setInterval(100);

    connect(d_->filter, &QLineEdit::textChanged, this,
            [this](const QString& text) { d_->proxy->setQuery(text); });
    connect(clear, &QPushButton::clicked, this, [this] {
        const QString channel = currentChannel();
        if (!channel.isEmpty()) {
            static_cast<void>(clearOverride(channel));
        }
    });
    connect(aliasesButton, &QPushButton::clicked, this, [this] { openAliases(); });
    connect(mappingsButton, &QPushButton::clicked, this, [this] { openMappings(); });
    connect(close, &QPushButton::clicked, this, [this] { this->close(); });
    connect(&session_, &qt::ProjectSession::documentsChanged, this,
            [this] { d_->model->overridesChanged(); });
    connect(&loader_, &qt::SourceLoader::sourcePublished, this, [this] { refresh(); });
    connect(&loader_, &qt::SourceLoader::sourceReplaced, this, [this] { refresh(); });
    connect(&loader_, &qt::SourceLoader::sourceClosed, this, [this] { refresh(); });
    if (maths_ != nullptr) {
        connect(maths_, &qt::MathsService::statusChanged, d_->refreshTimer, [this] {
            if (!d_->refreshTimer->isActive()) {
                d_->refreshTimer->start();
            }
        });
        connect(d_->refreshTimer, &QTimer::timeout, this, [this] { refresh(); });
    }
    refresh();
}

ChannelEditor::~ChannelEditor() {
    // ~QWidget deletes the children after the Impl is gone: their signals must not reach it any more.
    for (QObject* child : findChildren<QObject*>()) {
        child->disconnect(this);
    }
    d_->model->setEditor({});
    d_->proxy->loose = {};
}

QLineEdit& ChannelEditor::filterEdit() noexcept {
    return *d_->filter;
}

QAbstractItemModel& ChannelEditor::channelModel() noexcept {
    return *d_->model;
}

void ChannelEditor::refresh() {
    // Keep the current row (by source and name) across the rebuild.
    std::optional<std::pair<model::SourceId, std::string>> current;
    if (const QModelIndex i = table_->currentIndex(); i.isValid()) {
        const int row = d_->proxy->mapToSource(i).row();
        if (row >= 0 && row < static_cast<int>(d_->model->rows().size())) {
            const ChannelRow& r = d_->model->rows()[static_cast<std::size_t>(row)];
            current = std::make_pair(r.source, r.name);
        }
    }
    std::vector<ChannelRow> rows = d_->buildRows();
    const auto& old = d_->model->rows();
    const bool same =
        rows.size() == old.size() &&
        std::equal(rows.begin(), rows.end(), old.begin(), [](const ChannelRow& a, const ChannelRow& b) {
            return a.source == b.source && a.name == b.name && a.unit == b.unit &&
                   a.unitKnown == b.unitKnown && a.rateHz == b.rateHz && a.dps == b.dps && a.type == b.type;
        });
    if (same) {
        d_->model->overridesChanged();
        return;
    }
    d_->model->setRows(std::move(rows));
    if (current) {
        const auto& now = d_->model->rows();
        for (std::size_t i = 0; i < now.size(); ++i) {
            if (now[i].source == current->first && now[i].name == current->second) {
                const QModelIndex at =
                    d_->proxy->mapFromSource(d_->model->index(static_cast<int>(i), Col::NameCol));
                table_->setCurrentIndex(at);
                break;
            }
        }
    }
}

bool ChannelEditor::focusChannel(const QString& channel) {
    const std::string cf = caseFold(channel.toStdString());
    const auto& rows = d_->model->rows();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (caseFold(rows[i].name) != cf) {
            continue;
        }
        QModelIndex at = d_->proxy->mapFromSource(d_->model->index(static_cast<int>(i), Col::NameCol));
        if (!at.isValid()) {
            d_->filter->clear();
            at = d_->proxy->mapFromSource(d_->model->index(static_cast<int>(i), Col::NameCol));
        }
        table_->setCurrentIndex(at);
        table_->selectRow(at.row());
        table_->scrollTo(at);
        return true;
    }
    return false;
}

QString ChannelEditor::currentChannel() const {
    const QModelIndex i = table_->currentIndex();
    if (!i.isValid()) {
        return {};
    }
    const int row = d_->proxy->mapToSource(i).row();
    return row >= 0 && row < static_cast<int>(d_->model->rows().size())
               ? qs(d_->model->rows()[static_cast<std::size_t>(row)].name)
               : QString();
}

bool ChannelEditor::setDisplayUnit(const QString& channel, const QString& unit, QString* refusal) {
    const std::string name = channel.toStdString();
    project::ChannelOverrides::Entry entry = d_->entryOf(name);
    const QString text = unit.trimmed();
    if (text.isEmpty()) {
        entry.displayUnit.reset();
        return d_->push(name, entry);
    }
    const auto reg = d_->units();
    const std::optional<units::UnitId> id = reg != nullptr ? reg->find(text.toStdString()) : std::nullopt;
    if (!id) {
        return d_->refuse(tr("Unknown unit '%1'.").arg(text), refusal);
    }
    const ChannelRow* row = d_->rowOf(name);
    if (row != nullptr && !row->unitKnown) {
        return d_->refuse(tr("The unit of '%1' is not known until it has been calculated.").arg(channel),
                          refusal);
    }
    // 06 section 13.1 item 6: the unit must apply to every open file that carries the channel.
    if (const QStringList files = d_->conflicts(name, *id); !files.isEmpty()) {
        return d_->refuse(tr("The unit '%1' cannot apply to '%2' in: %3")
                              .arg(qs(reg->unit(*id).symbol), channel, files.join(QStringLiteral(", "))),
                          refusal);
    }
    entry.displayUnit = reg->unit(*id).symbol;
    return d_->push(name, entry);
}

bool ChannelEditor::setLimit(const QString& channel, bool max, std::optional<double> valueInDisplayUnit,
                             QString* refusal) {
    const std::string name = channel.toStdString();
    project::ChannelOverrides::Entry entry = d_->entryOf(name);
    std::optional<model::Quantityd>& slot = max ? entry.scaleMax : entry.scaleMin;
    if (!valueInDisplayUnit) {
        slot.reset();
        return d_->push(name, entry);
    }
    if (!std::isfinite(*valueInDisplayUnit)) {
        return d_->refuse(tr("The limit is not a finite number."), refusal);
    }
    const auto reg = d_->units();
    const units::UnitId unit = d_->displayUnitOf(name);
    model::Quantityd q;
    q.v = *valueInDisplayUnit;
    q.unit = unit;
    q.symbol = reg != nullptr && unit != units::kNoUnit && reg->contains(unit) ? reg->unit(unit).symbol
                                                                               : std::string();
    if (const ChannelRow* row = d_->rowOf(name)) {
        q.dps = d_->model->effective(*row).dps;
    }
    slot = std::move(q);
    return d_->push(name, entry);
}

bool ChannelEditor::setDps(const QString& channel, std::optional<int> dps) {
    const std::string name = channel.toStdString();
    project::ChannelOverrides::Entry entry = d_->entryOf(name);
    entry.dps = dps ? std::optional<int>(std::clamp(*dps, 0, 9)) : std::nullopt;
    return d_->push(name, entry);
}

bool ChannelEditor::setColourIndex(const QString& channel, std::optional<int> colour) {
    const std::string name = channel.toStdString();
    project::ChannelOverrides::Entry entry = d_->entryOf(name);
    entry.colourIndex = colour;
    return d_->push(name, entry);
}

bool ChannelEditor::setInterp(const QString& channel, const QString& interp, QString* refusal) {
    const std::string name = channel.toStdString();
    project::ChannelOverrides::Entry entry = d_->entryOf(name);
    const QString text = interp.trimmed();
    if (text.isEmpty()) {
        entry.interp.reset();
        return d_->push(name, entry);
    }
    const auto it = std::find_if(kInterpNames.begin(), kInterpNames.end(), [&text](const char* n) {
        return text.compare(QString::fromLatin1(n), Qt::CaseInsensitive) == 0;
    });
    if (it == kInterpNames.end()) {
        return d_->refuse(tr("Unknown interpolation '%1' (Linear, Closest, Previous or Next).").arg(text),
                          refusal);
    }
    entry.interp = std::string(*it);
    return d_->push(name, entry);
}

bool ChannelEditor::clearOverride(const QString& channel) {
    if (session_.overrides().find(channel.toStdString()) == nullptr) {
        return false;
    }
    return d_->push(channel.toStdString(), project::ChannelOverrides::Entry{});
}

QString ChannelEditor::lastRefusal() const {
    return d_->refusal;
}

void ChannelEditor::openAliases() {
    if (d_->aliases.isNull()) {
        d_->aliases = new AliasesDialog(session_, this);
    }
    d_->aliases->show();
    d_->aliases->raise();
    d_->aliases->activateWindow();
}

void ChannelEditor::openMappings() {
    if (d_->mappings.isNull()) {
        d_->mappings = new MappingsDialog(session_, this);
    }
    d_->mappings->show();
    d_->mappings->raise();
    d_->mappings->activateWindow();
}

AliasesDialog* ChannelEditor::aliasesDialog() const {
    return d_->aliases.data();
}

MappingsDialog* ChannelEditor::mappingsDialog() const {
    return d_->mappings.data();
}

} // namespace esses::widgets
