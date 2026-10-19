// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The About dialog and the build identification (about_dialog.hpp). Root of the M4 packaging package (wave 2)
// in esses_widgets (docs/milestones/M4.md section 3): the M4-STUB bodies below are replaced here.

#include <esses/base/version.hpp>
#include <esses/widgets/shell/about_dialog.hpp>

#include <QDialogButtonBox>
#include <QLabel>
#include <QStringList>
#include <QVBoxLayout>
#include <QtGlobal>
#include <string_view>

namespace esses::widgets {

namespace {

QString fromView(std::string_view v) {
    return QString::fromUtf8(v.data(), static_cast<qsizetype>(v.size()));
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
    // M4-STUB (packaging): dockLibrary
    return b;
}

QString versionReport() {
    // M4-STUB (packaging): version, commit, Qt version and build date (AC-M4-09); the first line stays.
    return QStringLiteral("esses ") + fromView(versionString());
}

QString qtLicenceNotice() {
    return QStringLiteral("Qt is used under the GNU LGPL version 3."); // M4-STUB (packaging)
}

QString sourceUrl() {
    return QStringLiteral("https://github.com/mitchdetailed/jubilant-octo-meme"); // M4-STUB (packaging)
}

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("aboutDialog"));
    const BuildInfo b = buildInfo();
    setWindowTitle(tr("About %1").arg(b.appName));
    auto* layout = new QVBoxLayout(this);
    label_ = new QLabel(this);
    label_->setObjectName(QStringLiteral("aboutText"));
    label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label_->setText(QStringList{versionReport(), qtLicenceNotice(), sourceUrl()}.join(QLatin1Char('\n')));
    layout->addWidget(label_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

AboutDialog::~AboutDialog() = default;

QString AboutDialog::text() const {
    return label_->text();
}

} // namespace esses::widgets
