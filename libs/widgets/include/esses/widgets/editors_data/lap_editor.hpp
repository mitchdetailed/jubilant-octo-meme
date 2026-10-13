// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LapEditor (06 section 13.3, D-014, D-016, D-032; AC-M2-05): the laps of the Main source (or the source
// picked in the combo): Number, Class, Start, Duration, Trusted, Stretch (% from analysis::LapStretch, blank
// outside the tolerance), Provenance of the start marker (Logged, Companion, User, Edited). Functions of 0.1:
// insert a beacon by absolute time, by lap time after a chosen lap or at the cursor (SHIFT+F7 equivalent; a
// user marker, laps rebuild synchronously); per-source beacon offset by distance with Apply (a MarkerEdit
// offsetDistance resolved by a worker from Corr Dist; "pending" until it lands); per-beacon time offsets;
// trusted flags; restore logged laps (one command). Ignored beacons are listed greyed; device resets show as
// Partial; terminology follows the profile. Every edit is one sidecar undo command. v1.0: auto-insert by lap
// distance and by GPS gate.
//
// As built (M2): rows are the laps of the profile's primary group ("Outings:Laps") in time order, then the
// ignored beacons of the primary marker group (IGRDBCN, or a beacon with an ignore edit) as greyed rows of
// class "Ignored" at their time. Times are session times (the layer's Micros) written m:ss.fff; typed times
// accept seconds, m:ss.fff and h:mm:ss.fff. Trust goes through DataSelectionModel::setTrusted (the Data
// window's command, undoable through connectSourceLoader with a session). The Stretch column and the stretch
// report come from qt::LapStretchService (setLapStretch); the "Lap stretching" check box is the same view
// state as the toolbar toggle. Per-beacon time offsets apply to the start beacon of the highlighted lap
// (beacons only: a lap that starts at the start of the outing has none). Until the offsetDistance resolver
// exists (docs/milestones/M2.md open issues) an applied beacon offset by distance stays "pending" and the
// laps do not move. The free functions below give the shell SHIFT+F7 (cursor.insert_marker) and keep the
// toolbar's lap-stretch toggle (data.lap_stretch_toggle) in step with the service.
#pragma once

#include <esses/base/time.hpp>
#include <esses/model/ids.hpp>
#include <esses/model/range.hpp>

#include <QDialog>
#include <QString>
#include <optional>
#include <string>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QTableView;

namespace esses::model {
struct DataSource;
} // namespace esses::model

namespace esses::qt {
class CommandRegistry;
class DataSelectionModel;
class LapStretchService;
class ProjectSession;
class SourceLoader;
class WorkspaceViewModel;
} // namespace esses::qt

namespace esses::widgets {

class CommandActions;
class LapRowsModel;

class LapEditor final : public QDialog {
    Q_OBJECT
public:
    static constexpr const char* kGeometryKey = "lapEditor"; // layout-1 "dialogs" key (02 section 6.7)

    enum Column : int {
        NumberCol = 0,
        ClassCol,
        StartCol,
        DurationCol,
        TrustedCol,
        StretchCol,
        ProvenanceCol,
        ColumnCount
    };

    LapEditor(qt::ProjectSession& session, qt::SourceLoader& loader, qt::DataSelectionModel& selection,
              qt::WorkspaceViewModel* workspace = nullptr, QWidget* parent = nullptr);
    ~LapEditor() override;

    QTableView& table() noexcept { return *table_; }
    QComboBox& sourceCombo() noexcept { return *sourceCombo_; }
    // The source whose laps are listed (the Main source by default).
    void setSource(const model::SourceId& source);
    const model::SourceId& source() const noexcept { return source_; }
    // The Stretch column, the report and the "Lap stretching" check box; null hides them.
    void setLapStretch(qt::LapStretchService* stretch);

    // Rows (time order): the lap of a row, nullopt for an ignored-beacon row or out of range.
    int rowCount() const;
    std::optional<model::RangeRef> lapAt(int row) const;
    // The row of a lap (by RangeId), -1 when not listed.
    int rowOf(const model::RangeRef& lap) const;
    // The highlighted row (-1 for none) and its lap.
    int currentRow() const;
    void setCurrentRow(int row);

