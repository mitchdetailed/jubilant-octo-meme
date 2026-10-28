// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The M4 docs package in esses_widgets (07 section 7.1 "Docs"; 06 section 2.2 Help > Help Contents, section
// 14.4 help.contents F1; D-215): DocsShell's lookup of the installed start page (beside the data directory,
// then beside the executable), the online start page named by the build's tag, and Help Contents in a
// MainWindow (registered in the Help menu with F1 and its keymap row implemented; the installed page, else
// the online one; the online page when the installed one cannot be opened; a Messages warning when nothing
// opens; the default opener going through QDesktopServices). No test opens a browser: the shell's opener is
// replaced, and the default opener's test claims the file and https schemes with QDesktopServices URL
// handlers. The F1 key itself, from a component, the Data window and the palette, is ui.tst_keymap's row.

#include "properties_shell/shell_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/base/utf8.hpp>
#include <esses/base/version.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/widgets/docks/messages_window.hpp>
#include <esses/widgets/shell/command_actions.hpp>
#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/docs_shell.hpp>

#include <QAction>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <system_error>

using namespace esses;
using namespace esses::widgets;
using namespace esses::widgets::shelltest;

namespace {

namespace fs = std::filesystem;

const QString kOnlineBase = QStringLiteral("https://github.com/mitchdetailed/jubilant-octo-meme/blob/");

void touch(const fs::path& file) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << "# Esses\n";
}

// Paths whose data directory and executable directory are the given folders (config in `config`).
Paths pathsWith(const fs::path& config, const fs::path& data, const fs::path& exe) {
    PathOverrides o;
    o.configDir = config;
    o.cacheDir = config / "cache";
    o.logDir = config / "logs";
    o.dataDir = data;
    o.exeDir = exe;
    auto p = Paths::resolve(o);
    if (!p) {
        qFatal("Paths::resolve: %s", p.error().toString().c_str());
    }
    return *p;
}

QString canonicalOf(const fs::path& p) {
    return QFileInfo(QString::fromStdWString(p.wstring())).canonicalFilePath();
}

QString canonicalOfUrl(const QString& url) {
    return QFileInfo(QUrl(url, QUrl::StrictMode).toLocalFile()).canonicalFilePath();
}

QString thisBuildsVersion() {
    const std::string_view v = versionInfo().string;
    return QString::fromUtf8(v.data(), static_cast<qsizetype>(v.size()));
}

struct Shell {
    QTemporaryDir config;
    std::unique_ptr<qt::AppRuntime> rt = runtime(2);
    std::unique_ptr<MainWindow> w;
    QStringList opened;     // what the recording opener was handed
    bool openFile = true;   // the recording opener's answer for file: URLs
    bool openOnline = true; // and for the others
    bool mdHandled = true;  // whether a program opens .md files (the handler query)

    // `paths` empty: the repository's data directory (beside docs/user/, as installed).
    bool start(std::optional<Paths> paths = std::nullopt) {
        if (!config.isValid()) {
            return false;
        }
        w = std::make_unique<MainWindow>(*rt, paths ? *paths : pathsFor(config.path()));
        w->setSaveLayoutOnExit(false);
        if (!showAndActivate(*w)) {
            return false;
        }
        docs()->setUrlOpener([this](const QString& url) {
            opened << url;
            return QUrl(url, QUrl::StrictMode).isLocalFile() ? openFile : openOnline;
        });
        docs()->setHandlerQuery(
            [this](const QString& suffix) { return suffix == QStringLiteral(".md") && mdHandled; });
        return true;
    }
    DocsShell* docs() const { return shellExtension<DocsShell>(*w); }
    ~Shell() {
        w.reset();
        if (rt != nullptr) {
            rt->shutdown();
        }
    }
};

} // namespace

// Claims the file and https schemes of QDesktopServices::openUrl for the default opener's test.
class UrlRecorder : public QObject {
    Q_OBJECT
public:
    QStringList urls;
public Q_SLOTS:
    void handle(const QUrl& url) { urls << url.toString(QUrl::FullyEncoded); }
};

