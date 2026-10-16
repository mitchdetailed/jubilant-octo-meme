// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LapEditor (06 section 13.3; AC-M2-05): the laps table, the beacon edits and the stretch report; SHIFT+F7
// and the toolbar's lap-stretch toggle. See lap_editor.hpp.

#include <esses/analysis/lap_stretch.hpp>
#include <esses/base/uuid.hpp>
#include <esses/model/source.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/lap_stretch_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/qtmodel/workspace_view_model.hpp>
#include <esses/widgets/editors_data/lap_editor.hpp>
#include <esses/widgets/shell/command_actions.hpp>

#include <QAbstractTableModel>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QTableView>
#include <QUndoStack>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>
#include <utility>
#include <vector>

namespace esses::widgets {

namespace {

constexpr std::string_view kDefaultGroup = "Outings:Laps";
constexpr std::string_view kDefaultMarkers = "Beacons";

QString sourceLabel(const model::DataSource& src) {
    if (const model::Detail* d = model::findDetail(src.details, "File Name")) {
        if (const auto* s = std::get_if<std::string>(&d->value)) {
            return QString::fromStdString(*s);
        }
    }
    return QString::fromStdWString(src.prov.path.filename().wstring());
}

bool isIgnoredBeacon(const model::Marker& m, std::string_view group) {
    return m.group == group &&
           (m.cls == model::MarkerClass::IGRDBCN || (m.cls == model::MarkerClass::BCN && !m.enabled));
}

QString provenanceText(const model::Marker& m) {
    if (m.originalT.has_value()) {
        return QObject::tr("Edited");
    }
    switch (m.prov) {
    case model::Provenance::Logged:
        return QObject::tr("Logged");
    case model::Provenance::Companion:
        return QObject::tr("Companion");
    case model::Provenance::User:
        return QObject::tr("User");
    case model::Provenance::Derived:
        break;
    }
    return QObject::tr("Derived");
}

// The beacon insert of the editor and of SHIFT+F7: a user BCN marker in the profile's marker group, one
// InsertMarkerCommand (the laps rebuild synchronously when it is pushed). The marker id, or the reason.
std::optional<model::MarkerId> pushBeacon(qt::ProjectSession& session, qt::SourceLoader& loader,
                                          const model::DataSource& src, const std::string& layer, Micros t,
                                          QString& reason) {
    const model::DataLayer* l = src.layer(layer);
    if (l == nullptr) {
        reason = QObject::tr("The source has no layer \"%1\".").arg(QString::fromStdString(layer));
        return std::nullopt;
    }
    if (t <= 0 || t >= l->duration) {
        reason = QObject::tr("%1 lies outside the data (0:00.000 to %2).")
                     .arg(LapEditor::formatTime(t), LapEditor::formatTime(l->duration));
        return std::nullopt;
    }
    const std::string group = session.profile().primaryMarkers.empty() ? std::string(kDefaultMarkers)
                                                                       : session.profile().primaryMarkers;
    for (const model::Marker& m : l->markers) {
        if (m.t == t && m.group == group && m.cls == model::MarkerClass::BCN) {
            reason = QObject::tr("A beacon already exists at %1.").arg(LapEditor::formatTime(t));
            return std::nullopt;
        }
    }
    model::Marker marker;
    marker.id = model::MarkerId{uuidRandom()};
    marker.cls = model::MarkerClass::BCN;
    marker.group = group;
    marker.t = t;
    marker.prov = model::Provenance::User;
    const model::MarkerId id = marker.id;
    session.undoStack().push(new qt::InsertMarkerCommand(loader, src, layer, std::move(marker)));
    return id;
}

} // namespace

// ---- rows ------------------------------------------------------------------------------------------------

struct LapRow {
    bool ignored = false;                // an ignored beacon, not a lap
    model::RangeRef ref;                 // laps
    model::Range range;                  // laps
    std::optional<model::Marker> marker; // the lap's start marker, or the ignored beacon
    std::optional<analysis::LapStretchRow> stretch;
    Micros t() const noexcept { return ignored && marker ? marker->t : range.start; }
};

class LapRowsModel final : public QAbstractTableModel {
public:
    LapRowsModel(LapEditor& editor, QObject* parent) : QAbstractTableModel(parent), editor_(editor) {}

    std::vector<LapRow> rows;
    QString rangeTerm = QStringLiteral("Lap");

