// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LayoutEditor (06 section 4.2). Owner: src/properties_shell/.

#include <esses/base/uuid.hpp>
#include <esses/project/workbook_file.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/widgets/properties_shell/layout_editor.hpp>

#include <QCheckBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QUndoStack>
#include <QVBoxLayout>

namespace esses::widgets {

namespace {

constexpr int kBookRole = Qt::UserRole;
constexpr int kSheetRole = Qt::UserRole + 1;

int componentCount(const project::WorkbookFile& book) {
    int n = 0;
    for (const project::WorksheetDef& s : book.worksheets) {
        n += static_cast<int>(s.components.size());
    }
    return n;
}

// A copy of a worksheet under a fresh id with fresh component ids (build keys, undo merges and the
// properties dialogs stay per component).
project::WorksheetDef freshCopy(project::WorksheetDef sheet) {
    sheet.id = uuidRandom();
    for (project::ComponentSpec& c : sheet.components) {
        c.id = uuidRandom();
    }
    return sheet;
}

bool sameName(const std::string& a, const QString& b) {
    return QString::fromStdString(a).compare(b, Qt::CaseInsensitive) == 0;
}

} // namespace

LayoutEditor::LayoutEditor(qt::ProjectSession& session, QWidget* parent)
    : QDialog(parent), session_(session) {
    setObjectName(QStringLiteral("layoutEditor"));
    setWindowTitle(tr("Layout Editor"));
    setModal(false);
    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("layoutTree"));
    tree_->setHeaderLabels({tr("Workbook / worksheet"), tr("Components")});
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    lock_ = new QCheckBox(tr("Lock layout"), this);
    lock_->setObjectName(QStringLiteral("lockLayout"));
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("refusal"));
    status_->setWordWrap(true);
    status_->setStyleSheet(QStringLiteral("color: #c02020;"));

    auto* buttons = new QGridLayout;
    const auto button = [&](const QString& text, const char* name, int row, int col, auto slot) {
        auto* b = new QPushButton(text, this);
        b->setObjectName(QLatin1String(name));
        b->setAutoDefault(false);
        buttons->addWidget(b, row, col);
        connect(b, &QPushButton::clicked, this, [this, slot] { (this->*slot)(); });
    };
    button(tr("Move Up"), "moveUp", 0, 0, &LayoutEditor::moveUp);
    button(tr("Move Down"), "moveDown", 0, 1, &LayoutEditor::moveDown);
    button(tr("Copy To..."), "copyTo", 1, 0, &LayoutEditor::copyTo);
    button(tr("Move To..."), "moveTo", 1, 1, &LayoutEditor::moveTo);
    button(tr("Duplicate"), "duplicate", 2, 0, &LayoutEditor::duplicate);
    button(tr("Delete"), "delete", 2, 1, &LayoutEditor::remove);
    button(tr("New Worksheet..."), "newWorksheet", 3, 0, &LayoutEditor::newWorksheet);
    button(tr("New Workbook..."), "newWorkbook", 3, 1, &LayoutEditor::newWorkbook);
    button(tr("Import Workbook..."), "importWorkbook", 4, 0, &LayoutEditor::importWorkbook);
    button(tr("Export Workbook..."), "exportWorkbook", 4, 1, &LayoutEditor::exportWorkbook);
    auto* renameButton = new QPushButton(tr("Rename"), this);
    renameButton->setObjectName(QStringLiteral("rename"));
    renameButton->setAutoDefault(false);
    buttons->addWidget(renameButton, 5, 0);
    connect(renameButton, &QPushButton::clicked, this, [this] {
        if (QTreeWidgetItem* item = tree_->currentItem()) {
            tree_->editItem(item, 0);
        }
    });

    auto* row = new QHBoxLayout;
    row->addWidget(tree_, 1);
    auto* side = new QVBoxLayout;
    side->addLayout(buttons);
    side->addStretch(1);
    side->addWidget(lock_);
    row->addLayout(side);
    auto* column = new QVBoxLayout(this);
    column->addLayout(row, 1);
    column->addWidget(status_);
    auto* close = new QPushButton(tr("Close"), this);
    close->setAutoDefault(false);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    auto* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    bottom->addWidget(close);
    column->addLayout(bottom);
    resize(560, 420);

    prompts_.choose = [this](const QString& title, const QStringList& items) -> std::optional<int> {
        bool ok = false;
        const QString item = QInputDialog::getItem(this, title, title, items, 0, false, &ok);
        if (!ok) {
            return std::nullopt;
        }
        return static_cast<int>(items.indexOf(item));
    };
    prompts_.text = [this](const QString& title, const QString& label,
                           const QString& initial) -> std::optional<QString> {
        bool ok = false;
        const QString text = QInputDialog::getText(this, title, label, QLineEdit::Normal, initial, &ok);
        if (!ok) {
            return std::nullopt;
        }
        return text;
    };
    prompts_.confirm = [this](const QString& question) {
        return QMessageBox::question(this, windowTitle(), question) == QMessageBox::Yes;
    };
    prompts_.file = [this](const QString& title, bool save) {
        const QString filter = tr("Workbooks (*.workbook.json)");
        return save ? QFileDialog::getSaveFileName(this, title, QString(), filter)
                    : QFileDialog::getOpenFileName(this, title, QString(), filter);
    };

    connect(tree_, &QTreeWidget::itemChanged, this, &LayoutEditor::onItemChanged);
    connect(lock_, &QCheckBox::toggled, this, [this](bool on) {
        if (refreshing_ == 0) {
            setLocked(on);
        }
    });
    connect(&session_, &qt::ProjectSession::documentsChanged, this, &LayoutEditor::refresh);
    refresh();
}

