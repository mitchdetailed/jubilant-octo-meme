// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackEditor (06 section 13.5; D-027; 07 section 6.1; AC-M3-04; D-199): the modeless Track Editor (Tools ->
// Track Editor, and from the Track Map dock).
//   1. source lap picker (every lap of the open sources; Main by default); method GPS (lat/lon read through
//      MathsService as the esses.gps_corrections chain heads, so (0, 0) fixes are invalid; optional inner and
//      outer laps of the same source; smoothing) or Speed + Lateral G (Corr Speed and a lateral-G channel,
//      chosen among the source's acceleration channels; "positive to the right" inverts it)
//   2. Generate (qt::TrackMapService::generate: analysis::TrackGenerator on a worker) -> a preview with the
//      closure error and RMS fit; Accept writes trackmaps/<venue>.track.json through TrackMapService (one
//      undoable TrackMapCommand; the previous file goes to backup/). The dialog stays open (it is modeless
//      and the sections are edited next); Close hides it
//   3. default sections (analysis::SectionDetector::defaults, Turns/Straights, >= 10 m) at the first
//      acceptance; a later acceptance keeps the map's user sections and regenerates the default ones (one
//      command); "Regenerate default sections"
//   4. sections table: name, set, start, end, category, speed class (M4: none, slow or fast), user-defined;
//      add (numerically, or from the cursor span the shell passes with setCursorSpan), edit, delete; >= 10 m
//      and inside the track enforced; user sections go to a named set ("User" by default) and may overlap
//      other sets. M4 (D-211 (c)): the table and the start / end fields show and edit the bounds in the Corr
//      Dist display unit (distanceUnit(): plot::corrDistDisplayUnit of the project's Corr Dist and Corr
//      Speed overrides, m without them), followed when the overrides change; the API below stays in metres
//   6. rename (moves the map to another venue's file) / delete the track file; "Auto-generate on open" (the
//      project option trackMaps.autoGenerateOnOpen, a SetProjectOptionsCommand)
// Every edit is one QUndoCommand on the project's stack (02 section 7). A generation whose maths inputs are
// still being evaluated waits and starts again on MathsService::channelReady / statusChanged. The contract's
// SourceLoader parameter is gone: the laps come from the DataSelectionModel's sources (D-199).
#pragma once

#include <esses/analysis/track_generator.hpp>
#include <esses/analysis/track_map.hpp>
#include <esses/model/range.hpp>

#include <QDialog>
#include <QString>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QTableWidget;

namespace esses::qt {
class DataSelectionModel;
class MathsService;
class ProjectSession;
class TrackMapService;
} // namespace esses::qt

namespace esses::widgets {

namespace detail {
class TrackPreview;
} // namespace detail

class TrackEditor final : public QDialog {
    Q_OBJECT
public:
    static constexpr const char* kUserSet = "User";

    TrackEditor(qt::ProjectSession& session, qt::DataSelectionModel& selection, qt::MathsService& maths,
                qt::TrackMapService& maps, QWidget* parent = nullptr);
    ~TrackEditor() override;

    // The venue whose map is being edited ("" before a source is selected).
    QString venue() const;
    // 1. the picked lap (every lap of the open sources; the Main lap by default) and the method.
    std::optional<model::RangeRef> lap() const;
    bool setLap(const model::RangeRef& lap);
    analysis::TrackMethod method() const;
    void setMethod(analysis::TrackMethod method);
    bool setInnerLap(const std::optional<model::RangeRef>& lap);
    bool setOuterLap(const std::optional<model::RangeRef>& lap);
    void setSmoothing(double metres);
    void setLateralChannel(const QString& name);
    // Generates a preview from the picked lap with the picked method (asynchronous; previewReady or
    // generationFailed follows).
    void generate();
    bool generating() const noexcept { return generating_; }
    bool waitingForInputs() const noexcept { return waiting_; }
    std::shared_ptr<const analysis::TrackMap> preview() const noexcept { return preview_; }
    const analysis::TrackGenerationReport& previewReport() const noexcept { return report_; }
    QString statusText() const;
    // Writes the previewed map (and default sections at the first acceptance); the dialog stays open.
    void accept() override;

