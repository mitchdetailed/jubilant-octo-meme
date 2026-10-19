// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Single-instance forwarding over the operating system's local IPC (instance_channel.hpp; D-113, D-212).

#include "instance_channel.hpp"

#include <esses/base/utf8.hpp>

#include <QByteArray>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QTimer>
#include <QtGlobal>
#include <algorithm>
#include <climits>
#include <cstdint>
#include <string>
#include <vector>

#if defined(Q_OS_WIN)
#include <QWinEventNotifier>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <QSocketNotifier>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <poll.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace esses::app {

namespace {

// Letters, digits, '-', '_' and '.' (ASCII) of `raw`, others as '_', at most 96 characters: a valid pipe or
// socket file name whatever ESSES_INSTANCE_NAME holds.
QString sanitizedName(const QString& raw) {
    QString out;
    for (const QChar c : raw) {
        const bool plain = c.unicode() < 128 && (c.isLetterOrNumber() || c == QLatin1Char('-') ||
                                                 c == QLatin1Char('_') || c == QLatin1Char('.'));
        out += plain ? c : QLatin1Char('_');
    }
    return out.left(96);
}

void setDetail(QString* detail, const QString& text) {
    if (detail != nullptr) {
        *detail = text;
    }
}

int remainingMs(const QDeadlineTimer& deadline) {
    return static_cast<int>(std::clamp<qint64>(deadline.remainingTime(), 0, INT_MAX));
}

#if defined(Q_OS_WIN)

QString win32Error(const char* call, DWORD error) {
    return QStringLiteral("%1 failed (Win32 error %2)").arg(QString::fromLatin1(call)).arg(error);
}

// TOKEN_USER of a process (a buffer aligned for it; empty on failure).
std::vector<std::uint64_t> tokenUserOf(HANDLE process) {
    HANDLE token = nullptr;
    if (OpenProcessToken(process, TOKEN_QUERY, &token) == 0) {
        return {};
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::uint64_t> buffer((size + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t));
    const bool ok = size != 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size) != 0;
    CloseHandle(token);
    if (!ok) {
        return {};
    }
    return buffer;
}

PSID sidOf(std::vector<std::uint64_t>& tokenUser) {
    void* raw = tokenUser.data();
    return static_cast<TOKEN_USER*>(raw)->User.Sid;
}

// Whether process `pid` runs as the user of this process.
bool sameUser(DWORD pid) {
    std::vector<std::uint64_t> ours = tokenUserOf(GetCurrentProcess());
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr || ours.empty()) {
        if (process != nullptr) {
            CloseHandle(process);
        }
        return false;
    }
    std::vector<std::uint64_t> theirs = tokenUserOf(process);
    CloseHandle(process);
    return !theirs.empty() && EqualSid(sidOf(ours), sidOf(theirs)) != 0;
}

// "S-1-<authority>-<sub authority>...", the string form of a SID (what ConvertSidToStringSid gives).
QString sidText(PSID sid) {
    if (IsValidSid(sid) == 0) {
        return QStringLiteral("user");
    }
    const SID_IDENTIFIER_AUTHORITY* authority = GetSidIdentifierAuthority(sid);
    quint64 value = 0;
    for (const BYTE b : authority->Value) {
        value = (value << 8) | b;
    }
    QString text = QStringLiteral("S-1-") + QString::number(value);
    const UCHAR count = *GetSidSubAuthorityCount(sid);
    for (UCHAR i = 0; i < count; ++i) {
        text += QLatin1Char('-') + QString::number(*GetSidSubAuthority(sid, i));
    }
    return text;
}

QString userPart() {
    std::vector<std::uint64_t> ours = tokenUserOf(GetCurrentProcess());
    QString sid = QStringLiteral("user");
    if (!ours.empty()) {
        sid = sidText(sidOf(ours));
    }
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return sid + QStringLiteral("-s") + QString::number(session);
}

std::wstring pipePath(const QString& name) {
    return (QStringLiteral("\\\\.\\pipe\\") + name).toStdWString();
}

#else // POSIX

QString userPart() {
    return QString::number(static_cast<qulonglong>(::getuid()));
}

