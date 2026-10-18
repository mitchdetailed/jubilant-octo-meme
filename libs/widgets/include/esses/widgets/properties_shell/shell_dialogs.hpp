// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The small shell dialogs of the M3 hotkey rows (06 sections 4.1 item 3, 2.2, 14.4; 07 section 6.1):
//   SelectWorksheetDialog  view.select_worksheet (F7): a two-level list (workbook > worksheet) with a
//                          type-to-filter line; UP/DOWN move, ENTER selects the workbook and the worksheet
//                          (chosen()), ESC cancels; the active entry is preselected.
//   UnitsListDialog        tools.units_list: the unit table (quantity, symbol, display, scale to base,
//                          aliases), read-only, with a filter line.
//   OpenLogDialog          file.open_log (CTRL+O): a multi-select file dialog (Qt's own, so the side pane can
//                          be added) with a Details pane that probes the highlighted file through the
//                          importer registry (format, duration, channels, markers, details) without opening
//                          it; typed paths work as in any file dialog; the shell remembers the last folder.
// New public names (D-200). Owner: src/properties_shell/.
#pragma once

#include <QDialog>
#include <QFileDialog>
#include <QStringList>
#include <memory>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QTableWidget;
class QTreeWidget;

namespace esses::formats {
class ImporterRegistry;
} // namespace esses::formats

namespace esses::units {
class UnitRegistry;
} // namespace esses::units

namespace esses::qt {
class ProjectSession;
} // namespace esses::qt

namespace esses::widgets {

class SelectWorksheetDialog final : public QDialog {
    Q_OBJECT
public:
    // `activeWorkbook`, `activeWorksheet`: the entry preselected.
    SelectWorksheetDialog(const qt::ProjectSession& session, int activeWorkbook, int activeWorksheet,
                          QWidget* parent = nullptr);
    ~SelectWorksheetDialog() override;

    void setFilter(const QString& text);
    QLineEdit& filterEdit() const noexcept { return *filter_; }
    QTreeWidget& tree() const noexcept { return *tree_; }
    // The highlighted worksheet (workbook, worksheet); (-1, -1) for none.
    std::pair<int, int> current() const;
    // ENTER: emits chosen() for the highlighted worksheet and closes; false when none is highlighted.
    bool choose();

Q_SIGNALS:
    void chosen(int workbook, int worksheet);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuild();

    const qt::ProjectSession& session_;
    QLineEdit* filter_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    int activeWorkbook_ = -1;
    int activeWorksheet_ = -1;
};

class UnitsListDialog final : public QDialog {
    Q_OBJECT
public:
    UnitsListDialog(std::shared_ptr<const units::UnitRegistry> units, QWidget* parent = nullptr);
    ~UnitsListDialog() override;

    QTableWidget& table() const noexcept { return *table_; }
    void setFilter(const QString& text);
    // Rows not hidden by the filter.
    int visibleRows() const;

private:
    std::shared_ptr<const units::UnitRegistry> units_;
    QLineEdit* filter_ = nullptr;
    QTableWidget* table_ = nullptr;
};

class OpenLogDialog final : public QFileDialog {
    Q_OBJECT
public:
    // `registry` (may be null: no details) must outlive the dialog.
    OpenLogDialog(const formats::ImporterRegistry* registry, const QString& folder,
                  QWidget* parent = nullptr);
    ~OpenLogDialog() override;

    // The Details pane's text for a file ("" for none): what the probe found, or why it could not.
    QString details() const;
    // Probes `path` into the Details pane (also done when the highlighted file changes).
    void showDetails(const QString& path);

private:
    const formats::ImporterRegistry* registry_ = nullptr;
    QPlainTextEdit* details_ = nullptr;
};

} // namespace esses::widgets