    void reset(std::vector<LapRow> next) {
        beginResetModel();
        rows = std::move(next);
        endResetModel();
    }
    bool valid(int row) const { return row >= 0 && row < static_cast<int>(rows.size()); }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows.size());
    }
    int columnCount(const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : LapEditor::ColumnCount;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return QAbstractTableModel::headerData(section, orientation, role);
        }
        switch (section) {
        case LapEditor::NumberCol:
            return rangeTerm;
        case LapEditor::ClassCol:
            return tr("Class");
        case LapEditor::StartCol:
            return tr("Start");
        case LapEditor::DurationCol:
            return tr("Duration");
        case LapEditor::TrustedCol:
            return tr("Trusted");
        case LapEditor::StretchCol:
            return tr("Stretch");
        case LapEditor::ProvenanceCol:
            return tr("Provenance");
        default:
            return {};
        }
    }

    Qt::ItemFlags flags(const QModelIndex& index) const override {
        Qt::ItemFlags f = QAbstractTableModel::flags(index);
        if (valid(index.row()) && rows[static_cast<std::size_t>(index.row())].ignored) {
            f &= ~Qt::ItemIsEnabled; // greyed (06 section 13.3 item 7), still listed
        } else if (index.column() == LapEditor::TrustedCol) {
            f |= Qt::ItemIsUserCheckable;
        }
        return f;
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || !valid(index.row())) {
            return {};
        }
        const LapRow& r = rows[static_cast<std::size_t>(index.row())];
        const int col = index.column();
        if (role == Qt::CheckStateRole) {
            return col == LapEditor::TrustedCol && !r.ignored
                       ? QVariant(r.range.trusted ? Qt::Checked : Qt::Unchecked)
                       : QVariant();
        }
        if (role == Qt::FontRole && !r.ignored && !r.range.trusted) {
            QFont f;
            f.setItalic(true);
            return f;
        }
        if (role == Qt::ForegroundRole) {
            const bool stretchOff = col == LapEditor::StretchCol && r.stretch && !r.stretch->applied;
            if (r.ignored || stretchOff) {
                return QApplication::palette().color(QPalette::Disabled, QPalette::Text);
            }
            return {};
        }
        if (role == Qt::TextAlignmentRole) {
            switch (col) {
            case LapEditor::NumberCol:
            case LapEditor::StartCol:
            case LapEditor::DurationCol:
            case LapEditor::StretchCol:
                return QVariant((Qt::AlignRight | Qt::AlignVCenter).toInt());
            default:
                return {};
            }
        }
        if (role == Qt::ToolTipRole) {
            if (r.ignored) {
                return tr("Ignored beacon: it starts no %1").arg(rangeTerm.toLower());
            }
            if (col == LapEditor::StretchCol && r.stretch) {
                const analysis::LapStretchRow& s = *r.stretch;
                if (s.trackLengthM <= 0.0) {
                    return tr("No track length for this source");
                }
                return tr("Measured %1 m, track %2 m%3")
                    .arg(s.lengthM, 0, 'f', 1)
                    .arg(s.trackLengthM, 0, 'f', 1)
                    .arg(s.withinTolerance ? (s.applied ? QString() : tr(" (stretching is off)"))
                                           : tr(" (outside the tolerance)"));
            }
            return QString::fromStdString(r.range.name);
        }
        if (role != Qt::DisplayRole) {
            return {};
        }
        switch (col) {
        case LapEditor::NumberCol:
            return r.ignored ? QString() : QString::number(r.range.number);
        case LapEditor::ClassCol:
            if (r.ignored) {
                return tr("Ignored");
            }
            {
                const std::string_view name = model::lapClassName(r.range.lapClass);
                return QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
            }
        case LapEditor::StartCol:
            return LapEditor::formatTime(r.t());
        case LapEditor::DurationCol:
            return r.ignored ? QString() : LapEditor::formatTime(r.range.duration());
        case LapEditor::StretchCol: {
            // Blank outside the tolerance (06 section 13.3 item 1); greyed while stretching is off.
            if (r.ignored || !r.stretch || !r.stretch->withinTolerance) {
                return QString();
            }
            const double pct = (r.stretch->candidate - 1.0) * 100.0;
            return (pct >= 0.0 ? QStringLiteral("+") : QString()) + QString::number(pct, 'f', 2) +
                   QStringLiteral(" %");
        }
        case LapEditor::ProvenanceCol:
            return r.marker ? provenanceText(*r.marker) : QString();
        default:
            return {};
        }
    }

    bool setData(const QModelIndex& index, const QVariant& value, int role) override {
        if (role != Qt::CheckStateRole || index.column() != LapEditor::TrustedCol || !valid(index.row())) {
            return false;
        }
        return editor_.setTrusted(index.row(), value.toInt() == Qt::Checked);
    }

private:
    LapEditor& editor_;
};

// ---- LapEditor
// ---------------------------------------------------------------------------------------------