// A directory of ours, not a symbolic link, closed to group and others.
bool privateDirectory(const std::string& dir) {
    struct stat st {};
    return ::lstat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == ::getuid() &&
           (st.st_mode & 077) == 0;
}

std::string socketDirectory() {
    for (const char* variable : {"XDG_RUNTIME_DIR", "TMPDIR"}) {
        const QByteArray value = qgetenv(variable);
        if (value.startsWith('/')) {
            std::string dir = value.toStdString();
            while (dir.size() > 1 && dir.back() == '/') {
                dir.pop_back();
            }
            if (privateDirectory(dir)) {
                return dir;
            }
        }
    }
    const std::string fallback = "/tmp/esses-" + std::to_string(static_cast<unsigned long long>(::getuid()));
    if (::mkdir(fallback.c_str(), 0700) != 0 && errno != EEXIST) {
        return {};
    }
    return privateDirectory(fallback) ? fallback : std::string();
}

bool socketAddress(const std::string& path, sockaddr_un& address) {
    address = sockaddr_un{};
    address.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(address.sun_path)) {
        return false;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return true;
}

std::string socketPath(const QString& name) {
    const std::string dir = socketDirectory();
    return dir.empty() ? std::string() : dir + "/" + name.toStdString() + ".sock";
}

void closeOnExec(int fd, bool nonBlocking) {
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (nonBlocking) {
        const int flags = ::fcntl(fd, F_GETFL);
        if (flags >= 0) {
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, static_cast<socklen_t>(sizeof(one)));
#endif
}

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// Whether the process at the other end of a connected socket runs as our user.
bool peerIsUs(int fd) {
#if defined(__APPLE__) || defined(__FreeBSD__)
    uid_t uid = 0;
    gid_t gid = 0;
    return ::getpeereid(fd, &uid, &gid) == 0 && uid == ::getuid();
#elif defined(SO_PEERCRED)
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    return ::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0 &&
           credentials.uid == ::getuid();
#else
    (void)fd;
    return true; // the private directory is the guard
#endif
}

int connectTo(const std::string& path, int& error) {
    sockaddr_un address{};
    if (!socketAddress(path, address)) {
        error = ENAMETOOLONG;
        return -1;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        error = errno;
        return -1;
    }
    closeOnExec(fd, false);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), static_cast<socklen_t>(sizeof(address))) !=
        0) {
        error = errno;
        ::close(fd);
        return -1;
    }
    error = 0;
    return fd;
}

#endif

} // namespace

QString instanceName(const std::filesystem::path& configDir) {
    const QString forced = qEnvironmentVariable("ESSES_INSTANCE_NAME").trimmed();
    if (!forced.isEmpty()) {
        return sanitizedName(forced);
    }
    QString folder = QDir::cleanPath(QString::fromStdString(pathToUtf8(configDir)));
#if defined(Q_OS_WIN)
    folder = folder.toLower(); // one settings folder, however it is spelled
#endif
    const QByteArray digest =
        QCryptographicHash::hash(folder.toUtf8(), QCryptographicHash::Sha256).toHex().left(12);
    return sanitizedName(QStringLiteral("esses-") + userPart() + QLatin1Char('-') +
                         QString::fromLatin1(digest));
}

QString instanceAddress(const QString& name) {
#if defined(Q_OS_WIN)
    return QString::fromStdWString(pipePath(name));
#else
    return QString::fromStdString(socketPath(name));
#endif
}

// ---- the server -------------------------------------------------------------------------------------------

#if defined(Q_OS_WIN)

struct InstanceServer::Impl {
    enum class Phase { Idle, Connecting, Reading, Writing, Draining };