LayoutEditor::~LayoutEditor() = default;

void LayoutEditor::setPrompts(Prompts prompts) {
    if (prompts.choose) {
        prompts_.choose = std::move(prompts.choose);
    }
    if (prompts.text) {
        prompts_.text = std::move(prompts.text);
    }
    if (prompts.confirm) {
        prompts_.confirm = std::move(prompts.confirm);
    }
    if (prompts.file) {
        prompts_.file = std::move(prompts.file);
    }
}

void LayoutEditor::setUnresolvedChannels(std::function<QStringList(const project::WorkbookFile&)> resolver) {
    unresolved_ = std::move(resolver);
}

QString LayoutEditor::lastRefusal() const {
    return refusal_;
}

bool LayoutEditor::refuse(const QString& reason) {
    refusal_ = reason;
    status_->setText(reason);
    return false;
}

void LayoutEditor::refresh() {
    ++refreshing_;
    std::pair<int, int> keep = current();
    if (keep.first < 0) {
        keep = {session_.activeWorkbook(), -1};
    }
    tree_->clear();
    const auto& books = session_.workbooks();
    for (std::size_t b = 0; b < books.size(); ++b) {
        auto* bookItem = new QTreeWidgetItem(
            tree_, {QString::fromStdString(books[b].name), QString::number(componentCount(books[b]))});
        bookItem->setData(0, kBookRole, static_cast<int>(b));
        bookItem->setData(0, kSheetRole, -1);
        bookItem->setFlags(bookItem->flags() | Qt::ItemIsEditable);
        for (std::size_t s = 0; s < books[b].worksheets.size(); ++s) {
            const project::WorksheetDef& sheet = books[b].worksheets[s];
            auto* sheetItem = new QTreeWidgetItem(
                bookItem, {QString::fromStdString(sheet.name), QString::number(sheet.components.size())});
            sheetItem->setData(0, kBookRole, static_cast<int>(b));
            sheetItem->setData(0, kSheetRole, static_cast<int>(s));
            sheetItem->setFlags(sheetItem->flags() | Qt::ItemIsEditable);
        }
        bookItem->setExpanded(true);
    }
    lock_->setChecked(session_.projectFile().layoutLocked);
    lock_->setEnabled(session_.isOpen());
    setCurrent(keep.first, keep.second);
    --refreshing_;
}

