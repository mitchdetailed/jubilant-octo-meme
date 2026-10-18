// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The Aliases and Mappings editors of the Channel Editor (06 section 13.1 item 7).

#include <esses/base/casefold.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/units/unit_registry.hpp>
#include <esses/widgets/editors_maths/channel_documents_dialogs.hpp>

#include <QCheckBox>
#include <QColor>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPalette>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>
#include <set>
#include <string>
#include <utility>

namespace esses::widgets {

namespace {

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

QLabel* errorLabel(QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setWordWrap(true);
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, QColor(0xD3, 0x2F, 0x2F));
    label->setPalette(pal);
    label->hide();
    return label;
}

QPushButton* button(const QString& text, QWidget* parent) {
    auto* b = new QPushButton(text, parent);
    b->setAutoDefault(false);
    return b;
}

void showError(QLabel* label, const QString& text) {
    label->setText(text);
    label->setVisible(!text.isEmpty());
}

} // namespace

// ---- AliasesDialog ----------------------------------------------------------------------------------------

AliasesDialog::AliasesDialog(qt::ProjectSession& session, QWidget* parent)
    : QDialog(parent), session_(session) {
    setWindowTitle(tr("Aliases"));
    setModal(false);
    auto* root = new QVBoxLayout(this);
    auto* panes = new QHBoxLayout;
    root->addLayout(panes, 1);

    auto* left = new QVBoxLayout;
    left->addWidget(new QLabel(tr("Aliases"), this));
    aliases_ = new QListWidget(this);
    aliases_->setAccessibleName(tr("Aliases"));
    left->addWidget(aliases_, 1);
    auto* leftButtons = new QHBoxLayout;
    auto* add = button(tr("Add"), this);
    auto* remove = button(tr("Remove"), this);
    leftButtons->addWidget(add);
    leftButtons->addWidget(remove);
    left->addLayout(leftButtons);
    panes->addLayout(left, 1);

    auto* right = new QVBoxLayout;
    auto* form = new QFormLayout;
    name_ = new QLineEdit(this);
    name_->setProperty("esses.textEntry", true);
    name_->setAccessibleName(tr("Alias name"));
    form->addRow(tr("Name"), name_);
    right->addLayout(form);
    right->addWidget(new QLabel(tr("Candidates, in order (the first usable one wins)"), this));
    candidates_ = new QListWidget(this);
    candidates_->setAccessibleName(tr("Alias candidates"));
    right->addWidget(candidates_, 1);
    auto* rightButtons = new QHBoxLayout;
    auto* addCandidate = button(tr("Add"), this);
    auto* removeCandidate = button(tr("Remove"), this);
    auto* up = button(tr("Up"), this);
    auto* down = button(tr("Down"), this);
    for (QPushButton* b : {addCandidate, removeCandidate, up, down}) {
        rightButtons->addWidget(b);
    }
    right->addLayout(rightButtons);
    panes->addLayout(right, 2);

    error_ = errorLabel(this);
    root->addWidget(error_);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* ok = button(tr("OK"), this);
    auto* applyButton = button(tr("Apply"), this);
    auto* cancel = button(tr("Cancel"), this);
    buttons->addWidget(ok);
    buttons->addWidget(applyButton);
    buttons->addWidget(cancel);
    root->addLayout(buttons);

    connect(aliases_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (loading_ > 0) {
            return;
        }
        storeAlias();
        showAlias(row);
    });
    connect(name_, &QLineEdit::textEdited, this, [this](const QString& text) {
        storeAlias();
        if (QListWidgetItem* item = aliases_->item(current_)) {
            item->setText(text.trimmed());
        }
    });
    connect(candidates_, &QListWidget::itemChanged, this, [this] {
        if (loading_ == 0) {
            storeAlias();
        }
    });
    connect(add, &QPushButton::clicked, this, [this] {
        storeAlias();
        std::set<std::string> taken;
        for (const model::AliasRow& r : doc_.table.rows) {
            taken.insert(caseFold(r.name));
        }
        QString name = tr("New Alias");
        for (int i = 2; taken.count(caseFold(name.toStdString())) != 0; ++i) {
            name = tr("New Alias %1").arg(i);
        }
        doc_.table.rows.push_back(model::AliasRow{name.toStdString(), {}, -1, std::nullopt});
        dirty_ = true;
        const int row = static_cast<int>(doc_.table.rows.size()) - 1;
        ++loading_;
        aliases_->addItem(name);
        aliases_->setCurrentRow(row);
        --loading_;
        showAlias(row);
        name_->setFocus();
        name_->selectAll();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (current_ < 0 || current_ >= static_cast<int>(doc_.table.rows.size())) {
            return;
        }
        doc_.table.rows.erase(doc_.table.rows.begin() + current_);
        dirty_ = true;
        const int next = std::min(current_, static_cast<int>(doc_.table.rows.size()) - 1);
        current_ = -1;
        ++loading_;
        delete aliases_->takeItem(aliases_->currentRow());
        aliases_->setCurrentRow(next);
        --loading_;
        showAlias(next);
    });
    connect(addCandidate, &QPushButton::clicked, this, [this] {
        if (current_ < 0) {
            return;
        }
        auto* item = new QListWidgetItem(tr("Channel"), candidates_);
        item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        storeAlias();
        candidates_->setCurrentItem(item);
        candidates_->editItem(item);
    });
    connect(removeCandidate, &QPushButton::clicked, this, [this] {
        delete candidates_->takeItem(candidates_->currentRow());
        storeAlias();
    });
    const auto move = [this](int delta) {
        const int row = candidates_->currentRow();
        const int to = row + delta;
        if (row < 0 || to < 0 || to >= candidates_->count()) {
            return;
        }
        ++loading_;
        QListWidgetItem* item = candidates_->takeItem(row);
        candidates_->insertItem(to, item);
        candidates_->setCurrentRow(to);
        --loading_;
        storeAlias();
    };
    connect(up, &QPushButton::clicked, this, [move] { move(-1); });
    connect(down, &QPushButton::clicked, this, [move] { move(1); });
    connect(applyButton, &QPushButton::clicked, this, [this] { static_cast<void>(apply()); });
    connect(ok, &QPushButton::clicked, this, [this] {
        if (apply()) {
            accept();
        }
    });
    connect(cancel, &QPushButton::clicked, this, [this] {
        reload();
        reject();
    });
    connect(&session_, &qt::ProjectSession::documentsChanged, this, [this] {
        if (!dirty_) {
            reload();
        }
    });
    reload();
}

