// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DetailsEditor (CTRL+D; 06 section 13.4, D-015, D-032): one column per open source, one row per detail (the
// profile's standard details plus any custom ones). Double-click edits a typed cell (text, number with unit,
// date-time); empty strings are allowed and numbers are not rounded on reopen. Each edit is a
// SetDetailCommand into the source's sidecar with provenance User and the original value kept; edited cells
// show a dot and "Restore original" reverts one cell. Column sort, Copy as TSV. Renaming the Venue detail
// prompts to rename or regenerate the track map. v1.0: custom details add/remove, column reorder.
//
// As built (M2): rows are the profile's details in profile order, then every other detail id of the open
// sources in first-seen order, except the derived ones (the File and Statistics groups of 02 section 4.2,
// computed and never written to the sidecar). The type of a cell is that of the source's value, else the
// profile's ("string" / "number"), else that of the standard detail, else text. A number is typed as
// "<value> [unit]" (the unit must be one of the units table; without one the cell keeps its unit, else the
// profile's) and keeps the digits as typed (its display decimals follow them), a date-time as ISO 8601 with
// an offset. A refused edit leaves the cell unchanged and shows the reason. The header of column 0 sorts by
// detail name, a source column by its values; sorting off restores the profile order. Track maps arrive in
// M3: a Venue rename emits venueRenamed() and the shell asks then (no prompt in M2).
#pragma once

#include <esses/model/detail.hpp>
#include <esses/model/ids.hpp>
#include <esses/units/unit_registry.hpp>

#include <QDialog>
#include <QString>
#include <QStringList>
#include <optional>
#include <string>

class QLabel;
class QPushButton;
class QSortFilterProxyModel;
class QTableView;

namespace esses::qt {
class ProjectSession;
class SourceLoader;
} // namespace esses::qt

namespace esses::widgets {

class DetailsModel;

class DetailsEditor final : public QDialog {
    Q_OBJECT
public:
    static constexpr const char* kGeometryKey = "detailsEditor";
    // Qt::ItemDataRole of a source cell: true when the sidecar holds a user value for it (the dot).
    static constexpr int EditedRole = Qt::UserRole + 1;

    DetailsEditor(qt::ProjectSession& session, qt::SourceLoader& loader, QWidget* parent = nullptr);
    ~DetailsEditor() override;

    QTableView& grid() noexcept { return *grid_; }
    // The model the grid shows (sorting proxy over the details).
    QSortFilterProxyModel& viewModel() noexcept { return *proxy_; }
    void refresh();
    // The grid as tab-separated text in the shown order (Copy as TSV); copyAsTsv() puts it on the clipboard.
    QString tableText() const;
    void copyAsTsv() const;

    // Detail ids in the profile-first row order and the open sources in column order (column = index + 1).
    QStringList detailIds() const;
    int sourceCount() const;
    // The text a cell shows / edits ("" when the source has no such detail).
    QString cellText(const QString& detailId, int sourceIndex) const;
    // Sets a cell from text (one SetDetailCommand); false with lastError() when the text does not parse for
    // the cell's type or nothing changes.
    bool setCell(const QString& detailId, int sourceIndex, const QString& text);
    // Restore original: removes the user value of one cell (one SetDetailCommand); false when it has none.
    bool restoreOriginal(const QString& detailId, int sourceIndex);
    bool isEdited(const QString& detailId, int sourceIndex) const;
    const QString& lastError() const noexcept { return error_; }

    // Parses cell text for a detail type (06 section 13.4 item 1): text as is (empty allowed), "<number>
    // [unit]" with `defaultSymbol` when no unit is typed (a typed unit must be one of `units`, of the default
    // unit's quantity when it has one; null `units` checks nothing), an ISO 8601 date-time with an offset;
    // nullopt with `error` set otherwise.
    static std::optional<model::DetailValue> parseValue(model::DetailType type, const QString& text,
                                                        const std::string& defaultSymbol,
                                                        const units::UnitRegistry* units, QString* error);
    // The edit text of a value: numbers with every significant digit and their unit symbol.
    static QString valueText(const model::DetailValue& value);

Q_SIGNALS:
    // The Venue detail of a source changed through this editor (track maps follow in M3).
    void venueRenamed(const esses::model::SourceId& source, const QString& before, const QString& after);

private:
    void showContextMenu(const QPoint& pos);
    bool fail(const QString& reason);

    qt::ProjectSession& session_;
    qt::SourceLoader& loader_;
    DetailsModel* model_ = nullptr;
    QSortFilterProxyModel* proxy_ = nullptr;
    QTableView* grid_ = nullptr;
    QPushButton* restoreButton_ = nullptr;
    QLabel* status_ = nullptr;
    QString error_;
};

} // namespace esses::widgets
