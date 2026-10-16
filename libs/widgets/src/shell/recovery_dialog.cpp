// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/widgets/shell/recovery_dialog.hpp>

#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

namespace esses::widgets {

RecoveryDialog::RecoveryDialog(const project::JournalReplay& replay, const QString& projectName,
                               QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("recoveryDialog"));
    setWindowTitle(tr("Recover Unsaved Changes"));
    setAccessibleName(tr("Recover unsaved changes"));
    setWindowModality(Qt::WindowModal);
    auto* layout = new QVBoxLayout(this);
    auto* question = new QLabel(tr("<b>Recover unsaved changes?</b>"), this);
    question->setObjectName(QStringLiteral("recoveryQuestion"));
    layout->addWidget(question);
    auto* text =
        new QLabel(tr("The project \"%1\" was not closed normally. Its journal holds edits that were "
                      "never saved. Recover applies them now (save the project to keep them); "
                      "Discard deletes the journal; Later keeps it for the next time the project opens.")
                       .arg(projectName),
                   this);
    text->setWordWrap(true);
    layout->addWidget(text);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("recoveryDocuments"));
    list_->setAccessibleName(tr("Documents in the journal"));
    list_->setSelectionMode(QAbstractItemView::NoSelection);
    for (const project::JournalEntry& e : replay.entries) {
        const bool changed =
            std::find(replay.conflicts.begin(), replay.conflicts.end(), e.path) != replay.conflicts.end();
        auto* item = new QListWidgetItem(
            QString::fromStdString(e.path) + (changed ? tr(" (changed on disk since)") : QString()), list_);
        if (changed) {
            item->setToolTip(tr("The file changed after this edit was journalled; recovering replaces that "
                                "change in memory (nothing is written until Save)."));
        }
    }
    layout->addWidget(list_, 1);
    if (!replay.log.empty()) {
        QStringList lines;
        for (const std::string& line : replay.log) {
            lines << QString::fromStdString(line);
        }
        auto* log = new QLabel(lines.join(QLatin1Char('\n')), this);
        log->setObjectName(QStringLiteral("recoveryLog"));
        log->setWordWrap(true);
        layout->addWidget(log);
    }
    auto* buttons = new QDialogButtonBox(this);
    recover_ = buttons->addButton(tr("Recover"), QDialogButtonBox::AcceptRole);
    recover_->setObjectName(QStringLiteral("recover"));
    discard_ = buttons->addButton(tr("Discard"), QDialogButtonBox::DestructiveRole);
    discard_->setObjectName(QStringLiteral("discard"));
    later_ = buttons->addButton(tr("Later"), QDialogButtonBox::RejectRole);
    later_->setObjectName(QStringLiteral("later"));
    recover_->setDefault(true);
    layout->addWidget(buttons);
    connect(recover_, &QPushButton::clicked, this, [this] { choose(project::JournalRecovery::Recover); });
    connect(discard_, &QPushButton::clicked, this, [this] { choose(project::JournalRecovery::Discard); });
    connect(later_, &QPushButton::clicked, this, [this] { choose(project::JournalRecovery::Later); });
}

RecoveryDialog::~RecoveryDialog() = default;

QPushButton& RecoveryDialog::button(project::JournalRecovery which) const {
    switch (which) {
    case project::JournalRecovery::Recover:
        return *recover_;
    case project::JournalRecovery::Discard:
        return *discard_;
    case project::JournalRecovery::Later:
        break;
    }
    return *later_;
}

QStringList RecoveryDialog::documents() const {
    QStringList out;
    for (int i = 0; i < list_->count(); ++i) {
        out << list_->item(i)->text();
    }
    return out;
}

void RecoveryDialog::choose(project::JournalRecovery answer) {
    answer_ = answer;
    done(answer == project::JournalRecovery::Recover ? QDialog::Accepted : QDialog::Rejected);
}

void RecoveryDialog::done(int result) {
    QDialog::done(result);
    if (!chosen_) {
        chosen_ = true; // ESC or the close button answer Later
        Q_EMIT answered(answer_);
    }
}

} // namespace esses::widgets
