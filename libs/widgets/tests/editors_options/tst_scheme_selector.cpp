// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SchemeSelector (06 sections 2.1, 13.7 and 16; 07 section 5.1), offscreen over a ProjectSession with the
// shipped defaults and a private configuration directory: the shipped schemes then the user's by name; a user
// scheme that does not load or repeats a listed name is left out with a log line, one that lacks roles is
// filled from the shipped scheme of its background with one line per filled item (rule 8); a choice switches
// the session's scheme live (schemeChanged once, scheme.active in the settings, never on the undo stack),
// turns "follow OS" off and shows in every selector; restoreActive brings back a user scheme; following the
// OS does nothing while the OS reports no colour scheme (and picks the shipped dark scheme when it reports
// dark, Qt 6.8+); Duplicate, Import and Export; the application palette of a scheme.

#include "../../../qtmodel/tests/qtmodel_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/project/rgba.hpp>
#include <esses/project/scheme.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/settings_store.hpp>
#include <esses/widgets/editors_options/scheme_selector.hpp>

#include <QApplication>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>
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
    std::filesystem::path schemes() const { return fsPath(config.path()) / "schemes"; }
    // A user scheme: a copy of the shipped light (or dark) one under another name, optionally without roles.
    void writeScheme(const std::string& file, const std::string& name, bool dark = false,
                     bool partial = false) {
        project::Scheme s = *session->shippedScheme(dark);
        s.name = name;
        if (partial) {
            s.roles.erase("cursorMain");
            s.roles.erase("overlay0");
        }
        std::filesystem::create_directories(schemes());
        QVERIFY(s.save(schemes() / file).ok());
    }
    void writeText(const std::string& file, const std::string& text) {
        std::filesystem::create_directories(schemes());
        std::ofstream(schemes() / file, std::ios::binary) << text;
    }
    QString light() const { return QString::fromStdString(session->shippedScheme(false)->name); }
    QString dark() const { return QString::fromStdString(session->shippedScheme(true)->name); }
};

QColor colourOf(const project::Scheme& s, const char* role) {
    const project::Rgba c = s.roles.at(role);
    return QColor(project::red(c), project::green(c), project::blue(c), project::alpha(c));
}

} // namespace

