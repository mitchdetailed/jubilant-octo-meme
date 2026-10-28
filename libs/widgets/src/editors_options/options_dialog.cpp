// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/base/json_doc.hpp>
#include <esses/base/utf8.hpp>
#include <esses/project/keymap.hpp>
#include <esses/project/scheme.hpp>
#include <esses/project/shipped_data.hpp>
#include <esses/qtmodel/memory_budget.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/settings_store.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/widgets/editors_options/options_dialog.hpp>
#include <esses/widgets/editors_options/scheme_selector.hpp>

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace esses::widgets {

namespace {

QString qPath(const std::filesystem::path& p) {
    return QString::fromStdWString(p.wstring());
}

std::filesystem::path fsPath(const QString& p) {
    return std::filesystem::path(p.toStdWString());
}

QJsonObject objectOf(const JsonDoc& doc) {
    return doc.isObject() ? QJsonDocument::fromJson(QByteArray::fromStdString(doc.compact())).object()
                          : QJsonObject();
}

// The member at a dotted path ("general.autosaveMinutes"); undefined when a step is missing.
QJsonValue valueAt(const QJsonObject& root, const QString& dotted) {
    const QStringList steps = dotted.split(QLatin1Char('.'));
    QJsonValue v(root);
    for (const QString& s : steps) {
        if (!v.isObject()) {
            return QJsonValue(QJsonValue::Undefined);
        }
        v = v.toObject().value(s);
    }
    return v;
}

// Sets the member at a dotted path, creating (or replacing non-object) intermediate objects.
void setValueAt(QJsonObject& root, const QStringList& steps, qsizetype at, const QJsonValue& value) {
    const QString& key = steps[at];
    if (at + 1 == steps.size()) {
        root.insert(key, value);
        return;
    }
    QJsonObject child = root.value(key).toObject();
    setValueAt(child, steps, at + 1, value);
    root.insert(key, child);
}

QString keyText(const std::vector<std::string>& keys) {
    QStringList out;
    for (const std::string& k : keys) {
        out << QString::fromStdString(k);
    }
    return out.join(QStringLiteral(", "));
}

QString commandText(const project::KeyBinding& row) {
    QString text = QString::fromStdString(row.command);
    if (!row.args.isNull()) {
        QString args = QString::fromStdString(row.args.compact());
        if (args.startsWith(QLatin1Char('[')) && args.endsWith(QLatin1Char(']'))) {
            args = args.mid(1, args.size() - 2);
        }
        text += QStringLiteral(" (") + args + QLatin1Char(')');
    }
    return text;
}

QTableWidget* makeTable(const QStringList& headers, const QString& name, QWidget* parent) {
    auto* t = new QTableWidget(0, static_cast<int>(headers.size()), parent);
    t->setObjectName(name);
    t->setHorizontalHeaderLabels(headers);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->verticalHeader()->setVisible(false);
    t->horizontalHeader()->setStretchLastSection(true);
    t->setAlternatingRowColors(true);
    return t;
}

// The memory budget field (M4 budget package): gigabytes of 2^30 bytes with one decimal, from the floor of
// qt::kMinBudgetBytes to kMaxBudgetGb.
constexpr double kBytesPerGb = 1073741824.0;
constexpr double kMaxBudgetGb = 4096.0;

std::int64_t gbToBytes(double gb) {
    const double clamped = std::clamp(gb, 0.0, kMaxBudgetGb); // range-checked before the conversion
    return static_cast<std::int64_t>(std::llround(clamped * kBytesPerGb));
}

QTableWidgetItem* cell(const QString& text) {
    auto* item = new QTableWidgetItem(text);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return item;
}

} // namespace

struct OptionsDialog::Impl {
    // One option field: a project option (dotted path into project.json "options") or a user setting.
    // Gigabytes: a byte count shown in GB (the user setting memory.chunkCacheBudgetBytes, M4 budget package).
    enum class Kind : std::uint8_t { Bool, Int, Double, Choice, Gigabytes };
    struct Field {
        QString key;
        QString label;
        Kind kind = Kind::Bool;
        bool user = false; // a SettingsStore key instead of a project option
        QJsonValue fallback;
        QWidget* widget = nullptr;
        QJsonValue shown; // the effective value read by the last reload()
        bool touched = false;
    };

    Impl(qt::ProjectSession& s, qt::SettingsStore* st) : session(s), settings(st) {}

    qt::ProjectSession& session;
    qt::SettingsStore* settings = nullptr;
    std::vector<Field> fields;
    SchemeSelector* selector = nullptr;
    QLabel* error = nullptr;
    QLabel* coloursNote = nullptr;
    QLabel* keysNote = nullptr;
    QTableWidget* fonts = nullptr;
    QTableWidget* keys = nullptr;
    QDialogButtonBox* buttons = nullptr;
    RevealHandler reveal;
    std::optional<project::Keymap> shippedKeymap;
    bool loading = false;

    Field* field(const QString& key) {
        for (Field& f : fields) {
            if (f.key == key) {
                return &f;
            }
        }
        return nullptr;
    }

    static bool same(const Field& f, const QJsonValue& a, const QJsonValue& b) {
        switch (f.kind) {
        case Kind::Bool:
            return a.toBool() == b.toBool();
        case Kind::Int:
        case Kind::Double:
            return a.toDouble() == b.toDouble();
        case Kind::Choice:
            return a.toString() == b.toString();
        case Kind::Gigabytes: // equal as the field shows them (tenths of a GB)
            return std::llround(a.toDouble() / kBytesPerGb * 10.0) ==
                   std::llround(b.toDouble() / kBytesPerGb * 10.0);
        }
        return false;
    }

    QJsonValue widgetValue(const Field& f) const {
        switch (f.kind) {
        case Kind::Bool:
            return static_cast<QCheckBox*>(f.widget)->isChecked();
        case Kind::Int:
            return static_cast<QSpinBox*>(f.widget)->value();
        case Kind::Double:
            return static_cast<QDoubleSpinBox*>(f.widget)->value();
        case Kind::Choice:
            return static_cast<QComboBox*>(f.widget)->currentData().toString();
        case Kind::Gigabytes:
            return QJsonValue(
                static_cast<qint64>(gbToBytes(static_cast<QDoubleSpinBox*>(f.widget)->value())));
        }
        return {};
    }

    void setWidget(const Field& f, const QJsonValue& v) {
        switch (f.kind) {
        case Kind::Bool:
            static_cast<QCheckBox*>(f.widget)->setChecked(v.toBool());
            break;
        case Kind::Int:
            static_cast<QSpinBox*>(f.widget)->setValue(v.toInt());
            break;
        case Kind::Double:
            static_cast<QDoubleSpinBox*>(f.widget)->setValue(v.toDouble());
            break;
        case Kind::Choice: {
            auto* combo = static_cast<QComboBox*>(f.widget);
            const int i = combo->findData(v.toString());
            combo->setCurrentIndex(i >= 0 ? i : 0);
            break;
        }
        case Kind::Gigabytes:
            static_cast<QDoubleSpinBox*>(f.widget)->setValue(v.toDouble() / kBytesPerGb);
            break;
        }
    }

    // The value a field shows: the stored one when it has the right type, else the 02 section 6.2 default.
    QJsonValue effective(const Field& f, const QJsonObject& options) const {
        if (f.kind == Kind::Gigabytes) {
            // The budget as the shell applies it (raised to 1 GB), else the runtime's default (25 % of RAM).
            const std::optional<std::size_t> bytes =
                settings == nullptr ? std::nullopt : qt::budgetFromSettings(*settings);
            return bytes ? QJsonValue(static_cast<double>(*bytes)) : f.fallback;
        }
        QJsonValue v;
        if (f.user) {
            const QVariant s = settings == nullptr ? QVariant() : settings->value(f.key);
            v = s.isValid() ? QJsonValue::fromVariant(s) : QJsonValue(QJsonValue::Undefined);
        } else {
            v = valueAt(options, f.key);
        }
        switch (f.kind) {
        case Kind::Bool:
            return v.isBool() ? v : f.fallback;
        case Kind::Int:
            return v.isDouble() && v.toDouble() >= 0.0 && std::floor(v.toDouble()) == v.toDouble()
                       ? QJsonValue(v.toInt())
                       : f.fallback;
        case Kind::Double:
            return v.isDouble() ? v : f.fallback;
        case Kind::Choice: {
            const auto* combo = static_cast<const QComboBox*>(f.widget);
            return v.isString() && combo->findData(v.toString()) >= 0 ? v : f.fallback;
        }
        case Kind::Gigabytes:
            break; // above
        }
        return f.fallback;
    }
};

OptionsDialog::OptionsDialog(qt::ProjectSession& session, qt::SettingsStore* settings, QWidget* parent)
    : QDialog(parent), d_(std::make_unique<Impl>(session, settings)) {
    setObjectName(QStringLiteral("optionsDialog"));
    setWindowTitle(tr("Options"));
    setAccessibleName(tr("Options"));
    d_->reveal = [](const std::filesystem::path& folder) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(qPath(folder)));
    };
    auto* layout = new QVBoxLayout(this);
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("optionsTabs"));
    layout->addWidget(tabs_, 1);
    buildGeneral();
    buildAnalysis();
    buildColours();
    buildFonts();
    buildKeys();
    d_->error = new QLabel(this);
    d_->error->setObjectName(QStringLiteral("optionsError"));
    d_->error->setWordWrap(true);
    d_->error->setStyleSheet(QStringLiteral("color: #c62828"));
    d_->error->hide();
    layout->addWidget(d_->error);
    d_->buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
    d_->buttons->setObjectName(QStringLiteral("optionsButtons"));
    layout->addWidget(d_->buttons);
    connect(d_->buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(d_->buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(d_->buttons->button(QDialogButtonBox::Apply), &QAbstractButton::clicked, this,
            [this] { apply(); });

    // Every field marks the dialog edited when the user changes it (not while reload() fills it).
    for (Impl::Field& f : d_->fields) {
        const QString key = f.key;
        const auto touched = [this, key] {
            if (!d_->loading) {
                if (Impl::Field* changed = d_->field(key)) {
                    changed->touched = true;
                }
                markEdited();
            }
        };
        switch (f.kind) {
        case Impl::Kind::Bool:
            connect(static_cast<QCheckBox*>(f.widget), &QAbstractButton::toggled, this, touched);
            break;
        case Impl::Kind::Int:
            connect(static_cast<QSpinBox*>(f.widget), &QSpinBox::valueChanged, this, touched);
            break;
        case Impl::Kind::Double:
        case Impl::Kind::Gigabytes:
            connect(static_cast<QDoubleSpinBox*>(f.widget), &QDoubleSpinBox::valueChanged, this, touched);
            break;
        case Impl::Kind::Choice:
            connect(static_cast<QComboBox*>(f.widget), &QComboBox::currentIndexChanged, this, touched);
            break;
        }
    }
    // An undo or redo of an options edit (or a recovery) shows the restored values unless edits are pending.
    connect(&d_->session, &qt::ProjectSession::documentsChanged, this, [this] {
        if (!hasPendingEdits()) {
            reloadFields(); // cheap: the scheme list and the tables are re-read when the dialog is shown
        }
    });
    connect(&d_->session, &qt::ProjectSession::schemeChanged, this, [this] { fillFonts(); });
    // A user setting changed elsewhere (choosing a scheme turns "follow OS" off) shows unless edited here.
    if (d_->settings != nullptr) {
        connect(d_->settings, &qt::SettingsStore::changed, this, [this](const QString& key) {
            Impl::Field* f = d_->field(key);
            if (f == nullptr || !f->user || f->touched) {
                return;
            }
            d_->loading = true;
            f->shown = d_->effective(*f, QJsonObject());
            d_->setWidget(*f, f->shown);
            d_->loading = false;
        });
    }
    reload();
}

OptionsDialog::~OptionsDialog() = default;

SchemeSelector& OptionsDialog::schemeSelector() {
    return *d_->selector;
}

void OptionsDialog::setRevealHandler(RevealHandler reveal) {
    d_->reveal = std::move(reveal);
}

// ---- tabs -------------------------------------------------------------------------------------------------

namespace {

QCheckBox* addCheck(QFormLayout& form, const QString& key, const QString& text, QWidget* parent) {
    auto* box = new QCheckBox(text, parent);
    box->setObjectName(key);
    box->setAccessibleName(text);
    form.addRow(box);
    return box;
}

} // namespace

void OptionsDialog::buildGeneral() {
    auto* page = new QWidget(tabs_);
    page->setObjectName(QStringLiteral("optionsGeneral"));
    auto* form = new QFormLayout(page);
    using K = Impl::Kind;
    const auto check = [&](const char* key, const QString& text, bool fallback, bool user = false) {
        const QString k = QString::fromLatin1(key);
        d_->fields.push_back(
            {k, text, K::Bool, user, QJsonValue(fallback), addCheck(*form, k, text, page), {}, false});
    };
    check("general.saveOnExit", tr("Save the project on exit (ask when off)"), true);
    auto* autosave = new QSpinBox(page);
    autosave->setObjectName(QStringLiteral("general.autosaveMinutes"));
    autosave->setRange(0, 1440);
    autosave->setSuffix(tr(" min"));
    autosave->setSpecialValueText(tr("Off"));
    autosave->setAccessibleName(tr("Autosave interval"));
    form->addRow(tr("Autosave interval:"), autosave);
    d_->fields.push_back(
        {autosave->objectName(), tr("Autosave interval"), K::Int, false, QJsonValue(2), autosave, {}, false});
    check("general.loadPreviousData", tr("Load previous data with the project"), true);
    check("general.selectFastestOnLoad", tr("Select the fastest lap on load"), false);
    check("general.looseChannelSearch", tr("Loose channel-name search"), false);
    check("general.autoShowMessagesOnError", tr("Show the Messages window on an error"), true);
    check("general.reduceCpuDuringAnimation", tr("Reduce CPU use during animation"), false);
    // M4 budget package (07 section 7.1 row 1, 01 section 7.2): the chunk cache's memory budget, a user
    // setting; the open of a file whose estimate exceeds it waits for Continue or Cancel in Messages.
    const double defaultGb = static_cast<double>(store::ChunkCache::defaultBudgetBytes()) / kBytesPerGb;
    auto* budget = new QDoubleSpinBox(page);
    budget->setObjectName(QString::fromLatin1(qt::kBudgetSettingKey));
    budget->setAccessibleName(tr("Memory budget"));
    budget->setRange(static_cast<double>(qt::kMinBudgetBytes) / kBytesPerGb, kMaxBudgetGb);
    budget->setDecimals(1);
    budget->setSingleStep(0.5);
    budget->setSuffix(tr(" GB"));
    budget->setToolTip(
        tr("Memory for the logged and maths data held in memory, at least 1 GB. Opening a file "
           "estimated above it asks first (Continue or Cancel in Messages). Default: a quarter "
           "of the installed memory, %1 GB.")
            .arg(defaultGb, 0, 'f', 1));
    form->addRow(tr("Memory budget:"), budget);
    d_->fields.push_back({budget->objectName(),
                          tr("Memory budget"),
                          K::Gigabytes,
                          true,
                          QJsonValue(static_cast<double>(store::ChunkCache::defaultBudgetBytes())),
                          budget,
                          {},
                          false});
    auto* language = new QComboBox(page);
    language->setObjectName(QStringLiteral("language"));
    language->setAccessibleName(tr("Language"));
    language->addItem(QStringLiteral("English"), QStringLiteral("en"));
    form->addRow(tr("Language:"), language);
    d_->fields.push_back({language->objectName(),
                          tr("Language"),
                          K::Choice,
                          true,
                          QJsonValue(QStringLiteral("en")),
                          language,
                          {},
                          false});
    tabs_->addTab(page, tr("General"));
}

void OptionsDialog::buildAnalysis() {
    auto* page = new QWidget(tabs_);
    page->setObjectName(QStringLiteral("optionsAnalysis"));
    auto* form = new QFormLayout(page);
    using K = Impl::Kind;
    const auto check = [&](const char* key, const QString& text, bool fallback) {
        const QString k = QString::fromLatin1(key);
        d_->fields.push_back(
            {k, text, K::Bool, false, QJsonValue(fallback), addCheck(*form, k, text, page), {}, false});
    };
    const auto choice = [&](const char* key, const QString& text,
                            const QList<std::pair<QString, QString>>& items, const QString& fallback) {
        auto* combo = new QComboBox(page);
        combo->setObjectName(QString::fromLatin1(key));
        combo->setAccessibleName(text);
        for (const auto& [label, value] : items) {
            combo->addItem(label, value);
        }
        form->addRow(text + QLatin1Char(':'), combo);
        d_->fields.push_back(
            {combo->objectName(), text, K::Choice, false, QJsonValue(fallback), combo, {}, false});
    };
    const auto number = [&](const char* key, const QString& text, double max, double fallback) {
        auto* spin = new QDoubleSpinBox(page);
        spin->setObjectName(QString::fromLatin1(key));
        spin->setAccessibleName(text);
        spin->setRange(0.0, max);
        spin->setDecimals(2);
        spin->setSingleStep(0.1);
        spin->setSuffix(QStringLiteral(" %"));
        form->addRow(text + QLatin1Char(':'), spin);
        d_->fields.push_back(
            {spin->objectName(), text, K::Double, false, QJsonValue(fallback), spin, {}, false});
    };
    check("autoscale.includeUntrusted", tr("Include untrusted data in autoscale"), false);
    check("autoscale.includeOverlays", tr("Include overlay data in autoscale"), true);
    const QList<std::pair<QString, QString>> formats{
        {QStringLiteral("m:ss.fff"), QStringLiteral("m:ss.fff")},
        {QStringLiteral("ss.fff"), QStringLiteral("ss.fff")},
        {QStringLiteral("h:mm:ss.f"), QStringLiteral("h:mm:ss.f")}};
    choice("timeFormat", tr("Time format"), formats, QStringLiteral("m:ss.fff"));
    choice("lapTimeFormat", tr("Lap time format"), formats, QStringLiteral("m:ss.fff"));
    number("fastLapTolerancePct", tr("Fast-lap tolerance"), 100.0, 1.0);
    number("lapStretch.tolerancePct", tr("Lap distance stretching tolerance"), 100.0, 5.0);
    choice("maths.precision", tr("Maths precision"),
           {{tr("Single (F32)"), QStringLiteral("single")}, {tr("Double (F64)"), QStringLiteral("double")}},
           QStringLiteral("single"));
    check("maths.compat.unaryMinusAbovePower", tr("Unary minus above power (-2^2 = 4)"), false);
    tabs_->addTab(page, tr("Analysis"));
}

void OptionsDialog::buildColours() {
    auto* page = new QWidget(tabs_);
    page->setObjectName(QStringLiteral("optionsColours"));
    auto* form = new QFormLayout(page);
    d_->selector = new SchemeSelector(d_->session, d_->settings, page);
    auto* row = new QHBoxLayout;
    row->addWidget(d_->selector, 1);
    auto* duplicate = new QPushButton(tr("Duplicate"), page);
    duplicate->setObjectName(QStringLiteral("schemeDuplicate"));
    auto* importButton = new QPushButton(tr("Import..."), page);
    importButton->setObjectName(QStringLiteral("schemeImport"));
    auto* exportButton = new QPushButton(tr("Export..."), page);
    exportButton->setObjectName(QStringLiteral("schemeExport"));
    row->addWidget(duplicate);
    row->addWidget(importButton);
    row->addWidget(exportButton);
    form->addRow(tr("Scheme:"), row);
    using K = Impl::Kind;
    const auto check = [&](const char* key, const QString& text) {
        const QString k = QString::fromLatin1(key);
        d_->fields.push_back(
            {k, text, K::Bool, true, QJsonValue(false), addCheck(*form, k, text, page), {}, false});
    };
    check(SchemeSelector::kToolbarKey, tr("Show the scheme selector on the toolbar"));
    check(SchemeSelector::kFollowOsKey, tr("Follow the OS light/dark setting"));
    d_->coloursNote =
        new QLabel(tr("Schemes are edited as JSON files: Duplicate writes an editable copy to the "
                      "user scheme folder."),
                   page);
    d_->coloursNote->setObjectName(QStringLiteral("coloursNote"));
    d_->coloursNote->setWordWrap(true);
    form->addRow(d_->coloursNote);
    connect(duplicate, &QPushButton::clicked, this, [this] {
        const Result<std::filesystem::path> file = d_->selector->duplicateActive();
        if (!file) {
            d_->coloursNote->setText(tr("The scheme could not be duplicated: %1")
                                         .arg(QString::fromStdString(file.error().toString())));
            return;
        }
        d_->coloursNote->setText(tr("Copied to %1").arg(qPath(*file)));
        if (d_->reveal) {
            d_->reveal(file->parent_path());
        }
    });
    connect(importButton, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, tr("Import Scheme"), {},
                                                          tr("Schemes (*.scheme.json);;JSON files (*.json)"));
        if (file.isEmpty()) {
            return;
        }
        const Result<QString> name = d_->selector->importScheme(fsPath(file));
        d_->coloursNote->setText(name ? tr("Imported \"%1\"").arg(*name)
                                      : tr("The scheme could not be imported: %1")
                                            .arg(QString::fromStdString(name.error().toString())));
    });
    connect(exportButton, &QPushButton::clicked, this, [this] {
        const QString file =
            QFileDialog::getSaveFileName(this, tr("Export Scheme"), {}, tr("Schemes (*.scheme.json)"));
        if (file.isEmpty()) {
            return;
        }
        const Result<void> r = d_->selector->exportActive(fsPath(file));
        d_->coloursNote->setText(
            r ? tr("Exported to %1").arg(file)
              : tr("The scheme could not be exported: %1").arg(QString::fromStdString(r.error().toString())));
    });
    tabs_->addTab(page, tr("Colours"));
}