LapEditor::LapEditor(qt::ProjectSession& session, qt::SourceLoader& loader, qt::DataSelectionModel& selection,
                     qt::WorkspaceViewModel* workspace, QWidget* parent)
    : QDialog(parent), session_(session), loader_(loader), selection_(selection), workspace_(workspace) {
    setModal(false);
    setObjectName(QStringLiteral("lapEditor"));
    // Terminology from the profile (D-036): {"range": "Lap", "ranges": "Laps", "editor": "Lap Editor"}.
    const QJsonObject terms =
        QJsonDocument::fromJson(QByteArray::fromStdString(session_.profile().terms.compact())).object();
    rangeTerm_ = terms.value(QStringLiteral("range")).toString(tr("Lap"));
    rangesTerm_ = terms.value(QStringLiteral("ranges")).toString(tr("Laps"));
    setWindowTitle(terms.value(QStringLiteral("editor")).toString(tr("Lap Editor")));

    rows_ = new LapRowsModel(*this, this);
    rows_->rangeTerm = rangeTerm_;
    sourceCombo_ = new QComboBox(this);
    sourceCombo_->setObjectName(QStringLiteral("lapSource"));
    sourceCombo_->setAccessibleName(tr("Source"));
    table_ = new QTableView(this);
    table_->setObjectName(QStringLiteral("lapTable"));
    table_->setAccessibleName(rangesTerm_);
    table_->setModel(rows_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setStretchLastSection(true);

    // Insert beacon (06 section 13.3 item 2).
    auto* insertBox = new QGroupBox(tr("Insert beacon"), this);
    insertTime_ = new QLineEdit(insertBox);
    insertTime_->setObjectName(QStringLiteral("insertTime"));
    insertTime_->setPlaceholderText(tr("m:ss.fff or seconds"));
    bySessionTime_ = new QRadioButton(tr("At session time"), insertBox);
    byLapTime_ = new QRadioButton(tr("%1 time after the selected %2").arg(rangeTerm_, rangeTerm_.toLower()),
                                  insertBox);
    bySessionTime_->setChecked(true);
    insertButton_ = new QPushButton(tr("Insert"), insertBox);
    insertButton_->setObjectName(QStringLiteral("insertBeacon"));
    cursorButton_ = new QPushButton(tr("At Cursor"), insertBox);
    cursorButton_->setObjectName(QStringLiteral("insertAtCursor"));
    cursorButton_->setToolTip(tr("Insert a beacon at the main cursor (Shift+F7 in a graph)"));
    auto* insertLayout = new QGridLayout(insertBox);
    insertLayout->addWidget(new QLabel(tr("Time:"), insertBox), 0, 0);
    insertLayout->addWidget(insertTime_, 0, 1);
    insertLayout->addWidget(insertButton_, 0, 2);
    insertLayout->addWidget(bySessionTime_, 1, 0, 1, 2);
    insertLayout->addWidget(byLapTime_, 2, 0, 1, 2);
    insertLayout->addWidget(cursorButton_, 1, 2);

    // Beacon offsets (06 section 13.3 item 3).
    auto* offsetBox = new QGroupBox(tr("Beacon offset"), this);
    offsetDistance_ = new QDoubleSpinBox(offsetBox);
    offsetDistance_->setObjectName(QStringLiteral("offsetDistance"));
    offsetDistance_->setRange(-100000.0, 100000.0);
    offsetDistance_->setDecimals(2);
    offsetDistance_->setSuffix(QStringLiteral(" m"));
    offsetDistance_->setToolTip(tr("Positive: the beacon was further along the track than usual. Moves every "
                                   "beacon of the source so its laps line up with other files."));
    offsetDistanceApply_ = new QPushButton(tr("Apply"), offsetBox);
    offsetDistanceApply_->setObjectName(QStringLiteral("offsetDistanceApply"));
    offsetDistanceState_ = new QLabel(offsetBox);
    offsetDistanceState_->setObjectName(QStringLiteral("offsetDistanceState"));
    beaconOffset_ = new QDoubleSpinBox(offsetBox);
    beaconOffset_->setObjectName(QStringLiteral("beaconOffset"));
    beaconOffset_->setRange(-3600.0, 3600.0);
    beaconOffset_->setDecimals(3);
    beaconOffset_->setSuffix(QStringLiteral(" s"));
    beaconOffsetApply_ = new QPushButton(tr("Apply"), offsetBox);
    beaconOffsetApply_->setObjectName(QStringLiteral("beaconOffsetApply"));
    auto* offsetLayout = new QGridLayout(offsetBox);
    offsetLayout->addWidget(new QLabel(tr("By distance (this source):"), offsetBox), 0, 0);
    offsetLayout->addWidget(offsetDistance_, 0, 1);
    offsetLayout->addWidget(offsetDistanceApply_, 0, 2);
    offsetLayout->addWidget(offsetDistanceState_, 1, 0, 1, 3);
    offsetLayout->addWidget(
        new QLabel(tr("Time of the selected %1's beacon:").arg(rangeTerm_.toLower()), offsetBox), 2, 0);
    offsetLayout->addWidget(beaconOffset_, 2, 1);
    offsetLayout->addWidget(beaconOffsetApply_, 2, 2);

    stretchToggle_ = new QCheckBox(tr("Lap distance stretching"), this);
    stretchToggle_->setObjectName(QStringLiteral("lapStretchToggle"));
    reportButton_ = new QPushButton(tr("Stretch Report..."), this);
    reportButton_->setObjectName(QStringLiteral("stretchReport"));
    restoreButton_ = new QPushButton(tr("Restore Logged %1").arg(rangesTerm_), this);
    restoreButton_->setObjectName(QStringLiteral("restoreLogged"));
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("lapEditorStatus"));
    status_->setWordWrap(true);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);

    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Source:"), this));
    top->addWidget(sourceCombo_, 1);
    auto* tools = new QHBoxLayout;
    tools->addWidget(stretchToggle_);
    tools->addWidget(reportButton_);
    tools->addStretch(1);
    tools->addWidget(restoreButton_);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(table_, 1);
    layout->addWidget(insertBox);
    layout->addWidget(offsetBox);
    layout->addLayout(tools);
    layout->addWidget(status_);
    layout->addWidget(close);

    connect(close, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(insertButton_, &QPushButton::clicked, this, &LapEditor::runInsert);
    connect(insertTime_, &QLineEdit::returnPressed, this, &LapEditor::runInsert);
    connect(cursorButton_, &QPushButton::clicked, this,
            [this] { static_cast<void>(insertBeaconAtCursor()); });
    connect(offsetDistanceApply_, &QPushButton::clicked, this,
            [this] { static_cast<void>(applyBeaconOffsetDistance(offsetDistance_->value())); });
    connect(beaconOffsetApply_, &QPushButton::clicked, this, [this] {
        static_cast<void>(setBeaconTimeOffset(
            currentRow(), static_cast<Micros>(std::llround(beaconOffset_->value() * 1'000'000.0))));
    });
    connect(restoreButton_, &QPushButton::clicked, this, [this] { static_cast<void>(restoreLogged()); });
    connect(reportButton_, &QPushButton::clicked, this, [this] {
        auto* report = new QDialog(this);
        report->setAttribute(Qt::WA_DeleteOnClose);
        report->setObjectName(QStringLiteral("stretchReportDialog"));
        report->setWindowTitle(tr("Lap Stretch Report"));
        auto* text = new QPlainTextEdit(stretchReportText(), report);
        text->setReadOnly(true);
        auto* box = new QVBoxLayout(report);
        box->addWidget(text);
        report->resize(480, 320);
        report->show();
    });
    connect(stretchToggle_, &QCheckBox::toggled, this, [this](bool on) {
        if (stretch_ != nullptr && stretch_->enabled() != on) {
            stretch_->setEnabled(on);
        }
    });
    connect(sourceCombo_, &QComboBox::activated, this, [this](int index) {
        const QVariant v = sourceCombo_->itemData(index);
        for (const auto& s : loader_.sources()) {
            if (QString::fromStdString(s->id.hex()) == v.toString()) {
                setSource(s->id);
                return;
            }
        }
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this] { refreshControls(); });

    const auto onSource = [this](const std::shared_ptr<const model::DataSource>& src) {
        refreshSources();
        if (src != nullptr && src->id == source_) {
            rebuild();
        }
    };
    connect(&loader_, &qt::SourceLoader::sourcePublished, this,
            [this, onSource](const std::shared_ptr<const model::DataSource>& src) {
                if (source_ == model::SourceId{} && followMain_ && selection_.hasMain()) {
                    source_ = selection_.snapshot().main.src;
                }
                onSource(src);
                if (src != nullptr && source_ == model::SourceId{}) {
                    source_ = src->id;
                    rebuild();
                }
            });
    connect(&loader_, &qt::SourceLoader::sourceReplaced, this, onSource);
    connect(&loader_, &qt::SourceLoader::sourceClosed, this, [this](const model::SourceId& id) {
        refreshSources();
        if (id == source_) {
            source_ = selection_.hasMain() ? selection_.snapshot().main.src : model::SourceId{};
            rebuild();
        }
    });
    connect(&selection_, &qt::DataSelectionModel::changed, this,
            [this](const plot::SelectionSnapshot& snap, qt::DataSelectionModel::Changes what) {
                if (followMain_ && what.testFlag(qt::DataSelectionModel::Change::Main) &&
                    selection_.hasMain() && snap.main.src != source_) {
                    source_ = snap.main.src;
                    refreshSources();
                    rebuild();
                }
                refreshControls();
            });

    if (selection_.hasMain()) {
        source_ = selection_.snapshot().main.src;
    } else if (const auto all = loader_.sources(); !all.empty()) {
        source_ = all.front()->id;
    }
    setLapStretch(nullptr);
    refreshSources();
    rebuild();
    // ENTER in a field never presses a button (the time field inserts through returnPressed).
    for (QPushButton* b : findChildren<QPushButton*>()) {
        b->setAutoDefault(false);
        b->setDefault(false);
    }
    resize(720, 560);
}

