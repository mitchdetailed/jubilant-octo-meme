// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The About dialog and the build identification (about_dialog.hpp; D-212). Root of the M4 packaging package
// (wave 2) in esses_widgets (docs/milestones/M4.md section 3). ESSES_RELEASE_TAG is defined for this file
// only, by tools/packaging/CMakeLists.txt, when a release build is configured with
// -DESSES_RELEASE_TAG=v<version>.

#include <esses/base/paths.hpp>
#include <esses/base/utf8.hpp>
#include <esses/base/version.hpp>
#include <esses/widgets/shell/about_dialog.hpp>
#include <esses/widgets/shell/dock_host.hpp>

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>
#include <QVBoxLayout>
#include <QtGlobal>
#include <filesystem>
#include <string_view>
#include <system_error>

#if ESSES_HAVE_ADS
#include <ads_version.h> // ADS_VERSION_MAJOR/MINOR/PATCH of the docking library this build links (version only)
#endif

namespace esses::widgets {

namespace {

QString fromView(std::string_view v) {
    return QString::fromUtf8(v.data(), static_cast<qsizetype>(v.size()));
}

QString releaseTagOfBuild() {
#ifdef ESSES_RELEASE_TAG
    return QStringLiteral(ESSES_RELEASE_TAG);
#else
    return {};
#endif
}

QString adsVersion() {
#if ESSES_HAVE_ADS
    return QStringLiteral("%1.%2.%3").arg(ADS_VERSION_MAJOR).arg(ADS_VERSION_MINOR).arg(ADS_VERSION_PATCH);
#else
    return {};
#endif
}

// The commit as a bare sha ("abc1234" of "abc1234-dirty"), empty unless it is 7 to 40 hexadecimal digits.
QString shaOf(const QString& commit) {
    QString sha = commit;
    if (sha.endsWith(QLatin1String("-dirty"))) {
        sha.chop(6);
    }
    static const QRegularExpression hex(QStringLiteral("^[0-9a-fA-F]{7,40}$"));
    return hex.match(sha).hasMatch() ? sha.toLower() : QString();
}

QString repository() {
    return QString::fromLatin1(kRepositoryUrl);
}

QString link(const QString& url) {
    const QString e = url.toHtmlEscaped();
    return QStringLiteral("<a href=\"%1\">%1</a>").arg(e);
}

// A paragraph of plain text whose URLs become links (each URL is a whole word).
QString paragraph(const QString& plain) {
    QStringList words = plain.split(QLatin1Char(' '));
    for (QString& w : words) {
        if (w.startsWith(QLatin1String("https://")) || w.startsWith(QLatin1String("file:"))) {
            w = link(w);
        } else {
            w = w.toHtmlEscaped();
        }
    }
    return QStringLiteral("<p>") + words.join(QLatin1Char(' ')) + QStringLiteral("</p>");
}

QString essesNotice() {
    return QStringLiteral("%1 is free software under the MIT licence (LICENSE). Source of this build: %2")
        .arg(buildInfo().appName, sourceUrl());
}

} // namespace

BuildInfo buildInfo() {
    const VersionInfo& v = versionInfo();
    BuildInfo b;
    b.appName = fromView(v.appName);
    b.version = fromView(v.string);
    b.commit = fromView(v.gitSha);
    b.buildDate = fromView(v.buildDate);
    b.qtRuntime = QString::fromLatin1(qVersion());
    b.qtBuild = QStringLiteral(QT_VERSION_STR);
    const QString ads = adsVersion();
    b.dockLibrary = !ads.isEmpty() && DockHost::defaultBackendName() == QLatin1String("ads")
                        ? QStringLiteral("ADS ") + ads
                        : QStringLiteral("QDockWidget");
    b.releaseTag = releaseTagOfBuild();
    return b;
}

QString versionReport() {
    const BuildInfo b = buildInfo();
    QStringList lines;
    lines << QStringLiteral("esses ") + b.version; // the first line keeps its M1 form (app.version_console)
    lines << QStringLiteral("commit: ") + b.commit;
    lines << QStringLiteral("Qt: %1 (built with %2)").arg(b.qtRuntime, b.qtBuild);
    lines << QStringLiteral("build date: ") + b.buildDate;
    lines << QStringLiteral("dock library: ") + b.dockLibrary;
    lines << QStringLiteral("source: ") + sourceUrl();
    return lines.join(QLatin1Char('\n'));
}

QString qtSourceArchiveUrl(const QString& qtVersion) {
    const QStringList parts = qtVersion.split(QLatin1Char('.'));
    const QString majorMinor = parts.size() >= 2 ? parts[0] + QLatin1Char('.') + parts[1] : qtVersion;
    return QStringLiteral(
               "https://download.qt.io/official_releases/qt/%1/%2/single/qt-everywhere-src-%2.tar.xz")
        .arg(majorMinor, qtVersion);
}

QString qtLicenceNotice() {
    const QString qt = QString::fromLatin1(qVersion());
    return QStringLiteral(
               "This application uses Qt %1 under the GNU LGPL v3. Qt is linked dynamically and is "
               "not modified; you may replace its libraries with your own build of Qt %1 (NOTICES.md "
               "says how). The licence texts are licenses/LGPL-3.0-only.txt and "
               "licenses/GPL-3.0-only.txt beside NOTICES.md. Source of Qt %1: %2")
        .arg(qt, qtSourceArchiveUrl(qt));
}

QString dockLibraryNotice() {
    const QString ads = adsVersion();
    if (ads.isEmpty()) {
        return {};
    }
    return QStringLiteral(
               "The dock windows use the Qt Advanced Docking System %1 under the GNU LGPL v2.1 or later "
               "(licenses/LGPL-2.1-or-later.txt), built unmodified as a separate library that you may "
               "replace. Source: https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System/tree/v%1")
        .arg(ads);
}

QString sourceUrlFor(const QString& releaseTag, const QString& commit) {
    if (!releaseTag.isEmpty()) {
        return repository() + QStringLiteral("/tree/") + releaseTag;
    }
    const QString sha = shaOf(commit);
    if (!sha.isEmpty()) {
        return repository() + QStringLiteral("/commit/") + sha;
    }
    return repository();
}

QString sourceUrl() {
    const BuildInfo b = buildInfo();
    return sourceUrlFor(b.releaseTag, b.commit);
}

QString noticesUrl(const Paths& paths) {
    if (!paths.dataDir().empty()) {
        const std::filesystem::path file = paths.dataDir().parent_path() / "NOTICES.md";
        std::error_code ec;
        if (std::filesystem::is_regular_file(file, ec)) {
            return QUrl::fromLocalFile(QString::fromStdString(pathToUtf8(file))).toString(QUrl::FullyEncoded);
        }
    }
    const BuildInfo b = buildInfo();
    QString ref = b.releaseTag;
    if (ref.isEmpty()) {
        ref = shaOf(b.commit);
    }
    if (ref.isEmpty()) {
        ref = QStringLiteral("main");
    }
    return repository() + QStringLiteral("/blob/") + ref + QStringLiteral("/NOTICES.md");
}

// ---- the dialog
// --------------------------------------------------------------------------------------------

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
    build(nullptr);
}