void OptionsDialog::buildFonts() {
    auto* page = new QWidget(tabs_);
    page->setObjectName(QStringLiteral("optionsFonts"));
    auto* layout = new QVBoxLayout(page);
    d_->fonts = makeTable({tr("Role"), tr("Family"), tr("Size"), tr("Weight"), tr("Style")},
                          QStringLiteral("fontsTable"), page);
    d_->fonts->setAccessibleName(tr("Fonts of the active scheme"));
    layout->addWidget(d_->fonts, 1);
    auto* note =
        new QLabel(tr("The fonts belong to the colour scheme; edit a duplicated scheme file to change "
                      "them."),
                   page);
    note->setWordWrap(true);
    layout->addWidget(note);
    tabs_->addTab(page, tr("Fonts"));
}

void OptionsDialog::buildKeys() {
    auto* page = new QWidget(tabs_);
    page->setObjectName(QStringLiteral("optionsKeys"));
    auto* layout = new QVBoxLayout(page);
    d_->keys = makeTable({tr("Command"), tr("Context"), tr("Keys"), tr("Default keys"), tr("Status")},
                         QStringLiteral("keysTable"), page);
    d_->keys->setAccessibleName(tr("Key bindings"));
    layout->addWidget(d_->keys, 1);
    auto* row = new QHBoxLayout;
    auto* importButton = new QPushButton(tr("Import..."), page);
    importButton->setObjectName(QStringLiteral("keysImport"));
    auto* exportButton = new QPushButton(tr("Export..."), page);
    exportButton->setObjectName(QStringLiteral("keysExport"));
    auto* reveal = new QPushButton(tr("Reveal"), page);
    reveal->setObjectName(QStringLiteral("keysReveal"));
    row->addWidget(importButton);
    row->addWidget(exportButton);
    row->addWidget(reveal);
    row->addStretch(1);
    layout->addLayout(row);
    d_->keysNote = new QLabel(tr("Keys are changed in keymap.json in the configuration folder (Reveal); an "
                                 "imported keymap applies at the next start."),
                              page);
    d_->keysNote->setObjectName(QStringLiteral("keysNote"));
    d_->keysNote->setWordWrap(true);
    layout->addWidget(d_->keysNote);
    connect(importButton, &QPushButton::clicked, this, [this] {
        const QString file =
            QFileDialog::getOpenFileName(this, tr("Import Keymap"), {}, tr("Keymaps (*.json)"));
        if (file.isEmpty()) {
            return;
        }
        const Result<void> r = importKeymap(fsPath(file));
        d_->keysNote->setText(
            r ? tr("Imported; the keymap applies at the next start")
              : tr("The keymap could not be imported: %1").arg(QString::fromStdString(r.error().toString())));
    });
    connect(exportButton, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getSaveFileName(
            this, tr("Export Keymap"), QStringLiteral("keymap.json"), tr("Keymaps (*.json)"));
        if (file.isEmpty()) {
            return;
        }
        const Result<void> r = exportKeymap(fsPath(file));
        d_->keysNote->setText(
            r ? tr("Exported to %1").arg(file)
              : tr("The keymap could not be exported: %1").arg(QString::fromStdString(r.error().toString())));
    });
    connect(reveal, &QPushButton::clicked, this, [this] { revealKeymap(); });
    tabs_->addTab(page, tr("Keys"));
}