LapEditor::~LapEditor() = default;

std::shared_ptr<const model::DataSource> LapEditor::current() const {
    return loader_.source(source_);
}

std::string LapEditor::layerName() const {
    return std::string(model::kNormalLayer);
}

std::string LapEditor::groupPath() const {
    const std::string& g = session_.profile().primaryRanges;
    return g.empty() ? std::string(kDefaultGroup) : g;
}

std::string LapEditor::markerGroup() const {
    const std::string& g = session_.profile().primaryMarkers;
    return g.empty() ? std::string(kDefaultMarkers) : g;
}

void LapEditor::setSource(const model::SourceId& source) {
    followMain_ = false; // an explicit pick stays until the editor closes
    source_ = source;
    refreshSources();
    rebuild();
}

void LapEditor::setLapStretch(qt::LapStretchService* stretch) {
    if (stretch_ != nullptr) {
        disconnect(stretch_, nullptr, this, nullptr);
    }
    stretch_ = stretch;
    stretchToggle_->setVisible(stretch_ != nullptr);
    reportButton_->setVisible(stretch_ != nullptr);
    table_->setColumnHidden(StretchCol, stretch_ == nullptr);
    if (stretch_ != nullptr) {
        connect(stretch_, &qt::LapStretchService::reportChanged, this, [this](const model::SourceId& id) {
            if (id == source_) {
                rebuild();
            }
        });
        connect(stretch_, &qt::LapStretchService::enabledChanged, this, [this](bool) {
            refreshControls();
            rebuild();
        });
        connect(stretch_, &QObject::destroyed, this, [this] { setLapStretch(nullptr); });
    }
    rebuild();
}

