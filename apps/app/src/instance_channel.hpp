// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Single-instance forwarding (06 section 18 step 1, section 19 item 6; D-113 as built by D-212). A launch
// that finds a running Esses of the same user and settings folder hands it its files and exits 0; the running
// one opens them and comes to the front. D-113 names QLocalSocket, which is Qt Network, a module the
// application does not link (01 section 2.2 rule 4, tools/check_layering.py); the channel is built on the
// operating system's own local IPC with Qt Core's notifiers, so it needs no thread and no new Qt module:
//   Windows  a named pipe \\.\pipe\<name>, one instance (FILE_FLAG_FIRST_PIPE_INSTANCE), local clients only,
//            its DACL this user's alone (D-217), overlapped I/O watched by QWinEventNotifier. The client
//            opens it at the identification level (the server cannot impersonate it); each side checks that
//            the other process runs as the same user (GetNamedPipeServerProcessId /
//            GetNamedPipeClientProcessId, EqualSid), and the client lets the server take the foreground
//            (AllowSetForegroundWindow).
//   POSIX    a Unix-domain stream socket <dir>/<name>.sock in a private directory: $XDG_RUNTIME_DIR, else
//            $TMPDIR, else /tmp/esses-<uid> (created 0700); a directory is used only when it is ours, not a
//            symbolic link and closed to group and others. The peer's uid is checked (SO_PEERCRED /
//            getpeereid). A socket file left by a crashed instance (connection refused) is replaced. Connects
//            are non-blocking and bounded by the caller's deadline (a full backlog of a busy instance never
//            hangs a launch; D-217).
// The name: "esses-<user>-<12 hex digits of SHA-256 of the settings folder and its salt>", the user being the
// SID and the session on Windows, the uid on POSIX, the salt 16 random bytes kept in the settings folder
// (instance.salt, made on first use; D-217), so that another local user cannot derive the name and take it
// first; a portable copy (its own settings folder) and the installed program, or a test run with
// ESSES_CONFIG_DIR, never forward to each other. ESSES_INSTANCE_NAME replaces the name (tests). The protocol:
// the client sends one line (LaunchRequest::toMessage, at most kMaxMessageBytes) and reads one line back,
// "ok" when the server accepted the request, "error" otherwise. The server emits requestReceived only once
// the "ok" has reached the client (a launch that gave up waiting opens its own window, and the files never
// open in both; D-217). The server drops a client that sends nothing complete within kClientTimeoutMs; it
// serves clients one after another.
#pragma once

#include "launch_request.hpp"

#include <QObject>
#include <QString>
#include <filesystem>
#include <memory>
#include <optional>

namespace esses::app {

inline constexpr int kMaxMessageBytes = 1 << 20;
inline constexpr int kClientTimeoutMs = 5000;
// How long a launch waits for the running instance's answer: covers an instance still building its window
// (it answers once its event loop runs).
inline constexpr int kForwardTimeoutMs = 20000;

// The channel name for this user and settings folder (see above).
QString instanceName(const std::filesystem::path& configDir);
// Where the channel `name` lives: the pipe (\\.\pipe\<name>) on Windows, the socket file on POSIX (empty when
// no private directory is available). For diagnostics and tests.
QString instanceAddress(const QString& name);

enum class ForwardResult { Delivered, NoInstance, Failed };

// Sends `message` (one line ending in "\n") to the instance listening on `name` and waits up to `timeoutMs`
// for its answer. NoInstance when nobody listens; Failed for a refused, foreign, silent or broken instance
// (with the reason in `detail`).
ForwardResult forwardToInstance(const QString& name, const QByteArray& message, int timeoutMs,
                                QString* detail = nullptr);

class InstanceServer final : public QObject {
    Q_OBJECT
public:
    explicit InstanceServer(QObject* parent = nullptr);
    ~InstanceServer() override;
    InstanceServer(const InstanceServer&) = delete;
    InstanceServer& operator=(const InstanceServer&) = delete;

    // Starts listening on `name`; false when another process holds it (or the platform refused, see
    // `detail`).
    bool listen(const QString& name, QString* detail = nullptr);
    bool isListening() const;
    QString name() const;
    // Stops listening and drops a client in progress (the destructor does it too).
    void close();
    // Requests accepted so far.
    int accepted() const noexcept { return accepted_; }

Q_SIGNALS:
    // A request a client sent, once its "ok" has reached the client (D-217).
    void requestReceived(const esses::app::LaunchRequest& request);
    // A client was refused (malformed line, too long, another user, timeout); for diagnostics and tests.
    void clientRefused(const QString& reason);

private:
    struct Impl;
    friend struct Impl;
    // A complete line from a client: the answer to send (an accepted request waits in pending_).
    QByteArray handleLine(const QByteArray& line);
    // The answer was written to the client (`delivered`) or not: the pending request is emitted or dropped.
    void answered(bool delivered);
    void refuse(const QString& reason);

    std::optional<LaunchRequest> pending_; // before d_: the Impl may still use it while it goes
    std::unique_ptr<Impl> d_;
    int accepted_ = 0;
};

} // namespace esses::app
