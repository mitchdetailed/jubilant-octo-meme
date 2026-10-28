// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Helpers of the esses application's Qt Tests (apps/app/tests/tst_*.cpp; D-212): the repository and the built
// esses executable (ESSES_APP_EXE), a private configuration for a window or for a child esses, a unique
// instance name (ESSES_INSTANCE_NAME), a runtime as esses makes it, a window shown and active, and waiting
// for a child process without fixed sleeps, the text of a file a child keeps rewriting. Header only;
// namespace esses::app::test.
#pragma once

#include <esses/base/paths.hpp>
#include <esses/base/read_file.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/widgets/components/component_widget.hpp>

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QTest>
#include <QUuid>
#include <QWidget>
#include <filesystem>
#include <memory>
#include <string>

namespace esses::app::test {

inline QString repoRoot() {
    return QStringLiteral(ESSES_TEST_REPO_ROOT);
}

inline QString essesExe() {
    return QStringLiteral(ESSES_APP_EXE);
}

inline std::filesystem::path fsPath(const QString& p) {
    return std::filesystem::path(p.toStdWString());
}

// A name of our own for the single-instance channel, so a test never meets a real Esses or another test.
inline QString uniqueInstanceName() {
    return QStringLiteral("esses-test-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// Paths with the configuration in `configDir` and the repository's shipped data.
inline Paths pathsFor(const QString& configDir) {
    PathOverrides o;
    o.configDir = fsPath(configDir);
    o.cacheDir = fsPath(configDir + QStringLiteral("/cache"));
    o.logDir = fsPath(configDir + QStringLiteral("/logs"));
    o.dataDir = fsPath(repoRoot() + QStringLiteral("/data"));
    auto p = Paths::resolve(o);
    if (!p) {
        qFatal("Paths::resolve: %s", p.error().toString().c_str());
    }
    return *p;
}

inline std::unique_ptr<qt::AppRuntime> runtime(unsigned workers = 2) {
    widgets::registerBuiltinComponents();
    qt::AppRuntimeOptions o;
    o.workers = workers;
    o.cacheBudget = std::size_t{128} << 20;
    o.markGuiThreadNoBlock = true; // as esses
    return std::make_unique<qt::AppRuntime>(o);
}

inline bool showAndActivate(QWidget& w) {
    w.show();
    if (!QTest::qWaitForWindowExposed(&w)) {
        return false;
    }
    w.activateWindow();
    return QTest::qWaitForWindowActive(&w);
}

// The environment of a child esses: offscreen, its configuration, cache and logs under `configDir`, the
// instance name `instance` (empty: none set).
inline QProcessEnvironment childEnvironment(const QString& configDir, const QString& instance) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    env.insert(QStringLiteral("ESSES_CONFIG_DIR"), configDir);
    env.insert(QStringLiteral("ESSES_CACHE_DIR"), configDir + QStringLiteral("/cache"));
    env.insert(QStringLiteral("ESSES_LOG_DIR"), configDir + QStringLiteral("/logs"));
    if (instance.isEmpty()) {
        env.remove(QStringLiteral("ESSES_INSTANCE_NAME"));
    } else {
        env.insert(QStringLiteral("ESSES_INSTANCE_NAME"), instance);
    }
    return env;
}

// The UTF-8 text of a file that another process may be replacing (a child's settings.json: AtomicFile renames
// a new version over it): read through esses::readWholeFile, which shares delete access on Windows so the
// child's rename is never refused because of this read (a QFile or std::ifstream would hold the name); empty
// when it cannot be read (not written yet, or between the old and the new version).
inline QString fileText(const QString& path) {
    const Result<std::string> bytes = readWholeFile(fsPath(path));
    return bytes.ok() ? QString::fromStdString(bytes.value()) : QString();
}

// Ends a child that runs a window (there is no other way to close it from outside) and waits for it.
inline void stopChild(QProcess& p) {
    if (p.state() != QProcess::NotRunning) {
        p.kill();
        p.waitForFinished(30000);
    }
}

} // namespace esses::app::test