void LapEditor::refreshSources() {
    const QSignalBlocker block(sourceCombo_);
    sourceCombo_->clear();
    for (const auto& s : loader_.sources()) {
        sourceCombo_->addItem(sourceLabel(*s), QString::fromStdString(s->id.hex()));
        if (s->id == source_) {
            sourceCombo_->setCurrentIndex(sourceCombo_->count() - 1);
        }
    }
}

void LapEditor::rebuild() {
    // Keep the highlighted row across a rebuild (by lap id, else by time).
    std::optional<model::RangeId> keepLap;
    std::optional<Micros> keepT;
    if (const int row = currentRow(); rows_->valid(row)) {
        const LapRow& r = rows_->rows[static_cast<std::size_t>(row)];
        if (!r.ignored) {
            keepLap = r.ref.range;
        }
        keepT = r.t();
    }
    std::vector<LapRow> next;
    const auto src = current();
    const model::DataLayer* layer = src == nullptr ? nullptr : src->layer(layerName());
    if (layer != nullptr) {
        std::map<model::RangeId, analysis::LapStretchRow> stretchRows;
        if (stretch_ != nullptr) {
            for (const analysis::LapStretchRow& s : stretch_->report(src->id)) {
                stretchRows.emplace(s.lap, s);
            }
        }
        const std::string path = groupPath();
        if (const model::RangeGroup* g = layer->ranges.find(path)) {
            for (const model::Range& range : g->ranges) {
                LapRow r;
                r.ref = model::RangeRef{src->id, layerName(), path, range.id, range.name};
                r.range = range;
                if (const model::Marker* m = layer->findMarker(range.startMarker)) {
                    r.marker = *m;
                }
                if (const auto it = stretchRows.find(range.id); it != stretchRows.end()) {
                    r.stretch = it->second;
                }
                next.push_back(std::move(r));
            }
        }
        const std::string group = markerGroup();
        for (const model::Marker& m : layer->markers) {
            if (isIgnoredBeacon(m, group)) {
                LapRow r;
                r.ignored = true;
                r.marker = m;
                next.push_back(std::move(r));
            }
        }
        std::stable_sort(next.begin(), next.end(),
                         [](const LapRow& a, const LapRow& b) { return a.t() < b.t(); });
    }
    rows_->reset(std::move(next));
    int select = -1;
    for (int i = 0; i < rowCount(); ++i) {
        const LapRow& r = rows_->rows[static_cast<std::size_t>(i)];
        if ((keepLap && !r.ignored && r.ref.range == *keepLap) || (!keepLap && keepT && r.t() == *keepT)) {
            select = i;
            break;
        }
    }
    if (select < 0 && keepT) { // the lap went away (a beacon insert splits it): the row at its time
        for (int i = 0; i < rowCount(); ++i) {
            if (rows_->rows[static_cast<std::size_t>(i)].t() <= *keepT) {
                select = i;
            }
        }
    }
    setCurrentRow(select);
    refreshControls();
    Q_EMIT rowsChanged();
}