void OptionsDialog::fillFonts() {
    QTableWidget& t = *d_->fonts;
    t.setRowCount(0);
    const std::shared_ptr<const project::Scheme> scheme = d_->session.scheme();
    if (scheme == nullptr) {
        return;
    }
    for (const std::string_view role : project::kSchemeFontRoles) {
        const project::FontSpec f = scheme->font(role);
        const int r = t.rowCount();
        t.insertRow(r);
        QStringList style;
        if (f.italic) {
            style << tr("italic");
        }
        if (f.strikeOut) {
            style << tr("strike-out");
        }
        t.setItem(r, 0, cell(QString::fromUtf8(role.data(), static_cast<qsizetype>(role.size()))));
        t.setItem(r, 1, cell(QString::fromStdString(f.family)));
        t.setItem(r, 2, cell(QString::number(f.pointSize, 'g', 4)));
        t.setItem(r, 3, cell(QString::number(f.weight)));
        t.setItem(r, 4, cell(style.isEmpty() ? tr("normal") : style.join(QStringLiteral(", "))));
    }
    t.resizeColumnsToContents();
}

void OptionsDialog::fillKeys() {
    QTableWidget& t = *d_->keys;
    t.setRowCount(0);
    if (!d_->shippedKeymap) {
        std::vector<std::string> log;
        Result<project::Keymap> shipped =
            project::Keymap::load(project::shippedFile(d_->session.paths(), project::kDefaultKeymap), log);
        d_->shippedKeymap = shipped ? std::move(shipped).value() : project::Keymap{};
    }
    const project::Keymap& active = d_->session.keymap();
    const auto same = [](const project::KeyBinding& a, const project::KeyBinding& b) {
        return a.command == b.command && a.context == b.context && a.args == b.args;
    };
    const auto add = [&t](const project::KeyBinding& row, const QString& keys, const QString& defaults,
                          const QString& status) {
        const int r = t.rowCount();
        t.insertRow(r);
        QTableWidgetItem* command = cell(commandText(row));
        if (!row.doc.empty()) {
            command->setToolTip(QString::fromStdString(row.doc));
        }
        t.setItem(r, 0, command);
        t.setItem(r, 1, cell(QString::fromStdString(row.context)));
        t.setItem(r, 2, cell(keys));
        t.setItem(r, 3, cell(defaults));
        t.setItem(r, 4, cell(status));
    };
    for (const project::KeyBinding& base : d_->shippedKeymap->bindings) {
        const project::KeyBinding* current = nullptr;
        for (const project::KeyBinding& row : active.bindings) {
            if (same(row, base)) {
                current = &row;
                break;
            }
        }
        const QString status =
            QString::fromLatin1(project::keymapStatusName(base.status).data(),
                                static_cast<qsizetype>(project::keymapStatusName(base.status).size()));
        add(base, current == nullptr ? tr("(unbound)") : keyText(current->keys), keyText(base.keys),
            base.milestone.empty()
                ? status
                : status + QStringLiteral(" (") + QString::fromStdString(base.milestone) + QLatin1Char(')'));
    }
    for (const project::KeyBinding& row : active.bindings) {
        bool known = false;
        for (const project::KeyBinding& base : d_->shippedKeymap->bindings) {
            known = known || same(row, base);
        }
        if (!known) {
            add(row, keyText(row.keys), QString(), tr("user"));
        }
    }
    t.resizeColumnsToContents();
}

