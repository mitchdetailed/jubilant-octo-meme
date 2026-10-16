// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// OptionsDialog (06 section 13.7, 02 section 6.2; D-032), offscreen over a ProjectSession with the shipped
// defaults and a private configuration directory: the five tabs; every field shows the project option (the 02
// section 6.2 default when the member is absent) or the user setting; Apply writes only the changed members
// as one undoable "Change options" command (nothing for no change, view state untouched, undo and redo shown
// in the fields) and the user settings; an invalid field refuses Apply and OK with the reason and keeps the
// dialog open; the autosave interval and the maths compat switch reach the session; the Fonts tab lists the
// active scheme's font roles and follows a scheme change; the Keys tab lists every shipped row with the
// user's keys and imports (refusing conflicts), exports and reveals the keymap.

#include "../../../qtmodel/tests/qtmodel_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/project/keymap.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/settings_store.hpp>
#include <esses/widgets/editors_options/options_dialog.hpp>
#include <esses/widgets/editors_options/scheme_selector.hpp>

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <chrono>
#include <fstream>
#include <memory>

// The qtmodel helpers (repoRoot), compiled into this executable only.
#include "../../../qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;

namespace {

std::filesystem::path fsPath(const QString& p) {
    return std::filesystem::path(p.toStdWString());
}

struct World {
    QTemporaryDir config;
    std::unique_ptr<qt::SettingsStore> settings;
    std::unique_ptr<qt::ProjectSession> session;

    bool start() {
        if (!config.isValid()) {
            return false;
        }
        PathOverrides po;
        po.configDir = fsPath(config.path());
        po.dataDir = qt::test::repoRoot() / "data";
        auto paths = Paths::resolve(po);
        if (!paths) {
            return false;
        }
        settings = std::make_unique<qt::SettingsStore>(paths->configDir() / "settings.json");
        session = std::make_unique<qt::ProjectSession>(*paths, settings.get());
        return session->loadDefaults().ok();
    }
    QJsonObject options() const {
        return QJsonDocument::fromJson(QByteArray::fromStdString(session->projectFile().options.compact()))
            .object();
    }
    // Replaces the session's options without the undo stack (a project as read from disk).
    void setOptions(const QJsonObject& o) {
        project::ProjectFile p = session->projectFile();
        p.options = JsonDoc::parse(QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString()).value();
        session->setProjectFile(std::move(p));
    }
};

template <class W> W* field(OptionsDialog& d, const char* key) {
    W* w = d.findChild<W*>(QString::fromLatin1(key));
    if (w == nullptr) {
        qFatal("no field %s", key);
    }
    return w;
}

QAbstractButton* button(OptionsDialog& d, QDialogButtonBox::StandardButton which) {
    return d.findChild<QDialogButtonBox*>(QStringLiteral("optionsButtons"))->button(which);
}

QString cellText(const QTableWidget& t, int row, int column) {
    const QTableWidgetItem* item = t.item(row, column);
    return item == nullptr ? QString() : item->text();
}

} // namespace