AboutDialog::AboutDialog(const Paths& paths, QWidget* parent) : QDialog(parent) {
    build(&paths);
}

AboutDialog::~AboutDialog() = default;

void AboutDialog::build(const Paths* paths) {
    setObjectName(QStringLiteral("aboutDialog"));
    const BuildInfo b = buildInfo();
    setWindowTitle(tr("About %1").arg(b.appName));

    if (paths != nullptr) {
        notices_ = noticesUrl(*paths);
    } else {
        QString ref = b.releaseTag.isEmpty() ? shaOf(b.commit) : b.releaseTag;
        notices_ = repository() + QStringLiteral("/blob/") + (ref.isEmpty() ? QStringLiteral("main") : ref) +
                   QStringLiteral("/NOTICES.md");
    }

    // The plain text (text(), Copy) and the same paragraphs as rich text with links.
    QStringList plain;
    QString rich;
    const QString report = versionReport();
    plain << report;
    QStringList reportLines = report.split(QLatin1Char('\n'));
    rich += QStringLiteral("<p><b>") + reportLines.takeFirst().toHtmlEscaped() + QStringLiteral("</b><br>");
    QStringList richLines;
    for (const QString& line : reportLines) {
        const qsizetype at = line.indexOf(QStringLiteral(": "));
        const QString value = at < 0 ? QString() : line.mid(at + 2);
        richLines << (value.startsWith(QLatin1String("https://"))
                          ? line.left(at + 2).toHtmlEscaped() + link(value)
                          : line.toHtmlEscaped());
    }
    rich += richLines.join(QStringLiteral("<br>")) + QStringLiteral("</p>");

    QStringList notices{qtLicenceNotice()};
    if (const QString dock = dockLibraryNotice(); !dock.isEmpty()) {
        notices << dock;
    }
    notices << essesNotice();
    notices << QStringLiteral("Notices and licences: ") + notices_;
    if (paths != nullptr) {
        QString settings =
            QStringLiteral("Settings: ") + QString::fromStdString(pathToUtf8(paths->configDir()));
        if (paths->portable()) {
            settings += QStringLiteral(" (portable: beside the program, portable.marker)");
        }
        notices << settings;
    }
    for (const QString& n : notices) {
        plain << n;
        rich += paragraph(n);
    }
    text_ = plain.join(QStringLiteral("\n\n"));

    auto* layout = new QVBoxLayout(this);
    auto* row = new QHBoxLayout;
    const QIcon icon = QApplication::windowIcon();
    if (!icon.isNull()) {
        auto* picture = new QLabel(this);
        picture->setObjectName(QStringLiteral("aboutIcon"));
        picture->setPixmap(icon.pixmap(64, 64));
        picture->setAlignment(Qt::AlignTop);
        row->addWidget(picture);
    }
    label_ = new QLabel(this);
    label_->setObjectName(QStringLiteral("aboutText"));
    label_->setTextFormat(Qt::RichText);
    label_->setWordWrap(true);
    label_->setOpenExternalLinks(true);
    label_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    label_->setText(rich);
    label_->setMinimumWidth(480);
    label_->setAccessibleName(tr("About %1").arg(b.appName));
    label_->setAccessibleDescription(report);
    row->addWidget(label_, 1);
    layout->addLayout(row);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    copy_ = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    copy_->setObjectName(QStringLiteral("aboutCopy"));
    copy_->setToolTip(tr("Copy the version and the notices to the clipboard"));
    connect(copy_, &QPushButton::clicked, this, [this] {
        if (QClipboard* clipboard = QApplication::clipboard()) {
            clipboard->setText(text_);
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString AboutDialog::text() const {
    return text_;
}

QString AboutDialog::html() const {
    return label_->text();
}

} // namespace esses::widgets
