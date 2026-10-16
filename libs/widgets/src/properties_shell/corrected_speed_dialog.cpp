// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// CorrectedSpeedDialog (06 section 13.6). Owner: src/properties_shell/.

#include <esses/project/schema_registry.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/widgets/properties_shell/corrected_speed_dialog.hpp>
#include <esses/widgets/properties_shell/field_binder.hpp>
#include <esses/widgets/properties_shell/field_editors.hpp>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <cmath>

namespace esses::widgets {

namespace {

constexpr double kMpsToKmh = 3.6;

QJsonObject toObject(const JsonDoc& doc) {
    return doc.isObject() ? QJsonDocument::fromJson(QByteArray::fromStdString(doc.compact())).object()
                          : QJsonObject();
}

JsonDoc toDoc(const QJsonObject& o) {
    Result<JsonDoc> d = JsonDoc::parse(QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString());
    return d ? std::move(d).value() : JsonDoc();
}

QJsonArray names(const std::vector<std::string>& list) {
    QJsonArray a;
    for (const std::string& s : list) {
        a.append(QString::fromStdString(s));
    }
    return a;
}

QJsonValue speed(const std::optional<double>& mps) {
    if (!mps) {
        return QJsonValue(QJsonValue::Null);
    }
    return QJsonObject{{QStringLiteral("v"), *mps * kMpsToKmh},
                       {QStringLiteral("unit"), QStringLiteral("km/h")}};
}

// The settings object of a CorrectedSpeedSettings (speeds written in km/h).
QJsonObject settingsJson(const analysis::CorrectedSpeedSettings& s) {
    const char* method = s.method == analysis::SpeedMethod::FastestWheel    ? "fastestWheel"
                         : s.method == analysis::SpeedMethod::AlternateList ? "alternateList"
                                                                            : "gCorrection";
    return QJsonObject{
        {QStringLiteral("method"), QString::fromLatin1(method)},
        {QStringLiteral("fastestWheel"),
         QJsonObject{{QStringLiteral("channels"), names(s.fastestWheelChannels)}}},
        {QStringLiteral("alternateList"),
         QJsonObject{{QStringLiteral("channels"), names(s.alternateListChannels)}}},
        {QStringLiteral("gCorrection"),
         QJsonObject{{QStringLiteral("speedChannel"), QString::fromStdString(s.gCorrection.speedChannel)},
                     {QStringLiteral("longGChannel"), QString::fromStdString(s.gCorrection.longGChannel)},
                     {QStringLiteral("lockThreshold"), speed(s.gCorrection.lockThresholdMps)},
                     {QStringLiteral("recoverSeconds"), s.gCorrection.recoverSeconds}}},
        {QStringLiteral("clamp"), QJsonObject{{QStringLiteral("min"), speed(s.clamp.minMps)},
                                              {QStringLiteral("minInvalidate"), s.clamp.minInvalidate},
                                              {QStringLiteral("max"), speed(s.clamp.maxMps)},
                                              {QStringLiteral("maxInvalidate"), s.clamp.maxInvalidate}}},
        {QStringLiteral("distance"),
         QJsonObject{{QStringLiteral("source"), s.distanceSource == analysis::DistanceSource::Integrate
                                                    ? QStringLiteral("integrate")
                                                    : QStringLiteral("logged")},
                     {QStringLiteral("loggedChannel"), QString::fromStdString(s.loggedDistanceChannel)},
                     {QStringLiteral("integration"), s.integration == analysis::Integration::Trapezoid
                                                         ? QStringLiteral("trapezoid")
                                                         : QStringLiteral("rectangle")}}},
        {QStringLiteral("outputs"),
         QJsonObject{{QStringLiteral("speed"), QString::fromStdString(s.speedOutput)},
                     {QStringLiteral("distance"), QString::fromStdString(s.distanceOutput)}}}};
}

FieldRow row(const char* pointer, FieldKind kind, const QString& label,
             std::function<FieldEditor*(QWidget*)> make = {}) {
    FieldRow r;
    r.pointer = QString::fromLatin1(pointer);
    r.kind = kind;
    r.label = label;
    r.make = std::move(make);
    return r;
}

FieldEditor* channelList(QWidget* parent, const QString& newItem) {
    return new RecordListEditor(
        [](QWidget* p) -> FieldEditor* { return new LeafEditor(FieldKind::Channel, {}, p); }, newItem, {},
        parent);
}

FieldEditor* optionalSpeed(QWidget* parent, const QString& label) {
    return new OptionalEditor(
        label, new UnitValueEdit(parent),
        QJsonObject{{QStringLiteral("v"), 0}, {QStringLiteral("unit"), QStringLiteral("km/h")}}, parent);
}

} // namespace

CorrectedSpeedDialog::CorrectedSpeedDialog(qt::ProjectSession& session, qt::MathsService& maths,
                                           QWidget* parent)
    : QDialog(parent), session_(session), maths_(maths), schema_(std::make_unique<SchemaTree>()),
      binder_(new FieldBinder(this)) {
    setObjectName(QStringLiteral("correctedSpeedDialog"));
    setWindowTitle(tr("Corrected Speed and Distance"));
    setModal(false);
    if (const std::filesystem::path dir = project::SchemaRegistry::findSchemasDir(); !dir.empty()) {
        if (Result<JsonDoc> s = JsonDoc::load(dir / "builtin-esses.corrected_speed_distance-1.schema.json")) {
            *schema_ = SchemaTree::fromDoc(*s);
        }
    }
    binder_->setSchema(schema_.get());
    auto* column = new QVBoxLayout(this);

    // 1. method and its inputs.
    auto* methodBox = new QGroupBox(tr("Speed"), this);
    auto* methodForm = new QFormLayout(methodBox);
    const auto add = [&](QFormLayout* form, const FieldRow& r) {
        QWidget* editor = nullptr;
        if (r.make) {
            editor = r.make(form->parentWidget());
        } else {
            editor = makeLeafEditor(r.kind, {}, schema_->node(r.pointer), form->parentWidget());
        }
        editor->setObjectName(r.pointer);
        form->addRow(r.label, editor);
        binder_->bind(r.pointer, r.kind, editor, r.label, SchemaTree::enumValues(schema_->node(r.pointer)));
    };
    enabled_ = new QCheckBox(tr("Compute Corr Speed and Corr Dist"), methodBox);
    enabled_->setObjectName(QStringLiteral("enabled"));
    methodForm->addRow(enabled_);
    add(methodForm, row("/method", FieldKind::Enum, tr("Method")));
    add(methodForm, row("/fastestWheel/channels", FieldKind::Custom, tr("Fastest wheel of"),
                        [](QWidget* p) { return channelList(p, QStringLiteral("Wheel Speed")); }));
    add(methodForm, row("/alternateList/channels", FieldKind::Custom, tr("First channel found of"),
                        [](QWidget* p) { return channelList(p, QStringLiteral("Ground Speed")); }));
    add(methodForm, row("/gCorrection/speedChannel", FieldKind::Channel, tr("G correction: speed")));
    add(methodForm, row("/gCorrection/longGChannel", FieldKind::Channel, tr("G correction: longitudinal G")));
    add(methodForm, row("/gCorrection/lockThreshold", FieldKind::UnitValue,
                        tr("G correction: lock threshold"), [](QWidget* p) { return new UnitValueEdit(p); }));
    add(methodForm, row("/gCorrection/recoverSeconds", FieldKind::Number, tr("G correction: recovery (s)")));
    column->addWidget(methodBox);

    // 2. clamping.
    auto* clampBox = new QGroupBox(tr("Limits (speed display unit)"), this);
    auto* clampForm = new QFormLayout(clampBox);
    add(clampForm, row("/clamp/min", FieldKind::Custom, tr("Minimum"),
                       [](QWidget* p) { return optionalSpeed(p, tr("Limit")); }));
    add(clampForm, row("/clamp/minInvalidate", FieldKind::Bool, tr("Invalidate below the minimum")));
    add(clampForm, row("/clamp/max", FieldKind::Custom, tr("Maximum"),
                       [](QWidget* p) { return optionalSpeed(p, tr("Limit")); }));
    add(clampForm, row("/clamp/maxInvalidate", FieldKind::Bool, tr("Invalidate above the maximum")));
    column->addWidget(clampBox);

    // 3. distance (its unit follows the speed unit; display units are set in the Channel Editor).
    auto* distanceBox = new QGroupBox(tr("Distance"), this);
    auto* distanceForm = new QFormLayout(distanceBox);
    add(distanceForm, row("/distance/source", FieldKind::Enum, tr("Source")));
    add(distanceForm, row("/distance/loggedChannel", FieldKind::Channel, tr("Logged distance channel")));
    add(distanceForm, row("/distance/integration", FieldKind::Enum, tr("Integration")));
    add(distanceForm, row("/outputs/speed", FieldKind::Text, tr("Speed channel name")));
    add(distanceForm, row("/outputs/distance", FieldKind::Text, tr("Distance channel name")));
    auto* hint =
        new QLabel(tr("The distance unit follows the speed unit; change display units in the Channel "
                      "Editor."),
                   distanceBox);
    hint->setWordWrap(true);
    distanceForm->addRow(hint);
    column->addWidget(distanceBox);

    // 4. lap distance stretching (D-016).
    auto* stretchBox = new QGroupBox(tr("Lap distance stretching"), this);
    auto* stretchForm = new QFormLayout(stretchBox);
    stretch_ = new QCheckBox(tr("Stretch every lap to the reference length"), stretchBox);
    stretch_->setObjectName(QStringLiteral("stretch"));
    tolerance_ = new NumberEdit(stretchBox);
    tolerance_->setObjectName(QStringLiteral("tolerance"));
    stretchForm->addRow(stretch_);
    stretchForm->addRow(tr("Tolerance (%)"), tolerance_);
    auto* report = new QLabel(tr("The stretch of each lap is listed in the Lap Editor."), stretchBox);
    report->setWordWrap(true);
    stretchForm->addRow(report);
    column->addWidget(stretchBox);

    // 5. status and the comparability note.
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("status"));
    status_->setWordWrap(true);
    column->addWidget(status_);
    auto* note =
        new QLabel(tr("Results are close to, but not bit-identical with, other analysis tools."), this);
    note->setWordWrap(true);
    column->addWidget(note);
    message_ = new QLabel(this);
    message_->setObjectName(QStringLiteral("validation"));
    message_->setStyleSheet(QStringLiteral("color: #c02020;"));
    column->addWidget(message_);
    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
    column->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        apply();
        if (validationMessage().isEmpty()) {
            accept();
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this,
            &CorrectedSpeedDialog::apply);
    const auto revalidate = [this, buttons] {
        const QString reason = validationMessage();
        message_->setText(reason);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(reason.isEmpty());
        buttons->button(QDialogButtonBox::Apply)->setEnabled(reason.isEmpty());
    };
    connect(binder_, &FieldBinder::edited, this, revalidate);
    connect(tolerance_, &QLineEdit::textChanged, this, revalidate);
    connect(&session_, &qt::ProjectSession::documentsChanged, this, [this] {
        if (!binder_->isModified()) {
            reload(); // undo/redo of an Apply, or another edit of project.json
        }
    });
    reload();
}

