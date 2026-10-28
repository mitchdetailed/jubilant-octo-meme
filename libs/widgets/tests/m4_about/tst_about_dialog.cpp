// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The About dialog and the build identification as built (M4 packaging, D-212; AC-M4-06, AC-M4-09; 08 section
// 9 items 2-3). Expectations come from the design and the build, never from the code under test: the version
// fields from esses::versionInfo() and Qt's own qVersion()/QT_VERSION_STR, the docking library's version from
// the vendored ADS project file (third_party/qtads/CMakeLists.txt), the URLs written out by hand from 08
// section 9 item 3 and D-212, the portable layout from 01 section 9 (portable.marker beside the executable).

#include "properties_shell/shell_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/base/utf8.hpp>
#include <esses/base/version.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/widgets/shell/about_dialog.hpp>
#include <esses/widgets/shell/about_shell.hpp>
#include <esses/widgets/shell/shell_extension.hpp>

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <fstream>
#include <memory>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::shelltest;

namespace {

const QString kRepo = QStringLiteral("https://github.com/mitchdetailed/jubilant-octo-meme");

QString fromView(std::string_view v) {
    return QString::fromUtf8(v.data(), static_cast<qsizetype>(v.size()));
}

// The version in the vendored ADS project (project(QtADS VERSION x.y.z)).
QString adsVersionFromSource() {
    QFile f(QStringLiteral(ESSES_TEST_REPO_ROOT) + QStringLiteral("/third_party/qtads/CMakeLists.txt"));
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QString text = QString::fromUtf8(f.readAll());
    const QRegularExpression re(QStringLiteral(R"(project\(\s*QtADS\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+))"));
    const QRegularExpressionMatch m = re.match(text);
    return m.hasMatch() ? m.captured(1) : QString();
}

// The backend in use as DockHost picks it (06 section 3; D-153): ADS when the build has it and the
// environment does not name qdockwidget.
QString expectedDockLibrary() {
#if ESSES_HAVE_ADS
    if (qEnvironmentVariable("ESSES_DOCK_BACKEND").trimmed().toLower() != QLatin1String("qdockwidget")) {
        return QStringLiteral("ADS ") + adsVersionFromSource();
    }
#endif
    return QStringLiteral("QDockWidget");
}

class EnvGuard {
public:
    explicit EnvGuard(const char* name) : name_(name), had_(qEnvironmentVariableIsSet(name)) {
        if (had_) {
            value_ = qgetenv(name);
        }
    }
    ~EnvGuard() {
        if (had_) {
            qputenv(name_, value_);
        } else {
            qunsetenv(name_);
        }
    }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

private:
    const char* name_;
    bool had_;
    QByteArray value_;
};

Paths portablePaths(const QString& exeDir) {
    PathOverrides o;
    o.exeDir = fsPath(exeDir);
    o.dataDir = cmptest::repoRoot() / "data";
    auto p = Paths::resolve(o);
    if (!p) {
        qFatal("Paths::resolve: %s", p.error().toString().c_str());
    }
    return *p;
}

} // namespace

