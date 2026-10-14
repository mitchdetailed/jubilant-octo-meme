// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LayoutEditor (06 section 4.2, layout.editor CTRL+F7, MVP basics; D-027; 07 section 6.1): a modeless dialog
// with the tree of workbooks and their worksheets (the active item preselected): Move Up/Down, Copy
// To.../Move To... another workbook (components, linkTo and domain kept), inline rename (duplicates refused
// with the reason), Delete (confirmation naming the component count; never the last worksheet of a workbook
// or the last workbook), Duplicate ("<name> (2)"), New worksheet (name, linkTo from the profile's data views,
// domain) and New workbook (slugified file name), Import Workbook... (validate workbook-1, migrate,
// unresolved channel names to Messages) and Export Workbook... (never .local state), Lock Layout
// (project.json ui.layoutLocked). Every change is one QUndoCommand; ProjectSession::documentsChanged
// refreshes the tree.
// As built (M3): the edits are the qtmodel workbook commands (MoveWorksheetCommand, AddWorksheetCommand,
// RemoveWorksheetCommand, RenameWorksheetCommand, the workbook set commands; Move To is one undo macro of an
// add and a remove; Lock Layout a SetProjectOptionsCommand). A copied, moved or duplicated worksheet gets a
// fresh id and fresh component ids. The workbook's display name is also its file name (the session's
// workbook set), so a name with a character a file name cannot hold is refused instead of slugified (D-200).
// The questions (target workbook, names, confirmations, files) go through Prompts, which tests replace; the
// defaults are the standard Qt dialogs. Unresolved channel names of an imported workbook are reported
// through unresolvedChannels() and message().
#pragma once

#include <QDialog>
#include <QStringList>
#include <functional>
#include <optional>

class QCheckBox;
class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace esses::project {
struct WorkbookFile;
} // namespace esses::project

namespace esses::qt {
class ProjectSession;
} // namespace esses::qt

namespace esses::widgets {

class LayoutEditor final : public QDialog {
    Q_OBJECT
public:
    static constexpr const char* kGeometryKey = "layoutEditor";

    explicit LayoutEditor(qt::ProjectSession& session, QWidget* parent = nullptr);
    ~LayoutEditor() override;

    // Rebuilds the tree from the session's workbooks.
    void refresh();

    // Added in M3 (as built).
    struct Prompts {
        // A choice among `items` (the target workbook); nullopt = cancelled.
        std::function<std::optional<int>(const QString& title, const QStringList& items)> choose;
        // A line of text; nullopt = cancelled.
        std::function<std::optional<QString>(const QString& title, const QString& label,
                                             const QString& initial)>
            text;
        std::function<bool(const QString& question)> confirm;
        // A file to open (save = false) or write; "" = cancelled.
        std::function<QString(const QString& title, bool save)> file;
    };
    void setPrompts(Prompts prompts);
    // The channel names an imported workbook uses that no open source has (the shell's resolver).
    void setUnresolvedChannels(std::function<QStringList(const project::WorkbookFile&)> resolver);

    QTreeWidget& tree() const noexcept { return *tree_; }
    QCheckBox& lockBox() const noexcept { return *lock_; }
    // The highlighted item: (workbook, worksheet), worksheet -1 for a workbook row; (-1, -1) for none.
    std::pair<int, int> current() const;
    void setCurrent(int workbook, int worksheet = -1);
    // The reason the last edit was refused ("" when it was done).
    QString lastRefusal() const;

    // The actions (each one undo command; false when refused or cancelled).
    bool moveUp();
    bool moveDown();
    bool copyTo();
    bool moveTo();
    bool rename(const QString& name);
    bool remove();
    bool duplicate();
    bool newWorksheet();
    bool newWorkbook();
    bool importWorkbook();
    bool exportWorkbook();
    bool setLocked(bool locked);

Q_SIGNALS:
    void message(const QString& text); // Messages rows (unresolved channels of an import)

private:
    bool refuse(const QString& reason);
    bool move(int delta);
    bool copyOrMove(bool move);
    void onItemChanged(QTreeWidgetItem* item, int column);

    qt::ProjectSession& session_;
    QTreeWidget* tree_ = nullptr;
    QCheckBox* lock_ = nullptr;
    QLabel* status_ = nullptr;
    Prompts prompts_;
    std::function<QStringList(const project::WorkbookFile&)> unresolved_;
    QString refusal_;
    int refreshing_ = 0;
};

} // namespace esses::widgets