std::pair<int, int> LayoutEditor::current() const {
    const QTreeWidgetItem* item = tree_->currentItem();
    if (item == nullptr) {
        return {-1, -1};
    }
    return {item->data(0, kBookRole).toInt(), item->data(0, kSheetRole).toInt()};
}

void LayoutEditor::setCurrent(int workbook, int worksheet) {
    for (int b = 0; b < tree_->topLevelItemCount(); ++b) {
        QTreeWidgetItem* book = tree_->topLevelItem(b);
        if (book->data(0, kBookRole).toInt() != workbook) {
            continue;
        }
        if (worksheet < 0) {
            tree_->setCurrentItem(book);
            return;
        }
        for (int s = 0; s < book->childCount(); ++s) {
            if (book->child(s)->data(0, kSheetRole).toInt() == worksheet) {
                tree_->setCurrentItem(book->child(s));
                return;
            }
        }
        tree_->setCurrentItem(book);
        return;
    }
}

bool LayoutEditor::move(int delta) {
    const auto [b, s] = current();
    refusal_.clear();
    const auto& books = session_.workbooks();
    if (b < 0) {
        return refuse(tr("Choose a workbook or a worksheet"));
    }
    if (s < 0) {
        const int to = b + delta;
        if (to < 0 || to >= static_cast<int>(books.size())) {
            return false;
        }
        session_.undoStack().push(new qt::MoveWorkbookCommand(session_, b, to));
        setCurrent(to);
        return true;
    }
    const int to = s + delta;
    if (to < 0 || to >= static_cast<int>(books[static_cast<std::size_t>(b)].worksheets.size())) {
        return false;
    }
    session_.undoStack().push(new qt::MoveWorksheetCommand(session_, b, s, to));
    setCurrent(b, to);
    return true;
}

bool LayoutEditor::moveUp() {
    return move(-1);
}

bool LayoutEditor::moveDown() {
    return move(1);
}

bool LayoutEditor::copyOrMove(bool moving) {
    const auto [b, s] = current();
    refusal_.clear();
    const auto& books = session_.workbooks();
    if (b < 0 || s < 0) {
        return refuse(tr("Choose a worksheet"));
    }
    if (books.size() < 2) {
        return refuse(tr("There is no other workbook"));
    }
    const project::WorkbookFile& from = books[static_cast<std::size_t>(b)];
    if (moving && from.worksheets.size() < 2) {
        return refuse(tr("The last worksheet of a workbook cannot be moved"));
    }
    QStringList targets;
    std::vector<int> indexes;
    for (std::size_t i = 0; i < books.size(); ++i) {
        if (static_cast<int>(i) != b) {
            targets << QString::fromStdString(books[i].name);
            indexes.push_back(static_cast<int>(i));
        }
    }
    const std::optional<int> pick =
        prompts_.choose(moving ? tr("Move worksheet to") : tr("Copy worksheet to"), targets);
    if (!pick || *pick < 0 || *pick >= static_cast<int>(indexes.size())) {
        return false;
    }
    const int target = indexes[static_cast<std::size_t>(*pick)];
    const project::WorksheetDef sheet = from.worksheets[static_cast<std::size_t>(s)];
    QUndoStack& stack = session_.undoStack();
    if (moving) {
        stack.beginMacro(tr("Move worksheet to %1").arg(targets[*pick]));
        stack.push(new qt::AddWorksheetCommand(session_, target, sheet));
        stack.push(new qt::RemoveWorksheetCommand(session_, b, s));
        stack.endMacro();
    } else {
        project::WorksheetDef copy = freshCopy(sheet);
        stack.push(new qt::AddWorksheetCommand(session_, target, std::move(copy)));
    }
    setCurrent(target,
               static_cast<int>(session_.workbooks()[static_cast<std::size_t>(target)].worksheets.size()) -
                   1);
    return true;
}

bool LayoutEditor::copyTo() {
    return copyOrMove(false);
}

bool LayoutEditor::moveTo() {
    return copyOrMove(true);
}

