// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathsEditor (CTRL+M; 03 section 11, 06 section 13.2, D-019, D-020, D-021, D-027, D-032): the modeless maths
// dialog. Three panes:
//   modules  the project's global modules in project order (enable box, Add / Delete / Move up / Move down /
//            Import / Export, the per-module condition over the details), a "Local maths" node with one
//            entry per open source (sidecar localMaths) and a "Transient" node per source that has filters
//            from a channel label (listed, read-only: they live for the session, 03 section 4.2);
//   items    the module's items and constants (enable box, kind, name, rate, unit, status from MathsService
//            for the Main source, comment); multi-select cut / copy / paste / delete / enable, paste keeps
//            the names;
//   item     the kind's page: Expression (name, quantity -> result and display units, rate Auto or Hz, DPS,
//            colour, interpolation, comment, the expression), Constant (name, value, unit, comment), Filter
//            (channel, points or seconds), Scale & Offset (channel, scale, offset with its unit); other kinds
//            (2D/3D tables, built-in, plugin, regression) are shown and preserved but not edited in 0.1.
// The expression field is monospace; 150 ms after each keystroke qt::LiveDiagnostics checks the text against
// the Main source's scope (setMainSource; else the first open source; a data-less scope with nothing open,
// where unknown names are I-UNRESOLVED) and the diagnostics line under it shows the first one (Errors red,
// Warnings amber, Info grey) with every span underlined and the full list below it. Typing inside the [...]
// after a channel reference shows the unit popup (the units of that channel's quantity; ENTER or TAB inserts
// the symbol and the closing bracket, ESC closes); Insert Range inserts a double-quoted group path of the
// Main source; Insert Channel inserts 'Name'[unit]; hovering an operator shows the precedence rule with the
// current unaryMinusAbovePower setting, hovering a function its signature and description.
// Editing rules (03 section 11 items 3 and 9, 06 section 13.2 item 6): Apply, or switching to another item
// or module, writes the edited item as one undo command (EditMathModulesCommand for a global module,
// SetLocalMathsCommand for local maths); an item with an Error is never applied, the switch is refused, and
// the dialog refuses to close while the edited item has an Error, offering Revert in a bar at its top;
// Warnings and Info never block. Structural edits (add, delete, paste, enable, reorder, condition) are undo
// commands at once. Auto-save on close (D-019): when this dialog changed a global module and a project is
// open, closing it saves the modules and their order (ProjectSession::saveMathsModules, D-190), leaving the
// project's other edits unsaved; local maths is already in the sidecar and transient maths is never saved.
// ESC closes like the close button. The text fields carry the property "esses.textEntry" (their own CTRL+Z is
// text undo). v1.0 (not built in 0.1): auto-complete, Insert Function, the 2D/3D table grids, enumerations
// editing, the built-in settings pages.
#pragma once

#include <esses/maths/diagnostics.hpp>
#include <esses/maths/math_item.hpp>
#include <esses/model/ids.hpp>

#include <QDialog>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

class QCloseEvent;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QLineEdit;
class QTableWidget;
class QTreeWidget;
class QWidget;

namespace esses::qt {
class MathsService;
class ProjectSession;
class SourceLoader;
} // namespace esses::qt

namespace esses::widgets {

class MathsEditor final : public QDialog {
    Q_OBJECT
public:
    static constexpr const char* kGeometryKey = "mathsEditor"; // layout-1 "dialogs" key (02 section 6.7)
    static constexpr const char* kItemsMimeType = "application/x-esses-maths-items"; // a maths-1 module

    // Where the edited items live.
    struct ModuleRef {
        enum class Scope : std::uint8_t { Global, Local, Transient };
        Scope scope = Scope::Global;
        QString moduleId;       // Global
        model::SourceId source; // Local, Transient
        friend bool operator==(const ModuleRef&, const ModuleRef&) = default;
    };

    MathsEditor(qt::ProjectSession& session, qt::MathsService& maths, qt::SourceLoader* loader = nullptr,
                QWidget* parent = nullptr);
    ~MathsEditor() override;

    // The source whose scope the live diagnostics, the unit popup, Insert Range and the status column use
    // (DataSelectionModel's Main); nullopt: the first open source, or the data-less scope.
    void setMainSource(std::optional<model::SourceId> source);
    std::optional<model::SourceId> mainSource() const;

