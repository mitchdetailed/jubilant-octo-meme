// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MappingPanel (05 section 8.1, 06 section 13.8, D-028, D-029, D-168; AC-M2-11): the non-modal panel that
// appears in the open flow (a bar above the worksheet area, the panel below) when an importer leaves unknown
// unit strings or channel names that no unit map or dictionary resolves. Table: Vendor name, Vendor unit,
// Sample values, Proposed analysis name, Quantity, Unit, Rule (nameOnly / unitOnly / nameAndUnit), Apply to
// future files, Status. Actions: Scan (re-run resolution against the project's unit map of the importer and
// the units table), Resolve (accept the proposals: task B through qt::applyMappingDecision and
// SourceLoader::resolveMapping), Save (the rows as the project's channels/unitmaps/<importerStem>.json, an
// undoable SetUnitMapCommand written with the project), Open Anyway (unknowns stay unitless), Cancel (drops
// the pending open). The open completes only after Resolve or Open Anyway; no worker waits for the panel. A
// complete unit map applies silently and the panel never appears. Decisions are undoable as project document
// edits ("Resolve mapping"); outcomes go to Messages through the loader's Import Log.
//
// As built (M2): the panel follows SourceLoader::mappingRequired() itself and queues further pending opens
// behind the one it shows (showPending() is the same entry for callers that drive it directly; an id already
// shown or queued is ignored). Proposals: a rule of the project's unit map for the importer, else the units
// table (a unit learnt since task A), else the importer's inferred quantity. Resolve writes the rows whose
// "Apply to future files" is ticked into the importer's unit map; the analysis names become project Mappings
// (not unit-map analysisName members, so one mapping serves every importer). Quantity, Unit and Rule cells
// edit through combo boxes whose choices are the ChoicesRole of the cell (units filtered by the quantity).
#pragma once

#include <esses/formats/mapping.hpp>
#include <esses/formats/unit_mapping_profile.hpp>
#include <esses/units/unit_registry.hpp>

#include <QString>
#include <QStringList>
#include <QWidget>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class QLabel;
class QPushButton;
class QTableView;

namespace esses::qt {
class ProjectSession;
class SourceLoader;
} // namespace esses::qt

namespace esses::widgets {

class MappingRowsModel;

class MappingPanel final : public QWidget {
    Q_OBJECT
public:
    enum Column : int {
        VendorNameCol = 0,
        VendorUnitCol,
        SamplesCol,
        AnalysisNameCol,
        QuantityCol,
        UnitCol,
        RuleCol,
        FutureCol,
        StatusCol,
        ColumnCount
    };
    // QStringList: the choices of a Quantity, Unit or Rule cell (its combo box editor).
    static constexpr int ChoicesRole = Qt::UserRole + 1;

    struct Row {
        std::string loggedName;
        std::string vendorUnit;
        std::vector<double> samples;
        std::string analysisName; // "" keeps the logged name
        std::string quantity;     // quantity name; "" while unresolved
        std::string unit;         // unit symbol; "" while unresolved
        formats::UnitMappingProfile::Match match = formats::UnitMappingProfile::Match::NameAndUnit;
        bool applyToFuture = true;
        QString origin; // where the proposal came from: "unit map", "units table", "inferred" or ""
        bool complete() const noexcept { return !quantity.empty() && !unit.empty(); }
    };

    MappingPanel(qt::SourceLoader& loader, qt::ProjectSession& session,
                 std::shared_ptr<const units::UnitRegistry> units, QWidget* parent = nullptr);
    ~MappingPanel() override;

    // Shows a pending open (SourceLoader::mappingRequired); queued behind the one shown, if any.
    void showPending(int pendingId, const formats::PendingResolution& pending);
    int pendingId() const noexcept { return pendingId_; }
    // True while a shown open waits for Resolve or Open Anyway (AC-M2-11).
    bool blocksPublish() const noexcept { return pendingId_ >= 0; }
    // Pending opens waiting behind the shown one.
    int queued() const noexcept { return static_cast<int>(queue_.size()); }
    const formats::PendingResolution& pending() const noexcept { return pending_; }
    QTableView& table() noexcept { return *table_; }
    // The bar above the table: the file, the number of rows to resolve and the queued opens.
    QLabel& bar() noexcept { return *bar_; }
    const std::vector<Row>& rows() const noexcept;

    // Row edits (the table's editors write through these). False, and nothing changes, for an unknown
    // quantity or unit, a unit outside the row's quantity or unitOnly for a row without a vendor unit. A unit
    // picked before a quantity sets the quantity; a quantity that the unit does not belong to clears it.
    bool setQuantity(int row, const QString& quantity);
    bool setUnit(int row, const QString& unit);
    bool setMatch(int row, formats::UnitMappingProfile::Match match);
    void setAnalysisName(int row, const QString& name);
    void setApplyToFuture(int row, bool on);
    // The reason of the last refused action ("" after a successful one); also shown under the table.
    const QString& lastError() const noexcept { return error_; }

    void scan();
    // Resolve: false (and nothing submitted) while a row has no quantity and unit.
    bool resolve();
    // Save: the complete rows into the project's unit map of the importer (SetUnitMapCommand); false with the
    // reason when nothing is shown or no row is complete. The open stays pending until Resolve.
    bool save(QString* error = nullptr);
    void openAnyway();
    void cancel();
    // The decision the current table describes: the complete rows as unit-map rules merged into the project's
    // map of the importer, and the analysis names as mappings.
    formats::MappingDecision decision() const;

    // The unit-map stem of the shown open ("generic.csv"); "" when nothing is shown.
    std::string importerStem() const;
    // The editors' choices: the quantities of the units table (Unitless excluded) and the units of a quantity
    // (every unit when "" is given); legacy-compatibility units are not offered.
    QStringList quantityChoices() const;
    QStringList unitChoices(const QString& quantity) const;

Q_SIGNALS:
    // blocksPublish() changed or another pending open is shown.
    void pendingChanged(bool blocking);
    // Status-bar and Messages hints ("Unit map saved", refused actions).
    void hint(const QString& text);

private:
    void enqueue(int pendingId, const formats::PendingResolution& pending);
    void present(int pendingId, formats::PendingResolution pending);
    void finish();
    void refresh();
    bool fail(const QString& reason);
    void submit(formats::MappingDecision decision, const formats::UnitMappingProfile* save);
    formats::UnitMappingProfile existingMap() const;
    formats::UnitMappingProfile mergedMap(bool futureOnly) const;

    qt::SourceLoader& loader_;
    qt::ProjectSession& session_;
    std::shared_ptr<const units::UnitRegistry> units_;
    MappingRowsModel* rows_ = nullptr;
    QTableView* table_ = nullptr;
    QLabel* bar_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* scanButton_ = nullptr;
    QPushButton* resolveButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QPushButton* anywayButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    int pendingId_ = -1;
    formats::PendingResolution pending_;
    std::deque<std::pair<int, formats::PendingResolution>> queue_;
    QString error_;
};

} // namespace esses::widgets