// ---- state ------------------------------------------------------------------------------------------------

void OptionsDialog::showTab(Tab tab) {
    if (tabs_->count() > static_cast<int>(tab)) {
        tabs_->setCurrentIndex(static_cast<int>(tab));
    }
}

void OptionsDialog::reload() {
    reloadFields();
    d_->selector->refresh();
    fillFonts();
    fillKeys();
}

void OptionsDialog::reloadFields() {
    d_->loading = true;
    const QJsonObject options = objectOf(d_->session.projectFile().options);
    for (Impl::Field& f : d_->fields) {
        f.shown = d_->effective(f, options);
        d_->setWidget(f, f.shown);
        f.touched = false;
    }
    d_->loading = false;
    d_->error->clear();
    d_->error->hide();
    markEdited();
}

bool OptionsDialog::hasPendingEdits() const {
    for (const Impl::Field& f : d_->fields) {
        if (f.touched) {
            return true;
        }
    }
    return false;
}

void OptionsDialog::markEdited() {
    if (d_->buttons != nullptr) {
        d_->buttons->button(QDialogButtonBox::Apply)->setEnabled(hasPendingEdits());
    }
}

QString OptionsDialog::errorText() const {
    return d_->error->text();
}

bool OptionsDialog::validate(QString& error) const {
    for (const Impl::Field& f : d_->fields) {
        const auto* spin = qobject_cast<const QAbstractSpinBox*>(f.widget);
        if (spin == nullptr || spin->hasAcceptableInput()) {
            continue;
        }
        if (f.kind == Impl::Kind::Int) {
            const auto* s = static_cast<const QSpinBox*>(f.widget);
            error = tr("%1: enter a whole number of minutes from %2 to %3 (0 = off)")
                        .arg(f.label)
                        .arg(s->minimum())
                        .arg(s->maximum());
        } else if (f.kind == Impl::Kind::Gigabytes) {
            const auto* s = static_cast<const QDoubleSpinBox*>(f.widget);
            error = tr("%1: enter a size from %2 to %3 GB")
                        .arg(f.label)
                        .arg(s->minimum(), 0, 'f', 1)
                        .arg(s->maximum(), 0, 'f', 1);
        } else {
            const auto* s = static_cast<const QDoubleSpinBox*>(f.widget);
            error =
                tr("%1: enter a percentage from %2 to %3").arg(f.label).arg(s->minimum()).arg(s->maximum());
        }
        return false;
    }
    return true;
}

