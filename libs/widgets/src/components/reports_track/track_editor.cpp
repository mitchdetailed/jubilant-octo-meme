// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackEditor (06 section 13.5). Owner: src/components/reports_track/.

#include <esses/analysis/section_detector.hpp>
#include <esses/base/utf8.hpp>
#include <esses/plotcore/track_geometry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/track_map_service.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/widgets/components/track_editor.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadioButton>
#include <QTableWidget>
#include <QTimer>
#include <QUndoStack>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <set>
#include <string_view>

namespace esses::widgets {

namespace detail {

// The generated map before Accept: the centre, inner and outer lines fitted to the widget (plot::TrackFit),
// the sections tinted by category.
class TrackPreview final : public QWidget {
public:
    explicit TrackPreview(QWidget* parent) : QWidget(parent) {
        setMinimumSize(220, 180);
        setObjectName(QStringLiteral("trackPreview"));
    }
    void setMap(std::shared_ptr<const analysis::TrackMap> map) {
        map_ = std::move(map);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), palette().base());
        if (map_ == nullptr || map_->centre.size() < 2) {
            p.setPen(palette().color(QPalette::Text));
            p.drawText(rect(), Qt::AlignCenter, tr("No preview"));
            return;
        }
        p.setRenderHint(QPainter::Antialiasing);
        const plot::RectPx area{6.0F, 6.0F, static_cast<float>(width() - 12),
                                static_cast<float>(height() - 12)};
        const plot::TrackFit fit = plot::TrackGeometry::fit(*map_, area, plot::TrackPositionBody{});
        const auto line = [&](const std::vector<analysis::TrackPoint>& pts, const QColor& colour, qreal w) {
            if (pts.size() < 2) {
                return;
            }
            QPainterPath path;
            for (std::size_t i = 0; i < pts.size(); ++i) {
                const plot::PointPx q = fit.toPx(pts[i].x, pts[i].y);
                if (i == 0) {
                    path.moveTo(static_cast<double>(q.x), static_cast<double>(q.y));
                } else {
                    path.lineTo(static_cast<double>(q.x), static_cast<double>(q.y));
                }
            }
            p.setPen(QPen(colour, w));
            p.drawPath(path);
        };
        line(map_->inner, QColor(160, 160, 160), 1.0);
        line(map_->outer, QColor(160, 160, 160), 1.0);
        line(map_->centre, palette().color(QPalette::Text), 1.5);
    }

private:
    std::shared_ptr<const analysis::TrackMap> map_;
};

} // namespace detail

namespace {

constexpr int kColName = 0;
constexpr int kColSet = 1;
constexpr int kColStart = 2;
constexpr int kColEnd = 3;
constexpr int kColCategory = 4;
constexpr int kColUser = 5;
constexpr const char* kGpsLateralHints[] = {"G Force Lat", "G Lat", "Lateral G", "Accel Lat", "Lat Accel"};

QString fromUtf8(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

QString sourceLabel(const model::DataSource& src) {
    const QString name = QString::fromStdWString(src.prov.path.filename().wstring());
    return name.isEmpty() ? QStringLiteral("(source)") : name;
}

} // namespace

TrackEditor::TrackEditor(qt::ProjectSession& session, qt::DataSelectionModel& selection,
                         qt::MathsService& maths, qt::TrackMapService& maps, QWidget* parent)
    : QDialog(parent), session_(session), selection_(selection), maths_(maths), maps_(maps) {
    setObjectName(QStringLiteral("trackEditor"));
    setWindowTitle(tr("Track Editor"));
    setModal(false);

    // 1. lap and method
    venue_ = new QLabel(this);
    venue_->setObjectName(QStringLiteral("trackEditorVenue"));
    lapCombo_ = new QComboBox(this);
    lapCombo_->setObjectName(QStringLiteral("trackEditorLap"));
    gps_ = new QRadioButton(tr("GPS"), this);
    gps_->setObjectName(QStringLiteral("trackEditorGps"));
    speedLatG_ = new QRadioButton(tr("Speed + Lateral G"), this);
    speedLatG_->setObjectName(QStringLiteral("trackEditorSpeedLatG"));
    gps_->setChecked(true);
    inner_ = new QComboBox(this);
    inner_->setObjectName(QStringLiteral("trackEditorInner"));
    outer_ = new QComboBox(this);
    outer_->setObjectName(QStringLiteral("trackEditorOuter"));
    smoothing_ = new QDoubleSpinBox(this);
    smoothing_->setObjectName(QStringLiteral("trackEditorSmoothing"));
    smoothing_->setRange(0.0, 100.0);
    smoothing_->setDecimals(1);
    smoothing_->setSuffix(QStringLiteral(" m"));
    smoothing_->setValue(analysis::TrackGeneratorOptions{}.smoothingM);
    lateral_ = new QComboBox(this);
    lateral_->setObjectName(QStringLiteral("trackEditorLateral"));
    lateral_->setEditable(true);
    invert_ = new QCheckBox(tr("Lateral G is positive to the right"), this);
    invert_->setObjectName(QStringLiteral("trackEditorInvert"));
    generate_ = new QPushButton(tr("Generate"), this);
    generate_->setObjectName(QStringLiteral("trackEditorGenerate"));
    accept_ = new QPushButton(tr("Accept"), this);
    accept_->setObjectName(QStringLiteral("trackEditorAccept"));
    accept_->setEnabled(false);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("trackEditorStatus"));
    status_->setWordWrap(true);
    previewView_ = new detail::TrackPreview(this);