    explicit Impl(InstanceServer& owner) : q(owner) {
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &q, [this] {
            if (phase == Phase::Reading) {
                q.refuse(QStringLiteral("the client sent no complete request in time"));
            }
            restart();
        });
    }
    ~Impl() { shut(); }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    bool open(const QString& n, QString* detail) {
        name = n;
        const std::wstring path = pipePath(n);
        pipe = CreateNamedPipeW(path.c_str(),
                                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                1, 4096, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            setDetail(detail, win32Error("CreateNamedPipe", GetLastError()));
            return false;
        }
        event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event == nullptr) {
            setDetail(detail, win32Error("CreateEvent", GetLastError()));
            shut();
            return false;
        }
        notifier = new QWinEventNotifier(event, &q);
        QObject::connect(notifier, &QWinEventNotifier::activated, &q, [this] { signalled(); });
        if (!connectNext()) {
            setDetail(detail, win32Error("ConnectNamedPipe", GetLastError()));
            shut();
            return false;
        }
        return true;
    }

    // Waits for the next client.
    bool connectNext() {
        buffer.clear();
        reply.clear();
        arm();
        phase = Phase::Connecting;
        if (ConnectNamedPipe(pipe, &ov) != 0) {
            pending = true; // completed at once: the event is signalled
            return true;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            pending = true;
            return true;
        }
        if (error == ERROR_PIPE_CONNECTED) { // a client came between CreateNamedPipe/Disconnect and now
            pending = false;
            SetEvent(event);
            return true;
        }
        pending = false;
        return false;
    }

    void arm() {
        ResetEvent(event);
        ov = OVERLAPPED{};
        ov.hEvent = event;
    }

    void read() {
        arm();
        if (ReadFile(pipe, chunk, static_cast<DWORD>(sizeof(chunk)), nullptr, &ov) != 0 ||
            GetLastError() == ERROR_IO_PENDING) {
            pending = true;
            return;
        }
        pending = false;
        restart();
    }

    void write() {
        arm();
        if (WriteFile(pipe, reply.constData(), static_cast<DWORD>(reply.size()), nullptr, &ov) != 0 ||
            GetLastError() == ERROR_IO_PENDING) {
            pending = true;
            return;
        }
        pending = false;
        restart();
    }

    void signalled() {
        if (pipe == INVALID_HANDLE_VALUE) {
            return;
        }
        DWORD n = 0;
        bool ok = true;
        if (pending) {
            if (GetOverlappedResult(pipe, &ov, &n, FALSE) == 0) {
                if (GetLastError() == ERROR_IO_INCOMPLETE) {
                    return;
                }
                ok = false;
            }
            pending = false;
        }
        switch (phase) {
        case Phase::Connecting: {
            ULONG pid = 0;
            if (!ok || GetNamedPipeClientProcessId(pipe, &pid) == 0 || !sameUser(pid)) {
                if (ok) {
                    q.refuse(QStringLiteral("a client that runs as another user"));
                }
                restart();
                return;
            }
            phase = Phase::Reading;
            timer.start(kClientTimeoutMs);
            read();
            return;
        }
        case Phase::Reading: {
            if (!ok || n == 0) {
                restart(); // the client went away
                return;
            }
            buffer.append(chunk, static_cast<qsizetype>(n));
            const qsizetype newline = buffer.indexOf('\n');
            if (newline >= 0) {
                timer.stop();
                reply = q.handleLine(buffer.left(newline));
                phase = Phase::Writing;
                write();
                return;
            }
            if (buffer.size() > kMaxMessageBytes) {
                q.refuse(QStringLiteral("a request longer than %1 bytes").arg(kMaxMessageBytes));
                restart();
                return;
            }
            read();
            return;
        }
        case Phase::Writing:
            // Disconnecting now would discard the answer before the client reads it (DisconnectNamedPipe), so
            // wait until the client closes its end (the read fails), at most kClientTimeoutMs.
            phase = Phase::Draining;
            timer.start(kClientTimeoutMs);
            read();
            return;
        case Phase::Draining:
            if (ok && n > 0) {
                read(); // anything more is ignored
                return;
            }
            restart();
            return;
        case Phase::Idle:
            return;
        }
    }

    void cancelPending() {
        timer.stop();
        if (pending && pipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(pipe, &ov);
            DWORD n = 0;
            GetOverlappedResult(pipe, &ov, &n, TRUE);
        }
        pending = false;
    }

    // The current client is done (or failed): the next one.
    void restart() {
        cancelPending();
        if (pipe == INVALID_HANDLE_VALUE) {
            return;
        }
        DisconnectNamedPipe(pipe);
        if (!connectNext()) {
            q.refuse(win32Error("ConnectNamedPipe", GetLastError()) +
                     QStringLiteral("; no longer listening"));
            shut();
        }
    }

    void shut() {
        cancelPending();
        if (notifier != nullptr) {
            notifier->setEnabled(false);
            notifier->deleteLater(); // may be inside its own activated()
            notifier = nullptr;
        }
        if (pipe != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
        if (event != nullptr) {
            CloseHandle(event);
            event = nullptr;
        }
        phase = Phase::Idle;
    }

    bool listening() const { return pipe != INVALID_HANDLE_VALUE; }

    InstanceServer& q;
    QString name;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    OVERLAPPED ov{};
    QWinEventNotifier* notifier = nullptr;
    Phase phase = Phase::Idle;
    bool pending = false; // an overlapped operation on `ov` is in flight
    QByteArray buffer;
    QByteArray reply;
    char chunk[4096] = {};
    QTimer timer;
};

