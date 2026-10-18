// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The two small editors of the Channel Editor (06 section 13.1 item 7, D-013; tools.aliases, tools.mappings),
// modeless dialogs over the project's channel documents:
//   AliasesDialog: aliases (channels/aliases.json, aliases-1), a name and an ordered list of candidates, each
//     with its enable box (Add / Remove / Up / Down). Apply refuses empty or repeated names, empty candidates
//     and circular references ("A -> B -> A", project::Aliases::cycleThrough), else pushes one
//     EditAliasesCommand.
//   MappingsDialog: mappings (channels/mappings.json, mappings-1), logged name -> analysis name with an
//     optional unit stamp and "unitless only". Apply refuses empty names, a logged name mapped twice and
//     unknown unit symbols, else pushes one EditMappingsCommand.
// Each works on a copy of the document, reloaded from the session while it has no unapplied change; OK
// applies and closes, Cancel drops the changes. Unknown members of the documents are kept.
#pragma once

#include <esses/project/aliases.hpp>
#include <esses/project/mappings.hpp>

#include <QDialog>
#include <QString>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QTableWidget;

namespace esses::qt {
class ProjectSession;
} // namespace esses::qt

namespace esses::widgets {

class AliasesDialog final : public QDialog {
    Q_OBJECT
public:
    explicit AliasesDialog(qt::ProjectSession& session, QWidget* parent = nullptr);
    ~AliasesDialog() override;

    // The edited copy and its replacement (the panes follow).
    const project::Aliases& document() const noexcept { return doc_; }
    void setDocument(project::Aliases doc);
    bool isDirty() const noexcept { return dirty_; }
    // Validates and pushes the edit; false with the reason (also shown in the dialog) when refused.
    bool apply(QString* error = nullptr);
    QString lastError() const;
    // Shows the alias `name` (adding nothing); false when it does not exist.
    bool selectAlias(const QString& name);

    QListWidget& aliasList() noexcept { return *aliases_; }
    QListWidget& candidateList() noexcept { return *candidates_; }
    QLineEdit& nameEdit() noexcept { return *name_; }

private:
    void reload();
    void showAlias(int row);
    void storeAlias();

    qt::ProjectSession& session_;
    project::Aliases doc_;
    QListWidget* aliases_ = nullptr;
    QLineEdit* name_ = nullptr;
    QListWidget* candidates_ = nullptr;
    QLabel* error_ = nullptr;
    int current_ = -1;
    int loading_ = 0;
    bool dirty_ = false;
};

class MappingsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit MappingsDialog(qt::ProjectSession& session, QWidget* parent = nullptr);
    ~MappingsDialog() override;

    // The rows of the table as a document (unknown members kept from the session's).
    project::Mappings document() const;
    void setDocument(const project::Mappings& doc);
    bool isDirty() const noexcept { return dirty_; }
    bool apply(QString* error = nullptr);
    QString lastError() const;
    // Shows the row of a logged name; false when there is none.
    bool selectMapping(const QString& loggedName);

    QTableWidget& table() noexcept { return *table_; }

private:
    void reload();
    void addRow(const project::Mappings::Row& row);

    qt::ProjectSession& session_;
    project::Mappings base_; // unknown members and schemaVersion of the edited document
    QTableWidget* table_ = nullptr;
    QLabel* error_ = nullptr;
    int loading_ = 0;
    bool dirty_ = false;
};

} // namespace esses::widgets