AliasesDialog::~AliasesDialog() = default;

void AliasesDialog::setDocument(project::Aliases doc) {
    doc_ = std::move(doc);
    dirty_ = true;
    ++loading_;
    aliases_->clear();
    for (const model::AliasRow& r : doc_.table.rows) {
        aliases_->addItem(qs(r.name));
    }
    current_ = -1;
    aliases_->setCurrentRow(doc_.table.rows.empty() ? -1 : 0);
    --loading_;
    showAlias(doc_.table.rows.empty() ? -1 : 0);
}

void AliasesDialog::reload() {
    setDocument(session_.aliases());
    dirty_ = false;
    showError(error_, {});
}

bool AliasesDialog::selectAlias(const QString& name) {
    for (int i = 0; i < static_cast<int>(doc_.table.rows.size()); ++i) {
        if (caseFold(doc_.table.rows[static_cast<std::size_t>(i)].name) == caseFold(name.toStdString())) {
            aliases_->setCurrentRow(i);
            return true;
        }
    }
    return false;
}

void AliasesDialog::showAlias(int row) {
    ++loading_;
    current_ = row >= 0 && row < static_cast<int>(doc_.table.rows.size()) ? row : -1;
    candidates_->clear();
    name_->setEnabled(current_ >= 0);
    candidates_->setEnabled(current_ >= 0);
    if (current_ < 0) {
        name_->clear();
    } else {
        const model::AliasRow& alias = doc_.table.rows[static_cast<std::size_t>(current_)];
        name_->setText(qs(alias.name));
        for (const model::AliasCandidate& c : alias.candidates) {
            auto* item = new QListWidgetItem(qs(c.name), candidates_);
            item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
            item->setCheckState(c.enabled ? Qt::Checked : Qt::Unchecked);
        }
    }
    --loading_;
}