class TstDocsShell : public QObject {
    Q_OBJECT
public:
    static void initMain() { cmptest::prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() {
        cmptest::registerGoldenFonts();
        registerBuiltinComponents();
    }

    // The installed start page: <data dir>/../docs/user/index.md, else <exe dir>/docs/user/index.md.
    void startPageBesideTheDataDirectoryOrTheExecutable() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const fs::path root = fsPath(tmp.path());
        const fs::path config = root / "config";

        // An installation: data/ and docs/user/ side by side, the executable elsewhere.
        const fs::path install = root / "install";
        fs::create_directories(install / "data");
        fs::create_directories(root / "bin");
        touch(install / "docs" / "user" / "index.md");
        std::optional<fs::path> page =
            DocsShell::installedStartPage(pathsWith(config, install / "data", root / "bin"));
        QVERIFY(page.has_value());
        QCOMPARE(canonicalOf(*page), canonicalOf(install / "docs" / "user" / "index.md"));
        // The data directory given with a trailing separator names the same installation.
        fs::path withSeparator = install / "data";
        withSeparator += fs::path::preferred_separator;
        page = DocsShell::installedStartPage(pathsWith(config, withSeparator, root / "bin"));
        QVERIFY(page.has_value());
        QCOMPARE(canonicalOf(*page), canonicalOf(install / "docs" / "user" / "index.md"));

        // The data directory elsewhere (an override): the pages beside the executable.
        const fs::path app = root / "app";
        fs::create_directories(root / "elsewhere" / "data");
        touch(app / "docs" / "user" / "index.md");
        page = DocsShell::installedStartPage(pathsWith(config, root / "elsewhere" / "data", app));
        QVERIFY(page.has_value());
        QCOMPARE(canonicalOf(*page), canonicalOf(app / "docs" / "user" / "index.md"));

        // Both: the one beside the data directory.
        page = DocsShell::installedStartPage(pathsWith(config, install / "data", app));
        QVERIFY(page.has_value());
        QCOMPARE(canonicalOf(*page), canonicalOf(install / "docs" / "user" / "index.md"));

        // Neither (a build tree): nothing; a directory named index.md is not a page.
        fs::create_directories(root / "build" / "bin" / "data");
        QVERIFY(!DocsShell::installedStartPage(
                     pathsWith(config, root / "build" / "bin" / "data", root / "build" / "bin"))
                     .has_value());
        fs::create_directories(root / "odd" / "docs" / "user" / "index.md");
        fs::create_directories(root / "odd" / "data");
        QVERIFY(!DocsShell::installedStartPage(pathsWith(config, root / "odd" / "data", root / "odd"))
                     .has_value());
    }

    // The online start page: the repository at the build's tag, else at main (D-215 (b)).
    void onlineUrlNamesTheBuildsTag_data() {
        QTest::addColumn<QString>("version");
        QTest::addColumn<QString>("url");
        const QString page = QStringLiteral("/docs/user/index.md");
        QTest::newRow("release") << QStringLiteral("0.1.0") << kOnlineBase + QStringLiteral("v0.1.0") + page;
        QTest::newRow("milestone tag")
            << QStringLiteral("0.1.0-m4") << kOnlineBase + QStringLiteral("v0.1.0-m4") + page;
        QTest::newRow("after a tag") << QStringLiteral("0.1.0+3-gabc1234")
                                     << kOnlineBase + QStringLiteral("main") + page;
        // D-217: a pre-release tag with hyphens, and a development build without a version tag.
        QTest::newRow("hyphenated pre-release tag")
            << QStringLiteral("0.1.0-beta-1") << kOnlineBase + QStringLiteral("v0.1.0-beta-1") + page;
        QTest::newRow("no version tag")
            << QStringLiteral("0.1.0+g56d8065") << kOnlineBase + QStringLiteral("main") + page;
        QTest::newRow("after a milestone, dirty")
            << QStringLiteral("0.1.0-m3+12-g56d8065.dirty") << kOnlineBase + QStringLiteral("main") + page;
        QTest::newRow("dirty at a tag")
            << QStringLiteral("0.1.0.dirty") << kOnlineBase + QStringLiteral("main") + page;
        QTest::newRow("dirty at a tag, as a pre-release")
            << QStringLiteral("0.1.0-dirty") << kOnlineBase + QStringLiteral("main") + page;
        QTest::newRow("empty") << QString() << kOnlineBase + QStringLiteral("main") + page;
        QTest::newRow("not a version")
            << QStringLiteral("unknown") << kOnlineBase + QStringLiteral("main") + page;
    }
    void onlineUrlNamesTheBuildsTag() {
        QFETCH(QString, version);
        QFETCH(QString, url);
        QCOMPARE(DocsShell::onlineUrl(version), url);
    }