bool LayoutEditor::rename(const QString& rawName) {
    const auto [b, s] = current();
    refusal_.clear();
    const QString name = rawName.trimmed();
    const auto& books = session_.workbooks();
    if (b < 0) {
        return refuse(tr("Choose a workbook or a worksheet"));
    }
    if (name.isEmpty()) {
        return refuse(tr("A name cannot be empty"));
    }
    if (s < 0) {
        static const QString bad = QStringLiteral("<>:\"/\\|?*");
        if (std::any_of(name.begin(), name.end(),
                        [](QChar c) { return bad.contains(c) || c.unicode() < 32; })) {
            return refuse(tr("A workbook name is also its file name and cannot hold any of %1").arg(bad));
        }
        for (std::size_t i = 0; i < books.size(); ++i) {
            if (static_cast<int>(i) != b && sameName(books[i].name, name)) {
                return refuse(tr("Another workbook is called \"%1\"").arg(name));
            }
        }
        if (sameName(books[static_cast<std::size_t>(b)].name, name) &&
            QString::fromStdString(books[static_cast<std::size_t>(b)].name) == name) {
            return false;
        }
        session_.undoStack().push(new qt::RenameWorkbookCommand(session_, b, name.toStdString()));
        setCurrent(b);
        return true;
    }
    const project::WorkbookFile& book = books[static_cast<std::size_t>(b)];
    for (std::size_t i = 0; i < book.worksheets.size(); ++i) {
        if (static_cast<int>(i) != s && sameName(book.worksheets[i].name, name)) {
            return refuse(tr("Another worksheet of \"%1\" is called \"%2\"")
                              .arg(QString::fromStdString(book.name), name));
        }
    }
    if (QString::fromStdString(book.worksheets[static_cast<std::size_t>(s)].name) == name) {
        return false;
    }
    session_.undoStack().push(new qt::RenameWorksheetCommand(session_, b, s, name.toStdString()));
    setCurrent(b, s);
    return true;
}

void LayoutEditor::onItemChanged(QTreeWidgetItem* item, int column) {
    if (refreshing_ > 0 || column != 0 || item == nullptr) {
        return;
    }
    const int b = item->data(0, kBookRole).toInt();
    const int s = item->data(0, kSheetRole).toInt();
    const QString text = item->text(0);
    // Later: the rename rebuilds the tree, which must not happen inside the item's own change.
    QTimer::singleShot(0, this, [this, b, s, text] {
        setCurrent(b, s);
        if (!rename(text)) {
            refresh(); // back to the stored name; the reason stays in the status line
            status_->setText(refusal_);
        }
    });
}

bool LayoutEditor::remove() {
    const auto [b, s] = current();
    refusal_.clear();
    const auto& books = session_.workbooks();
    if (b < 0) {
        return refuse(tr("Choose a workbook or a worksheet"));
    }
    const project::WorkbookFile& book = books[static_cast<std::size_t>(b)];
    if (s < 0) {
        if (books.size() < 2) {
            return refuse(tr("The last workbook of a project cannot be deleted"));
        }
        if (!prompts_.confirm(
                tr("Delete the workbook \"%1\" with %n component(s)?", nullptr, componentCount(book))
                    .arg(QString::fromStdString(book.name)))) {
            return false;
        }
        session_.undoStack().push(new qt::RemoveWorkbookCommand(session_, b));
        setCurrent(std::max(0, b - 1));
        return true;
    }
    if (book.worksheets.size() < 2) {
        return refuse(tr("The last worksheet of a workbook cannot be deleted"));
    }
    const project::WorksheetDef& sheet = book.worksheets[static_cast<std::size_t>(s)];
    if (!prompts_.confirm(tr("Delete the worksheet \"%1\" with %n component(s)?", nullptr,
                             static_cast<int>(sheet.components.size()))
                              .arg(QString::fromStdString(sheet.name)))) {
        return false;
    }
    session_.undoStack().push(new qt::RemoveWorksheetCommand(session_, b, s));
    setCurrent(b, std::max(0, s - 1));
    return true;
}

