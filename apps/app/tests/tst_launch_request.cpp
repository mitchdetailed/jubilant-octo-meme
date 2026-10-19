// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The command line of esses and the forwarded request (06 section 19, D-113, D-212): -w/-l and bare arguments
// made absolute against the launch's working directory (item 2), a bare project folder or project.json is the
// project (item 3), --new-instance, --scheme and --keymap, the runs that only print, and the one-line JSON a
// launch hands to a running instance, which accepts only absolute paths. Expectations are the paths the test
// builds itself from its temporary working directory.

#include "app_test_support.hpp"
#include "instance_channel.hpp"
#include "launch_request.hpp"

#include <esses/widgets/shell/about_dialog.hpp>

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

using namespace esses::app;

namespace {

void touch(const QString& path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write("{}\n");
    }
}

class EnvGuard {
public:
    explicit EnvGuard(const char* name)
        : name_(name), had_(qEnvironmentVariableIsSet(name)), value_(qgetenv(name)) {}
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

} // namespace

class TestLaunchRequest : public QObject {
    Q_OBJECT

    QTemporaryDir wd_;
    QString w(const QString& rel) const { return QDir::cleanPath(wd_.path() + QLatin1Char('/') + rel); }

private Q_SLOTS:
    void initTestCase() {
        QVERIFY(wd_.isValid());
        touch(w(QStringLiteral("proj/project.json")));
        QVERIFY(QDir().mkpath(w(QStringLiteral("emptydir"))));
        touch(w(QStringLiteral("keys/mine.json")));
    }

    void filesAndProject_data() {
        QTest::addColumn<QStringList>("args");
        QTest::addColumn<QString>("project");
        QTest::addColumn<QStringList>("logs");
        QTest::newRow("nothing") << QStringList{} << QString() << QStringList{};
        QTest::newRow("-l and --log")
            << QStringList{"-l", "a.ld", "--log", "b.csv"} << QString() << QStringList{w("a.ld"), w("b.csv")};
        QTest::newRow("-w folder") << QStringList{"-w", "proj", "-l", "x.ld"} << w("proj")
                                   << QStringList{w("x.ld")};
        QTest::newRow("--workspace project.json")
            << QStringList{"--workspace", "proj/project.json"} << w("proj/project.json") << QStringList{};
        QTest::newRow("bare project then logs")
            << QStringList{"proj", "one.ld", "two.ld"} << w("proj") << QStringList{w("one.ld"), w("two.ld")};
        QTest::newRow("bare project.json")
            << QStringList{"other/project.json"} << w("other/project.json") << QStringList{};
        QTest::newRow("bare folder without project.json")
            << QStringList{"emptydir"} << QString() << QStringList{w("emptydir")};
        QTest::newRow("a second bare project is a log")
            << QStringList{"proj", "proj"} << w("proj") << QStringList{w("proj")};
        QTest::newRow("-w wins over a bare project")
            << QStringList{"-w", "other", "proj"} << w("other") << QStringList{w("proj")};
        QTest::newRow("-w twice: the last") << QStringList{"-w", "a", "-w", "b"} << w("b") << QStringList{};
        QTest::newRow("absolute paths cleaned")
            << QStringList{"-l", w("sub") + "/../c.ld"} << QString() << QStringList{w("c.ld")};
        QTest::newRow("logs keep their order") << QStringList{"z.ld", "-l", "a.ld", "m.ld"} << QString()
                                               << QStringList{w("a.ld"), w("z.ld"), w("m.ld")};
    }
    void filesAndProject() {
        QFETCH(QStringList, args);
        QFETCH(QString, project);
        QFETCH(QStringList, logs);
        CommandLine cl;
        QCOMPARE(cl.parse(QStringList{QStringLiteral("esses")} + args, wd_.path()),
                 CommandLine::Outcome::Run);
        QCOMPARE(cl.request().project, project);
        QCOMPARE(cl.request().logs, logs); // the -l values first, then the bare files, each in order
        QVERIFY(shouldForward(cl.request()));
    }