ForwardResult forwardToInstance(const QString& name, const QByteArray& message, int timeoutMs,
                                QString* detail) {
    const std::wstring path = pipePath(name);
    const QDeadlineTimer deadline(timeoutMs);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (;;) {
        // SECURITY_IDENTIFICATION: the server may learn who we are, never act as us.
        pipe = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            break;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            return ForwardResult::NoInstance;
        }
        if (error != ERROR_PIPE_BUSY || deadline.hasExpired()) {
            setDetail(detail, win32Error("CreateFile", error));
            return ForwardResult::Failed;
        }
        WaitNamedPipeW(path.c_str(),
                       static_cast<DWORD>(std::max(1, remainingMs(deadline)))); // the busy instance
    }
    ULONG pid = 0;
    if (GetNamedPipeServerProcessId(pipe, &pid) == 0 || !sameUser(pid)) {
        CloseHandle(pipe);
        setDetail(detail, QStringLiteral("the instance runs as another user"));
        return ForwardResult::Failed;
    }
    AllowSetForegroundWindow(pid); // the running instance may come to the front for the user's click
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        CloseHandle(pipe);
        setDetail(detail, win32Error("CreateEvent", GetLastError()));
        return ForwardResult::Failed;
    }
    // One overlapped transfer, waited for until the deadline.
    const auto transfer = [&](bool toServer, char* data, DWORD size, DWORD& done) {
        OVERLAPPED ov{};
        ov.hEvent = event;
        ResetEvent(event);
        const BOOL started =
            toServer ? WriteFile(pipe, data, size, nullptr, &ov) : ReadFile(pipe, data, size, nullptr, &ov);
        if (started == 0 && GetLastError() != ERROR_IO_PENDING) {
            return false;
        }
        if (WaitForSingleObject(event, static_cast<DWORD>(remainingMs(deadline))) != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &ov);
            GetOverlappedResult(pipe, &ov, &done, TRUE);
            return false;
        }
        return GetOverlappedResult(pipe, &ov, &done, FALSE) != 0;
    };
    QByteArray out = message;
    bool ok = true;
    for (qsizetype sent = 0; ok && sent < out.size();) {
        DWORD done = 0;
        ok = transfer(true, out.data() + sent, static_cast<DWORD>(out.size() - sent), done) && done > 0;
        sent += static_cast<qsizetype>(done);
    }
    QByteArray answer;
    while (ok && !answer.contains('\n') && answer.size() < 64) {
        char buffer[64] = {};
        DWORD done = 0;
        ok = transfer(false, buffer, static_cast<DWORD>(sizeof(buffer)), done) && done > 0;
        answer.append(buffer, static_cast<qsizetype>(done));
    }
    CloseHandle(event);
    CloseHandle(pipe);
    if (answer.startsWith("ok\n")) {
        return ForwardResult::Delivered;
    }
    setDetail(detail, ok ? QStringLiteral("the instance refused the request")
                         : QStringLiteral("no answer from the instance"));
    return ForwardResult::Failed;
}

#else // POSIX

