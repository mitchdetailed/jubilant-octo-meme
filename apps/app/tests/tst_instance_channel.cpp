// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Single-instance forwarding (06 section 18 step 1, section 19 item 6; D-113, D-212), in process and with the
// built esses (ESSES_APP_EXE): nobody listening answers at once, one listener per name, a request reaches the
// server and is answered, a malformed, oversized or silent client is refused while the server keeps serving,
// a second esses hands its files (absolute against its own working directory, also when that folder is named
// through a link) to the running one and exits 0 without building a window, --new-instance runs its own
// window, and two real esses processes forward to each other. The blocking client side of the channel runs on
// a test thread when the server is in this process (as in esses, the server needs the event loop; the client
// is the short-lived launch). Every channel name is unique (ESSES_INSTANCE_NAME), so no test meets a real
// Esses or another test.

#include "app_test_support.hpp"
#include "instance_channel.hpp"
#include "launch.hpp"
#include "launch_request.hpp"

#include <esses/qtmodel/settings_store.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/widgets/shell/main_window.hpp>

#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <filesystem>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <QScopeGuard>
#include <cstring>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

using namespace esses;
using namespace esses::app;
using namespace esses::app::test;

namespace {

constexpr int kLong = 120000; // a child esses in a Debug build on a loaded machine

// A client that connects and says nothing (until close()).
class SilentClient {
public:
    explicit SilentClient(const QString& address) {
#if defined(Q_OS_WIN)
        const std::wstring path = address.toStdWString();
        handle_ =
            CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
#else
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        const QByteArray bytes = address.toLocal8Bit();
        std::memcpy(a.sun_path, bytes.constData(), static_cast<size_t>(bytes.size()) + 1);
        if (fd_ >= 0 &&
            ::connect(fd_, reinterpret_cast<const sockaddr*>(&a), static_cast<socklen_t>(sizeof(a))) != 0) {
            ::close(fd_);
            fd_ = -1;
        }
#endif
    }
    ~SilentClient() { close(); }
    SilentClient(const SilentClient&) = delete;
    SilentClient& operator=(const SilentClient&) = delete;

    bool connected() const {
#if defined(Q_OS_WIN)
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return fd_ >= 0;
#endif
    }
    void close() {
#if defined(Q_OS_WIN)
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
#endif
    }

private:
#if defined(Q_OS_WIN)
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

// forwardToInstance on a thread of its own (the server is in this thread).
class ClientThread {
public:
    ClientThread(const QString& name, QByteArray message, int timeoutMs = 30000)
        : thread_([this, name, line = std::move(message), timeoutMs] {
              result_ = forwardToInstance(name, line, timeoutMs, &detail_);
              done_ = true;
          }) {}
    ~ClientThread() { join(); }
    ClientThread(const ClientThread&) = delete;
    ClientThread& operator=(const ClientThread&) = delete;