    // Inserts a beacon at absolute layer time `t` (one InsertMarkerCommand); false outside the source.
    bool insertBeaconAt(Micros t);
    // Inserts a beacon `lapTime` after the start of the lap in `row`; false unless it falls inside that lap.
    bool insertBeaconAfterLap(int row, Micros lapTime);
    // SHIFT+F7: a beacon at the main cursor (Main.start + cursorT) of the Main source; false without a
    // workspace, a Main range or outside the source.
    bool insertBeaconAtCursor();
    // Applies the per-source beacon offset by distance in metres (one MarkerEditCommand; 0 removes it).
    bool applyBeaconOffsetDistance(double dM);
    // The source's beacon offset by distance (metres as written), nullopt when none; and whether it is still
    // waiting for its resolution through Corr Dist (the laps have not moved yet).
    std::optional<double> beaconOffsetDistance() const;
    bool beaconOffsetPending() const;
    // Time offset of the start beacon of the lap in `row` (one MarkerEditCommand; 0 removes it).
    bool setBeaconTimeOffset(int row, Micros dtUs);
    // Trusted flag of the lap in `row` (DataSelectionModel::setTrusted).
    bool setTrusted(int row, bool trusted);
    // Restore logged laps (one RestoreLoggedLapsCommand); false when nothing changes.
    bool restoreLogged();
    // The per-lap stretch report as tab-separated text (lap, length m, track length m, stretch %, applied).
    QString stretchReportText() const;
    // The profile's terms (D-036): "Lap", "Laps" and the editor title.
    const QString& rangeTerm() const noexcept { return rangeTerm_; }
    // The reason of the last refused action ("" after a successful one); also shown in the dialog.
    const QString& lastError() const noexcept { return error_; }

    // Time text of the editor: "m:ss.fff" (h:mm:ss.fff from an hour); parseTime reads seconds, m:ss.fff and
    // h:mm:ss.fff (nullopt otherwise).
    static QString formatTime(Micros t);
    static std::optional<Micros> parseTime(const QString& text);

Q_SIGNALS:
    // The listed rows changed (a new snapshot, another source, a stretch report).
    void rowsChanged();

private:
    std::shared_ptr<const model::DataSource> current() const;
    std::string layerName() const;
    std::string groupPath() const;
    std::string markerGroup() const;
    void rebuild();
    void refreshSources();
    void refreshControls();
    bool fail(const QString& reason);
    bool succeed();
    bool insertAt(const std::shared_ptr<const model::DataSource>& src, Micros t);
    void runInsert();

    qt::ProjectSession& session_;
    qt::SourceLoader& loader_;
    qt::DataSelectionModel& selection_;
    qt::WorkspaceViewModel* workspace_ = nullptr;
    qt::LapStretchService* stretch_ = nullptr;
    LapRowsModel* rows_ = nullptr;
    QTableView* table_ = nullptr;
    QComboBox* sourceCombo_ = nullptr;
    QLineEdit* insertTime_ = nullptr;
    QRadioButton* bySessionTime_ = nullptr;
    QRadioButton* byLapTime_ = nullptr;
    QPushButton* insertButton_ = nullptr;
    QPushButton* cursorButton_ = nullptr;
    QDoubleSpinBox* offsetDistance_ = nullptr;
    QPushButton* offsetDistanceApply_ = nullptr;
    QLabel* offsetDistanceState_ = nullptr;
    QDoubleSpinBox* beaconOffset_ = nullptr;
    QPushButton* beaconOffsetApply_ = nullptr;
    QPushButton* restoreButton_ = nullptr;
    QCheckBox* stretchToggle_ = nullptr;
    QPushButton* reportButton_ = nullptr;
    QLabel* status_ = nullptr;
    model::SourceId source_;
    bool followMain_ = true;
    QString rangeTerm_;
    QString rangesTerm_;
    QString error_;
};

// SHIFT+F7 (cursor.insert_marker, 06 sections 13.3 item 2 and 14.4): a user beacon in the Main source at the
// main cursor (Main.start + cursorT), one InsertMarkerCommand on the session's stack; laps rebuild at once.
// False without a Main range or when the time lies outside the source.
bool insertBeaconAtCursor(qt::ProjectSession& session, qt::SourceLoader& loader,
                          const qt::DataSelectionModel& selection, const qt::WorkspaceViewModel& workspace);
// Registers cursor.insert_marker (Graph context, Shift+F7) with insertBeaconAtCursor as its handler (enabled
// while a Main range exists). The objects must outlive the registry's use of the command.
void registerInsertBeaconCommand(qt::CommandRegistry& registry, qt::ProjectSession& session,
                                 qt::SourceLoader& loader, qt::DataSelectionModel& selection,
                                 qt::WorkspaceViewModel& workspace);
// The toolbar's lap-stretch toggle (data.lap_stretch_toggle of qt::registerLapStretchCommand, placed with
// CommandActions::populateToolBar): its shared QAction follows the service whoever changes it (this editor's
// check box, a project reopen), not only when CommandActions refreshes.
void connectLapStretchToggle(CommandActions& actions, qt::LapStretchService& stretch);

} // namespace esses::widgets