struct InstanceServer::Impl {
    explicit Impl(InstanceServer& owner) : q(owner) {
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &q, [this] {
            if (client >= 0) {
                q.refuse(QStringLiteral("the client sent no complete request in time"));
                dropClient();
            }
        });
    }
    ~Impl() { shut(); }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    bool open(const QString& n, QString* detail) {
        name = n;
        path = socketPath(n);
        sockaddr_un address{};
        if (path.empty() || !socketAddress(path, address)) {
            setDetail(detail,
                      QStringLiteral("no private directory for the socket (or its path is too long)"));
            return false;
        }
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            setDetail(detail, QStringLiteral("socket: ") + QString::fromLocal8Bit(std::strerror(errno)));
            return false;
        }
        closeOnExec(fd, true);
        const auto bindIt = [&] {
            return ::bind(fd, reinterpret_cast<const sockaddr*>(&address),
                          static_cast<socklen_t>(sizeof(address))) == 0;
        };
        if (!bindIt()) {
            if (errno != EADDRINUSE) {
                setDetail(detail, QStringLiteral("bind: ") + QString::fromLocal8Bit(std::strerror(errno)));
                shut();
                return false;
            }
            int error = 0;
            const int probe = connectTo(path, error);
            if (probe >= 0) {
                ::close(probe);
                setDetail(detail, QStringLiteral("another instance listens"));
                shut();
                return false;
            }
            if (error != ECONNREFUSED && error != ENOENT) {
                setDetail(detail, QStringLiteral("connect: ") + QString::fromLocal8Bit(std::strerror(error)));
                shut();
                return false;
            }
            ::unlink(path.c_str()); // left by an instance that ended without closing
            if (!bindIt()) {
                setDetail(detail, QStringLiteral("bind: ") + QString::fromLocal8Bit(std::strerror(errno)));
                shut();
                return false;
            }
        }
        if (::listen(fd, 8) != 0) {
            setDetail(detail, QStringLiteral("listen: ") + QString::fromLocal8Bit(std::strerror(errno)));
            ::unlink(path.c_str());
            shut();
            return false;
        }
        struct stat st {};
        if (::lstat(path.c_str(), &st) == 0) {
            device = st.st_dev;
            inode = st.st_ino;
            owned = true;
        }
        acceptor = new QSocketNotifier(fd, QSocketNotifier::Read, &q);
        QObject::connect(acceptor, &QSocketNotifier::activated, &q, [this] { accept(); });
        return true;
    }

    void accept() {
        if (client >= 0) {
            return;
        }
        const int c = ::accept(fd, nullptr, nullptr);
        if (c < 0) {
            return; // EAGAIN: nobody after all
        }
        closeOnExec(c, true);
        if (!peerIsUs(c)) {
            ::close(c);
            q.refuse(QStringLiteral("a client that runs as another user"));
            return;
        }
        client = c;
        buffer.clear();
        acceptor->setEnabled(false); // one client at a time; the others wait in the backlog
        reader = new QSocketNotifier(c, QSocketNotifier::Read, &q);
        QObject::connect(reader, &QSocketNotifier::activated, &q, [this] { readable(); });
        timer.start(kClientTimeoutMs);
    }

    void readable() {
        char chunk[4096];
        for (;;) {
            const ssize_t n = ::recv(client, chunk, sizeof(chunk), 0);
            if (n > 0) {
                buffer.append(chunk, static_cast<qsizetype>(n));
                const qsizetype newline = buffer.indexOf('\n');
                if (newline >= 0) {
                    const QByteArray answer = q.handleLine(buffer.left(newline));
                    // A few bytes into an empty socket buffer: one send (the client reads them after we
                    // close).
                    (void)::send(client, answer.constData(), static_cast<size_t>(answer.size()), kSendFlags);
                    dropClient();
                    return;
                }
                if (buffer.size() > kMaxMessageBytes) {
                    q.refuse(QStringLiteral("a request longer than %1 bytes").arg(kMaxMessageBytes));
                    dropClient();
                    return;
                }
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            dropClient(); // closed, or an error
            return;
        }
    }

    void dropClient() {
        timer.stop();
        if (reader != nullptr) {
            reader->setEnabled(false);
            reader->deleteLater(); // may be inside its own activated()
            reader = nullptr;
        }
        if (client >= 0) {
            ::close(client);
            client = -1;
        }
        if (acceptor != nullptr) {
            acceptor->setEnabled(true);
        }
    }

    void shut() {
        dropClient();
        if (acceptor != nullptr) {
            acceptor->setEnabled(false);
            acceptor->deleteLater();
            acceptor = nullptr;
        }
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
        if (owned) { // remove our socket file, never one another instance put there since
            struct stat st {};
            if (::lstat(path.c_str(), &st) == 0 && st.st_dev == device && st.st_ino == inode) {
                ::unlink(path.c_str());
            }
            owned = false;
        }
    }

    bool listening() const { return fd >= 0; }

    InstanceServer& q;
    QString name;
    std::string path;
    int fd = -1;
    int client = -1;
    bool owned = false;
    dev_t device{};
    ino_t inode{};
    QSocketNotifier* acceptor = nullptr;
    QSocketNotifier* reader = nullptr;
    QByteArray buffer;
    QTimer timer;
};