    auto* source = new QGroupBox(tr("Generate"), this);
    auto* form = new QFormLayout(source);
    form->addRow(tr("Venue:"), venue_);
    form->addRow(tr("Lap:"), lapCombo_);
    auto* methods = new QHBoxLayout;
    methods->addWidget(gps_);
    methods->addWidget(speedLatG_);
    methods->addStretch(1);
    form->addRow(tr("Method:"), methods);
    form->addRow(tr("Inner lap:"), inner_);
    form->addRow(tr("Outer lap:"), outer_);
    form->addRow(tr("Smoothing:"), smoothing_);
    form->addRow(tr("Lateral G:"), lateral_);
    form->addRow(QString(), invert_);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(generate_);
    buttons->addWidget(accept_);
    buttons->addStretch(1);
    form->addRow(buttons);
    form->addRow(status_);

    // 3-4. sections
    table_ = new QTableWidget(0, 6, this);
    table_->setObjectName(QStringLiteral("trackEditorSections"));
    table_->setHorizontalHeaderLabels(
        {tr("Name"), tr("Set"), tr("Start (m)"), tr("End (m)"), tr("Category"), tr("User")});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    name_ = new QLineEdit(this);
    name_->setObjectName(QStringLiteral("trackEditorSectionName"));
    set_ = new QLineEdit(QString::fromLatin1(kUserSet), this);
    set_->setObjectName(QStringLiteral("trackEditorSectionSet"));
    start_ = new QDoubleSpinBox(this);
    end_ = new QDoubleSpinBox(this);
    for (QDoubleSpinBox* s : {start_, end_}) {
        s->setRange(0.0, 1.0e6);
        s->setDecimals(1);
        s->setSuffix(QStringLiteral(" m"));
    }
    start_->setObjectName(QStringLiteral("trackEditorSectionStart"));
    end_->setObjectName(QStringLiteral("trackEditorSectionEnd"));
    category_ = new QComboBox(this);
    category_->setObjectName(QStringLiteral("trackEditorSectionCategory"));
    category_->setEditable(true);
    category_->addItems({QStringLiteral("corner"), QStringLiteral("straight")});
    add_ = new QPushButton(tr("Add"), this);
    add_->setObjectName(QStringLiteral("trackEditorAdd"));
    addSpan_ = new QPushButton(tr("Add from Cursor Span"), this);
    addSpan_->setObjectName(QStringLiteral("trackEditorAddSpan"));
    addSpan_->setEnabled(false);
    apply_ = new QPushButton(tr("Apply"), this);
    apply_->setObjectName(QStringLiteral("trackEditorApply"));
    delete_ = new QPushButton(tr("Delete"), this);
    delete_->setObjectName(QStringLiteral("trackEditorDelete"));
    defaults_ = new QPushButton(tr("Regenerate Default Sections"), this);
    defaults_->setObjectName(QStringLiteral("trackEditorDefaults"));
    auto* sections = new QGroupBox(tr("Sections"), this);
    auto* sectionsLayout = new QVBoxLayout(sections);
    sectionsLayout->addWidget(table_, 1);
    auto* fields = new QFormLayout;
    fields->addRow(tr("Name:"), name_);
    fields->addRow(tr("Set:"), set_);
    fields->addRow(tr("Start:"), start_);
    fields->addRow(tr("End:"), end_);
    fields->addRow(tr("Category:"), category_);
    sectionsLayout->addLayout(fields);
    auto* sectionButtons = new QHBoxLayout;
    for (QPushButton* b : {add_, addSpan_, apply_, delete_, defaults_}) {
        sectionButtons->addWidget(b);
    }
    sectionsLayout->addLayout(sectionButtons);