    void options() {
        CommandLine cl;
        QCOMPARE(cl.parse({"esses", "--new-instance", "-l", "a.ld"}, wd_.path()), CommandLine::Outcome::Run);
        QVERIFY(cl.request().newInstance);
        QVERIFY(!shouldForward(cl.request()));

        QCOMPARE(cl.parse({"esses", "--scheme", "Esses Dark", "--keymap", "laptop"}, wd_.path()),
                 CommandLine::Outcome::Run);
        QCOMPARE(cl.request().scheme, QStringLiteral("Esses Dark"));
        QCOMPARE(cl.request().keymap, QStringLiteral("laptop")); // a name: looked up by the window
        QVERIFY(shouldForward(cl.request()));                    // the files are forwarded, not the options

        QCOMPARE(cl.parse({"esses", "--keymap", "keys/mine.json"}, wd_.path()), CommandLine::Outcome::Run);
        QCOMPARE(cl.request().keymap, w("keys/mine.json")); // an existing file: absolute

        QCOMPARE(cl.parse({"esses", "--offscreen", "--dump-commands"}, wd_.path()),
                 CommandLine::Outcome::Run);
        QVERIFY(cl.request().offscreen);
        QVERIFY(cl.request().dumpCommands);
        QVERIFY(!shouldForward(cl.request()));

        QCOMPARE(cl.parse({"esses", "--version"}, wd_.path()), CommandLine::Outcome::Version);
        QVERIFY(!shouldForward(cl.request()));
        QCOMPARE(cl.parse({"esses", "--help"}, wd_.path()), CommandLine::Outcome::Help);
        QCOMPARE(cl.parse({"esses", "-h"}, wd_.path()), CommandLine::Outcome::Help);
        QVERIFY(cl.helpText().contains(QStringLiteral("--new-instance")));
        QVERIFY(cl.helpText().contains(QStringLiteral("--scheme")));
        QVERIFY(cl.helpText().contains(QStringLiteral("--keymap")));
    }

    void errors_data() {
        QTest::addColumn<QStringList>("args");
        QTest::newRow("unknown option") << QStringList{"esses", "--bogus"};
        QTest::newRow("--scheme without a value") << QStringList{"esses", "--scheme"};
        QTest::newRow("--scheme empty") << QStringList{"esses", "--scheme", " "};
        QTest::newRow("--keymap empty") << QStringList{"esses", "--keymap", ""};
        QTest::newRow("-l without a value") << QStringList{"esses", "-l"};
    }
    void errors() {
        QFETCH(QStringList, args);
        CommandLine cl;
        QCOMPARE(cl.parse(args, wd_.path()), CommandLine::Outcome::Error);
        QVERIFY(!cl.errorText().isEmpty());
    }

    // The forwarded line: one line of JSON with the files only; the receiving side gets the same paths.
    void messageRoundTrip() {
        CommandLine cl;
        QCOMPARE(
            cl.parse({"esses", "-w", "proj", "-l", "a b.ld", "--scheme", "x", "--new-instance"}, wd_.path()),
            CommandLine::Outcome::Run);
        const QByteArray line = cl.request().toMessage();
        QVERIFY(line.endsWith('\n'));
        QCOMPARE(line.count('\n'), 1);
        const std::optional<LaunchRequest> back = LaunchRequest::fromMessage(line);
        QVERIFY(back.has_value());
        QCOMPARE(back->project, w("proj"));
        QCOMPARE(back->logs, QStringList{w("a b.ld")});
        QVERIFY(back->scheme.isEmpty());
        QVERIFY(!back->newInstance);
        QVERIFY(LaunchRequest::fromMessage(line.chopped(1)).has_value()); // the newline is optional
    }