    // 3-4. the venue's stored map and its sections (all one undo command each).
    std::shared_ptr<const analysis::TrackMap> currentMap() const;
    bool regenerateDefaultSections();
    // The track-distance span "add from cursor span" uses (the shell sets it from the worksheet); nullopt
    // disables the button.
    void setCursorSpan(std::optional<std::pair<double, double>> span);
    // `speedClass`: "" (none), "slow" or "fast" (analysis::TrackMap::kSpeedClasses); editSection keeps the
    // section's class when it is nullopt.
    bool addSection(const QString& name, double startM, double endM, const QString& category,
                    const QString& set = QString::fromLatin1(kUserSet), const QString& speedClass = {});
    bool addSectionFromCursorSpan(const QString& name);
    bool editSection(const QString& id, const QString& name, double startM, double endM,
                     const QString& category, const std::optional<QString>& speedClass = std::nullopt);
    // M4 (D-211 (c)): the symbol of the unit the section bounds are shown and edited in (the Corr Dist
    // display unit) and the form's fields in it (tests).
    QString distanceUnit() const;
    QDoubleSpinBox* startField() const noexcept { return start_; }
    QDoubleSpinBox* endField() const noexcept { return end_; }
    QComboBox* speedClassField() const noexcept { return speedClass_; }
    bool deleteSection(const QString& id);
    // 6. files and the project option.
    bool renameVenue(const QString& to);
    bool deleteMap();
    bool autoGenerateOnOpen() const;
    void setAutoGenerateOnOpen(bool on);
    // The last refusal or error ("" when the last edit succeeded).
    QString lastError() const { return error_; }

    QTableWidget* sectionsTable() const noexcept { return table_; }

Q_SIGNALS:
    void previewReady();
    void generationFailed(const QString& reason);
    void mapWritten();

private:
    void refreshLaps();
    void refreshSections();
    void refreshAvailability();
    void loadForm(int row);
    // M4 (D-211 (c)): the Corr Dist display unit's conversions (metres <-> shown) and the fields' units.
    double toShown(double metres) const;
    double toMetres(double shown) const;
    void applyDistanceUnit();
    void setStatus(const QString& text);
    bool pushMap(std::shared_ptr<const analysis::TrackMap> after, const QString& text,
                 bool backupFirst = false);
    static void keepEditedDefaults(analysis::TrackMap& map);
    bool checkSection(const analysis::TrackMap& map, double startM, double endM, const QString& name);
    bool checkSpeedClass(const std::string& speedClass);
    std::optional<model::RangeRef> lapAt(const QComboBox* combo) const;
    std::string venueOf(const model::RangeRef& lap) const;

    qt::ProjectSession& session_;
    qt::DataSelectionModel& selection_;
    qt::MathsService& maths_;
    qt::TrackMapService& maps_;

    std::vector<model::RangeRef> laps_;
    std::shared_ptr<const analysis::TrackMap> preview_;
    analysis::TrackGenerationReport report_;
    std::optional<std::pair<double, double>> cursorSpan_;
    std::uint64_t generation_ = 0;
    bool generating_ = false;
    bool waiting_ = false;
    bool retryScheduled_ = false;
    QString error_;

    QLabel* venue_ = nullptr;
    QComboBox* lapCombo_ = nullptr;
    QRadioButton* gps_ = nullptr;
    QRadioButton* speedLatG_ = nullptr;
    QComboBox* inner_ = nullptr;
    QComboBox* outer_ = nullptr;
    QDoubleSpinBox* smoothing_ = nullptr;
    QComboBox* lateral_ = nullptr;
    QCheckBox* invert_ = nullptr;
    QPushButton* generate_ = nullptr;
    QPushButton* accept_ = nullptr;
    QLabel* status_ = nullptr;
    detail::TrackPreview* previewView_ = nullptr;
    QTableWidget* table_ = nullptr;
    QLineEdit* name_ = nullptr;
    QLineEdit* set_ = nullptr;
    QDoubleSpinBox* start_ = nullptr;
    QDoubleSpinBox* end_ = nullptr;
    QComboBox* category_ = nullptr;
    QComboBox* speedClass_ = nullptr; // M4: "" / slow / fast as item data
    QPushButton* add_ = nullptr;
    QPushButton* addSpan_ = nullptr;
    QPushButton* apply_ = nullptr;
    QPushButton* delete_ = nullptr;
    QPushButton* defaults_ = nullptr;
    QLineEdit* renameTo_ = nullptr;
    QPushButton* rename_ = nullptr;
    QPushButton* deleteMap_ = nullptr;
    QCheckBox* autoGenerate_ = nullptr;
};

} // namespace esses::widgets
