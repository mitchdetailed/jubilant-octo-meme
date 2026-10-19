// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DocsShell (docs_shell.hpp): the M4 docs package's shell wiring (wave 2; docs/milestones/M4.md section 3;
// D-215): Help > Help Contents (help.contents, F1) opens the installed start page of the user documentation,
// else (or when that cannot be opened) the online one, through the URL opener the tests replace.

#include "main_window_impl.hpp"

#include <esses/base/read_file.hpp>
#include <esses/base/utf8.hpp>
#include <esses/base/version.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/widgets/docks/messages_window.hpp>
#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/docs_shell.hpp>

#include <QDesktopServices>
#include <QDir>
#include <QPointer>
#include <QRegularExpression>
#include <QUrl>
#include <QVariantList>
#include <array>
#include <utility>

namespace esses::widgets {

namespace {

// `dir` without a trailing separator ("C:/x/data/" -> "C:/x/data"), so that parent_path() is its parent.
std::filesystem::path withoutTrailingSeparator(const std::filesystem::path& dir) {
    return dir.has_filename() || !dir.has_relative_path() ? dir : dir.parent_path();
}

QString qPath(const std::filesystem::path& path) {
    return QString::fromStdString(pathToUtf8(path));
}

QString fileUrl(const std::filesystem::path& page) {
    return QUrl::fromLocalFile(QDir::fromNativeSeparators(qPath(page))).toString(QUrl::FullyEncoded);
}

QString buildVersion() {
    const std::string_view v = versionInfo().string;
    return QString::fromUtf8(v.data(), static_cast<qsizetype>(v.size()));
}

} // namespace

DocsShell::DocsShell(MainWindow& window) : ShellExtension(window, kKey) {}

DocsShell::~DocsShell() = default;

void DocsShell::setUrlOpener(UrlOpener opener) {
    opener_ = std::move(opener);
}

std::optional<std::filesystem::path> DocsShell::installedStartPage(const Paths& paths) {
    const std::filesystem::path relative = pathFromUtf8(kStartPage);
    const std::filesystem::path data = withoutTrailingSeparator(paths.dataDir());
    const std::array<std::filesystem::path, 2> roots{data.empty() ? data : data.parent_path(),
                                                     paths.exeDir()};
    for (const std::filesystem::path& root : roots) {
        if (root.empty()) {
            continue;
        }
        const std::filesystem::path page = (root / relative).lexically_normal();
        if (requireRegularFile(page)) {
            return page;
        }
    }
    return std::nullopt;
}

QString DocsShell::onlineUrl(const QString& version) {
    // Exactly a tag: MAJOR.MINOR.PATCH with an optional pre-release ("0.1.0-m4"); cmake/version.cmake adds
    // "+N-gHASH" after a tag and ".dirty" (or, at a tag, "-dirty") to a modified tree.
    static const QRegularExpression tag(QStringLiteral("^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.]+)?$"));
    const bool exact = tag.match(version).hasMatch() && !version.contains(QLatin1String("dirty"));
    const QString ref = exact ? QStringLiteral("v") + version : QStringLiteral("main");
    return QString::fromLatin1(kRepositoryUrl) + QStringLiteral("/blob/") + ref + QLatin1Char('/') +
           QString::fromLatin1(kStartPage);
}

QString DocsShell::contentsUrl() const {
    const std::optional<std::filesystem::path> page = installedStartPage(impl().paths);
    return page ? fileUrl(*page) : QString();
}

bool DocsShell::open(const QString& url) const {
    if (opener_) {
        return opener_(url);
    }
    return QDesktopServices::openUrl(QUrl(url, QUrl::StrictMode));
}

QString DocsShell::openContents() {
    const std::optional<std::filesystem::path> page = installedStartPage(impl().paths);
    if (page) {
        const QString local = fileUrl(*page);
        if (open(local)) {
            hint(tr("Opening the user documentation"));
            return local;
        }
    }
    const QString online = onlineUrl(buildVersion());
    if (open(online)) {
        hint(page ? tr("No program opened the installed pages: opening the online user documentation")
                  : tr("Opening the online user documentation"));
        return online;
    }
    qt::Message m;
    m.severity = qt::Message::Severity::Warning;
    m.origin = qt::Message::Origin::System;
    m.source = QStringLiteral("Help");
    m.code = QString::fromLatin1(kNotOpenedCode);
    m.text = page ? tr("The user documentation could not be opened. It is installed at %1 and online at %2.")
                        .arg(QDir::toNativeSeparators(qPath(*page)), online)
                  : tr("The user documentation could not be opened. It is online at %1.").arg(online);
    window().messages().append(std::move(m));
    window().docks().show(QString::fromLatin1(MessagesWindow::kDockId));
    hint(tr("Could not open the user documentation: see Messages"));
    return {};
}

void DocsShell::registerCommands() {
    qt::CommandSpec s;
    s.id = QString::fromLatin1(kCommand);
    s.title = tr("Help Contents");
    s.menuPath = QStringLiteral("Help");
    s.context = qt::CommandContext::Global;
    s.defaultKeys = {QStringLiteral("F1")};
    s.invoke = [self = QPointer<DocsShell>(this)](const QVariantList&) {
        if (!self.isNull()) {
            static_cast<void>(self->openContents());
        }
    };
    window().commands().add(std::move(s));
}

} // namespace esses::widgets