    // 6. files
    renameTo_ = new QLineEdit(this);
    renameTo_->setObjectName(QStringLiteral("trackEditorRenameTo"));
    rename_ = new QPushButton(tr("Rename"), this);
    rename_->setObjectName(QStringLiteral("trackEditorRename"));
    deleteMap_ = new QPushButton(tr("Delete Track Map"), this);
    deleteMap_->setObjectName(QStringLiteral("trackEditorDeleteMap"));
    autoGenerate_ = new QCheckBox(tr("Auto-generate on open"), this);
    autoGenerate_->setObjectName(QStringLiteral("trackEditorAutoGenerate"));
    auto* files = new QGroupBox(tr("Track File"), this);
    auto* filesLayout = new QHBoxLayout(files);
    filesLayout->addWidget(renameTo_, 1);
    filesLayout->addWidget(rename_);
    filesLayout->addWidget(deleteMap_);
    filesLayout->addWidget(autoGenerate_);

    // ENTER in a field must not press a button of this modeless editor (QDialog's auto-default buttons).
    for (QPushButton* b :
         {generate_, accept_, add_, addSpan_, apply_, delete_, defaults_, rename_, deleteMap_}) {
        b->setAutoDefault(false);
    }
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* left = new QVBoxLayout;
    left->addWidget(source);
    left->addWidget(previewView_, 1);
    auto* top = new QHBoxLayout;
    top->addLayout(left, 1);
    top->addWidget(sections, 1);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(top, 1);
    layout->addWidget(files);
    layout->addWidget(close);

    connect(generate_, &QPushButton::clicked, this, &TrackEditor::generate);
    connect(accept_, &QPushButton::clicked, this, &TrackEditor::accept);
    connect(gps_, &QRadioButton::toggled, this, &TrackEditor::refreshAvailability);
    connect(lapCombo_, &QComboBox::currentIndexChanged, this, [this] {
        preview_.reset();
        previewView_->setMap(nullptr);
        refreshAvailability();
        refreshSections();
    });
    connect(table_, &QTableWidget::currentCellChanged, this, [this](int row) { loadForm(row); });
    connect(add_, &QPushButton::clicked, this, [this] {
        addSection(name_->text(), start_->value(), end_->value(), category_->currentText(), set_->text());
    });
    connect(addSpan_, &QPushButton::clicked, this, [this] { addSectionFromCursorSpan(name_->text()); });
    connect(apply_, &QPushButton::clicked, this, [this] {
        const int row = table_->currentRow();
        if (row >= 0) {
            editSection(table_->item(row, kColName)->data(Qt::UserRole).toString(), name_->text(),
                        start_->value(), end_->value(), category_->currentText());
        }
    });
    connect(delete_, &QPushButton::clicked, this, [this] {
        const int row = table_->currentRow();
        if (row >= 0) {
            deleteSection(table_->item(row, kColName)->data(Qt::UserRole).toString());
        }
    });
    connect(defaults_, &QPushButton::clicked, this, &TrackEditor::regenerateDefaultSections);
    connect(rename_, &QPushButton::clicked, this, [this] { renameVenue(renameTo_->text()); });
    connect(deleteMap_, &QPushButton::clicked, this, &TrackEditor::deleteMap);
    connect(autoGenerate_, &QCheckBox::toggled, this, &TrackEditor::setAutoGenerateOnOpen);
    connect(&selection_, &qt::DataSelectionModel::sourcesChanged, this, &TrackEditor::refreshLaps);
    connect(&maps_, &qt::TrackMapService::mapChanged, this, [this] { refreshSections(); });
    connect(&session_, &qt::ProjectSession::documentsChanged, this, [this] {
        const QSignalBlocker block(autoGenerate_);
        autoGenerate_->setChecked(autoGenerateOnOpen());
    });
    // A generation waiting for its maths inputs starts again when one is evaluated.
    const auto retry = [this] {
        if (!waiting_ || retryScheduled_) {
            return;
        }
        retryScheduled_ = true; // one retry per event-loop turn, however many outputs changed
        QTimer::singleShot(0, this, [this] {
            retryScheduled_ = false;
            if (waiting_) {
                generate();
            }
        });
    };
    connect(&maths_, &qt::MathsService::channelReady, this, retry);
    connect(&maths_, &qt::MathsService::statusChanged, this, retry);
    {
        const QSignalBlocker block(autoGenerate_);
        autoGenerate_->setChecked(autoGenerateOnOpen());
    }
    refreshLaps();
    resize(900, 560);
}