CorrectedSpeedDialog::~CorrectedSpeedDialog() = default;

void CorrectedSpeedDialog::reload() {
    loaded_ = toObject(session_.correctedSpeedSettings());
    binder_->loadJson(loaded_);
    enabled_->setChecked(session_.correctedSpeedEnabled());
    const analysis::LapStretchSettings stretch = session_.lapStretchSettings();
    stretch_->setChecked(stretch.enabled);
    tolerance_->setValue(stretch.tolerance * 100.0);
    if (std::shared_ptr<const units::UnitRegistry> units = session_.units()) {
        if (Result<analysis::CorrectedSpeedSettings> s =
                analysis::CorrectedSpeedSettings::fromJson(session_.correctedSpeedSettings(), *units)) {
            settings_ = std::move(s).value();
        }
    }
    message_->clear();
    updateStatus();
}

void CorrectedSpeedDialog::setSettings(const analysis::CorrectedSpeedSettings& settings) {
    settings_ = settings;
    loaded_ = settingsJson(settings); // Apply writes them
    binder_->loadJson(loaded_);
    updateStatus();
}

QJsonObject CorrectedSpeedDialog::editedSettings() const {
    return binder_->storeJson(loaded_).toObject();
}

QString CorrectedSpeedDialog::validationMessage() const {
    QString reason;
    if (!binder_->validate(&reason)) {
        return reason;
    }
    if (tolerance_->hasText() && (!tolerance_->value() || *tolerance_->value() < 0.0)) {
        return tr("Tolerance (%): must be a number of 0 or more");
    }
    if (!schema_->isNull() && !schema_->validate(schema_->node(QString()), editedSettings(), &reason)) {
        return reason;
    }
    return {};
}