void AliasesDialog::storeAlias() {
    if (loading_ > 0 || current_ < 0 || current_ >= static_cast<int>(doc_.table.rows.size())) {
        return;
    }
    model::AliasRow& alias = doc_.table.rows[static_cast<std::size_t>(current_)];
    alias.name = name_->text().trimmed().toStdString();
    alias.candidates.clear();
    for (int i = 0; i < candidates_->count(); ++i) {
        const QListWidgetItem* item = candidates_->item(i);
        alias.candidates.push_back(
            model::AliasCandidate{item->text().trimmed().toStdString(), item->checkState() == Qt::Checked});
    }
    dirty_ = true;
}

bool AliasesDialog::apply(QString* error) {
    storeAlias();
    QString problem;
    std::set<std::string> names;
    for (const model::AliasRow& r : doc_.table.rows) {
        if (r.name.empty()) {
            problem = tr("An alias needs a name.");
            break;
        }
        if (!names.insert(caseFold(r.name)).second) {
            problem = tr("The alias '%1' is defined twice.").arg(qs(r.name));
            break;
        }
        if (r.candidates.empty()) {
            problem = tr("The alias '%1' has no candidate.").arg(qs(r.name));
            break;
        }
        for (const model::AliasCandidate& c : r.candidates) {
            if (c.name.empty()) {
                problem = tr("The alias '%1' has an empty candidate.").arg(qs(r.name));
                break;
            }
        }
        if (!problem.isEmpty()) {
            break;
        }
        if (const std::optional<std::string> cycle = doc_.cycleThrough(r.name)) {
            problem = tr("Circular alias reference: %1").arg(qs(*cycle));
            break;
        }
    }
    if (!problem.isEmpty()) {
        showError(error_, problem);
        if (error != nullptr) {
            *error = problem;
        }
        return false;
    }
    showError(error_, {});
    if (!(doc_.table.rows.size() == session_.aliases().table.rows.size() &&
          doc_.toJson() == session_.aliases().toJson())) {
        session_.undoStack().push(new qt::EditAliasesCommand(session_, doc_, tr("Edit aliases")));
    }
    dirty_ = false;
    return true;
}

QString AliasesDialog::lastError() const {
    return error_->isHidden() ? QString() : error_->text();
}

// ---- MappingsDialog ---------------------------------------------------------------------------------------

namespace {
enum MappingColumn : int { FromCol, ToCol, UnitCol, UnitlessCol, MappingColumns };
} // namespace