class TstSchemeSelector : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void listsShippedThenUserSchemes() {
        World w;
        QVERIFY(w.start());
        w.writeScheme("b.scheme.json", "Zebra");
        w.writeScheme("a.scheme.json", "Aqua", true, true); // two roles missing
        w.writeScheme("c.scheme.json", "Esses Light");      // the shipped name again
        w.writeText("d.scheme.json", "{ not json");
        w.writeText("notes.txt", "ignored: not a scheme file");
        SchemeSelector selector(*w.session, w.settings.get());
        QSignalSpy log(&selector, &SchemeSelector::logMessage);
        selector.refresh();
        QCOMPARE(selector.schemeNames(),
                 (QStringList{w.light(), w.dark(), QStringLiteral("Aqua"), QStringLiteral("Zebra")}));
        QCOMPARE(selector.currentText(), w.light()); // the active scheme is shown
        // Rule 8: the partial scheme is filled from the shipped dark scheme, one line per role; the bad
        // file and the repeated name are left out with a line each.
        int filled = 0;
        int skipped = 0;
        for (const QList<QVariant>& args : log) {
            const QString text = args.front().toString();
            filled += text.startsWith(QStringLiteral("a.scheme.json")) ? 1 : 0;
            skipped += text.contains(QStringLiteral("skipped")) ? 1 : 0;
        }
        QCOMPARE(filled, 2);
        QCOMPARE(skipped, 2);
        const auto aqua = selector.scheme(QStringLiteral("Aqua"));
        QVERIFY(aqua != nullptr);
        QCOMPARE(aqua->roles.at("cursorMain"), w.session->shippedScheme(true)->roles.at("cursorMain"));
        QVERIFY(selector.scheme(QStringLiteral("Nope")) == nullptr);
        QCOMPARE(selector.objectName(), QStringLiteral("schemeSelector"));
    }

    void choosingSwitchesLiveAndTurnsFollowOff() {
        World w;
        QVERIFY(w.start());
        w.writeScheme("user.scheme.json", "Mine", true);
        SchemeSelector a(*w.session, w.settings.get());
        SchemeSelector b(*w.session, w.settings.get());
        b.refresh();
        w.settings->setValue(QString::fromLatin1(SchemeSelector::kFollowOsKey), true);
        QSignalSpy changed(w.session.get(), &qt::ProjectSession::schemeChanged);
        QVERIFY(a.selectScheme(w.dark()));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(w.session->scheme(), w.session->shippedScheme(true));
        QCOMPARE(w.settings->value(QString::fromLatin1(SchemeSelector::kActiveKey)).toString(), w.dark());
        QVERIFY(!a.followsSystem());         // an explicit choice wins over the OS
        QCOMPARE(b.currentText(), w.dark()); // every selector shows the active scheme
        // A user scheme through the combo, as the user picks it.
        b.setCurrentIndex(b.findText(QStringLiteral("Mine")));
        Q_EMIT b.activated(b.currentIndex());
        QCOMPARE(QString::fromStdString(w.session->scheme()->name), QStringLiteral("Mine"));
        QCOMPARE(a.currentText(), QStringLiteral("Mine"));
        QVERIFY(!a.selectScheme(QStringLiteral("Unknown")));
        QCOMPARE(a.currentText(), QStringLiteral("Mine"));
        QCOMPARE(w.session->undoStack().count(), 0); // view state (D-032)
        QCOMPARE(changed.count(), 2);
    }

    void restoreActiveBringsBackAUserScheme() {
        World w;
        QVERIFY(w.start());
        w.writeScheme("user.scheme.json", "Mine", true);
        w.settings->setValue(QString::fromLatin1(SchemeSelector::kActiveKey), QStringLiteral("Mine"));
        QCOMPARE(QString::fromStdString(w.session->scheme()->name),
                 w.light()); // the session knows the shipped
        SchemeSelector selector(*w.session, w.settings.get());
        selector.restoreActive();
        QCOMPARE(QString::fromStdString(w.session->scheme()->name), QStringLiteral("Mine"));
        QCOMPARE(selector.currentText(), QStringLiteral("Mine"));
    }

    void followsTheOsColourScheme() {
        World w;
        QVERIFY(w.start());
        SchemeSelector selector(*w.session, w.settings.get());
        QVERIFY(!selector.followsSystem()); // off by default
        QVERIFY(!selector.applySystemScheme());
        const Qt::ColorScheme os = QGuiApplication::styleHints()->colorScheme();
        selector.setFollowSystem(true);
        QVERIFY(selector.followsSystem());
        QVERIFY(w.settings->value(QString::fromLatin1(SchemeSelector::kFollowOsKey)).toBool());
        if (os == Qt::ColorScheme::Unknown) {
            QCOMPARE(w.session->scheme(), w.session->shippedScheme(false)); // nothing to follow
        } else {
            QCOMPARE(w.session->scheme(), w.session->shippedScheme(os == Qt::ColorScheme::Dark));
        }
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
        selector.applySystemScheme();
        QCOMPARE(w.session->scheme(), w.session->shippedScheme(true));
        QGuiApplication::styleHints()->unsetColorScheme();
#endif
    }

    void duplicateImportExport() {
        World w;
        QVERIFY(w.start());
        SchemeSelector selector(*w.session, w.settings.get());
        const Result<std::filesystem::path> copy = selector.duplicateActive();
        QVERIFY2(copy.ok(), copy.ok() ? "" : copy.error().toString().c_str());
        QCOMPARE(copy->parent_path(), w.schemes());
        const QString copyName = w.light() + QStringLiteral(" copy");
        QCOMPARE(QString::fromStdString(w.session->scheme()->name), copyName); // listed and selected
        QVERIFY(selector.schemeNames().contains(copyName));
        std::vector<std::string> log;
        const Result<project::Scheme> onDisk = project::Scheme::load(*copy, log);
        QVERIFY(onDisk.ok());
        QCOMPARE(onDisk->roles, w.session->shippedScheme(false)->roles);
        // A second duplicate of the copy gets its own name and file.
        const Result<std::filesystem::path> copy2 = selector.duplicateActive();
        QVERIFY(copy2.ok());
        QVERIFY(*copy2 != *copy);
        QVERIFY(selector.schemeNames().contains(copyName + QStringLiteral(" copy")));

        // Export the active scheme; importing it again is refused (the name is listed), under another name
        // it is copied into the user folder.
        QTemporaryDir out;
        const std::filesystem::path exported = fsPath(out.path()) / "exported.scheme.json";
        QVERIFY(selector.exportActive(exported).ok());
        QVERIFY(!selector.importScheme(exported).ok());
        project::Scheme other = *w.session->shippedScheme(true);
        other.name = "Imported One";
        const std::filesystem::path otherFile = fsPath(out.path()) / "other.scheme.json";
        QVERIFY(other.save(otherFile).ok());
        const Result<QString> imported = selector.importScheme(otherFile);
        QVERIFY(imported.ok());
        QCOMPARE(*imported, QStringLiteral("Imported One"));
        QVERIFY(selector.schemeNames().contains(QStringLiteral("Imported One")));
        QVERIFY(std::filesystem::exists(w.schemes() / "Imported One.scheme.json"));
        // Not a scheme: refused with the loader's error.
        {
            std::ofstream(fsPath(out.path()) / "bad.json", std::ios::binary) << "[1, 2]";
        }
        QVERIFY(!selector.importScheme(fsPath(out.path()) / "bad.json").ok());
        QCOMPARE(w.session->undoStack().count(), 0);
    }

    void paletteFollowsTheScheme() {
        World w;
        QVERIFY(w.start());
        for (const bool dark : {false, true}) {
            const project::Scheme& s = *w.session->shippedScheme(dark);
            const QPalette p = SchemeSelector::palette(s);
            QCOMPARE(p.color(QPalette::Active, QPalette::Window), colourOf(s, "windowBackground"));
            QCOMPARE(p.color(QPalette::Active, QPalette::Base), colourOf(s, "dataBackground"));
            QCOMPARE(p.color(QPalette::Active, QPalette::Text), colourOf(s, "dataForeground"));
            QCOMPARE(p.color(QPalette::Inactive, QPalette::WindowText), colourOf(s, "dataForeground"));
            QCOMPARE(p.color(QPalette::Active, QPalette::Highlight), colourOf(s, "dataHighlightBackground"));
            QCOMPARE(p.color(QPalette::Active, QPalette::HighlightedText),
                     colourOf(s, "dataHighlightForeground"));
            QCOMPARE(p.color(QPalette::Disabled, QPalette::Text), colourOf(s, "dataForeground2"));
        }
    }
};

QTEST_MAIN(TstSchemeSelector)
#include "tst_scheme_selector.moc"
