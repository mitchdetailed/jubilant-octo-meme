// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// esses: the GUI application (06 sections 18-19). Start-up order (01 section 5.3 rule 11): QApplication, then
// the AppRuntime (pool, cache, dispatcher; the GUI thread marked NoBlock), then the MainWindow (settings,
// shipped defaults, keymap, commands, the session's dock layout); then --scheme and --keymap, -w <project>
// (its own layout and its recovery prompt), then every -l <log> in order (with a project: after the prompt,
// through SessionRestore). After exec() returns the window is destroyed (it writes the layout), rt.shutdown()
// runs, and only then the runtime and the application.
//   esses [-w <project folder>] [-l <log file>]... [--new-instance] [--scheme <name>] [--keymap <name|file>]
//         [--offscreen] [--dump-commands] [--version] [--help] [<file>...]
// Bare arguments (06 section 19 item 3): a folder containing project.json, or a project.json, is -w; any
// other file is -l (the importer registry decides whether it can open it). launch_request.hpp parses the
// line. Single instance (06 section 18 step 1, D-113, D-212; instance_channel.hpp): unless --new-instance is
// given or the run only prints, a launch first hands its files to a running Esses of the same user and
// settings folder and exits 0; otherwise it listens itself, so later launches reach it. --version prints
// widgets::versionReport() (version, commit, Qt, build date, dock library, source; AC-M4-09). On Windows
// esses is a GUI-subsystem executable; ParentConsole makes its console output visible in the terminal that
// started it (08 section 3.10).

#include "instance_channel.hpp"
#include "launch.hpp"
#include "launch_request.hpp"

#include <esses/base/config.hpp>
#include <esses/base/paths.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/shell/about_dialog.hpp>
#include <esses/widgets/shell/main_window.hpp>

#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QSize>
#include <QTextStream>
#include <cstdio>
#include <cstring>
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

// The window icon (the application icon of tools/packaging/icons, compiled in by resources/esses.qrc).
QIcon applicationIcon() {
    QIcon icon;
    for (const int n : {16, 32, 48, 64, 128, 256, 512}) {
        icon.addFile(QStringLiteral(":/esses/icons/esses-%1.png").arg(n), QSize(n, n));
    }
    return icon;
}

// Hands the request to the instance on `name`; true when it took it.
bool forwarded(const QString& name, const esses::app::LaunchRequest& request) {
    QString detail;
    const esses::app::ForwardResult r =
        esses::app::forwardToInstance(name, request.toMessage(), esses::app::kForwardTimeoutMs, &detail);
    if (r == esses::app::ForwardResult::Failed) {
        QTextStream(stderr) << "esses: the running instance did not take the files (" << detail
                            << "); starting a new one\n";
    }
    return r == esses::app::ForwardResult::Delivered;
}

} // namespace

int main(int argc, char** argv) {
    // Before anything can print: QApplication's own warnings, --help, the parser's errors, --version.
    ParentConsole console;
    // --offscreen must act before the QApplication picks its platform plugin (06 section 19 item 4).
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--offscreen") == 0) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        }
    }
    // HiDPI (06 section 17 item 1, 04 section 6.6, D-025): a monitor at 125 % or 150 % gives a DPR of 1.25 or
    // 1.5, not one rounded to 1 or 2; the layout stays in logical pixels and the layers render at device
    // pixels. Qt 6's default; set before the QApplication exists so nothing else can change it
    // (QT_SCALE_FACTOR_ROUNDING_POLICY still overrides it for diagnosis).
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Esses"));
    QApplication::setOrganizationDomain(QStringLiteral("esses.invalid"));
    QApplication::setWindowIcon(applicationIcon());

    esses::app::CommandLine commandLine;
    switch (commandLine.parse(QApplication::arguments(), QDir::currentPath())) {
    case esses::app::CommandLine::Outcome::Help:
        QTextStream(stdout) << commandLine.helpText();
        return 0;
    case esses::app::CommandLine::Outcome::Error:
        QTextStream(stderr) << "esses: " << commandLine.errorText() << "\n(esses --help lists the options)\n";
        return 1;
    case esses::app::CommandLine::Outcome::Version:
        // AC-M4-09: version, commit, Qt version, build date (widgets::versionReport, about_dialog.hpp;
        // D-212).
        QTextStream(stdout) << esses::widgets::versionReport() << '\n';
        return 0;
    case esses::app::CommandLine::Outcome::Run:
        break;
    }
    const esses::app::LaunchRequest& request = commandLine.request();

    auto paths = esses::Paths::resolve();
    if (!paths) {
        QTextStream(stderr) << QString::fromStdString(paths.error().toString()) << '\n';
        return 2;
    }

    // Single instance (06 section 18 step 1, D-113): hand the files over and stop, else be the instance later
    // launches reach. Listening may fail when another instance started at the same moment: try it once more.
    esses::app::InstanceServer server;
    if (!request.dumpCommands) {
        const QString name = esses::app::instanceName(paths->configDir());
        if (esses::app::shouldForward(request) && forwarded(name, request)) {
            return 0;
        }
        if (!server.listen(name) && esses::app::shouldForward(request) && forwarded(name, request)) {
            return 0;
        }
        console.release(); // the GUI from here on: nothing more is printed for the terminal
    }

    esses::qt::AppRuntimeOptions options;
    options.markGuiThreadNoBlock = true;
    esses::qt::AppRuntime rt(options);
    esses::widgets::registerBuiltinComponents();

    int rc = 0;
    {
        esses::widgets::MainWindow window(rt, *paths);
        if (request.dumpCommands) {
            window.setSaveLayoutOnExit(false);
            QTextStream(stdout) << window.commands().dumpJson();
        } else {
            esses::app::applyStartOptions(window, request); // --scheme, --keymap (problems go to Messages)
            esses::app::openRequestedFiles(window, request, esses::app::LaunchKind::Start);
            QObject::connect(&server, &esses::app::InstanceServer::requestReceived, &window,
                             [&window](const esses::app::LaunchRequest& forwardedRequest) {
                                 esses::app::openRequestedFiles(window, forwardedRequest,
                                                                esses::app::LaunchKind::Forwarded);
                                 esses::app::bringToFront(window);
                             });
            window.show();
            rc = QApplication::exec();
            server.close(); // no request reaches a window that is going away
        }
    }
    rt.shutdown();
    return rc;
}
