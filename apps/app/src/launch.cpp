// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// What a LaunchRequest does to the window (launch.hpp; 06 sections 18-19, D-113, D-212).

#include "launch.hpp"

#include <esses/base/paths.hpp>
#include <esses/base/utf8.hpp>
#include <esses/project/keymap.hpp>
#include <esses/project/scheme.hpp>
#include <esses/project/shipped_data.hpp>
#include <esses/qtmodel/keymap_binder.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/widgets/editors_options/scheme_selector.hpp>
#include <esses/widgets/shell/main_window.hpp>

#include <QDateTime>
#include <QFileInfo>
#include <QWidget>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace esses::app {

namespace {

std::filesystem::path toPath(const QString& s) {
    return pathFromUtf8(s.toStdString());
}

QString fromPath(const std::filesystem::path& p) {
    return QString::fromStdString(pathToUtf8(p));
}

void warn(widgets::MainWindow& window, const QString& code, const QString& text) {
    qt::Message m;
    m.timestamp = QDateTime::currentDateTime();
    m.severity = qt::Message::Severity::Warning;
    m.origin = qt::Message::Origin::System;
    m.code = code;
    m.text = text;
    window.messages().append(std::move(m));
}

// The project folder a -w argument names (the folder itself or its project.json).
std::filesystem::path projectFolder(const QString& project) {
    std::filesystem::path p = toPath(project);
    if (p.filename() == "project.json") {
        p = p.parent_path();
    }
    return p;
}

bool isOpenProject(widgets::MainWindow& window, const QString& project) {
    const std::optional<std::filesystem::path>& open = window.session().folder();
    if (!open) {
        return false;
    }
    std::error_code ec;
    const bool same = std::filesystem::equivalent(*open, projectFolder(project), ec);
    return !ec && same;
}

} // namespace

void openRequestedFiles(widgets::MainWindow& window, const LaunchRequest& request, LaunchKind kind) {
    std::vector<std::filesystem::path> logs;
    for (const QString& log : request.logs) {
        logs.push_back(toPath(log));
    }
    if (kind == LaunchKind::Forwarded && !request.project.isEmpty() &&
        isOpenProject(window, request.project)) {
        for (const std::filesystem::path& log : logs) {
            window.openLog(log); // the project is open already: its files are added (06 section 19 item 6)
        }
        return;
    }
    if (request.project.isEmpty() || !window.openProject(toPath(request.project), logs)) {
        for (const std::filesystem::path& log : logs) {
            window.openLog(log);
        }
    }
}

QString applyScheme(widgets::MainWindow& window, const QString& name) {
    widgets::SchemeSelector& selector = window.schemeSelector();
    const QStringList names = selector.schemeNames();
    QString match;
    for (const QString& n : names) {
        if (n.compare(name, Qt::CaseInsensitive) == 0) {
            match = n;
            break;
        }
    }
    if (match.isEmpty()) {
        const bool light = name.compare(QLatin1String("light"), Qt::CaseInsensitive) == 0;
        const bool dark = name.compare(QLatin1String("dark"), Qt::CaseInsensitive) == 0;
        if (light || dark) {
            if (std::shared_ptr<const project::Scheme> shipped = window.session().shippedScheme(dark)) {
                match = QString::fromStdString(shipped->name);
            }
        }
    }
    if (match.isEmpty() || !selector.selectScheme(match)) {
        const QString problem = QStringLiteral("--scheme: there is no scheme named \"%1\" (schemes: %2)")
                                    .arg(name, names.join(QStringLiteral(", ")));
        warn(window, QStringLiteral("launch.scheme"), problem);
        return problem;
    }
    return {};
}

QString applyKeymap(widgets::MainWindow& window, const QString& nameOrFile) {
    const Paths& paths = window.session().paths();
    std::vector<std::filesystem::path> candidates;
    if (QFileInfo(nameOrFile).isFile()) {
        candidates.push_back(toPath(nameOrFile));
    } else {
        const std::string file = nameOrFile.toStdString() + ".json";
        if (!paths.configDir().empty()) {
            candidates.push_back(paths.configDir() / "keymaps" / pathFromUtf8(file));
        }
        if (!paths.dataDir().empty()) {
            candidates.push_back(paths.dataDir() / "keymaps" / pathFromUtf8(file));
        }
    }
    std::filesystem::path found;
    for (const std::filesystem::path& c : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(c, ec)) {
            found = c;
            break;
        }
    }
    const auto fail = [&](const QString& problem) {
        warn(window, QStringLiteral("launch.keymap"), problem);
        return problem;
    };
    if (found.empty()) {
        QStringList tried;
        for (const std::filesystem::path& c : candidates) {
            tried << fromPath(c);
        }
        return fail(QStringLiteral("--keymap: no keymap \"%1\" (looked for %2)")
                        .arg(nameOrFile, tried.join(QStringLiteral(", "))));
    }
    std::vector<std::string> log;
    Result<project::Keymap> loaded = project::Keymap::load(found, log);
    if (!loaded) {
        return fail(QStringLiteral("--keymap: %1: %2")
                        .arg(fromPath(found), QString::fromStdString(loaded.error().toString())));
    }
    Result<project::Keymap> shipped =
        project::Keymap::load(project::shippedFile(paths, project::kDefaultKeymap), log);
    if (!shipped) {
        return fail(QStringLiteral("--keymap: the shipped keymap: %1")
                        .arg(QString::fromStdString(shipped.error().toString())));
    }
    const project::Keymap merged = project::Keymap::merged(*shipped, *loaded);
    for (const QString& line : window.keymap().setKeymap(merged)) {
        warn(window, QStringLiteral("launch.keymap"), line);
    }
    for (const std::string& line : log) {
        warn(window, QStringLiteral("launch.keymap"),
             fromPath(found) + QStringLiteral(": ") + QString::fromStdString(line));
    }
    return {};
}

QStringList applyStartOptions(widgets::MainWindow& window, const LaunchRequest& request) {
    QStringList problems;
    if (!request.scheme.isEmpty()) {
        if (const QString p = applyScheme(window, request.scheme); !p.isEmpty()) {
            problems << p;
        }
    }
    if (!request.keymap.isEmpty()) {
        if (const QString p = applyKeymap(window, request.keymap); !p.isEmpty()) {
            problems << p;
        }
    }
    return problems;
}

void bringToFront(QWidget& window) {
    if (window.isMinimized()) {
        window.setWindowState((window.windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    }
    window.show();
    window.raise();
    window.activateWindow();
}

} // namespace esses::app