    // help.contents: Help > Help Contents with F1, a Global command whose keymap row is implemented.
    void helpContentsIsInTheHelpMenu() {
        Shell sh;
        QVERIFY(sh.start());
        const QString id = QString::fromLatin1(DocsShell::kCommand);
        const qt::CommandSpec* spec = sh.w->commands().find(id);
        QVERIFY(spec != nullptr);
        QCOMPARE(spec->title, QStringLiteral("Help Contents"));
        QCOMPARE(spec->menuPath, QStringLiteral("Help"));
        QCOMPARE(spec->context, qt::CommandContext::Global);
        QCOMPARE(spec->defaultKeys, QStringList{QStringLiteral("F1")});
        QVERIFY(sh.w->commands().isEnabled(id));
        bool rowImplemented = false;
        for (const project::KeyBinding& row : sh.w->session().keymap().bindings) {
            if (row.command == "help.contents") {
                rowImplemented = row.status == project::KeymapStatus::Implemented && row.keys.size() == 1 &&
                                 row.keys.front() == "F1" && row.context == "global";
            }
        }
        QVERIFY(rowImplemented);
        QAction* action = sh.w->actions().action(id);
        QVERIFY(action != nullptr && action->isEnabled());
        QVERIFY2(!action->toolTip().contains(QStringLiteral("planned")), qPrintable(action->toolTip()));
        QMenu* help = sh.w->actions().menu(QStringLiteral("Help"));
        QVERIFY(help != nullptr);
        QVERIFY(!help->actions().isEmpty());
        QCOMPARE(help->actions().front(), action); // 06 section 2.2: Help Contents comes first
        // The menu item runs it.
        action->trigger();
        QCOMPARE(sh.opened.size(), 1);
    }

    // Installed (the repository's layout): the start page's file URL, and nothing else.
    void opensTheInstalledStartPage() {
        Shell sh;
        QVERIFY(sh.start());
        const fs::path expected = cmptest::repoRoot() / "docs" / "user" / "index.md";
        QVERIFY(fs::is_regular_file(expected));
        const QString url = sh.docs()->contentsUrl();
        QVERIFY(QUrl(url, QUrl::StrictMode).isLocalFile());
        QCOMPARE(canonicalOfUrl(url), canonicalOf(expected));
        sh.w->clearHint();
        const int rows = sh.w->messages().rowCount();
        QVERIFY(sh.w->commands().invoke(QString::fromLatin1(DocsShell::kCommand)));
        QCOMPARE(sh.opened, QStringList{url});
        QVERIFY(!sh.w->lastHint().isEmpty());
        QCOMPARE(sh.w->messages().rowCount(), rows);
    }

