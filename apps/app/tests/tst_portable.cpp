// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Portable mode (01 section 9, D-055; 07 section 7.5 "settings live beside the exe"; D-212): with
// portable.marker beside the executable the settings, the session layout, the cache and the logs live in <exe
// dir>/config, /cache and /logs, and a portable copy is an instance of its own (its own settings folder, so
// its own single-instance channel). In process through the Paths override of the executable's folder, and
// with a copy of the built esses in a folder of its own (as the portable zip lays it out).

#include "app_test_support.hpp"
#include "instance_channel.hpp"

#include <esses/base/paths.hpp>
#include <esses/widgets/shell/main_window.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

using namespace esses;
using namespace esses::app;
using namespace esses::app::test;

namespace {

class EnvGuard {
public:
    explicit EnvGuard(const char* name)
        : name_(name), had_(qEnvironmentVariableIsSet(name)), value_(qgetenv(name)) {
        qunsetenv(name);
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

bool writeMarker(const QString& dir) {
    QFile f(dir + QStringLiteral("/portable.marker"));
    return f.open(QIODevice::WriteOnly); // empty, as the portable zip ships it
}

} // namespace

class TestPortable : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void portableWindowKeepsItsSettingsBesideTheProgram() {
        const EnvGuard config("ESSES_CONFIG_DIR");
        const EnvGuard cache("ESSES_CACHE_DIR");
        const EnvGuard logs("ESSES_LOG_DIR");
        QTemporaryDir stick;
        QVERIFY(stick.isValid());
        QVERIFY(writeMarker(stick.path()));
        PathOverrides o;
        o.exeDir = fsPath(stick.path());
        o.dataDir = fsPath(repoRoot() + QStringLiteral("/data"));
        auto p = Paths::resolve(o);
        QVERIFY(static_cast<bool>(p));
        QVERIFY(p->portable());
        QCOMPARE(p->configDir(), fsPath(stick.path()) / "config");
        QCOMPARE(p->cacheDir(), fsPath(stick.path()) / "cache");
        QCOMPARE(p->logDir(), fsPath(stick.path()) / "logs");

        // Its own instance: the installed program (another settings folder) never forwards to it.
        QTemporaryDir installed;
        QVERIFY(installed.isValid());
        PathOverrides io;
        io.exeDir = fsPath(installed.path());
        auto ip = Paths::resolve(io);
        QVERIFY(static_cast<bool>(ip));
        QVERIFY(!ip->portable());
        {
            const EnvGuard forced("ESSES_INSTANCE_NAME");
            QVERIFY(instanceName(p->configDir()) != instanceName(ip->configDir()));
        }

        std::unique_ptr<qt::AppRuntime> rt = runtime(2);
        const QString settings = stick.path() + QStringLiteral("/config/settings.json");
        {
            widgets::MainWindow w(*rt, *p);
            QVERIFY(showAndActivate(w));
            w.openLog(fsPath(stick.path() + QStringLiteral("/a session.csv")));
            QTRY_VERIFY_WITH_TIMEOUT(fileText(settings).contains(QStringLiteral("a session.csv")), 30000);
        }
        rt->shutdown();
        QVERIFY(QFileInfo::exists(stick.path() + QStringLiteral("/config/layout.json"))); // written at exit
    }

    // The real program, copied into a folder with portable.marker, writes its settings into <folder>/config.
    void realProgramHonoursTheMarker() {
        QTemporaryDir stick;
        QVERIFY(stick.isValid());
        QVERIFY(writeMarker(stick.path()));
        const QString exe = stick.path() + QLatin1Char('/') + QFileInfo(essesExe()).fileName();
        QVERIFY2(QFile::copy(essesExe(), exe), qPrintable(exe));
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        env.insert(QStringLiteral("ESSES_INSTANCE_NAME"), uniqueInstanceName());
        for (const char* v : {"ESSES_CONFIG_DIR", "ESSES_CACHE_DIR", "ESSES_LOG_DIR"}) {
            env.remove(QString::fromLatin1(v));
        }
        const QString bin = QStringLiteral(ESSES_TEST_BIN_DIR);
        env.insert(QStringLiteral("ESSES_DATA_DIR"),
                   bin + QStringLiteral("/data")); // the zip has data/ beside
        // The docking library and Qt stay in the build tree; Windows finds them through PATH.
        env.insert(QStringLiteral("PATH"),
                   QDir::toNativeSeparators(bin) + QDir::listSeparator() + env.value(QStringLiteral("PATH")));
        QProcess child;
        child.setProcessEnvironment(env);
        child.setWorkingDirectory(stick.path());
        child.start(exe, {QStringLiteral("-l"), stick.path() + QStringLiteral("/from the stick.csv")});
        QVERIFY2(child.waitForStarted(30000), qPrintable(child.errorString()));
        const QString settings = stick.path() + QStringLiteral("/config/settings.json");
        QTRY_VERIFY_WITH_TIMEOUT(fileText(settings).contains(QStringLiteral("from the stick.csv")), 120000);
        QVERIFY(child.state() != QProcess::NotRunning);
        stopChild(child);
    }
};

QTEST_MAIN(TestPortable)
#include "tst_portable.moc"