void LapEditor::refreshControls() {
    const auto src = current();
    const bool has = src != nullptr;
    for (QWidget* w : std::initializer_list<QWidget*>{insertButton_, insertTime_, offsetDistanceApply_,
                                                      offsetDistance_, restoreButton_}) {
        w->setEnabled(has);
    }
    cursorButton_->setEnabled(has && workspace_ != nullptr && selection_.hasMain() &&
                              selection_.snapshot().main.src == source_);
    const int row = currentRow();
    const auto lap = lapAt(row);
    const LapRow* r = rows_->valid(row) ? &rows_->rows[static_cast<std::size_t>(row)] : nullptr;
    const bool beacon = r != nullptr && !r->ignored && r->marker && r->marker->cls == model::MarkerClass::BCN;
    beaconOffset_->setEnabled(beacon);
    beaconOffsetApply_->setEnabled(beacon);
    byLapTime_->setEnabled(lap.has_value());
    if (beacon) {
        const QSignalBlocker block(beaconOffset_);
        const Micros dt = r->marker->originalT ? r->marker->t - *r->marker->originalT : 0;
        beaconOffset_->setValue(static_cast<double>(dt) / 1'000'000.0);
    }
    const std::optional<double> dM = beaconOffsetDistance();
    {
        const QSignalBlocker block(offsetDistance_);
        offsetDistance_->setValue(dM.value_or(0.0));
    }
    offsetDistanceState_->setText(
        dM ? (beaconOffsetPending()
                  ? tr("%1 m applied: pending (the beacons move once Corr Dist resolves it)")
                        .arg(*dM, 0, 'f', 2)
                  : tr("%1 m applied").arg(*dM, 0, 'f', 2))
           : tr("No offset"));
    if (stretch_ != nullptr) {
        const QSignalBlocker block(stretchToggle_);
        stretchToggle_->setChecked(stretch_->enabled());
    }
    status_->setText(error_);
}

bool LapEditor::fail(const QString& reason) {
    error_ = reason;
    status_->setText(error_);
    return false;
}

bool LapEditor::succeed() {
    error_.clear();
    status_->setText(error_);
    return true;
}

int LapEditor::rowCount() const {
    return rows_->rowCount();
}

std::optional<model::RangeRef> LapEditor::lapAt(int row) const {
    if (!rows_->valid(row) || rows_->rows[static_cast<std::size_t>(row)].ignored) {
        return std::nullopt;
    }
    return rows_->rows[static_cast<std::size_t>(row)].ref;
}

int LapEditor::rowOf(const model::RangeRef& lap) const {
    for (int i = 0; i < rowCount(); ++i) {
        const LapRow& r = rows_->rows[static_cast<std::size_t>(i)];
        if (!r.ignored && r.ref == lap) {
            return i;
        }
    }
    return -1;
}

int LapEditor::currentRow() const {
    const QModelIndex i = table_->currentIndex();
    return i.isValid() ? i.row() : -1;
}

void LapEditor::setCurrentRow(int row) {
    if (!rows_->valid(row)) {
        table_->setCurrentIndex({});
        return;
    }
    table_->setCurrentIndex(rows_->index(row, NumberCol));
}

// ---- edits
// ---------------------------------------------------------------------------------------------------

bool LapEditor::insertAt(const std::shared_ptr<const model::DataSource>& src, Micros t) {
    if (src == nullptr) {
        return fail(tr("No source is open."));
    }
    QString reason;
    if (!pushBeacon(session_, loader_, *src, layerName(), t, reason)) {
        return fail(reason);
    }
    return succeed();
}

bool LapEditor::insertBeaconAt(Micros t) {
    return insertAt(current(), t);
}

bool LapEditor::insertBeaconAfterLap(int row, Micros lapTime) {
    const auto lap = lapAt(row);
    if (!lap) {
        return fail(tr("Select a %1 first.").arg(rangeTerm_.toLower()));
    }
    const model::Range& range = rows_->rows[static_cast<std::size_t>(row)].range;
    const Micros t = range.start + lapTime;
    if (lapTime <= 0 || t >= range.end) {
        return fail(
            tr("%1 is not inside %2 (%3 long).")
                .arg(formatTime(lapTime), QString::fromStdString(range.name), formatTime(range.duration())));
    }
    return insertAt(current(), t);
}

bool LapEditor::insertBeaconAtCursor() {
    if (workspace_ == nullptr) {
        return fail(tr("No cursor: the editor is not attached to a worksheet."));
    }
    if (!widgets::insertBeaconAtCursor(session_, loader_, selection_, *workspace_)) {
        return fail(tr("No Main %1, or the cursor lies outside its data.").arg(rangeTerm_.toLower()));
    }
    return succeed();
}

void LapEditor::runInsert() {
    const auto t = parseTime(insertTime_->text());
    if (!t) {
        fail(tr("\"%1\" is not a time (m:ss.fff or seconds).").arg(insertTime_->text()));
        return;
    }
    if (byLapTime_->isChecked()) {
        static_cast<void>(insertBeaconAfterLap(currentRow(), *t));
    } else {
        static_cast<void>(insertBeaconAt(*t));
    }
}