TrackEditor::~TrackEditor() = default;

// ---- 1. the lap and the method ----------------------------------------------------------------------------

void TrackEditor::refreshLaps() {
    const std::optional<model::RangeRef> keep = lap();
    const std::optional<model::RangeRef> keepInner = lapAt(inner_);
    const std::optional<model::RangeRef> keepOuter = lapAt(outer_);
    laps_.clear();
    const QSignalBlocker b1(lapCombo_);
    const QSignalBlocker b2(inner_);
    const QSignalBlocker b3(outer_);
    lapCombo_->clear();
    inner_->clear();
    outer_->clear();
    inner_->addItem(tr("(none)"), -1);
    outer_->addItem(tr("(none)"), -1);
    for (const std::shared_ptr<const model::DataSource>& src : selection_.sources()) {
        const model::DataLayer* layer = src->layer(model::kNormalLayer);
        const model::RangeGroup* laps = layer == nullptr ? nullptr : layer->ranges.find("Outings:Laps");
        if (laps == nullptr) {
            continue;
        }
        for (const model::Range& r : laps->ranges) {
            model::RangeRef ref;
            ref.src = src->id;
            ref.layer = std::string(model::kNormalLayer);
            ref.groupPath = "Outings:Laps";
            ref.range = r.id;
            ref.name = r.name;
            const QString label = sourceLabel(*src) + QStringLiteral(" - ") + fromUtf8(r.name);
            const int index = static_cast<int>(laps_.size());
            laps_.push_back(ref);
            lapCombo_->addItem(label, index);
            inner_->addItem(label, index);
            outer_->addItem(label, index);
        }
    }
    const auto pick = [this](QComboBox* combo, const std::optional<model::RangeRef>& want) {
        if (!want) {
            return false;
        }
        for (std::size_t i = 0; i < laps_.size(); ++i) {
            if (laps_[i] == *want) {
                combo->setCurrentIndex(combo->findData(static_cast<int>(i)));
                return true;
            }
        }
        return false;
    };
    if (!pick(lapCombo_, keep)) {
        const model::RangeRef& main = selection_.snapshot().main;
        if (!pick(lapCombo_, std::optional<model::RangeRef>(main)) && !laps_.empty()) {
            lapCombo_->setCurrentIndex(0); // Main is not a lap: the first lap
        }
    }
    pick(inner_, keepInner);
    pick(outer_, keepOuter);
    refreshAvailability();
    refreshSections();
}

std::optional<model::RangeRef> TrackEditor::lapAt(const QComboBox* combo) const {
    if (combo == nullptr) {
        return std::nullopt;
    }
    const int index = combo->currentData().toInt();
    if (combo->currentIndex() < 0 || index < 0 || static_cast<std::size_t>(index) >= laps_.size()) {
        return std::nullopt;
    }
    return laps_[static_cast<std::size_t>(index)];
}

std::optional<model::RangeRef> TrackEditor::lap() const {
    return lapAt(lapCombo_);
}

bool TrackEditor::setLap(const model::RangeRef& lap) {
    for (std::size_t i = 0; i < laps_.size(); ++i) {
        if (laps_[i] == lap) {
            lapCombo_->setCurrentIndex(lapCombo_->findData(static_cast<int>(i)));
            return true;
        }
    }
    return false;
}

bool TrackEditor::setInnerLap(const std::optional<model::RangeRef>& lap) {
    if (!lap) {
        inner_->setCurrentIndex(0);
        return true;
    }
    for (std::size_t i = 0; i < laps_.size(); ++i) {
        if (laps_[i] == *lap) {
            inner_->setCurrentIndex(inner_->findData(static_cast<int>(i)));
            return true;
        }
    }
    return false;
}

bool TrackEditor::setOuterLap(const std::optional<model::RangeRef>& lap) {
    if (!lap) {
        outer_->setCurrentIndex(0);
        return true;
    }
    for (std::size_t i = 0; i < laps_.size(); ++i) {
        if (laps_[i] == *lap) {
            outer_->setCurrentIndex(outer_->findData(static_cast<int>(i)));
            return true;
        }
    }
    return false;
}

