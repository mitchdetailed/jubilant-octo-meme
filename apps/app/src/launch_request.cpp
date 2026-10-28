// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The command line of esses and the forwarded request (launch_request.hpp; 06 section 19, D-113, D-212).

#include "launch_request.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

namespace esses::app {

QString absoluteFrom(const QString& path, const QString& workingDir) {
    if (path.isEmpty()) {
        return {};
    }
    if (QDir::isAbsolutePath(path)) {
        return QDir::cleanPath(path);
    }
    return QDir::cleanPath(QDir(workingDir).absoluteFilePath(path));
}

bool isProjectArgument(const QString& absolutePath) {
    const QFileInfo info(absolutePath);
    return (info.isDir() && QFileInfo::exists(info.filePath() + QStringLiteral("/project.json"))) ||
           info.fileName() == QLatin1String("project.json");
}

bool shouldForward(const LaunchRequest& request) {
    return !request.newInstance && !request.version && !request.dumpCommands;
}

QByteArray LaunchRequest::toMessage() const {
    QJsonObject o;
    o.insert(QStringLiteral("protocol"), kProtocol);
    o.insert(QStringLiteral("project"), project);
    o.insert(QStringLiteral("logs"), QJsonArray::fromStringList(logs));
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

std::optional<LaunchRequest> LaunchRequest::fromMessage(const QByteArray& line, QString* error) {
    const auto refuse = [error](const QString& why) -> std::optional<LaunchRequest> {
        if (error != nullptr) {
            *error = why;
        }
        return std::nullopt;
    };
    QByteArray text = line;
    if (text.endsWith('\n')) {
        text.chop(1);
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(text, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return refuse(QStringLiteral("not a JSON object"));
    }
    const QJsonObject o = doc.object();
    if (o.value(QStringLiteral("protocol")).toInt(-1) != kProtocol) {
        return refuse(QStringLiteral("unknown protocol"));
    }
    LaunchRequest r;
    const QJsonValue project = o.value(QStringLiteral("project"));
    if (!project.isUndefined() && !project.isString()) {
        return refuse(QStringLiteral("project is not a string"));
    }
    r.project = project.toString();
    if (!r.project.isEmpty() && !QDir::isAbsolutePath(r.project)) {
        return refuse(QStringLiteral("project is not an absolute path"));
    }
    const QJsonValue logs = o.value(QStringLiteral("logs"));
    if (!logs.isUndefined() && !logs.isArray()) {
        return refuse(QStringLiteral("logs is not an array"));
    }
    const QJsonArray list = logs.toArray();
    if (list.size() > kMaxLogs) {
        return refuse(QStringLiteral("too many logs"));
    }
    for (const QJsonValue& v : list) {
        if (!v.isString() || v.toString().isEmpty() || !QDir::isAbsolutePath(v.toString())) {
            return refuse(QStringLiteral("a log is not an absolute path"));
        }
        r.logs << QDir::cleanPath(v.toString());
    }
    if (!r.project.isEmpty()) {
        r.project = QDir::cleanPath(r.project);
    }
    return r;
}

CommandLine::CommandLine()
    : version_(QStringLiteral("version"),
               QStringLiteral("Print the version, commit, Qt version and build date, then exit.")),
      project_(QStringList{QStringLiteral("w"), QStringLiteral("workspace")},
               QStringLiteral("Open the project folder <project> (or its project.json)."),
               QStringLiteral("project")),
      log_(QStringList{QStringLiteral("l"), QStringLiteral("log")},
           QStringLiteral("Open the log file <log> (repeatable)."), QStringLiteral("log")),
      newInstance_(QStringLiteral("new-instance"),
                   QStringLiteral("Start a new window instead of handing the files to a running Esses.")),
      scheme_(QStringLiteral("scheme"),
              QStringLiteral("Start with the colour scheme <name> (as the scheme selector)."),
              QStringLiteral("name")),
      keymap_(QStringLiteral("keymap"),
              QStringLiteral(
                  "Use the keymap <name> (<config>/keymaps/<name>.json, the shipped keymaps) or file for "
                  "this run."),
              QStringLiteral("name")),
      dump_(QStringLiteral("dump-commands"), QStringLiteral("Print the command registry as JSON and exit.")),
      offscreen_(QStringLiteral("offscreen"), QStringLiteral("Run without a display (tests).")),
      help_(parser_.addHelpOption()) {
    parser_.setApplicationDescription(QStringLiteral("Esses motorsport data analysis"));
    for (const QCommandLineOption* o :
         {&version_, &project_, &log_, &newInstance_, &scheme_, &keymap_, &dump_, &offscreen_}) {
        parser_.addOption(*o);
    }
    parser_.addPositionalArgument(QStringLiteral("files"),
                                  QStringLiteral("Project folders (with project.json) or log files."),
                                  QStringLiteral("[<file>...]"));
}

CommandLine::Outcome CommandLine::parse(const QStringList& arguments, const QString& workingDir) {
    request_ = LaunchRequest{};
    error_.clear();
    if (!parser_.parse(arguments)) {
        error_ = parser_.errorText();
        return Outcome::Error;
    }
    if (parser_.isSet(help_) || parser_.isSet(QStringLiteral("help-all"))) {
        return Outcome::Help;
    }
    LaunchRequest& r = request_;
    r.version = parser_.isSet(version_);
    r.newInstance = parser_.isSet(newInstance_);
    r.dumpCommands = parser_.isSet(dump_);
    r.offscreen = parser_.isSet(offscreen_);
    r.scheme = parser_.value(scheme_).trimmed();
    if (parser_.isSet(scheme_) && r.scheme.isEmpty()) {
        error_ = QStringLiteral("--scheme needs a scheme name");
        return Outcome::Error;
    }
    const QString keymap = parser_.value(keymap_).trimmed();
    if (parser_.isSet(keymap_) && keymap.isEmpty()) {
        error_ = QStringLiteral("--keymap needs a keymap name or file");
        return Outcome::Error;
    }
    // A keymap argument that names an existing file is that file wherever the run starts; a bare name is
    // looked up in the keymap folders by the running window (launch.hpp).
    const QString keymapFile = absoluteFrom(keymap, workingDir);
    r.keymap = !keymap.isEmpty() && QFileInfo(keymapFile).isFile() ? keymapFile : keymap;
    // -w given twice: the last one, as QCommandLineParser::value() has done since M1.
    r.project = absoluteFrom(parser_.value(project_), workingDir);
    for (const QString& log : parser_.values(log_)) {
        r.logs << absoluteFrom(log, workingDir);
    }
    for (const QString& arg : parser_.positionalArguments()) {
        const QString path = absoluteFrom(arg, workingDir);
        if (r.project.isEmpty() && isProjectArgument(path)) {
            r.project = path;
        } else {
            r.logs << path;
        }
    }
    if (r.logs.size() > LaunchRequest::kMaxLogs) {
        error_ = QStringLiteral("too many log files (at most %1)").arg(LaunchRequest::kMaxLogs);
        return Outcome::Error;
    }
    return r.version ? Outcome::Version : Outcome::Run;
}

} // namespace esses::app