std::optional<double> LapEditor::beaconOffsetDistance() const {
    const auto src = current();
    if (src == nullptr || !src->sidecar.hasLayer(layerName())) {
        return std::nullopt;
    }
    for (const model::MarkerEdit& e : src->sidecar.layer(layerName()).markerEdits) {
        if (e.op == model::MarkerEditOp::OffsetDistance) {
            return e.dM;
        }
    }
    return std::nullopt;
}

bool LapEditor::beaconOffsetPending() const {
    // No resolver moves the beacons yet (02 section 4.3 behaviour 7a; docs/milestones/M2.md open issues): an
    // applied offset stays pending.
    return beaconOffsetDistance().has_value();
}

bool LapEditor::applyBeaconOffsetDistance(double dM) {
    const auto src = current();
    if (src == nullptr) {
        return fail(tr("No source is open."));
    }
    if (!std::isfinite(dM)) {
        return fail(tr("The offset must be a number."));
    }
    if (beaconOffsetDistance().value_or(0.0) == dM) {
        return fail(dM == 0.0 ? tr("The source has no beacon offset.")
                              : tr("That offset is already applied."));
    }
    model::MarkerEdit edit;
    edit.op = model::MarkerEditOp::OffsetDistance;
    edit.dM = dM;
    edit.dMUnit = "m";
    session_.undoStack().push(new qt::MarkerEditCommand(loader_, *src, layerName(), edit));
    return succeed();
}

bool LapEditor::setBeaconTimeOffset(int row, Micros dtUs) {
    const auto src = current();
    if (src == nullptr || !rows_->valid(row)) {
        return fail(tr("Select a %1 first.").arg(rangeTerm_.toLower()));
    }
    const LapRow& r = rows_->rows[static_cast<std::size_t>(row)];
    if (r.ignored || !r.marker || r.marker->cls != model::MarkerClass::BCN) {
        return fail(tr("%1 does not start at a beacon.").arg(QString::fromStdString(r.range.name)));
    }
    const model::Marker& m = *r.marker;
    if (m.prov == model::Provenance::User) {
        // A user beacon is moved itself (marker edits apply to the logged markers only).
        if (dtUs == 0) {
            return fail(tr("The beacon is already at that time."));
        }
        model::SidecarDocument after = src->sidecar;
        model::Marker moved = m;
        moved.t = m.t + dtUs;
        moved.originalT.reset();
        const model::DataLayer* l = src->layer(layerName());
        if (l == nullptr || moved.t <= 0 || moved.t >= l->duration) {
            return fail(tr("%1 lies outside the data.").arg(formatTime(moved.t)));
        }
        after.removeUserMarker(layerName(), m.id);
        after.addUserMarker(layerName(), std::move(moved));
        session_.undoStack().push(
            new qt::SidecarCommand(loader_, src->id, src->sidecar, std::move(after), tr("Move beacon")));
        return succeed();
    }
    const Micros applied = m.originalT ? m.t - *m.originalT : 0;
    if (applied == dtUs) {
        return fail(tr("That offset is already applied."));
    }
    model::MarkerEdit edit;
    edit.ref = m.id;
    edit.op = dtUs == 0 ? model::MarkerEditOp::Restore : model::MarkerEditOp::Offset;
    edit.dtUs = dtUs;
    session_.undoStack().push(new qt::MarkerEditCommand(loader_, *src, layerName(), edit));
    return succeed();
}

bool LapEditor::setTrusted(int row, bool trusted) {
    const auto lap = lapAt(row);
    if (!lap) {
        return fail(tr("Select a %1 first.").arg(rangeTerm_.toLower()));
    }
    if (rows_->rows[static_cast<std::size_t>(row)].range.trusted == trusted) {
        return false;
    }
    selection_.setTrusted(*lap, trusted); // the Data window's command (06 section 13.3 item 4)
    return succeed();
}

bool LapEditor::restoreLogged() {
    const auto src = current();
    if (src == nullptr) {
        return fail(tr("No source is open."));
    }
    const model::SidecarLayer& l = src->sidecar.layer(layerName());
    if (l.markers.empty() && l.markerEdits.empty()) {
        return fail(tr("The %1 are already the logged ones.").arg(rangesTerm_.toLower()));
    }
    session_.undoStack().push(new qt::RestoreLoggedLapsCommand(loader_, *src, layerName()));
    return succeed();
}

QString LapEditor::stretchReportText() const {
    QStringList lines{tr("%1\tLength (m)\tTrack (m)\tStretch (%)\tApplied").arg(rangeTerm_)};
    for (const LapRow& r : rows_->rows) {
        if (r.ignored || !r.stretch) {
            continue;
        }
        const analysis::LapStretchRow& s = *r.stretch;
        lines << QStringLiteral("%1\t%2\t%3\t%4\t%5")
                     .arg(QString::fromStdString(r.range.name))
                     .arg(s.lengthM, 0, 'f', 1)
                     .arg(s.trackLengthM, 0, 'f', 1)
                     .arg(s.withinTolerance ? QString::number((s.candidate - 1.0) * 100.0, 'f', 2)
                                            : QString())
                     .arg(s.applied ? tr("yes") : tr("no"));
    }
    return lines.join(QLatin1Char('\n'));
}