    // Opens at an item or constant (Channel Status double-click, CTRL+SHIFT+M on a maths channel): a global
    // module id, or "local" (the Main source's local maths, else the first source whose local maths defines
    // it); false when not found or when the edited item cannot be left (the selection is unchanged).
    bool openAt(const QString& moduleId, const QString& itemName);
    bool openAt(const ModuleRef& module, const QString& itemName);
    std::optional<ModuleRef> currentModule() const;
    // The edited item's or constant's name ("" when none is selected).
    QString currentItemName() const;

    // The diagnostics of the edited item (after the debounce; tests call flushDiagnostics()): the
    // expression's and the page's own (empty name, unknown unit, a filter window below one sample).
    maths::Diagnostics currentDiagnostics() const;
    void flushDiagnostics();
    // False with the reason while the edited item has unapplied changes with an Error (03 section 11 item 3).
    bool canClose(QString* reason = nullptr) const;
    // Applies the edited item (one undo command); false when it has an Error (reason in the bar) or nothing
    // can be written (a read-only module, local maths without a SourceLoader). True when there is nothing to
    // apply.
    bool apply();
    // Discards the page's unapplied changes.
    void revert();
    bool isPageDirty() const noexcept;
    // The bar shown when an apply, a switch or the close was refused ("" when hidden).
    QString refusalText() const;

    // ---- module and item operations (the buttons and menus; each is one undo command) ---------------------
    bool addModule(const QString& id);
    bool removeModule(const QString& id);
    bool moveModule(const QString& id, int delta);
    bool setModuleEnabled(const QString& id, bool enabled);
    bool setModuleCondition(const QString& id, const QString& condition); // "" removes it
    bool importModule(const std::filesystem::path& file, QString* error = nullptr);
    bool exportModule(const QString& id, const std::filesystem::path& file, QString* error = nullptr) const;
    // A new item of `kind` (Expression, Filter, ScaleOffset) or a constant in the current module, selected.
    bool addItem(maths::ItemKind kind);
    bool addConstant();
    // On the selected rows of the items table.
    bool removeSelected();
    bool copySelected() const;
    bool cutSelected();
    bool paste();
    bool setSelectedEnabled(bool enabled);
    // Recalculate (03 section 11 item 11): the current module's outputs become Pending.
    void recalculate();

    // ---- expression assistance ----------------------------------------------------------------------------
    // The unit popup: whether it shows and its entries (unit symbols in registry order).
    bool unitPopupVisible() const;
    QStringList unitPopupEntries() const;
    // The units the popup would offer at a caret position of the expression (empty when the caret is not
    // inside the brackets after a channel reference whose quantity is known and has units).
    QStringList unitsAt(int position) const;
    // Inserts the popup's current entry (ENTER); false when the popup is hidden.
    bool acceptUnitPopup();
    // The group paths of the Main source's range tree (Insert Range) and the insertion at the caret.
    QStringList rangePaths() const;
    void insertRange(const QString& groupPath);
    // The channels of the Main source (Insert Channel) and the insertion of 'Name'[unit] at the caret.
    QStringList channelNames() const;
    void insertChannel(const QString& name);
    // The hover help at a character position of the expression ("" when nothing has help there).
    QString hoverText(int position) const;

    // ---- the panes (tests, accessibility) -----------------------------------------------------------------
    QTreeWidget& modulesTree() noexcept;
    QTableWidget& itemsTable() noexcept;
    QLineEdit& nameEdit() noexcept;
    QPlainTextEdit& expressionEdit() noexcept;
    QLabel& diagnosticsLine() noexcept;
    QListWidget& diagnosticsList() noexcept;
    QLabel& statusLine() noexcept;
    // The widget of a named field of the current page ("quantity", "resultUnit", "displayUnit", "rateAuto",
    // "rateHz", "dps", "colour", "interp", "comment", "constValue", "constUnit", "filterChannel",
    // "filterMode", "filterValue", "soChannel", "soScale", "soOffset", "soOffsetUnit"), or nullptr.
    QWidget* field(const QString& name) const;

Q_SIGNALS:
    // A failed auto-save or import (for the Messages dock, origin Maths).
    void message(const QString& text);

protected:
    void closeEvent(QCloseEvent* event) override;
    void reject() override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
    qt::ProjectSession& session_;
    qt::MathsService& maths_;
    qt::SourceLoader* loader_ = nullptr;
};

} // namespace esses::widgets