MappingsDialog::MappingsDialog(qt::ProjectSession& session, QWidget* parent)
    : QDialog(parent), session_(session) {
    setWindowTitle(tr("Mappings"));
    setModal(false);
    auto* root = new QVBoxLayout(this);
    table_ = new QTableWidget(0, MappingColumns, this);
    table_->setHorizontalHeaderLabels(
        {tr("Logged name"), tr("Analysis name"), tr("Unit stamp"), tr("Unitless only")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->hide();
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setAccessibleName(tr("Mappings"));
    root->addWidget(table_, 1);
    auto* rowButtons = new QHBoxLayout;
    auto* add = button(tr("Add"), this);
    auto* remove = button(tr("Remove"), this);
    rowButtons->addWidget(add);
    rowButtons->addWidget(remove);
    rowButtons->addStretch(1);
    root->addLayout(rowButtons);
    error_ = errorLabel(this);
    root->addWidget(error_);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* ok = button(tr("OK"), this);
    auto* applyButton = button(tr("Apply"), this);
    auto* cancel = button(tr("Cancel"), this);
    buttons->addWidget(ok);
    buttons->addWidget(applyButton);
    buttons->addWidget(cancel);
    root->addLayout(buttons);

    connect(table_, &QTableWidget::itemChanged, this, [this] {
        if (loading_ == 0) {
            dirty_ = true;
        }
    });
    connect(add, &QPushButton::clicked, this, [this] {
        addRow(project::Mappings::Row{});
        dirty_ = true;
        const int row = table_->rowCount() - 1;
        table_->setCurrentCell(row, FromCol);
        table_->editItem(table_->item(row, FromCol));
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = table_->currentRow();
        if (row >= 0) {
            table_->removeRow(row);
            dirty_ = true;
        }
    });
    connect(applyButton, &QPushButton::clicked, this, [this] { static_cast<void>(apply()); });
    connect(ok, &QPushButton::clicked, this, [this] {
        if (apply()) {
            accept();
        }
    });
    connect(cancel, &QPushButton::clicked, this, [this] {
        reload();
        reject();
    });
    connect(&session_, &qt::ProjectSession::documentsChanged, this, [this] {
        if (!dirty_) {
            reload();
        }
    });
    reload();
}

MappingsDialog::~MappingsDialog() = default;

void MappingsDialog::addRow(const project::Mappings::Row& row) {
    ++loading_;
    const int r = table_->rowCount();
    table_->insertRow(r);
    table_->setItem(r, FromCol, new QTableWidgetItem(qs(row.from)));
    table_->setItem(r, ToCol, new QTableWidgetItem(qs(row.to)));
    table_->setItem(r, UnitCol, new QTableWidgetItem(row.unit ? qs(*row.unit) : QString()));
    auto* unitless = new QTableWidgetItem;
    unitless->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    unitless->setCheckState(row.unitlessOnly ? Qt::Checked : Qt::Unchecked);
    table_->setItem(r, UnitlessCol, unitless);
    --loading_;
}

void MappingsDialog::setDocument(const project::Mappings& doc) {
    base_ = doc;
    ++loading_;
    table_->setRowCount(0);
    for (const project::Mappings::Row& row : doc.rows) {
        addRow(row);
    }
    --loading_;
    dirty_ = true;
}

void MappingsDialog::reload() {
    setDocument(session_.mappings());
    dirty_ = false;
    showError(error_, {});
}

project::Mappings MappingsDialog::document() const {
    project::Mappings doc = base_;
    doc.rows.clear();
    for (int r = 0; r < table_->rowCount(); ++r) {
        const auto text = [this, r](int col) {
            const QTableWidgetItem* item = table_->item(r, col);
            return item != nullptr ? item->text().trimmed() : QString();
        };
        project::Mappings::Row row;
        row.from = text(FromCol).toStdString();
        row.to = text(ToCol).toStdString();
        if (const QString unit = text(UnitCol); !unit.isEmpty()) {
            row.unit = unit.toStdString();
        }
        const QTableWidgetItem* unitless = table_->item(r, UnitlessCol);
        row.unitlessOnly = unitless != nullptr && unitless->checkState() == Qt::Checked;
        doc.rows.push_back(std::move(row));
    }
    return doc;
}

bool MappingsDialog::apply(QString* error) {
    const project::Mappings doc = document();
    const std::shared_ptr<const units::UnitRegistry> units = session_.units();
    QString problem;
    std::set<std::string> from;
    for (const project::Mappings::Row& r : doc.rows) {
        if (r.from.empty() || r.to.empty()) {
            problem = tr("A mapping needs a logged name and an analysis name.");
            break;
        }
        if (!from.insert(caseFold(r.from)).second) {
            problem = tr("The logged name '%1' is mapped twice.").arg(qs(r.from));
            break;
        }
        if (r.unit && units != nullptr && !units->find(*r.unit)) {
            problem = tr("Unknown unit '%1' for '%2'.").arg(qs(*r.unit), qs(r.from));
            break;
        }
    }
    if (!problem.isEmpty()) {
        showError(error_, problem);
        if (error != nullptr) {
            *error = problem;
        }
        return false;
    }
    showError(error_, {});
    if (!(doc == session_.mappings())) {
        session_.undoStack().push(new qt::EditMappingsCommand(session_, doc, tr("Edit mappings")));
    }
    dirty_ = false;
    return true;
}

QString MappingsDialog::lastError() const {
    return error_->isHidden() ? QString() : error_->text();
}

bool MappingsDialog::selectMapping(const QString& loggedName) {
    for (int r = 0; r < table_->rowCount(); ++r) {
        const QTableWidgetItem* item = table_->item(r, FromCol);
        if (item != nullptr && item->text().compare(loggedName, Qt::CaseInsensitive) == 0) {
            table_->setCurrentCell(r, FromCol);
            table_->selectRow(r);
            return true;
        }
    }
    return false;
}

} // namespace esses::widgets