bool OptionsDialog::apply() {
    QString error;
    if (!validate(error)) {
        d_->error->setText(error);
        d_->error->show();
        return false;
    }
    d_->error->clear();
    d_->error->hide();
    // Project options: only the members the user changed, so an untouched absent member stays absent.
    project::ProjectFile next = d_->session.projectFile();
    QJsonObject options = objectOf(next.options);
    bool optionsChanged = false;
    for (Impl::Field& f : d_->fields) {
        const QJsonValue v = d_->widgetValue(f);
        if (!f.touched || Impl::same(f, v, f.shown)) {
            continue;
        }
        if (f.user) {
            if (d_->settings != nullptr) {
                d_->settings->setValue(f.key, v.toVariant());
            }
        } else {
            setValueAt(options, f.key.split(QLatin1Char('.')), 0, v);
            optionsChanged = true;
        }
    }
    if (optionsChanged) {
        Result<JsonDoc> doc =
            JsonDoc::parse(QJsonDocument(options).toJson(QJsonDocument::Compact).toStdString());
        if (!doc) {
            d_->error->setText(tr("The options could not be written: %1")
                                   .arg(QString::fromStdString(doc.error().toString())));
            d_->error->show();
            return false;
        }
        if (*doc != next.options) {
            next.options = std::move(doc).value();
            d_->session.undoStack().push(
                new qt::SetProjectOptionsCommand(d_->session, std::move(next), tr("Change options")));
        }
    }
    // The selector applies "follow OS" at once (the user settings are written above).
    if (const Impl::Field* follow = d_->field(QString::fromLatin1(SchemeSelector::kFollowOsKey));
        follow != nullptr && follow->touched && d_->selector->followsSystem()) {
        d_->selector->applySystemScheme();
    }
    const QJsonObject now = objectOf(d_->session.projectFile().options);
    for (Impl::Field& f : d_->fields) {
        f.shown = d_->effective(f, now);
        f.touched = false;
    }
    markEdited();
    Q_EMIT applied();
    return true;
}