class TestAboutDialog : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // AC-M4-09: version, commit, Qt version and build date, one field per line after "esses <version>".
    void versionReportHasTheBuildFields() {
        const VersionInfo& v = versionInfo();
        const QStringList lines = versionReport().split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 6);
        QCOMPARE(lines[0], QStringLiteral("esses ") + fromView(v.string));
        QCOMPARE(lines[1], QStringLiteral("commit: ") + fromView(v.gitSha));
        QCOMPARE(lines[2], QStringLiteral("Qt: %1 (built with %2)")
                               .arg(QString::fromLatin1(qVersion()), QStringLiteral(QT_VERSION_STR)));
        QCOMPARE(lines[3], QStringLiteral("build date: ") + fromView(v.buildDate));
        QVERIFY(QRegularExpression(QStringLiteral("^build date: [0-9]{4}-[0-9]{2}-[0-9]{2}$"))
                    .match(lines[3])
                    .hasMatch());
        QCOMPARE(lines[4], QStringLiteral("dock library: ") + expectedDockLibrary());
        QVERIFY(lines[5].startsWith(QStringLiteral("source: ") + kRepo));
        QCOMPARE(lines[5], QStringLiteral("source: ") + sourceUrl());
    }

    // A development build names its commit (D-212); a release build its tag, which equals v<version>.
    void sourceUrlOfThisBuild() {
        const BuildInfo b = buildInfo();
        if (!b.releaseTag.isEmpty()) {
            QCOMPARE(b.releaseTag, QStringLiteral("v") + b.version);
            QCOMPARE(sourceUrl(), kRepo + QStringLiteral("/tree/") + b.releaseTag);
            return;
        }
        QString sha = b.commit;
        if (sha.endsWith(QStringLiteral("-dirty"))) {
            sha.chop(6);
        }
        if (QRegularExpression(QStringLiteral("^[0-9a-f]{7,40}$")).match(sha).hasMatch()) {
            QCOMPARE(sourceUrl(), kRepo + QStringLiteral("/commit/") + sha);
        } else {
            QCOMPARE(sourceUrl(), kRepo);
        }
    }

    void sourceUrlRule_data() {
        QTest::addColumn<QString>("tag");
        QTest::addColumn<QString>("commit");
        QTest::addColumn<QString>("url");
        const QString tree = kRepo + QStringLiteral("/tree/");
        const QString commit = kRepo + QStringLiteral("/commit/");
        QTest::newRow("release, commit unknown") << "v0.1.0" << "unknown" << tree + "v0.1.0";
        QTest::newRow("release with its commit") << "v0.1.0" << "abc1234" << tree + "v0.1.0";
        QTest::newRow("pre-release tag") << "v0.1.0-rc1" << "" << tree + "v0.1.0-rc1";
        QTest::newRow("development") << "" << "56d8065" << commit + "56d8065";
        QTest::newRow("full sha") << "" << "0123456789abcdef0123456789abcdef01234567"
                                  << commit + "0123456789abcdef0123456789abcdef01234567";
        QTest::newRow("modified tree") << "" << "abc1234-dirty" << commit + "abc1234";
        QTest::newRow("upper case") << "" << "ABC1234" << commit + "abc1234";
        QTest::newRow("unknown") << "" << "unknown" << kRepo;
        QTest::newRow("empty") << "" << "" << kRepo;
        QTest::newRow("too short") << "" << "abc12" << kRepo;
        QTest::newRow("not hexadecimal") << "" << "xyz1234" << kRepo;
    }
    void sourceUrlRule() {
        QFETCH(QString, tag);
        QFETCH(QString, commit);
        QFETCH(QString, url);
        QCOMPARE(sourceUrlFor(tag, commit), url);
    }

    // 08 section 9 item 3: the archive of exactly the Qt version shipped.
    void qtSourceArchive() {
        QCOMPARE(qtSourceArchiveUrl(QStringLiteral("6.12.0")),
                 QStringLiteral("https://download.qt.io/official_releases/qt/6.12/6.12.0/single/"
                                "qt-everywhere-src-6.12.0.tar.xz"));
        QCOMPARE(qtSourceArchiveUrl(QStringLiteral("6.7.2")),
                 QStringLiteral("https://download.qt.io/official_releases/qt/6.7/6.7.2/single/"
                                "qt-everywhere-src-6.7.2.tar.xz"));
    }

    // 08 section 9 item 2: the statement, the licence texts, replacement, the source of the Qt in use.
    void qtNoticeStatesTheLgplAndTheSource() {
        const QString qt = QString::fromLatin1(qVersion());
        const QString n = qtLicenceNotice();
        QVERIFY2(n.startsWith(QStringLiteral("This application uses Qt ") + qt +
                              QStringLiteral(" under the GNU LGPL v3.")),
                 qPrintable(n));
        QVERIFY(n.contains(QStringLiteral("licenses/LGPL-3.0-only.txt")));
        QVERIFY(n.contains(QStringLiteral("licenses/GPL-3.0-only.txt")));
        // The replacement is a build of the Qt this was built against (docs/user/relinking.md, D-220).
        QVERIFY(n.contains(QStringLiteral("replace its libraries with your own build of Qt ") +
                           QStringLiteral(QT_VERSION_STR) + QLatin1Char(' ')));
        QVERIFY(n.endsWith(QStringLiteral("https://download.qt.io/official_releases/qt/") +
                           qt.section('.', 0, 1) + QLatin1Char('/') + qt +
                           QStringLiteral("/single/qt-everywhere-src-") + qt + QStringLiteral(".tar.xz")));
    }

    void dockNoticeNamesTheLibraryAndItsTag() {
        const QString n = dockLibraryNotice();
#if ESSES_HAVE_ADS
        const QString ads = adsVersionFromSource();
        QVERIFY(!ads.isEmpty());
        QVERIFY(n.contains(QStringLiteral("Qt Advanced Docking System ") + ads));
        QVERIFY(n.contains(QStringLiteral("GNU LGPL v2.1 or later")));
        QVERIFY(n.contains(QStringLiteral("licenses/LGPL-2.1-or-later.txt")));
        // The vendored copy has Esses changes (LGPL-2.1 section 2): the notice names their list.
        QVERIFY(n.contains(QStringLiteral("third_party/qtads/UPSTREAM.md")));
        QVERIFY(!n.contains(QStringLiteral("unmodified")));
        QVERIFY(n.endsWith(
            QStringLiteral("https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System/tree/v") + ads));
#else
        QVERIFY(n.isEmpty());
#endif
    }

    // The docking library of --version and About is the backend in use (ESSES_DOCK_BACKEND, D-153).
    void dockLibraryFollowsTheBackendInUse() {
        EnvGuard guard("ESSES_DOCK_BACKEND");
        qputenv("ESSES_DOCK_BACKEND", "qdockwidget");
        QCOMPARE(buildInfo().dockLibrary, QStringLiteral("QDockWidget"));
        qputenv("ESSES_DOCK_BACKEND", "ads");
#if ESSES_HAVE_ADS
        QCOMPARE(buildInfo().dockLibrary, QStringLiteral("ADS ") + adsVersionFromSource());
#else
        QCOMPARE(buildInfo().dockLibrary, QStringLiteral("QDockWidget"));
#endif
    }

    // AC-M4-06: the dialog shows the version report, the Qt notice with its source link, the source of this
    // build and the notices; links are links.
    void dialogShowsTheNoticesAndTheLinks() {
        QTemporaryDir exe;
        QVERIFY(exe.isValid());
        const Paths paths = portablePaths(exe.path());
        AboutDialog dialog(paths, nullptr);
        const QString text = dialog.text();
        QCOMPARE(text.section(QLatin1Char('\n'), 0, 0), versionReport().section(QLatin1Char('\n'), 0, 0));
        QVERIFY(text.contains(versionReport()));
        QVERIFY(text.contains(qtLicenceNotice()));
        QVERIFY(text.contains(QStringLiteral("Source of this build: ") + sourceUrl()));
        QVERIFY(text.contains(QStringLiteral("Notices and licences: ") + dialog.noticesLink()));
        if (!dockLibraryNotice().isEmpty()) {
            QVERIFY(text.contains(dockLibraryNotice()));
        }
        const QString html = dialog.html();
        const QString qtArchive = qtSourceArchiveUrl(QString::fromLatin1(qVersion()));
        QVERIFY2(html.contains(QStringLiteral("<a href=\"%1\">").arg(qtArchive)), qPrintable(html));
        QVERIFY(html.contains(QStringLiteral("<a href=\"%1\">").arg(sourceUrl().toHtmlEscaped())));
        QVERIFY(html.contains(QStringLiteral("<a href=\"%1\">").arg(dialog.noticesLink().toHtmlEscaped())));
        auto* label = dialog.findChild<QLabel*>(QStringLiteral("aboutText"));
        QVERIFY(label != nullptr);
        QVERIFY(label->openExternalLinks());
    }

    // NOTICES.md installed beside the data folder is linked as a file (the installation's own copy, 08
    // section 7.4); without it, the repository's copy at the build's commit or tag.
    void noticesLinkPrefersTheInstalledFile() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString base = root.path() + QStringLiteral("/an install");
        QVERIFY(QDir().mkpath(base + QStringLiteral("/data")));
        PathOverrides o;
        o.configDir = fsPath(root.path() + QStringLiteral("/config"));
        o.dataDir = fsPath(base + QStringLiteral("/data"));
        auto p = Paths::resolve(o);
        QVERIFY(static_cast<bool>(p));
        const QString fallback = noticesUrl(*p);
        QVERIFY2(fallback.startsWith(kRepo + QStringLiteral("/blob/")) &&
                     fallback.endsWith(QStringLiteral("/NOTICES.md")),
                 qPrintable(fallback));
        {
            QFile f(base + QStringLiteral("/NOTICES.md"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("# Notices\n");
        }
        const QString local = noticesUrl(*p);
        QCOMPARE(local,
                 QUrl::fromLocalFile(base + QStringLiteral("/NOTICES.md")).toString(QUrl::FullyEncoded));
        QVERIFY(!local.contains(QLatin1Char(' '))); // a link survives the space in the folder name
        QCOMPARE(QUrl(local).toLocalFile(), base + QStringLiteral("/NOTICES.md"));
        AboutDialog dialog(*p, nullptr);
        QCOMPARE(dialog.noticesLink(), local);
    }

    // Portable mode (01 section 9, D-055; 07 section 7.5): portable.marker beside the executable puts the
    // settings in <exe dir>/config, and About says so.
    void portableRunNamesItsSettingsFolder() {
        EnvGuard config("ESSES_CONFIG_DIR");
        qunsetenv("ESSES_CONFIG_DIR");
        QTemporaryDir exe;
        QVERIFY(exe.isValid());
        {
            const Paths installed = portablePaths(exe.path());
            QVERIFY(!installed.portable());
            AboutDialog dialog(installed, nullptr);
            QVERIFY(dialog.text().contains(QStringLiteral("Settings: ")));
            QVERIFY(!dialog.text().contains(QStringLiteral("(portable")));
        }
        std::ofstream(fsPath(exe.path()) / "portable.marker").put('\n');
        const Paths portable = portablePaths(exe.path());
        QVERIFY(portable.portable());
        QCOMPARE(portable.configDir(), fsPath(exe.path()) / "config");
        AboutDialog dialog(portable, nullptr);
        const QString expected = QStringLiteral("Settings: ") +
                                 QString::fromStdString(pathToUtf8(fsPath(exe.path()) / "config")) +
                                 QStringLiteral(" (portable");
        QVERIFY2(dialog.text().contains(expected), qPrintable(dialog.text()));
    }

    void copyPutsTheTextOnTheClipboard() {
        AboutDialog dialog;
        QVERIFY(dialog.copyButton() != nullptr);
        QClipboard* clipboard = QApplication::clipboard();
        QVERIFY(clipboard != nullptr);
        clipboard->clear();
        QTest::mouseClick(dialog.copyButton(), Qt::LeftButton);
        QCOMPARE(clipboard->text(), dialog.text());
        QVERIFY(dialog.text().startsWith(QStringLiteral("esses ")));
    }

    // Help > About in the shell passes the window's paths: the dialog names that window's settings folder.
    void helpAboutUsesTheWindowsPaths() {
        QTemporaryDir config;
        QVERIFY(config.isValid());
        std::unique_ptr<qt::AppRuntime> rt = runtime(2);
        {
            MainWindow w(*rt, pathsFor(config.path()));
            w.setSaveLayoutOnExit(false);
            QVERIFY(showAndActivate(w));
            AboutShell* about = shellExtension<AboutShell>(w);
            QVERIFY(about != nullptr);
            QVERIFY(w.commands().invoke(QString::fromLatin1(AboutShell::kCommand)));
            QTRY_VERIFY(about->dialog() != nullptr && about->dialog()->isVisible());
            QCOMPARE(about->dialog()->windowModality(), Qt::WindowModal);
            const QString settings =
                QStringLiteral("Settings: ") + QString::fromStdString(pathToUtf8(fsPath(config.path())));
            QVERIFY2(about->dialog()->text().contains(settings), qPrintable(about->dialog()->text()));
            about->dialog()->reject();
            QTRY_VERIFY(about->dialog() == nullptr);
        }
        rt->shutdown();
    }
};

QTEST_MAIN(TestAboutDialog)
#include "tst_about_dialog.moc"