    // Not installed (the data directory of a build tree): the online start page of this build.
    void notInstalledOpensTheOnlineStartPage() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const fs::path root = fsPath(tmp.path());
        std::error_code ec;
        fs::create_directories(root / "bin", ec);
        QVERIFY2(!ec, ec.message().c_str());
        fs::copy(cmptest::repoRoot() / "data", root / "bin" / "data", fs::copy_options::recursive, ec);
        QVERIFY2(!ec, ec.message().c_str());
        Shell sh;
        QVERIFY(sh.start(pathsWith(fsPath(sh.config.path()), root / "bin" / "data", root / "bin")));
        QVERIFY(sh.docs()->contentsUrl().isEmpty());
        const QString online = DocsShell::onlineUrl(thisBuildsVersion());
        QVERIFY(online.startsWith(kOnlineBase));
        QVERIFY(online.endsWith(QStringLiteral("/docs/user/index.md")));
        QCOMPARE(sh.docs()->openContents(), online);
        QCOMPARE(sh.opened, QStringList{online});
    }

    // An installed page that nothing opens (no program for .md): the online one instead.
    void aPageThatDoesNotOpenFallsBackOnline() {
        Shell sh;
        QVERIFY(sh.start());
        sh.openFile = false;
        const int rows = sh.w->messages().rowCount();
        const QString local = sh.docs()->contentsUrl();
        const QString online = DocsShell::onlineUrl(thisBuildsVersion());
        QCOMPARE(sh.docs()->openContents(), online);
        QCOMPARE(sh.opened, (QStringList{local, online}));
        QCOMPARE(sh.w->messages().rowCount(), rows);
    }

    // Regression (PR #5 review U9, D-217): no program for .md files (Windows would show its chooser and
    // report success): the installed page is not handed over at all; the online one opens.
    void noProgramForMarkdownGoesOnline() {
        Shell sh;
        QVERIFY(sh.start());
        sh.mdHandled = false;
        const QString online = DocsShell::onlineUrl(thisBuildsVersion());
        QCOMPARE(sh.docs()->openContents(), online);
        QCOMPARE(sh.opened, QStringList{online});
        QVERIFY(sh.w->lastHint().contains(QStringLiteral("online")));
    }

    // Nothing opens: a Messages warning naming both places, the dock shown, the status bar's hint.
    void nothingOpensWarnsInMessages() {
        Shell sh;
        QVERIFY(sh.start());
        sh.openFile = false;
        sh.openOnline = false;
        sh.w->docks().setState(QString::fromLatin1(MessagesWindow::kDockId), DockState::Closed);
        QVERIFY(!sh.w->docks().isVisible(QString::fromLatin1(MessagesWindow::kDockId)));
        const int rows = sh.w->messages().rowCount();
        const QString online = DocsShell::onlineUrl(thisBuildsVersion());
        QVERIFY(sh.docs()->openContents().isEmpty());
        QCOMPARE(sh.opened.size(), 2);
        QCOMPARE(sh.w->messages().rowCount(), rows + 1);
        const qt::Message& m = sh.w->messages().at(rows);
        QCOMPARE(m.severity, qt::Message::Severity::Warning);
        QCOMPARE(m.origin, qt::Message::Origin::System);
        QCOMPARE(m.code, QString::fromLatin1(DocsShell::kNotOpenedCode));
        QVERIFY2(m.text.contains(online), qPrintable(m.text));
        const fs::path page = (cmptest::repoRoot() / "docs" / "user" / "index.md").lexically_normal();
        QVERIFY2(m.text.contains(QDir::toNativeSeparators(QString::fromStdString(pathToUtf8(page)))),
                 qPrintable(m.text));
        QVERIFY(sh.w->docks().isVisible(QString::fromLatin1(MessagesWindow::kDockId)));
        QVERIFY(sh.w->lastHint().contains(QStringLiteral("Messages")));
    }

    // The default opener (an empty opener restores it) is QDesktopServices::openUrl: claimed here by URL
    // handlers for the two schemes, so nothing outside the test is started.
    void theDefaultOpenerIsTheDesktopServices() {
        Shell sh;
        QVERIFY(sh.start());
        UrlRecorder recorder;
        QDesktopServices::setUrlHandler(QStringLiteral("file"), &recorder, "handle");
        QDesktopServices::setUrlHandler(QStringLiteral("https"), &recorder, "handle");
        sh.docs()->setUrlOpener({});
        const QString opened = sh.docs()->openContents();
        QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
        QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
        QCOMPARE(opened, sh.docs()->contentsUrl());
        QCOMPARE(recorder.urls.size(), 1);
        QCOMPARE(canonicalOfUrl(recorder.urls.front()), canonicalOfUrl(opened));
        QVERIFY(sh.opened.isEmpty()); // the recording opener is gone
    }
};

QTEST_MAIN(TstDocsShell)
#include "tst_docs_shell.moc"