void OptionsDialog::showEvent(QShowEvent* event) {
    if (!event->spontaneous()) {
        reload(); // every opening starts from the current options
    }
    QDialog::showEvent(event);
}

void OptionsDialog::done(int result) {
    if (result == QDialog::Accepted && !apply()) {
        return; // an invalid field keeps the dialog open with the reason (06 section 13)
    }
    QDialog::done(result);
}

// ---- keys -------------------------------------------------------------------------------------------------

Result<void> OptionsDialog::importKeymap(const std::filesystem::path& file) {
    std::vector<std::string> log;
    Result<project::Keymap> user = project::Keymap::load(file, log);
    if (!user) {
        return Failure{user.error()};
    }
    if (!d_->shippedKeymap) {
        fillKeys();
    }
    const project::Keymap merged = project::Keymap::merged(*d_->shippedKeymap, *user);
    if (const std::vector<std::string> conflicts = merged.conflicts(); !conflicts.empty()) {
        return fail(ErrorCode::Invalid, conflicts.front());
    }
    const std::filesystem::path config = d_->session.paths().configDir();
    std::error_code ec;
    std::filesystem::create_directories(config, ec);
    return user->save(config / "keymap.json");
}

Result<void> OptionsDialog::exportKeymap(const std::filesystem::path& file) const {
    return d_->session.keymap().save(file);
}

void OptionsDialog::revealKeymap() {
    if (d_->reveal) {
        d_->reveal(d_->session.paths().configDir());
    }
}

} // namespace esses::widgets