bool LayoutEditor::duplicate() {
    const auto [b, s] = current();
    refusal_.clear();
    if (b < 0 || s < 0) {
        return refuse(tr("Choose a worksheet"));
    }
    const project::WorkbookFile& book = session_.workbooks()[static_cast<std::size_t>(b)];
    project::WorksheetDef copy = freshCopy(book.worksheets[static_cast<std::size_t>(s)]);
    const QString base = QString::fromStdString(copy.name);
    for (int k = 2;; ++k) {
        const QString candidate = QStringLiteral("%1 (%2)").arg(base).arg(k);
        if (std::none_of(book.worksheets.begin(), book.worksheets.end(),
                         [&](const project::WorksheetDef& w) { return sameName(w.name, candidate); })) {
            copy.name = candidate.toStdString();
            break;
        }
    }
    session_.undoStack().push(new qt::AddWorksheetCommand(session_, b, std::move(copy), s + 1));
    setCurrent(b, s + 1);
    return true;
}

bool LayoutEditor::newWorksheet() {
    auto [b, s] = current();
    refusal_.clear();
    if (b < 0) {
        b = std::max(0, session_.activeWorkbook());
    }
    if (b >= static_cast<int>(session_.workbooks().size())) {
        return refuse(tr("There is no workbook"));
    }
    const std::optional<QString> name = prompts_.text(tr("New Worksheet"), tr("Name"), tr("Worksheet"));
    if (!name || name->trimmed().isEmpty()) {
        return false;
    }
    project::WorksheetDef sheet;
    sheet.name = name->trimmed().toStdString();
    QStringList views;
    for (const project::DataView& v : session_.profile().dataViews) {
        views << QString::fromStdString(v.title);
    }
    if (views.size() > 1) {
        const std::optional<int> pick = prompts_.choose(tr("Data view of the worksheet"), views);
        if (!pick || *pick < 0 || *pick >= views.size()) {
            return false;
        }
        sheet.linkTo = views[*pick].toStdString();
    } else if (views.size() == 1) {
        sheet.linkTo = views.front().toStdString();
    }
    const std::optional<int> domain = prompts_.choose(tr("Domain"), {tr("Time"), tr("Distance")});
    if (!domain) {
        return false;
    }
    sheet.domain = *domain == 1 ? "distance" : "time";
    const project::WorkbookFile& book = session_.workbooks()[static_cast<std::size_t>(b)];
    const int at = s < 0 ? static_cast<int>(book.worksheets.size()) : s + 1;
    session_.undoStack().push(new qt::AddWorksheetCommand(session_, b, std::move(sheet), at));
    setCurrent(b, at);
    return true;
}

bool LayoutEditor::newWorkbook() {
    refusal_.clear();
    const std::optional<QString> name = prompts_.text(tr("New Workbook"), tr("Name"), tr("Workbook"));
    if (!name) {
        return false;
    }
    const QString n = name->trimmed();
    static const QString bad = QStringLiteral("<>:\"/\\|?*");
    if (n.isEmpty() ||
        std::any_of(n.begin(), n.end(), [](QChar c) { return bad.contains(c) || c.unicode() < 32; })) {
        return refuse(
            tr("A workbook name is also its file name and cannot be empty or hold any of %1").arg(bad));
    }
    for (const project::WorkbookFile& wb : session_.workbooks()) {
        if (sameName(wb.name, n)) {
            return refuse(tr("Another workbook is called \"%1\"").arg(n));
        }
    }
    project::WorkbookFile book;
    book.name = n.toStdString();
    book.profile = session_.profile().id.empty() ? std::string("circuit") : session_.profile().id;
    project::WorksheetDef sheet;
    sheet.id = uuidRandom();
    sheet.name = "Worksheet";
    if (!session_.profile().dataViews.empty()) {
        sheet.linkTo = session_.profile().dataViews.front().title;
    }
    book.worksheets.push_back(std::move(sheet));
    session_.undoStack().push(new qt::AddWorkbookCommand(session_, std::move(book)));
    setCurrent(static_cast<int>(session_.workbooks().size()) - 1);
    return true;
}

