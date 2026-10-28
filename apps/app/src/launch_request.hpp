// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The command line of esses (06 section 19; D-113, D-212):
//   esses [-w <project folder>] [-l <log file>]... [--new-instance] [--scheme <name>] [--keymap <name|file>]
//         [--offscreen] [--dump-commands] [--version] [--help] [<file>...]
// CommandLine::parse() reads it into a LaunchRequest without exiting (QCommandLineParser::process would), so
// tests drive it with any argument list and working directory. Paths become absolute against the working
// directory of the launch (06 section 19 item 2), because a request may be forwarded to an instance started
// elsewhere. A bare argument is the project when it is a folder holding project.json or a project.json itself
// (the first such argument, unless -w gave one), otherwise a log (item 3).
// A launch that forwards (shouldForward) sends only its files (D-113: "-w/-l"); --scheme and --keymap are
// options of an instance's start. The forwarded message is one line of JSON:
//   {"protocol": 1, "project": "<absolute folder or project.json, or empty>",
//    "logs": ["<absolute path>", ...]}
// fromMessage() accepts exactly that (unknown members ignored) and refuses anything else, so a running
// instance never opens a relative path or a malformed list.
#pragma once

#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QString>
#include <QStringList>
#include <optional>

namespace esses::app {

struct LaunchRequest {
    QString project;          // -w/--workspace or a bare project argument; absolute; empty: none
    QStringList logs;         // -l/--log and the bare files, in order; absolute
    bool newInstance = false; // --new-instance
    QString scheme;           // --scheme <name>
    QString keymap;           // --keymap <name|file> (a file is made absolute)
    bool offscreen = false;   // --offscreen
    bool dumpCommands = false;
    bool version = false;

    static constexpr int kProtocol = 1;
    static constexpr int kMaxLogs = 4096;
    // The forwarded form (project and logs only), one line of compact JSON ending in "\n".
    QByteArray toMessage() const;
    // A forwarded line (with or without its "\n"); nullopt, with the reason in `error`, unless it is a
    // protocol-1 object whose project is empty or absolute and whose logs are at most kMaxLogs absolute
    // paths.
    static std::optional<LaunchRequest> fromMessage(const QByteArray& line, QString* error = nullptr);
};

// Whether this launch hands its files to a running instance (06 section 18 step 1): not with --new-instance,
// and never for a run that only prints (--version, --help, --dump-commands).
bool shouldForward(const LaunchRequest& request);

// `path` made absolute against `workingDir` and cleaned (QDir::cleanPath); empty stays empty.
QString absoluteFrom(const QString& path, const QString& workingDir);
// 06 section 19 item 3: a folder that holds project.json, or a file named project.json.
bool isProjectArgument(const QString& absolutePath);

class CommandLine {
public:
    CommandLine();

    enum class Outcome { Run, Help, Version, Error };
    // `arguments` as QCoreApplication::arguments() gives them (the program first).
    Outcome parse(const QStringList& arguments, const QString& workingDir);

    const LaunchRequest& request() const noexcept { return request_; }
    QString errorText() const { return error_; }
    QString helpText() const { return parser_.helpText(); }

private:
    QCommandLineParser parser_;
    QCommandLineOption version_;
    QCommandLineOption project_;
    QCommandLineOption log_;
    QCommandLineOption newInstance_;
    QCommandLineOption scheme_;
    QCommandLineOption keymap_;
    QCommandLineOption dump_;
    QCommandLineOption offscreen_;
    QCommandLineOption help_;
    LaunchRequest request_;
    QString error_;
};

} // namespace esses::app