// ---- time text
// -------------------------------------------------------------------------------------------------

QString LapEditor::formatTime(Micros t) {
    const bool negative = t < 0;
    const long long ms = std::llround(std::abs(static_cast<double>(t)) / 1000.0);
    const long long hours = ms / 3'600'000;
    const long long minutes = (ms / 60'000) % 60;
    const long long seconds = (ms / 1000) % 60;
    const long long millis = ms % 1000;
    QString text = hours > 0 ? QStringLiteral("%1:%2:%3.%4")
                                   .arg(hours)
                                   .arg(minutes, 2, 10, QLatin1Char('0'))
                                   .arg(seconds, 2, 10, QLatin1Char('0'))
                                   .arg(millis, 3, 10, QLatin1Char('0'))
                             : QStringLiteral("%1:%2.%3")
                                   .arg(minutes)
                                   .arg(seconds, 2, 10, QLatin1Char('0'))
                                   .arg(millis, 3, 10, QLatin1Char('0'));
    return negative ? QLatin1Char('-') + text : text;
}

std::optional<Micros> LapEditor::parseTime(const QString& text) {
    const QStringList parts = text.trimmed().split(QLatin1Char(':'));
    if (parts.isEmpty() || parts.size() > 3) {
        return std::nullopt;
    }
    bool ok = false;
    const double seconds = parts.back().trimmed().toDouble(&ok);
    if (!ok || !std::isfinite(seconds) || seconds < 0.0 || (parts.size() > 1 && seconds >= 60.0)) {
        return std::nullopt;
    }
    double total = seconds;
    double scale = 60.0;
    for (qsizetype i = parts.size() - 2; i >= 0; --i) {
        const int v = parts[i].trimmed().toInt(&ok);
        if (!ok || v < 0 || (i > 0 && v >= 60)) {
            return std::nullopt;
        }
        total += v * scale;
        scale *= 60.0;
    }
    return static_cast<Micros>(std::llround(total * 1'000'000.0));
}

// ---- SHIFT+F7 and the toolbar toggle ----------------------------------------------------------------------

bool insertBeaconAtCursor(qt::ProjectSession& session, qt::SourceLoader& loader,
                          const qt::DataSelectionModel& selection, const qt::WorkspaceViewModel& workspace) {
    if (!selection.hasMain()) {
        return false;
    }
    const model::RangeRef& main = selection.snapshot().main;
    const model::Range* range = selection.resolve(main);
    const auto src = loader.source(main.src);
    if (range == nullptr || src == nullptr) {
        return false;
    }
    QString reason;
    return pushBeacon(session, loader, *src, std::string(model::canonicalLayerName(main.layer)),
                      range->start + workspace.cursorT(), reason)
        .has_value();
}

void registerInsertBeaconCommand(qt::CommandRegistry& registry, qt::ProjectSession& session,
                                 qt::SourceLoader& loader, qt::DataSelectionModel& selection,
                                 qt::WorkspaceViewModel& workspace) {
    const QPointer<qt::ProjectSession> s(&session);
    const QPointer<qt::SourceLoader> l(&loader);
    const QPointer<qt::DataSelectionModel> sel(&selection);
    const QPointer<qt::WorkspaceViewModel> ws(&workspace);
    qt::CommandSpec spec;
    spec.id = QStringLiteral("cursor.insert_marker");
    spec.title = QObject::tr("Insert Beacon");
    spec.menuPath = QStringLiteral("Component/Cursor");
    spec.context = qt::CommandContext::Graph;
    spec.defaultKeys = {QStringLiteral("Shift+F7")};
    spec.enabled = [sel] { return !sel.isNull() && sel->hasMain(); };
    spec.invoke = [s, l, sel, ws](const QVariantList&) {
        if (!s.isNull() && !l.isNull() && !sel.isNull() && !ws.isNull()) {
            static_cast<void>(insertBeaconAtCursor(*s, *l, *sel, *ws));
        }
    };
    registry.add(std::move(spec));
}

void connectLapStretchToggle(CommandActions& actions, qt::LapStretchService& stretch) {
    const QString id = QStringLiteral("data.lap_stretch_toggle");
    QObject::connect(&stretch, &qt::LapStretchService::enabledChanged, &actions,
                     [a = QPointer<CommandActions>(&actions), id](bool) {
                         if (!a.isNull()) {
                             a->refresh(id);
                         }
                     });
    actions.refresh(id);
}

} // namespace esses::widgets