bool LayoutEditor::importWorkbook() {
    refusal_.clear();
    const QString file = prompts_.file(tr("Import Workbook"), false);
    if (file.isEmpty()) {
        return false;
    }
    std::vector<std::string> log;
    Result<project::WorkbookFile> loaded = project::WorkbookFile::load(file.toStdWString(), log);
    if (!loaded) {
        return refuse(tr("The workbook could not be imported: %1")
                          .arg(QString::fromStdString(loaded.error().toString())));
    }
    project::WorkbookFile book = std::move(loaded).value();
    for (const project::WorkbookFile& wb : session_.workbooks()) {
        if (wb.id == book.id) {
            book.id = Uuid(); // a second copy of a workbook gets its own perspective key
        }
    }
    // The name is the workbook's file name (workbooks/<name>.workbook.json): the rule of New and Rename
    // applies, so an imported name with a path or reserved character, or one another workbook has, gets a
    // safe unique name and the user is told (D-203 (p)).
    {
        static const QString bad = QStringLiteral("<>:\"/\\|?*");
        const QString given = QString::fromStdString(book.name);
        QString safe;
        for (const QChar c : given) {
            safe.append(bad.contains(c) || c.unicode() < 32 ? QLatin1Char('_') : c);
        }
        safe = safe.trimmed();
        if (safe.isEmpty()) {
            safe = tr("Workbook");
        }
        const auto taken = [this](const QString& n) {
            const auto& books = session_.workbooks();
            return std::any_of(books.begin(), books.end(),
                               [&n](const project::WorkbookFile& wb) { return sameName(wb.name, n); });
        };
        QString unique = safe;
        for (int k = 2; taken(unique); ++k) {
            unique = QStringLiteral("%1 (%2)").arg(safe).arg(k);
        }
        if (unique != given) {
            Q_EMIT message(
                tr("The imported workbook \"%1\" is named \"%2\" (a workbook name is also its file "
                   "name)")
                    .arg(given, unique));
            book.name = unique.toStdString();
        }
    }
    for (const std::string& line : log) {
        Q_EMIT message(QString::fromStdString(line));
    }
    if (unresolved_) {
        const QStringList missing = unresolved_(book);
        if (!missing.isEmpty()) {
            Q_EMIT message(tr("The imported workbook \"%1\" uses channels no open log has: %2")
                               .arg(QString::fromStdString(book.name), missing.join(QStringLiteral(", "))));
        }
    }
    session_.undoStack().push(new qt::AddWorkbookCommand(session_, std::move(book)));
    setCurrent(static_cast<int>(session_.workbooks().size()) - 1);
    return true;
}

bool LayoutEditor::exportWorkbook() {
    const auto [b, s] = current();
    refusal_.clear();
    static_cast<void>(s);
    if (b < 0) {
        return refuse(tr("Choose a workbook"));
    }
    const QString file = prompts_.file(tr("Export Workbook"), true);
    if (file.isEmpty()) {
        return false;
    }
    // The workbook document only: no .local state (layout, perspectives) travels with it.
    if (const Result<void> r = session_.workbooks()[static_cast<std::size_t>(b)].save(file.toStdWString());
        !r) {
        return refuse(
            tr("The workbook could not be written: %1").arg(QString::fromStdString(r.error().toString())));
    }
    return true;
}

bool LayoutEditor::setLocked(bool locked) {
    refusal_.clear();
    if (!session_.isOpen()) {
        return refuse(tr("Open a project to lock its layout"));
    }
    if (session_.projectFile().layoutLocked == locked) {
        return false;
    }
    project::ProjectFile next = session_.projectFile();
    next.layoutLocked = locked;
    session_.undoStack().push(new qt::SetProjectOptionsCommand(
        session_, std::move(next), locked ? tr("Lock layout") : tr("Unlock layout")));
    return true;
}

} // namespace esses::widgets