analysis::TrackMethod TrackEditor::method() const {
    return gps_->isChecked() ? analysis::TrackMethod::Gps : analysis::TrackMethod::SpeedLatG;
}

void TrackEditor::setMethod(analysis::TrackMethod method) {
    (method == analysis::TrackMethod::SpeedLatG ? speedLatG_ : gps_)->setChecked(true);
}

void TrackEditor::setSmoothing(double metres) {
    smoothing_->setValue(metres);
}

void TrackEditor::setLateralChannel(const QString& name) {
    lateral_->setCurrentText(name);
}

std::string TrackEditor::venueOf(const model::RangeRef& lap) const {
    const std::shared_ptr<const model::DataSource> src = selection_.source(lap.src);
    return src == nullptr ? std::string() : qt::TrackMapService::venueOf(*src);
}

QString TrackEditor::venue() const {
    const std::optional<model::RangeRef> l = lap();
    return l ? fromUtf8(venueOf(*l)) : QString();
}

void TrackEditor::refreshAvailability() {
    const bool gps = gps_->isChecked();
    inner_->setEnabled(gps);
    outer_->setEnabled(gps);
    smoothing_->setEnabled(gps);
    lateral_->setEnabled(!gps);
    invert_->setEnabled(!gps);
    const std::optional<model::RangeRef> l = lap();
    venue_->setText(l ? (venue().isEmpty() ? tr("(no Venue detail)") : venue()) : tr("(no lap)"));
    generate_->setEnabled(l.has_value() && !generating_);
    // The lateral channel candidates: the source's acceleration channels and the usual names.
    const QString current = lateral_->currentText();
    const QSignalBlocker block(lateral_);
    lateral_->clear();
    QStringList names;
    if (l) {
        if (const auto src = selection_.source(l->src)) {
            if (const model::DataLayer* layer = src->layer(l->layer)) {
                for (const char* hint : kGpsLateralHints) {
                    if (layer->find(hint) != nullptr) {
                        names << QString::fromLatin1(hint);
                    }
                }
                const units::UnitRegistry* registry = maths_.units().get();
                const std::optional<units::UnitId> g =
                    registry != nullptr ? registry->find("G") : std::nullopt;
                const std::optional<units::QuantityId> accel =
                    registry != nullptr && g ? std::optional<units::QuantityId>(registry->quantityOf(*g))
                                             : std::nullopt;
                for (const model::Channel& c : layer->channels) {
                    const QString n = QString::fromStdString(c.desc.name);
                    if (accel && c.desc.quantity == *accel && !names.contains(n)) {
                        names << n;
                    }
                }
            }
        }
    }
    if (names.isEmpty()) {
        names << QString::fromLatin1(kGpsLateralHints[0]);
    }
    lateral_->addItems(names);
    lateral_->setCurrentText(current.isEmpty() || !names.contains(current) ? names.front() : current);
}

void TrackEditor::setStatus(const QString& text) {
    status_->setText(text);
}

QString TrackEditor::statusText() const {
    return status_->text();
}

void TrackEditor::generate() {
    const std::optional<model::RangeRef> l = lap();
    if (!l) {
        setStatus(tr("Pick a lap first"));
        return;
    }
    qt::TrackGenerationRequest request;
    request.lap = *l;
    request.method = method();
    request.smoothingM = smoothing_->value();
    request.innerLap = lapAt(inner_);
    request.outerLap = lapAt(outer_);
    request.lateral = lateral_->currentText().toStdString();
    request.invertLatAccel = invert_->isChecked();
    const std::uint64_t serial = ++generation_;
    const qt::TrackGenerationStatus status =
        maps_.generate(request, this, [this, serial](Result<analysis::GeneratedTrack> result) {
            if (serial != generation_) {
                return; // a newer Generate superseded it
            }
            generating_ = false;
            generate_->setEnabled(true);
            if (!result) {
                const QString reason = QString::fromStdString(result.error().toString());
                setStatus(tr("Generation failed: %1").arg(reason));
                Q_EMIT generationFailed(reason);
                return;
            }
            analysis::GeneratedTrack track = std::move(result).value();
            report_ = track.report;
            preview_ = std::make_shared<const analysis::TrackMap>(std::move(track.map));
            previewView_->setMap(preview_);
            accept_->setEnabled(true);
            QString text = tr("Length %1 m, closure error %2 m")
                               .arg(preview_->lengthM, 0, 'f', 1)
                               .arg(report_.closureErrorM, 0, 'f', 1);
            if (method() == analysis::TrackMethod::Gps) {
                text += tr(", RMS fit %1 m").arg(report_.rmsM, 0, 'f', 2);
            }
            if (!report_.diagnostics.empty()) {
                QStringList codes;
                for (const std::string& d : report_.diagnostics) {
                    codes << QString::fromStdString(d);
                }
                text += QStringLiteral(" (") + codes.join(QStringLiteral(", ")) + QLatin1Char(')');
            }
            setStatus(text);
            Q_EMIT previewReady();
        });
    switch (status.state) {
    case qt::TrackGenerationState::Started:
        waiting_ = false;
        generating_ = true;
        generate_->setEnabled(false);
        setStatus(tr("Generating..."));
        break;
    case qt::TrackGenerationState::Pending:
        waiting_ = true;
        setStatus(status.reason);
        break;
    case qt::TrackGenerationState::Failed:
        waiting_ = false;
        setStatus(status.reason);
        Q_EMIT generationFailed(status.reason);
        break;
    }
}