    bool done() const { return done_; }
    ForwardResult join() {
        if (thread_.joinable()) {
            thread_.join();
        }
        return result_;
    }
    QString detail() const { return detail_; }

private:
    ForwardResult result_ = ForwardResult::Failed;
    QString detail_;
    std::atomic<bool> done_{false};
    std::thread thread_;
};

LaunchRequest requestFor(const QStringList& logs, const QString& project = {}) {
    LaunchRequest r;
    r.project = project;
    r.logs = logs;
    return r;
}

QString settingsText(const QString& configDir) {
    return fileText(configDir + QStringLiteral("/settings.json")); // the child keeps rewriting it (D-216)
}

// Empty when `paths` are, in order, absolute clean paths of the files `names` in the folder `dir`, else what
// differs. The folder is compared as a folder (std::filesystem::equivalent), not as text: a launch makes a
// relative argument absolute against QDir::currentPath(), the system's name of its working directory, and
// POSIX getcwd() answers the physical path (on macOS a launch started in /var/folders/... forwards
// /private/var/folders/..., /var being a symbolic link), while Windows keeps the spelling it was given.
QString namesTheFilesIn(const QStringList& paths, const QStringList& names, const QString& dir) {
    if (paths.size() != names.size()) {
        return QStringLiteral("%1 paths, expected %2: %3")
            .arg(paths.size())
            .arg(names.size())
            .arg(paths.join(QStringLiteral(" | ")));
    }
    for (qsizetype i = 0; i < paths.size(); ++i) {
        const QString& p = paths.at(i);
        const QFileInfo info(p);
        std::error_code ec;
        if (!QDir::isAbsolutePath(p) || p != QDir::cleanPath(p) || info.fileName() != names.at(i) ||
            !std::filesystem::equivalent(fsPath(info.absolutePath()), fsPath(dir), ec) || ec) {
            return QStringLiteral("%1 is not %2 in %3").arg(p, names.at(i), dir);
        }
    }
    return {};
}

// A second name `path` of the existing folder `target` while it lives: a symbolic link on POSIX, a junction
// on Windows (a symbolic link there needs a privilege). Removed (the link only) at the end.
class FolderLink {
public:
    FolderLink(const QString& target, const QString& path) : path_(path) {
#if defined(Q_OS_WIN)
        QProcess mklink;
        mklink.setProgram(qEnvironmentVariable("ComSpec", QStringLiteral("cmd.exe")));
        mklink.setNativeArguments(QStringLiteral("/c mklink /J \"%1\" \"%2\"")
                                      .arg(QDir::toNativeSeparators(path), QDir::toNativeSeparators(target)));
        mklink.start();
        made_ = mklink.waitForFinished(30000) && mklink.exitStatus() == QProcess::NormalExit &&
                mklink.exitCode() == 0;
#else
        made_ = QFile::link(target, path);
#endif
        made_ = made_ && QFileInfo(path).isDir();
    }
    ~FolderLink() {
        if (!made_) {
            return;
        }
#if defined(Q_OS_WIN)
        QDir().rmdir(path_); // removes the junction, not what it names
#else
        QFile::remove(path_);
#endif
    }
    FolderLink(const FolderLink&) = delete;
    FolderLink& operator=(const FolderLink&) = delete;

    bool made() const { return made_; }

private:
    QString path_;
    bool made_ = false;
};

} // namespace

class TestInstanceChannel : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // Regression (PR #5 review U1, D-217): a forwarded request that arrives inside a nested event loop (what
    // a modal prompt runs) or while a modal widget is shown waits; it is applied at the top of the event
    // loop.
    void forwardedRequestsWaitForTheTopOfTheEventLoop() {
        int applied = 0;
        int appliedAtLevel = -1;
        RequestQueue queue(
            [&](const LaunchRequest&) {
                ++applied;
                appliedAtLevel = QThread::currentThread()->loopLevel();
            },
            0); // this test runs outside QApplication::exec: its top is level 0
        int appliedInside = -1;
        QEventLoop nested;
        QTimer::singleShot(0, &nested, [&] {
            queue.enqueue(requestFor({QStringLiteral("a.ld")})); // as InstanceServer::requestReceived would
            QTimer::singleShot(4 * RequestQueue::kRetryMs, &nested, [&] {
                appliedInside = applied;
                nested.quit();
            });
        });
        nested.exec(QEventLoop::ExcludeUserInputEvents); // a modal dialog's loop excludes user input too
        QCOMPARE(appliedInside, 0);
        QTRY_COMPARE_WITH_TIMEOUT(applied, 1, 10000);
        QCOMPARE(appliedAtLevel, 0);
        // A modal widget shown (its exec() not even running): the request waits until it is gone.
        QDialog modal;
        modal.setModal(true);
        modal.show();
        QTRY_VERIFY(QApplication::activeModalWidget() != nullptr);
        queue.enqueue(requestFor({QStringLiteral("b.ld")}));
        QTest::qWait(3 * RequestQueue::kRetryMs);
        QCOMPARE(applied, 1);
        QCOMPARE(queue.pending(), 1);
        modal.close();
        QTRY_COMPARE_WITH_TIMEOUT(applied, 2, 10000);
        QCOMPARE(queue.pending(), 0);
    }