class TstOptionsDialog : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void tabsAndDefaults() {
        World w;
        QVERIFY(w.start());
        // An older project.json without most options: the fields show the 02 section 6.2 defaults.
        w.setOptions(
            QJsonObject{{QStringLiteral("general"), QJsonObject{{QStringLiteral("autosaveMinutes"), 7}}},
                        {QStringLiteral("timeFormat"), QStringLiteral("ss.fff")}});
        OptionsDialog d(*w.session, w.settings.get());
        QCOMPARE(d.windowTitle(), QStringLiteral("Options"));
        QStringList tabs;
        for (int i = 0; i < d.tabs().count(); ++i) {
            tabs << d.tabs().tabText(i);
        }
        QCOMPARE(tabs,
                 (QStringList{QStringLiteral("General"), QStringLiteral("Analysis"),
                              QStringLiteral("Colours"), QStringLiteral("Fonts"), QStringLiteral("Keys")}));
        d.showTab(OptionsDialog::Tab::Keys);
        QCOMPARE(d.tabs().currentIndex(), 4);
        QCOMPARE(field<QSpinBox>(d, "general.autosaveMinutes")->value(), 7);
        QVERIFY(field<QCheckBox>(d, "general.saveOnExit")->isChecked());
        QVERIFY(field<QCheckBox>(d, "general.loadPreviousData")->isChecked());
        QVERIFY(!field<QCheckBox>(d, "general.looseChannelSearch")->isChecked());
        QVERIFY(field<QCheckBox>(d, "general.autoShowMessagesOnError")->isChecked());
        QVERIFY(field<QCheckBox>(d, "autoscale.includeOverlays")->isChecked());
        QCOMPARE(field<QComboBox>(d, "timeFormat")->currentData().toString(), QStringLiteral("ss.fff"));
        QCOMPARE(field<QComboBox>(d, "lapTimeFormat")->currentData().toString(), QStringLiteral("m:ss.fff"));
        QCOMPARE(field<QDoubleSpinBox>(d, "fastLapTolerancePct")->value(), 1.0);
        QCOMPARE(field<QDoubleSpinBox>(d, "lapStretch.tolerancePct")->value(), 5.0);
        QCOMPARE(field<QComboBox>(d, "maths.precision")->currentData().toString(), QStringLiteral("single"));
        QVERIFY(!field<QCheckBox>(d, "maths.compat.unaryMinusAbovePower")->isChecked());
        QCOMPARE(field<QComboBox>(d, "language")->currentData().toString(), QStringLiteral("en"));
        QVERIFY(!field<QCheckBox>(d, SchemeSelector::kToolbarKey)->isChecked());
        QVERIFY(!field<QCheckBox>(d, SchemeSelector::kFollowOsKey)->isChecked());
        QVERIFY(!d.hasPendingEdits());
        QVERIFY(!button(d, QDialogButtonBox::Apply)->isEnabled());
        // Apply without an edit changes nothing (no command, absent members stay absent).
        QVERIFY(d.apply());
        QCOMPARE(w.session->undoStack().count(), 0);
        QVERIFY(!w.options().contains(QStringLiteral("autoscale")));
    }

    void applyIsOneUndoableCommand() {
        World w;
        QVERIFY(w.start());
        project::SelectionKeys keys;
        keys.singleColourMain = true;
        w.session->setLastSelection(keys); // view state in project.json
        const QJsonObject before = w.options();
        OptionsDialog d(*w.session, w.settings.get());
        field<QSpinBox>(d, "general.autosaveMinutes")->setValue(5);
        field<QCheckBox>(d, "maths.compat.unaryMinusAbovePower")->setChecked(true);
        field<QComboBox>(d, "maths.precision")->setCurrentIndex(1);
        field<QDoubleSpinBox>(d, "fastLapTolerancePct")->setValue(2.5);
        field<QComboBox>(d, "lapTimeFormat")->setCurrentIndex(2);
        QVERIFY(d.hasPendingEdits());
        QVERIFY(button(d, QDialogButtonBox::Apply)->isEnabled());
        QSignalSpy applied(&d, &OptionsDialog::applied);
        button(d, QDialogButtonBox::Apply)->click();
        QCOMPARE(applied.count(), 1);
        QVERIFY(!d.hasPendingEdits());
        QCOMPARE(w.session->undoStack().count(), 1);
        QCOMPARE(w.session->undoStack().undoText(), QStringLiteral("Change options"));
        const QJsonObject after = w.options();
        QCOMPARE(after.value(QStringLiteral("general"))
                     .toObject()
                     .value(QStringLiteral("autosaveMinutes"))
                     .toInt(),
                 5);
        QCOMPARE(
            after.value(QStringLiteral("maths")).toObject().value(QStringLiteral("precision")).toString(),
            QStringLiteral("double"));
        QCOMPARE(after.value(QStringLiteral("fastLapTolerancePct")).toDouble(), 2.5);
        QCOMPARE(after.value(QStringLiteral("lapTimeFormat")).toString(), QStringLiteral("h:mm:ss.f"));
        // Untouched members keep their values; the session sees the edit.
        QCOMPARE(after.value(QStringLiteral("timeFormat")), before.value(QStringLiteral("timeFormat")));
        QCOMPARE(after.value(QStringLiteral("reports")), before.value(QStringLiteral("reports")));
        QVERIFY(w.session->mathsCompat().unaryMinusAbovePower);
        QCOMPARE(w.session->autosaveInterval(), std::chrono::milliseconds(std::chrono::minutes(5)));
        QVERIFY(w.session->isDirty());

        // Undo restores the options, the fields follow, the view state stays.
        w.session->undoStack().undo();
        QCOMPARE(w.options(), before);
        QCOMPARE(field<QSpinBox>(d, "general.autosaveMinutes")->value(), 2);
        QVERIFY(!field<QCheckBox>(d, "maths.compat.unaryMinusAbovePower")->isChecked());
        QVERIFY(w.session->projectFile().lastSelection.singleColourMain);
        QVERIFY(!d.hasPendingEdits());
        w.session->undoStack().redo();
        QCOMPARE(w.options(), after);
        QCOMPARE(field<QDoubleSpinBox>(d, "fastLapTolerancePct")->value(), 2.5);

        // A second Apply with nothing changed pushes nothing.
        QVERIFY(d.apply());
        QCOMPARE(w.session->undoStack().count(), 1);
    }

    void userSettingsAreNotProjectOptions() {
        World w;
        QVERIFY(w.start());
        OptionsDialog d(*w.session, w.settings.get());
        field<QCheckBox>(d, SchemeSelector::kToolbarKey)->setChecked(true);
        QVERIFY(d.apply());
        QVERIFY(w.settings->value(QString::fromLatin1(SchemeSelector::kToolbarKey)).toBool());
        QCOMPARE(w.session->undoStack().count(), 0); // nothing in project.json
        // A setting changed elsewhere shows in the dialog (choosing a scheme turns "follow OS" off).
        w.settings->setValue(QString::fromLatin1(SchemeSelector::kFollowOsKey), true);
        QVERIFY(field<QCheckBox>(d, SchemeSelector::kFollowOsKey)->isChecked());
        QVERIFY(d.schemeSelector().selectScheme(
            QString::fromStdString(w.session->shippedScheme(true)->name))); // the Colours tab's selector
        QVERIFY(!field<QCheckBox>(d, SchemeSelector::kFollowOsKey)->isChecked());
        QCOMPARE(w.session->scheme(), w.session->shippedScheme(true));
        QVERIFY(!d.hasPendingEdits());
    }

    void invalidFieldRefusesApplyAndOk() {
        World w;
        QVERIFY(w.start());
        OptionsDialog d(*w.session, w.settings.get());
        d.show();
        QVERIFY(QTest::qWaitForWindowExposed(&d));
        auto* spin = field<QDoubleSpinBox>(d, "fastLapTolerancePct");
        auto* edit = spin->findChild<QLineEdit*>();
        QVERIFY(edit != nullptr);
        edit->setText(QString()); // typed away: not a number
        QVERIFY(!spin->hasAcceptableInput());
        field<QSpinBox>(d, "general.autosaveMinutes")->setValue(9); // a pending edit
        QVERIFY(!d.apply());
        QVERIFY(d.errorText().contains(QStringLiteral("Fast-lap tolerance")));
        QVERIFY(d.findChild<QLabel*>(QStringLiteral("optionsError"))->isVisible());
        QCOMPARE(w.session->undoStack().count(), 0);
        QTest::mouseClick(button(d, QDialogButtonBox::Ok), Qt::LeftButton);
        QVERIFY(d.isVisible()); // OK refused: the dialog stays with the reason
        QCOMPARE(d.result(), 0);
        edit->setText(QStringLiteral("3"));
        QVERIFY(spin->hasAcceptableInput());
        QTest::mouseClick(button(d, QDialogButtonBox::Ok), Qt::LeftButton);
        QVERIFY(!d.isVisible());
        QCOMPARE(d.result(), static_cast<int>(QDialog::Accepted));
        QCOMPARE(w.session->undoStack().count(), 1);
        QCOMPARE(w.options()
                     .value(QStringLiteral("general"))
                     .toObject()
                     .value(QStringLiteral("autosaveMinutes"))
                     .toInt(),
                 9);
    }

    void cancelDropsTheEdits() {
        World w;
        QVERIFY(w.start());
        OptionsDialog d(*w.session, w.settings.get());
        d.show();
        QVERIFY(QTest::qWaitForWindowExposed(&d));
        field<QCheckBox>(d, "general.selectFastestOnLoad")->setChecked(true);
        QTest::mouseClick(button(d, QDialogButtonBox::Cancel), Qt::LeftButton);
        QVERIFY(!d.isVisible());
        QCOMPARE(w.session->undoStack().count(), 0);
        d.show(); // shown again: the current options
        QVERIFY(QTest::qWaitForWindowExposed(&d));
        QVERIFY(!field<QCheckBox>(d, "general.selectFastestOnLoad")->isChecked());
        QVERIFY(!d.hasPendingEdits());
    }

    void fontsTab() {
        World w;
        QVERIFY(w.start());
        OptionsDialog d(*w.session, w.settings.get());
        auto* fonts = d.findChild<QTableWidget*>(QStringLiteral("fontsTable"));
        QVERIFY(fonts != nullptr);
        QCOMPARE(fonts->rowCount(), static_cast<int>(project::kSchemeFontRoles.size()));
        QCOMPARE(fonts->editTriggers(), QAbstractItemView::NoEditTriggers);
        const project::FontSpec caption = w.session->scheme()->font("caption");
        for (int r = 0; r < fonts->rowCount(); ++r) {
            if (cellText(*fonts, r, 0) == QStringLiteral("caption")) {
                QCOMPARE(cellText(*fonts, r, 1), QString::fromStdString(caption.family));
                QCOMPARE(cellText(*fonts, r, 3), QString::number(caption.weight));
            }
        }
        // A scheme whose caption font differs: the tab follows the switch.
        project::Scheme other = *w.session->shippedScheme(true);
        other.name = "Wide";
        other.fonts["caption"].family = "Test Family";
        w.session->setScheme(std::make_shared<const project::Scheme>(other));
        bool found = false;
        for (int r = 0; r < fonts->rowCount(); ++r) {
            found = found || (cellText(*fonts, r, 0) == QStringLiteral("caption") &&
                              cellText(*fonts, r, 1) == QStringLiteral("Test Family"));
        }
        QVERIFY(found);
    }

    void keysTab() {
        World w;
        QVERIFY(w.start());
        // The user's keymap rebinds F8 (06 section 14.3: inherits the default).
        {
            std::ofstream out(fsPath(w.config.path()) / "keymap.json", std::ios::binary);
            out << R"({"$schema": "urn:esses:keymap-1", "schemaVersion": 1, "name": "user", "inherits": "default",
                      "bindings": [{"command": "view.data_window", "keys": ["Shift+F8"], "context": "global"}],
                      "unbind": []})";
        }
        QVERIFY(w.session->loadDefaults().ok());
        OptionsDialog d(*w.session, w.settings.get());
        auto* keys = d.findChild<QTableWidget*>(QStringLiteral("keysTable"));
        QVERIFY(keys != nullptr);
        std::vector<std::string> log;
        const auto shipped = project::Keymap::load(qt::test::repoRoot() / "data/keymaps/default.json", log);
        QVERIFY(shipped.ok());
        QCOMPARE(keys->rowCount(), static_cast<int>(shipped->bindings.size()));
        bool rebound = false;
        for (int r = 0; r < keys->rowCount(); ++r) {
            if (cellText(*keys, r, 0) == QStringLiteral("view.data_window")) {
                QCOMPARE(cellText(*keys, r, 2), QStringLiteral("Shift+F8"));
                QCOMPARE(cellText(*keys, r, 3), QStringLiteral("F8"));
                QVERIFY(cellText(*keys, r, 4).startsWith(QStringLiteral("implemented")));
                rebound = true;
            }
        }
        QVERIFY(rebound);

        QStringList revealed;
        d.setRevealHandler([&revealed](const std::filesystem::path& folder) {
            revealed << QString::fromStdWString(folder.wstring());
        });
        d.revealKeymap();
        QCOMPARE(revealed.size(), 1);
        QVERIFY(std::filesystem::equivalent(fsPath(revealed.front()), fsPath(w.config.path())));

        // Export the effective keymap; import a user keymap (written to <config>/keymap.json); a keymap that
        // binds a key twice in one context is refused.
        QTemporaryDir out;
        const std::filesystem::path exported = fsPath(out.path()) / "keymap.json";
        QVERIFY(d.exportKeymap(exported).ok());
        QVERIFY(project::Keymap::load(exported, log).ok());
        const std::filesystem::path mine = fsPath(out.path()) / "mine.json";
        {
            std::ofstream(mine, std::ios::binary)
                << R"({"$schema": "urn:esses:keymap-1", "schemaVersion": 1, "name": "mine", "inherits": "default",
                       "bindings": [{"command": "view.values_window", "keys": ["Shift+V"], "context": "worksheet"}],
                       "unbind": []})";
        }
        QVERIFY(d.importKeymap(mine).ok());
        const auto written = project::Keymap::load(fsPath(w.config.path()) / "keymap.json", log);
        QVERIFY(written.ok());
        QCOMPARE(written->name, std::string("mine"));
        const std::filesystem::path clash = fsPath(out.path()) / "clash.json";
        {
            std::ofstream(clash, std::ios::binary)
                << R"({"$schema": "urn:esses:keymap-1", "schemaVersion": 1, "name": "clash", "inherits": "default",
                       "bindings": [{"command": "view.values_window", "keys": ["F9"], "context": "global"}],
                       "unbind": []})";
        }
        QVERIFY(!d.importKeymap(clash).ok());
        QCOMPARE(project::Keymap::load(fsPath(w.config.path()) / "keymap.json", log)->name,
                 std::string("mine"));
    }

    void duplicateRevealsTheUserFolder() {
        World w;
        QVERIFY(w.start());
        OptionsDialog d(*w.session, w.settings.get());
        QStringList revealed;
        d.setRevealHandler([&revealed](const std::filesystem::path& folder) {
            revealed << QString::fromStdWString(folder.wstring());
        });
        d.findChild<QPushButton*>(QStringLiteral("schemeDuplicate"))->click();
        QCOMPARE(revealed.size(), 1);
        QVERIFY(std::filesystem::equivalent(fsPath(revealed.front()), fsPath(w.config.path()) / "schemes"));
        QVERIFY(
            d.findChild<QLabel*>(QStringLiteral("coloursNote"))->text().startsWith(QStringLiteral("Copied")));
        QVERIFY(d.schemeSelector().currentText().endsWith(QStringLiteral(" copy")));
    }
};

QTEST_MAIN(TstOptionsDialog)
#include "tst_options_dialog.moc"