// ---- 2. Accept
// ----------------------------------------------------------------------------------------------

void TrackEditor::accept() {
    if (preview_ == nullptr) {
        error_ = tr("Generate a track first");
        setStatus(error_);
        return;
    }
    const std::string venue = preview_->venue;
    const std::shared_ptr<const analysis::TrackMap> previous = maps_.map(venue);
    analysis::TrackMap next = *preview_;
    next.sections.clear();
    if (previous != nullptr) {
        // A regeneration keeps the user sections that still fit the new length (one command, 02 section 7).
        for (const analysis::TrackSection& s : previous->sections) {
            if (s.userDefined && s.endM <= next.lengthM + 1e-6) {
                next.sections.push_back(s);
            }
        }
    }
    if (Result<void> sections = analysis::SectionDetector::defaults(next); !sections) {
        setStatus(tr("Default sections: %1").arg(QString::fromStdString(sections.error().toString())));
    }
    if (pushMap(std::make_shared<const analysis::TrackMap>(std::move(next)),
                previous != nullptr ? tr("Regenerate track map") : tr("Generate track map"), true)) {
        accept_->setEnabled(false);
        Q_EMIT mapWritten();
    }
}

bool TrackEditor::pushMap(std::shared_ptr<const analysis::TrackMap> after, const QString& text,
                          bool backupFirst) {
    const std::optional<model::RangeRef> l = lap();
    const std::string venue = after != nullptr ? after->venue : (l ? venueOf(*l) : std::string());
    auto* command =
        new qt::TrackMapCommand(maps_, venue, maps_.map(venue), std::move(after), text, backupFirst);
    session_.undoStack().push(command); // redo() runs now
    if (command->failed()) {
        const std::optional<Error>& e = command->lastError();
        error_ = e ? QString::fromStdString(e->toString()) : tr("The track map could not be written");
        setStatus(error_);
        session_.undoStack().undo(); // the cache never changed: undo is a no-op write of the same state
        return false;
    }
    error_.clear();
    return true;
}

// ---- 3-4. sections
// --------------------------------------------------------------------------------------------

std::shared_ptr<const analysis::TrackMap> TrackEditor::currentMap() const {
    const std::optional<model::RangeRef> l = lap();
    return l ? maps_.map(venueOf(*l)) : nullptr;
}

void TrackEditor::refreshSections() {
    const std::shared_ptr<const analysis::TrackMap> map = currentMap();
    const QSignalBlocker block(table_);
    table_->setRowCount(0);
    const bool hasMap = map != nullptr;
    for (QPushButton* b : {add_, apply_, delete_, defaults_, rename_, deleteMap_}) {
        b->setEnabled(hasMap);
    }
    addSpan_->setEnabled(hasMap && cursorSpan_.has_value());
    if (!hasMap) {
        return;
    }
    table_->setRowCount(static_cast<int>(map->sections.size()));
    for (int row = 0; row < table_->rowCount(); ++row) {
        const analysis::TrackSection& s = map->sections[static_cast<std::size_t>(row)];
        const auto cell = [&](int column, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            table_->setItem(row, column, item);
            return item;
        };
        cell(kColName, fromUtf8(s.name))->setData(Qt::UserRole, fromUtf8(s.id));
        cell(kColSet, fromUtf8(s.set));
        cell(kColStart, QString::number(s.startM, 'f', 1));
        cell(kColEnd, QString::number(s.endM, 'f', 1));
        cell(kColCategory, fromUtf8(s.category));
        cell(kColUser, s.userDefined ? tr("yes") : QString());
    }
    end_->setMaximum(std::max(0.0, map->lengthM));
    start_->setMaximum(std::max(0.0, map->lengthM));
}

