// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// esses: the GUI application (06 sections 18-19). Start-up order (01 section 5.3 rule 11): QApplication, then
// the AppRuntime (pool, cache, dispatcher; the GUI thread marked NoBlock), then the MainWindow (settings,
// shipped defaults, keymap, commands, the session's dock layout); then -w <project> (its own layout), then
// every -l <log> in order. After exec() returns the window is destroyed (it writes the layout), rt.shutdown()
// runs, and only then the runtime and the application.
//   esses [-w <project folder>] [-l <log file>]... [--offscreen] [--dump-commands] [--version] [<file>...]
// Bare arguments (06 section 19 item 3): a folder containing project.json, or a project.json, is -w; any
// other file is -l (the importer registry decides whether it can open it).
// On Windows esses is a GUI-subsystem executable; ParentConsole makes its console output visible in the
// terminal that started it (08 section 3.10).

#include <esses/base/config.hpp>
#include <esses/base/paths.hpp>
#include <esses/base/version.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/shell/main_window.hpp>

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QTextStream>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>

#if defined(ESSES_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

#if defined(ESSES_OS_WINDOWS)
// Windows starts a GUI-subsystem executable without a console, and its stdout and stderr have no handle
// unless the caller redirected them (to a file, or to a pipe as ctest does). Started from a terminal,
// --version, --help, --dump-commands and the parser's errors would go nowhere. ParentConsole attaches to the
// console of the parent process when it has one (a terminal) and points each stream without a handle at it.
// Started from Explorer the parent has no console: AttachConsole fails, nothing changes and no console
// window is created. Streams the caller redirected are left alone. release() detaches before the GUI runs,
// so the terminal's Ctrl+C, or closing it, cannot end the application.
class ParentConsole {
public:
    ParentConsole() {
        const bool haveOut = _fileno(stdout) >= 0; // -2 when the stream has no handle (UCRT)
        const bool haveErr = _fileno(stderr) >= 0;
        if ((haveOut && haveErr) || AttachConsole(ATTACH_PARENT_PROCESS) == 0) {
            return;
        }
        attached_ = true;
        out_ = !haveOut && reopen(stdout, "CONOUT$");
        err_ = !haveErr && reopen(stderr, "CONOUT$");
    }
    ParentConsole(const ParentConsole&) = delete;
    ParentConsole& operator=(const ParentConsole&) = delete;
    ~ParentConsole() = default; // the console detaches when the process ends

    void release() {
        if (!attached_) {
            return;
        }
        std::fflush(stdout);
        std::fflush(stderr);
        // Away from the console handles first: they are invalid after FreeConsole() and their values may be
        // reused for other objects.
        if (out_) {
            reopen(stdout, "NUL");
        }
        if (err_) {
            reopen(stderr, "NUL");
        }
        FreeConsole();
        attached_ = false;
    }

private:
    static bool reopen(FILE* stream, const char* device) {
        FILE* reopened = nullptr;
        return freopen_s(&reopened, device, "w", stream) == 0;
    }

    bool attached_ = false;
    bool out_ = false; // stdout was pointed at the console
    bool err_ = false;
};
#else
struct ParentConsole {
    void release() {}
};
#endif

std::filesystem::path toPath(const QString& s) {
    return std::filesystem::path(s.toStdWString());
}

bool isProject(const QString& arg) {
    const QFileInfo info(arg);
    return (info.isDir() && QFileInfo::exists(info.filePath() + QStringLiteral("/project.json"))) ||
           info.fileName() == QLatin1String("project.json");
}

} // namespace

int main(int argc, char** argv) {
    // Before anything can print: QApplication's own warnings, the parser's --help and errors, --version.
    ParentConsole console;
    // --offscreen must act before the QApplication picks its platform plugin (06 section 19 item 4).
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--offscreen") == 0) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        }
    }
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Esses"));
    QApplication::setOrganizationDomain(QStringLiteral("esses.invalid"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Esses motorsport data analysis"));
    parser.addHelpOption();
    const QCommandLineOption versionOption(QStringLiteral("version"),
                                           QStringLiteral("Print the version and exit."));
    const QCommandLineOption projectOption(QStringList{QStringLiteral("w"), QStringLiteral("workspace")},
                                           QStringLiteral("Open the project folder <project>."),
                                           QStringLiteral("project"));
    const QCommandLineOption logOption(QStringList{QStringLiteral("l"), QStringLiteral("log")},
                                       QStringLiteral("Open the log file <log> (repeatable)."),
                                       QStringLiteral("log"));
    const QCommandLineOption dumpOption(QStringLiteral("dump-commands"),
                                        QStringLiteral("Print the command registry as JSON and exit."));
    const QCommandLineOption offscreenOption(QStringLiteral("offscreen"),
                                             QStringLiteral("Run without a display (tests)."));
    parser.addOption(versionOption);
    parser.addOption(projectOption);
    parser.addOption(logOption);
    parser.addOption(dumpOption);
    parser.addOption(offscreenOption);
    parser.addPositionalArgument(QStringLiteral("files"),
                                 QStringLiteral("Project folders (with project.json) or log files."),
                                 QStringLiteral("[<file>...]"));
    parser.process(app);

    if (parser.isSet(versionOption)) {
        QTextStream(stdout) << "esses " << QString::fromStdString(std::string(esses::versionString()))
                            << '\n';
        return 0;
    }

    auto paths = esses::Paths::resolve();
    if (!paths) {
        QTextStream(stderr) << QString::fromStdString(paths.error().toString()) << '\n';
        return 2;
    }
    if (!parser.isSet(dumpOption)) {
        console.release(); // the GUI from here on: nothing more is printed for the terminal
    }

    QString project = parser.value(projectOption);
    QStringList logs = parser.values(logOption);
    for (const QString& arg : parser.positionalArguments()) {
        if (isProject(arg) && project.isEmpty()) {
            project = arg;
        } else {
            logs << arg;
        }
    }

    esses::qt::AppRuntimeOptions options;
    options.markGuiThreadNoBlock = true;
    esses::qt::AppRuntime rt(options);
    esses::widgets::registerBuiltinComponents();

    int rc = 0;
    {
        esses::widgets::MainWindow window(rt, *paths);
        if (parser.isSet(dumpOption)) {
            window.setSaveLayoutOnExit(false);
            QTextStream(stdout) << window.commands().dumpJson();
        } else {
            if (!project.isEmpty()) {
                window.openProject(toPath(project));
            }
            for (const QString& log : logs) {
                window.openLog(toPath(log));
            }
            window.show();
            rc = QApplication::exec();
        }
    }
    rt.shutdown();
    return rc;
}