    void messageRefusals_data() {
        const QByteArray abs = QDir::cleanPath(QDir::tempPath() + QStringLiteral("/a.ld")).toUtf8();
        QTest::addColumn<QByteArray>("line");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("empty request") << QByteArray(R"({"protocol":1})") << true;
        QTest::newRow("unknown members")
            << QByteArray(R"({"protocol":1,"later":true,"logs":[")" + abs + R"("]})") << true;
        QTest::newRow("not JSON") << QByteArray("garbage") << false;
        QTest::newRow("an array") << QByteArray("[]") << false;
        QTest::newRow("no protocol") << QByteArray(R"({"logs":[]})") << false;
        QTest::newRow("protocol 2") << QByteArray(R"({"protocol":2})") << false;
        QTest::newRow("project not a string") << QByteArray(R"({"protocol":1,"project":5})") << false;
        QTest::newRow("relative project") << QByteArray(R"({"protocol":1,"project":"proj"})") << false;
        QTest::newRow("logs not an array") << QByteArray(R"({"protocol":1,"logs":"a.ld"})") << false;
        QTest::newRow("relative log") << QByteArray(R"({"protocol":1,"logs":["a.ld"]})") << false;
        QTest::newRow("empty log") << QByteArray(R"({"protocol":1,"logs":[""]})") << false;
        QTest::newRow("log not a string") << QByteArray(R"({"protocol":1,"logs":[3]})") << false;
        QByteArray many = R"({"protocol":1,"logs":[)";
        for (int i = 0; i <= LaunchRequest::kMaxLogs; ++i) {
            many += (i > 0 ? "," : "") + QByteArray("\"") + abs + "\"";
        }
        many += "]}";
        QTest::newRow("too many logs") << many << false;
    }
    void messageRefusals() {
        QFETCH(QByteArray, line);
        QFETCH(bool, accepted);
        QString why;
        const std::optional<LaunchRequest> r = LaunchRequest::fromMessage(line, &why);
        QCOMPARE(r.has_value(), accepted);
        QCOMPARE(why.isEmpty(), accepted);
    }

    // AC-M4-09: `esses --version` prints exactly widgets::versionReport() (whose fields
    // widgets.tst_about_dialog checks against the build: version, commit, Qt, build date, dock library,
    // source) and exits 0.
    void versionOfTheProgram() {
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        p.setProcessEnvironment(env);
        p.start(test::essesExe(), {QStringLiteral("--version")});
        QVERIFY2(p.waitForStarted(30000), qPrintable(p.errorString()));
        QVERIFY(p.waitForFinished(120000));
        QCOMPARE(p.exitStatus(), QProcess::NormalExit);
        QCOMPARE(p.exitCode(), 0);
        QString out = QString::fromUtf8(p.readAllStandardOutput());
        out.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        QCOMPARE(out, esses::widgets::versionReport() + QLatin1Char('\n'));
        const QStringList lines = out.split(QLatin1Char('\n'));
        QVERIFY(lines.size() >= 4);
        QVERIFY(lines[0].startsWith(QStringLiteral("esses ")));
        QVERIFY(lines[1].startsWith(QStringLiteral("commit: ")));
        QVERIFY(lines[2].startsWith(QStringLiteral("Qt: ")));
        QVERIFY(lines[3].startsWith(QStringLiteral("build date: ")));
    }

    // D-212: one channel per user and settings folder; ESSES_INSTANCE_NAME replaces it, made safe.
    void instanceNames() {
        EnvGuard guard("ESSES_INSTANCE_NAME");
        qunsetenv("ESSES_INSTANCE_NAME");
        const QString a = instanceName(test::fsPath(w(QStringLiteral("config-a"))));
        const QString b = instanceName(test::fsPath(w(QStringLiteral("config-b"))));
        QCOMPARE(instanceName(test::fsPath(w(QStringLiteral("config-a")))), a);
        QVERIFY(a != b);
        QVERIFY(a.startsWith(QStringLiteral("esses-")));
        const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9._-]{1,96}$"));
        QVERIFY2(safe.match(a).hasMatch(), qPrintable(a));
        QVERIFY(QRegularExpression(QStringLiteral("-[0-9a-f]{12}$")).match(a).hasMatch());
#if defined(Q_OS_WIN)
        // One folder however it is spelled.
        QCOMPARE(instanceName(test::fsPath(w(QStringLiteral("CONFIG-A")))), a);
#endif
        qputenv("ESSES_INSTANCE_NAME", "my test/инстанс name");
        const QString forced = instanceName(test::fsPath(w(QStringLiteral("config-a"))));
        QVERIFY2(safe.match(forced).hasMatch(), qPrintable(forced));
        QVERIFY(forced.startsWith(QStringLiteral("my_test_")));
    }
};

QTEST_GUILESS_MAIN(TestLaunchRequest)
#include "tst_launch_request.moc"