void CorrectedSpeedDialog::updateStatus() {
    const QString method = QString::fromLatin1(
        settings_.method == analysis::SpeedMethod::FastestWheel    ? "fastest wheel"
        : settings_.method == analysis::SpeedMethod::AlternateList ? "alternate channel list"
                                                                   : "G correction");
    status_->setText(
        session_.correctedSpeedEnabled()
            ? tr("Module %1: %2, applied to every open source").arg(QString::fromLatin1(kModuleId), method)
            : tr("Module %1 is off").arg(QString::fromLatin1(kModuleId)));
}

void CorrectedSpeedDialog::apply() {
    const QString reason = validationMessage();
    message_->setText(reason);
    if (!reason.isEmpty()) {
        return;
    }
    const QJsonObject settings = editedSettings();
    project::ProjectFile next = session_.projectFile();
    project::BuiltinMathsEntry& entry = next.builtinMaths[kModuleId];
    entry.version = 1;
    entry.enabled = enabled_->isChecked();
    if (!(settings == toObject(entry.settings)) && !(settings.isEmpty() && entry.settings.isNull())) {
        entry.settings = toDoc(settings);
    }
    // options.lapStretch {enabled, tolerancePct}
    const analysis::LapStretchSettings current = session_.lapStretchSettings();
    const double pct = tolerance_->value().value_or(current.tolerance * 100.0);
    if (stretch_->isChecked() != current.enabled || std::abs(pct - current.tolerance * 100.0) > 1e-12) {
        QJsonObject options = toObject(next.options);
        QJsonObject ls = options.value(QStringLiteral("lapStretch")).toObject();
        ls.insert(QStringLiteral("enabled"), stretch_->isChecked());
        ls.insert(QStringLiteral("tolerancePct"), pct);
        options.insert(QStringLiteral("lapStretch"), ls);
        next.options = toDoc(options);
    }
    if (next == session_.projectFile()) {
        return;
    }
    session_.undoStack().push(
        new qt::SetProjectOptionsCommand(session_, std::move(next), tr("Corrected speed and distance")));
    reload();
}

} // namespace esses::widgets