ForwardResult forwardToInstance(const QString& name, const QByteArray& message, int timeoutMs,
                                QString* detail) {
    const std::string path = socketPath(name);
    if (path.empty()) {
        return ForwardResult::NoInstance; // no private directory: no instance can listen either
    }
    const QDeadlineTimer deadline(timeoutMs);
    int error = 0;
    const int fd = connectTo(path, error);
    if (fd < 0) {
        if (error == ENOENT || error == ECONNREFUSED) {
            return ForwardResult::NoInstance;
        }
        setDetail(detail, QStringLiteral("connect: ") + QString::fromLocal8Bit(std::strerror(error)));
        return ForwardResult::Failed;
    }
    if (!peerIsUs(fd)) {
        ::close(fd);
        setDetail(detail, QStringLiteral("the instance runs as another user"));
        return ForwardResult::Failed;
    }
    bool ok = true;
    for (qsizetype sent = 0; ok && sent < message.size();) {
        pollfd p{fd, POLLOUT, 0};
        if (::poll(&p, 1, remainingMs(deadline)) <= 0) {
            ok = errno == EINTR && !deadline.hasExpired();
            continue;
        }
        const ssize_t n =
            ::send(fd, message.constData() + sent, static_cast<size_t>(message.size() - sent), kSendFlags);
        if (n < 0) {
            ok = errno == EINTR || errno == EAGAIN;
            continue;
        }
        sent += static_cast<qsizetype>(n);
    }
    QByteArray answer;
    while (ok && !answer.contains('\n') && answer.size() < 64) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, remainingMs(deadline)) <= 0) {
            ok = errno == EINTR && !deadline.hasExpired();
            continue;
        }
        char buffer[64];
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n < 0) {
            ok = errno == EINTR || errno == EAGAIN;
            continue;
        }
        if (n == 0) {
            break;
        }
        answer.append(buffer, static_cast<qsizetype>(n));
    }
    ::close(fd);
    if (answer.startsWith("ok\n")) {
        return ForwardResult::Delivered;
    }
    setDetail(detail, ok ? QStringLiteral("the instance refused the request")
                         : QStringLiteral("no answer from the instance"));
    return ForwardResult::Failed;
}

#endif

InstanceServer::InstanceServer(QObject* parent) : QObject(parent), d_(std::make_unique<Impl>(*this)) {}

InstanceServer::~InstanceServer() = default;

bool InstanceServer::listen(const QString& name, QString* detail) {
    close();
    return d_->open(name, detail);
}

bool InstanceServer::isListening() const {
    return d_->listening();
}

QString InstanceServer::name() const {
    return d_->listening() ? d_->name : QString();
}

void InstanceServer::close() {
    d_->shut();
}

QByteArray InstanceServer::handleLine(const QByteArray& line) {
    QString why;
    std::optional<LaunchRequest> request = LaunchRequest::fromMessage(line, &why);
    if (!request) {
        refuse(QStringLiteral("a malformed request: ") + why);
        return QByteArrayLiteral("error\n");
    }
    ++accepted_;
    // Delivered after the answer is on its way: opening a project may ask a question (a nested event loop).
    QTimer::singleShot(0, this, [this, r = std::move(*request)] { Q_EMIT requestReceived(r); });
    return QByteArrayLiteral("ok\n");
}

void InstanceServer::refuse(const QString& reason) {
    Q_EMIT clientRefused(reason);
}

} // namespace esses::app