void TrackEditor::loadForm(int row) {
    const std::shared_ptr<const analysis::TrackMap> map = currentMap();
    if (map == nullptr || row < 0 || static_cast<std::size_t>(row) >= map->sections.size()) {
        return;
    }
    const analysis::TrackSection& s = map->sections[static_cast<std::size_t>(row)];
    name_->setText(fromUtf8(s.name));
    set_->setText(fromUtf8(s.set));
    start_->setValue(s.startM);
    end_->setValue(s.endM);
    category_->setCurrentText(fromUtf8(s.category));
}

bool TrackEditor::checkSection(const analysis::TrackMap& map, double startM, double endM,
                               const QString& name) {
    if (name.trimmed().isEmpty()) {
        error_ = tr("A section needs a name");
    } else if (!(endM - startM >= analysis::TrackMap::kMinSectionM - 1e-6)) {
        error_ = tr("A section is at least %1 m long").arg(analysis::TrackMap::kMinSectionM);
    } else if (startM < -1e-6 || endM > map.lengthM + 1e-6) {
        error_ = tr("A section lies on the track (0 to %1 m)").arg(map.lengthM, 0, 'f', 1);
    } else {
        return true;
    }
    setStatus(error_);
    return false;
}

bool TrackEditor::regenerateDefaultSections() {
    const std::shared_ptr<const analysis::TrackMap> map = currentMap();
    if (map == nullptr) {
        error_ = tr("No track map");
        return false;
    }
    analysis::TrackMap next = *map;
    if (Result<void> r = analysis::SectionDetector::defaults(next); !r) {
        error_ = QString::fromStdString(r.error().toString());
        setStatus(error_);
        return false;
    }
    return pushMap(std::make_shared<const analysis::TrackMap>(std::move(next)),
                   tr("Regenerate default sections"));
}

void TrackEditor::setCursorSpan(std::optional<std::pair<double, double>> span) {
    cursorSpan_ = span;
    addSpan_->setEnabled(currentMap() != nullptr && cursorSpan_.has_value());
}

bool TrackEditor::addSection(const QString& name, double startM, double endM, const QString& category,
                             const QString& set) {
    const std::shared_ptr<const analysis::TrackMap> map = currentMap();
    if (map == nullptr) {
        error_ = tr("No track map");
        return false;
    }
    if (!checkSection(*map, startM, endM, name)) {
        return false;
    }
    analysis::TrackMap next = *map;
    std::set<std::string> ids;
    for (const analysis::TrackSection& s : next.sections) {
        ids.insert(s.id);
    }
    int n = 1;
    while (ids.contains("u" + std::to_string(n))) {
        ++n;
    }
    analysis::TrackSection s;
    s.id = "u" + std::to_string(n);
    s.set = set.trimmed().isEmpty() ? std::string(kUserSet) : set.trimmed().toStdString();
    s.name = name.trimmed().toStdString();
    s.startM = startM;
    s.endM = endM;
    s.category = category.trimmed().isEmpty() ? std::string("corner") : category.trimmed().toStdString();
    s.userDefined = true;
    next.sections.push_back(std::move(s));
    return pushMap(std::make_shared<const analysis::TrackMap>(std::move(next)),
                   tr("Add section %1").arg(name));
}

bool TrackEditor::addSectionFromCursorSpan(const QString& name) {
    if (!cursorSpan_) {
        error_ = tr("No cursor span");
        return false;
    }
    const auto [a, b] = *cursorSpan_;
    return addSection(name, std::min(a, b), std::max(a, b), category_->currentText(), set_->text());
}

