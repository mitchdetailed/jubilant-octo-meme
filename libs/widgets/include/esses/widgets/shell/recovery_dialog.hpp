// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// RecoveryDialog (06 section 18 step 3, 02 section 6.8, D-019, D-172; AC-M2-06): the prompt "Recover unsaved
// changes?" that MainWindow shows when a project opens with a pending journal (ProjectSession::
// recoveryAvailable()). It lists the project documents the journal holds (project-relative paths, newest
// entry each) and marks those whose file changed on disk after the entry was written (the replay's conflicts:
// a recovery overwrites those changes in memory, and only an explicit Save writes them), plus the replay's
// log lines (skipped journal files). Three answers: Recover (apply the journal in memory; the project becomes
// dirty and the journal stays until Save), Discard (delete the journal) and Later (keep it untouched;
// autosave stays suspended and the prompt returns at the next open). Closing the dialog any other way is
// Later. The dialog is window-modal and never blocks the caller: the shell opens it with open() and acts on
// answered().
#pragma once

#include <esses/project/journal.hpp>

#include <QDialog>
#include <QString>
#include <QStringList>

class QListWidget;
class QPushButton;

namespace esses::widgets {

class RecoveryDialog final : public QDialog {
    Q_OBJECT
public:
    RecoveryDialog(const project::JournalReplay& replay, const QString& projectName,
                   QWidget* parent = nullptr);
    ~RecoveryDialog() override;

    // The answer given (Later until a button is pressed).
    project::JournalRecovery answer() const noexcept { return answer_; }
    // The button of an answer (tests press them as the user would).
    QPushButton& button(project::JournalRecovery which) const;
    // The listed documents, as shown ("workbooks/General.workbook.json", "project.json (changed on disk
    // since)").
    QStringList documents() const;

Q_SIGNALS:
    void answered(esses::project::JournalRecovery answer);

protected:
    void done(int result) override;

private:
    void choose(project::JournalRecovery answer);

    project::JournalRecovery answer_ = project::JournalRecovery::Later;
    bool chosen_ = false;
    QListWidget* list_ = nullptr;
    QPushButton* recover_ = nullptr;
    QPushButton* discard_ = nullptr;
    QPushButton* later_ = nullptr;
};

} // namespace esses::widgets