    // Regression (PR #5 review U12, D-217): a client that leaves before reading its answer (a launch whose
    // wait timed out opens its own window) never has its request applied by the server.
    void requestOfAClientThatLeftIsDropped() {
        const QString name = uniqueInstanceName();
        InstanceServer server;
        QVERIFY(server.listen(name));
        QSignalSpy received(&server, &InstanceServer::requestReceived);
        QSignalSpy refused(&server, &InstanceServer::clientRefused);
        const QByteArray line =
            requestFor({QDir::temp().absoluteFilePath(QStringLiteral("gone.ld"))}).toMessage();
#if defined(Q_OS_WIN)
        const std::wstring path = instanceAddress(name).toStdWString();
        HANDLE pipe =
            CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(pipe != INVALID_HANDLE_VALUE);
        DWORD written = 0;
        QVERIFY(WriteFile(pipe, line.constData(), static_cast<DWORD>(line.size()), &written, nullptr) != 0);
        CloseHandle(pipe); // gone before the server ran: its answer cannot reach it
#else
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        QVERIFY(fd >= 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        const QByteArray address = instanceAddress(name).toLocal8Bit();
        std::memcpy(a.sun_path, address.constData(), static_cast<size_t>(address.size()) + 1);
        QVERIFY(::connect(fd, reinterpret_cast<const sockaddr*>(&a), static_cast<socklen_t>(sizeof(a))) == 0);
        QVERIFY(::send(fd, line.constData(), static_cast<size_t>(line.size()), 0) == line.size());
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
#endif
        QTRY_VERIFY_WITH_TIMEOUT(server.accepted() + refused.count() > 0, 10000);
        QTest::qWait(200); // a request that were emitted would arrive by now
        QCOMPARE(received.count(), 0);
        QCOMPARE(server.accepted(),
                 1); // the line was a valid request: it was dropped, not refused as malformed
        // The server goes on: the next client is served.
        ClientThread client(
            name, requestFor({QDir::temp().absoluteFilePath(QStringLiteral("next.ld"))}).toMessage());
        QTRY_VERIFY_WITH_TIMEOUT(client.done(), 30000);
        QStringList reasons;
        for (const QList<QVariant>& r : refused) {
            reasons << r.at(0).toString();
        }
        QVERIFY2(client.join() == ForwardResult::Delivered,
                 qPrintable(client.detail() + QStringLiteral(" | refused: ") +
                            reasons.join(QStringLiteral("; ")) +
                            QStringLiteral(" | accepted %1").arg(server.accepted())));
        QTRY_COMPARE_WITH_TIMEOUT(received.count(), 1, 30000);
    }

    void nobodyListening() {
        const QString name = uniqueInstanceName();
        QString detail;
        QCOMPARE(forwardToInstance(name, requestFor({}).toMessage(), 5000, &detail),
                 ForwardResult::NoInstance);
    }

    // One listener per name; the name is free again once it closes (POSIX: its socket file goes with it).
    void oneListenerPerName() {
        const QString name = uniqueInstanceName();
        InstanceServer first;
        QString detail;
        QVERIFY2(first.listen(name, &detail), qPrintable(detail));
        QVERIFY(first.isListening());
        QCOMPARE(first.name(), name);
        InstanceServer second;
        QVERIFY(!second.listen(name, &detail));
        QVERIFY(!detail.isEmpty());
        QVERIFY(!second.isListening());
        first.close();
        QVERIFY(!first.isListening());
#if !defined(Q_OS_WIN)
        QVERIFY(!QFileInfo::exists(instanceAddress(name)));
#endif
        QVERIFY2(second.listen(name, &detail), qPrintable(detail));
    }

#if !defined(Q_OS_WIN)
    // A socket file left by an instance that ended without closing (nobody accepts on it) is replaced.
    void staleSocketFileIsReplaced() {
        const QString name = uniqueInstanceName();
        const QString address = instanceAddress(name);
        QVERIFY(!address.isEmpty());
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        QVERIFY(fd >= 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        const QByteArray bytes = address.toLocal8Bit();
        std::memcpy(a.sun_path, bytes.constData(), static_cast<size_t>(bytes.size()) + 1);
        QCOMPARE(::bind(fd, reinterpret_cast<const sockaddr*>(&a), static_cast<socklen_t>(sizeof(a))), 0);
        ::close(fd); // bound, never listened, not removed
        QVERIFY(QFileInfo::exists(address));
        QCOMPARE(forwardToInstance(name, requestFor({}).toMessage(), 5000), ForwardResult::NoInstance);
        InstanceServer server;
        QString detail;
        QVERIFY2(server.listen(name, &detail), qPrintable(detail));
        server.close();
        QVERIFY(!QFileInfo::exists(address));
    }

    // A launch that has bound its socket but not listened yet keeps it (D-219 (d)): another launch waits for
    // the election lock on "<socket file>.lock" instead of taking the file for a stale one and unlinking it,
    // which made both of them listen (Codex review of PR #5). The test is the first launch: it holds the lock
    // and binds.
    void aLaunchBetweenBindAndListenKeepsItsSocket() {
        const QString name = uniqueInstanceName();
        const QByteArray path = instanceAddress(name).toLocal8Bit();
        QVERIFY(!path.isEmpty());
        const QByteArray lockPath = path + ".lock";
        int lockFd = ::open(lockPath.constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        const auto cleanup = qScopeGuard([&] {
            if (fd >= 0) {
                ::close(fd);
            }
            if (lockFd >= 0) {
                ::close(lockFd);
            }
            ::unlink(path.constData());
            ::unlink(lockPath.constData());
        });
        QVERIFY(lockFd >= 0);
        QVERIFY(fd >= 0);
        QCOMPARE(::flock(lockFd, LOCK_EX | LOCK_NB), 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::memcpy(a.sun_path, path.constData(), static_cast<size_t>(path.size()) + 1);
        QCOMPARE(::bind(fd, reinterpret_cast<const sockaddr*>(&a), static_cast<socklen_t>(sizeof(a))), 0);
        struct stat bound = {};
        QCOMPARE(::lstat(path.constData(), &bound), 0);
        // Between the first launch's bind and listen the second neither listens nor touches its file.
        InstanceServer second;
        QString detail;
        QVERIFY(!second.listen(name, &detail));
        QVERIFY(!second.isListening());
        QVERIFY(!detail.isEmpty());
        struct stat now = {};
        QCOMPARE(::lstat(path.constData(), &now), 0);
        QCOMPARE(now.st_ino, bound.st_ino);
        // Once the first launch listens and lets the lock go, the second finds it listening and stays a
        // client.
        QCOMPARE(::listen(fd, 8), 0);
        ::close(lockFd);
        lockFd = -1;
        QVERIFY(!second.listen(name, &detail));
        QCOMPARE(detail, QStringLiteral("another instance listens"));
        QCOMPARE(::lstat(path.constData(), &now), 0);
        QCOMPARE(now.st_ino, bound.st_ino);
    }
#endif

    void requestReachesTheServer() {
        const QString name = uniqueInstanceName();
        InstanceServer server;
        QVERIFY(server.listen(name));
        std::vector<LaunchRequest> got;
        connect(&server, &InstanceServer::requestReceived, this,
                [&got](const LaunchRequest& r) { got.push_back(r); });
        const QString log = QDir::cleanPath(QDir::tempPath() + QStringLiteral("/a log.ld"));
        const QString project = QDir::cleanPath(QDir::tempPath() + QStringLiteral("/a project"));
        ClientThread client(name, requestFor({log}, project).toMessage());
        QTRY_VERIFY_WITH_TIMEOUT(client.done(), 30000);
        QCOMPARE(client.join(), ForwardResult::Delivered);
        QTRY_COMPARE(got.size(), std::size_t{1}); // delivered after the answer, on the next turn
        QCOMPARE(got[0].logs, QStringList{log});
        QCOMPARE(got[0].project, project);
        QCOMPARE(server.accepted(), 1);
        // An empty request (a bare `esses`): it only brings the window to the front.
        ClientThread empty(name, requestFor({}).toMessage());
        QTRY_VERIFY_WITH_TIMEOUT(empty.done(), 30000);
        QCOMPARE(empty.join(), ForwardResult::Delivered);
        QTRY_COMPARE(got.size(), std::size_t{2});
        QVERIFY(got[1].logs.isEmpty() && got[1].project.isEmpty());
    }

    // A malformed line is answered "error" (the launch then starts its own window); the server goes on.
    void malformedRequestIsRefused() {
        const QString name = uniqueInstanceName();
        InstanceServer server;
        QVERIFY(server.listen(name));
        QSignalSpy refused(&server, &InstanceServer::clientRefused);
        int received = 0;
        connect(&server, &InstanceServer::requestReceived, this,
                [&received](const LaunchRequest&) { ++received; });
        ClientThread bad(name, QByteArrayLiteral("{\"protocol\":1,\"logs\":[\"relative.ld\"]}\n"));
        QTRY_VERIFY_WITH_TIMEOUT(bad.done(), 30000);
        QCOMPARE(bad.join(), ForwardResult::Failed);
        QCOMPARE(refused.count(), 1);
        ClientThread good(name, requestFor({}).toMessage());
        QTRY_VERIFY_WITH_TIMEOUT(good.done(), 30000);
        QCOMPARE(good.join(), ForwardResult::Delivered);
        QTRY_COMPARE(received, 1);
    }

    void oversizedRequestIsRefused() {
        const QString name = uniqueInstanceName();
        InstanceServer server;
        QVERIFY(server.listen(name));
        QSignalSpy refused(&server, &InstanceServer::clientRefused);
        ClientThread big(name, QByteArray(kMaxMessageBytes + 4096, 'x'));
        QTRY_VERIFY_WITH_TIMEOUT(big.done(), 60000);
        QCOMPARE(big.join(), ForwardResult::Failed);
        QTRY_COMPARE(refused.count(), 1);
        QVERIFY(refused.at(0).at(0).toString().contains(QStringLiteral("longer")));
        ClientThread good(name, requestFor({}).toMessage());
        QTRY_VERIFY_WITH_TIMEOUT(good.done(), 30000);
        QCOMPARE(good.join(), ForwardResult::Delivered);
    }

    // A client that connects and sends nothing holds the server at most kClientTimeoutMs; the next one is
    // served.
    void silentClientIsDropped() {
        const QString name = uniqueInstanceName();
        InstanceServer server;
        QVERIFY(server.listen(name));
        QSignalSpy refused(&server, &InstanceServer::clientRefused);
        int received = 0;
        connect(&server, &InstanceServer::requestReceived, this,
                [&received](const LaunchRequest&) { ++received; });
        SilentClient silent(instanceAddress(name));
        QVERIFY(silent.connected());
        ClientThread next(name, requestFor({}).toMessage(), 60000);
        QTRY_COMPARE_WITH_TIMEOUT(refused.count(), 1, 60000);
        QVERIFY(refused.at(0).at(0).toString().contains(QStringLiteral("in time")));
        QTRY_VERIFY_WITH_TIMEOUT(next.done(), 60000);
        QCOMPARE(next.join(), ForwardResult::Delivered);
        QTRY_COMPARE(received, 1);
    }

    // A second esses hands its files to the running instance (this test) and exits 0 before it builds a
    // window. Its relative arguments name files of its working directory, however that folder is named: the
    // forwarded paths are the system's (namesTheFilesIn). Regression (PR #5, the macOS qt-app run of
    // fe6cf07): a comparison as text failed on macOS, where the temporary folder is under /var, a link to
    // /private/var; "through a link" starts the launch in a link to the folder on every OS.
    void secondLaunchHandsItsFilesOver_data() {
        QTest::addColumn<bool>("linked");
        QTest::newRow("in the folder") << false;
        QTest::newRow("through a link") << true;
    }
    void secondLaunchHandsItsFilesOver() {
        QFETCH(bool, linked);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString folder = dir.path() + QStringLiteral("/session");
        QVERIFY(QDir().mkpath(folder));
        const QString alias = dir.path() + QStringLiteral("/link");
        std::optional<FolderLink> named;
        if (linked) {
            named.emplace(folder, alias);
            QVERIFY2(named->made(), qPrintable(alias));
        }
        const QString name = uniqueInstanceName();
        InstanceServer server;
        QVERIFY(server.listen(name));
        std::vector<LaunchRequest> got;
        connect(&server, &InstanceServer::requestReceived, this,
                [&got](const LaunchRequest& r) { got.push_back(r); });
        QProcess child;
        const QString config = dir.path() + QStringLiteral("/config");
        child.setProcessEnvironment(childEnvironment(config, name));
        child.setWorkingDirectory(linked ? alias : folder);
        child.start(essesExe(),
                    {QStringLiteral("-l"), QStringLiteral("relative.ld"), QStringLiteral("bare one.csv")});
        QVERIFY2(child.waitForStarted(30000), qPrintable(child.errorString()));
        QTRY_COMPARE_WITH_TIMEOUT(got.size(), std::size_t{1}, kLong);
        QTRY_VERIFY_WITH_TIMEOUT(child.state() == QProcess::NotRunning, kLong);
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
        const QString problem = namesTheFilesIn(
            got[0].logs, {QStringLiteral("relative.ld"), QStringLiteral("bare one.csv")}, folder);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        QVERIFY(got[0].project.isEmpty());
        QVERIFY(!QFileInfo::exists(config + QStringLiteral("/settings.json"))); // no window, no settings
    }

    // --new-instance: the launch runs its own window although an instance listens (06 section 19 item 6).
    void newInstanceRunsItsOwnWindow() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString name = uniqueInstanceName();
        InstanceServer server;
        QVERIFY(server.listen(name));
        int received = 0;
        connect(&server, &InstanceServer::requestReceived, this,
                [&received](const LaunchRequest&) { ++received; });
        QProcess child;
        const QString config = dir.path() + QStringLiteral("/config");
        child.setProcessEnvironment(childEnvironment(config, name));
        child.setWorkingDirectory(dir.path());
        const QString log = QDir::cleanPath(dir.path()) + QStringLiteral("/own window.csv");
        child.start(essesExe(), {QStringLiteral("--new-instance"), QStringLiteral("-l"), log});
        QVERIFY2(child.waitForStarted(30000), qPrintable(child.errorString()));
        // Its window opened the log: the recent-files list in its own settings names it.
        QTRY_VERIFY_WITH_TIMEOUT(settingsText(config).contains(QStringLiteral("own window.csv")), kLong);
        QCOMPARE(received, 0); // it would have forwarded before building the window
        QVERIFY(child.state() != QProcess::NotRunning);
        stopChild(child);
    }

    // The running window opens what another launch forwards (06 section 19 item 6), in this process.
    void forwardedFilesOpenInTheRunningWindow() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString name = uniqueInstanceName();
        std::unique_ptr<qt::AppRuntime> rt = runtime(2);
        {
            widgets::MainWindow window(*rt, pathsFor(dir.path() + QStringLiteral("/window")));
            window.setSaveLayoutOnExit(false);
            QVERIFY(showAndActivate(window));
            InstanceServer server;
            QVERIFY(server.listen(name));
            connect(&server, &InstanceServer::requestReceived, &window, [&window](const LaunchRequest& r) {
                openRequestedFiles(window, r, LaunchKind::Forwarded);
                bringToFront(window);
            });
            QSignalSpy opened(&window, &widgets::MainWindow::sourceOpened);
            const QString csv = repoRoot() + QStringLiteral("/fixtures/generic.csv/sparse-gps/input.csv");
            QVERIFY(QFileInfo::exists(csv));
            QProcess child;
            child.setProcessEnvironment(childEnvironment(dir.path() + QStringLiteral("/child"), name));
            child.start(essesExe(), {csv});
            QVERIFY2(child.waitForStarted(30000), qPrintable(child.errorString()));
            QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, kLong);
            QCOMPARE(QDir::cleanPath(opened.at(0).at(0).toString()), QDir::cleanPath(csv));
            QTRY_VERIFY_WITH_TIMEOUT(child.state() == QProcess::NotRunning, kLong);
            QCOMPARE(child.exitCode(), 0);
            QTRY_COMPARE_WITH_TIMEOUT(window.loader().sources().size(), std::size_t{1}, kLong); // published
            QVERIFY(window.isVisible());
            server.close();
        }
        rt->shutdown();
    }

    // Two real esses: the first listens, the second forwards to it and exits, the first opens the file.
    void twoEssesProcesses() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString name = uniqueInstanceName();
        const QString config = dir.path() + QStringLiteral("/config");
        QProcess first;
        first.setProcessEnvironment(childEnvironment(config, name));
        first.start(essesExe(),
                    {QStringLiteral("-l"), QDir::cleanPath(dir.path()) + QStringLiteral("/first.csv")});
        QVERIFY2(first.waitForStarted(30000), qPrintable(first.errorString()));
        // Up once its channel answers (an empty request only raises its window).
        QTRY_VERIFY_WITH_TIMEOUT(
            forwardToInstance(name, requestFor({}).toMessage(), 2000) == ForwardResult::Delivered, kLong);
        QProcess second;
        second.setProcessEnvironment(childEnvironment(dir.path() + QStringLiteral("/other"), name));
        second.start(essesExe(),
                     {QStringLiteral("-l"), QDir::cleanPath(dir.path()) + QStringLiteral("/second.csv")});
        QVERIFY2(second.waitForStarted(30000), qPrintable(second.errorString()));
        QTRY_VERIFY_WITH_TIMEOUT(second.state() == QProcess::NotRunning, kLong);
        QCOMPARE(second.exitCode(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(settingsText(config).contains(QStringLiteral("second.csv")), kLong);
        QVERIFY(first.state() != QProcess::NotRunning);
        stopChild(first);
    }
};

QTEST_MAIN(TestInstanceChannel)
#include "tst_instance_channel.moc"