bool TrackEditor::editSection(const QString& id, const QString& name, double startM, double endM,
                              const QString& category) {
    const std::shared_ptr<const analysis::TrackMap> map = currentMap();
    if (map == nullptr) {
        error_ = tr("No track map");
        return false;
    }
    if (!checkSection(*map, startM, endM, name)) {
        return false;
    }
    analysis::TrackMap next = *map;
    const auto it = std::find_if(next.sections.begin(), next.sections.end(),
                                 [&](const analysis::TrackSection& s) { return fromUtf8(s.id) == id; });
    if (it == next.sections.end()) {
        error_ = tr("No section %1").arg(id);
        return false;
    }
    it->name = name.trimmed().toStdString();
    it->startM = startM;
    it->endM = endM;
    if (!category.trimmed().isEmpty()) {
        it->category = category.trimmed().toStdString();
    }
    return pushMap(std::make_shared<const analysis::TrackMap>(std::move(next)),
                   tr("Edit section %1").arg(name));
}

bool TrackEditor::deleteSection(const QString& id) {
    const std::shared_ptr<const analysis::TrackMap> map = currentMap();
    if (map == nullptr) {
        error_ = tr("No track map");
        return false;
    }
    analysis::TrackMap next = *map;
    const auto it = std::find_if(next.sections.begin(), next.sections.end(),
                                 [&](const analysis::TrackSection& s) { return fromUtf8(s.id) == id; });
    if (it == next.sections.end()) {
        error_ = tr("No section %1").arg(id);
        return false;
    }
    const QString name = fromUtf8(it->name);
    next.sections.erase(it);
    return pushMap(std::make_shared<const analysis::TrackMap>(std::move(next)),
                   tr("Delete section %1").arg(name));
}

// ---- 6. files and the project option
// --------------------------------------------------------------------------

bool TrackEditor::renameVenue(const QString& to) {
    const std::shared_ptr<const analysis::TrackMap> map = currentMap();
    const std::string target = to.trimmed().toStdString();
    if (map == nullptr || target.empty() || target == map->venue) {
        error_ = tr("Nothing to rename");
        return false;
    }
    if (maps_.map(target) != nullptr) {
        error_ = tr("%1 has a track map already").arg(to);
        setStatus(error_);
        return false;
    }
    analysis::TrackMap moved = *map;
    moved.venue = target;
    moved.name = target;
    auto* parent = new QUndoCommand(tr("Rename track map to %1").arg(to));
    auto* remove = new qt::TrackMapCommand(maps_, map->venue, map, nullptr, QString(), false, parent);
    auto* add = new qt::TrackMapCommand(
        maps_, target, nullptr, std::make_shared<const analysis::TrackMap>(moved), QString(), false, parent);
    session_.undoStack().push(parent);
    if (remove->failed() || add->failed()) {
        error_ = tr("The track map could not be renamed");
        setStatus(error_);
        session_.undoStack().undo();
        return false;
    }
    error_.clear();
    return true;
}

bool TrackEditor::deleteMap() {
    const std::shared_ptr<const analysis::TrackMap> map = currentMap();
    if (map == nullptr) {
        error_ = tr("No track map");
        return false;
    }
    auto* command = new qt::TrackMapCommand(maps_, map->venue, map, nullptr, tr("Delete track map"));
    session_.undoStack().push(command);
    if (command->failed()) {
        error_ = tr("The track map could not be deleted");
        setStatus(error_);
        session_.undoStack().undo();
        return false;
    }
    error_.clear();
    return true;
}

bool TrackEditor::autoGenerateOnOpen() const {
    const QJsonObject options =
        QJsonDocument::fromJson(QByteArray::fromStdString(session_.projectFile().options.compact())).object();
    return options.value(QStringLiteral("trackMaps"))
        .toObject()
        .value(QStringLiteral("autoGenerateOnOpen"))
        .toBool(true);
}

void TrackEditor::setAutoGenerateOnOpen(bool on) {
    if (on == autoGenerateOnOpen()) {
        return;
    }
    project::ProjectFile after = session_.projectFile();
    QJsonObject options =
        QJsonDocument::fromJson(QByteArray::fromStdString(after.options.compact())).object();
    QJsonObject trackMaps = options.value(QStringLiteral("trackMaps")).toObject();
    trackMaps.insert(QStringLiteral("autoGenerateOnOpen"), on);
    options.insert(QStringLiteral("trackMaps"), trackMaps);
    Result<JsonDoc> doc = JsonDoc::parse(QJsonDocument(options).toJson(QJsonDocument::Compact).toStdString());
    if (!doc) {
        return;
    }
    after.options = std::move(doc).value();
    session_.undoStack().push(new qt::SetProjectOptionsCommand(session_, std::move(after),
                                                               on ? tr("Auto-generate track maps on open")
                                                                  : tr("Do not auto-generate track maps")));
}

} // namespace esses::widgets
