// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// WorksheetViewModel (06 section 4.5, 04 sections 2-3; D-016, D-018, D-133; AC-M1-01, AC-M1-09): one per
// worksheet; owns its domain, offsetAxis and size and composes the plot::ViewState copied to workers from
// them and the WorkspaceViewModel's shared zoom/cursor/datum. Its zoom/cursor/datum slots forward to the
// WorkspaceViewModel.
// setDomain (F9, D-016): with the Main range's DistanceChain covering the span the visible span converts
// through it (Time->Distance a' = chain.distAt(R.start + a); Distance->Time a' = chain.timeAt(a) - R.start);
// otherwise the fraction rule a' = fa * extent'(R) (zoomFrac); clamp to the outing extent; cursor and datum
// unchanged; emit Change::Domain. Without Corr Dist the "no distance data" banner shows for that source.
// N/P keep the zoom relative to the range start and the cursor/datum at the same lap distance in distance
// mode (04 section 2.1 item 1). GUI thread only.
// Additions (M1): activate() makes this the worksheet whose domain the shared cursor/zoom conversions follow
// (the first worksheet constructed is active until another is activated; 06 section 4.1 item 4) and applies
// the F9 conversion when the shared zoom is in the other domain; state() of an inactive worksheet shows the
// shared zoom converted on the fly. setActiveSamplePeriod is the active trace's sample period (zoom minimum
// of 4 samples, cursor steps; 0 = the fastest channel of the Main layer). setOffsetCommit receives the
// alignment offsets 250 ms after the last nudge (04 section 3.3 item 4; the undoable sidecar edit is M3).
// noDistance() is the banner state of AC-M1-09.
// Additions (M3, the time_distance completion; AC-M3-09): beginOffsetDrag/endOffsetDrag hold the commit while
// a sub-axis is dragged (no 250 ms commit in the middle of a slow drag) and commit at the release, so a drag
// is one undo command; offsetRangeBy nudges by an amount the caller computed; setSourceSamplePeriod records
// the sample period of the active channel in each visible slot's source (the focused graph reports them), so
// offsetRange steps a range by its own source's period (04 section 3.3 item 3), the Main source's
// activeSamplePeriod() when none was reported.
#pragma once

#include <esses/base/time.hpp>
#include <esses/model/range.hpp>
#include <esses/plotcore/change_mask.hpp>
#include <esses/plotcore/selection_snapshot.hpp>
#include <esses/plotcore/view_state.hpp>
#include <esses/project/workbook_file.hpp>

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

class QTimer;

namespace esses::qt {

class DataSelectionModel;
class DistanceService;
class WorkspaceViewModel;

enum class CursorSource : std::uint8_t { Click, Keyboard, Values, Report, Animation, Programmatic };

class WorksheetViewModel final : public QObject {
    Q_OBJECT
public:
    WorksheetViewModel(const project::WorksheetDef& def, WorkspaceViewModel& workspace,
                       DataSelectionModel& selection, DistanceService& distance, QObject* parent = nullptr);
    ~WorksheetViewModel() override;

    using OffsetCommit =
        std::function<void(const model::RangeRef& range, plot::Domain domain, double offset)>;

    const plot::ViewState& state() const noexcept { return state_; } // copied to workers
    plot::Domain domain() const noexcept { return state_.domain; }
    int widthPx() const noexcept { return widthPx_; }
    const project::WorksheetDef& definition() const noexcept { return def_; }
    DistanceService& distanceService() const noexcept { return distance_; }
    DataSelectionModel& selectionModel() const noexcept { return selection_; }
    bool isActive() const noexcept;
    // The Main source has no distance data and the worksheet is in distance mode (banner, AC-M1-09).
    bool noDistance() const noexcept { return noDistance_; }
    // The sample period of the active trace in microseconds (0 = automatic).
    Micros activeSamplePeriod() const;
    void setActiveSamplePeriod(Micros periodUs) { samplePeriodUs_ = periodUs; }
    void setAutoCentre(bool on) { autoCentre_ = on; }
    bool animating() const noexcept;
    double animationRate() const noexcept { return animationRate_; }
    void setOffsetCommit(OffsetCommit commit) { offsetCommit_ = std::move(commit); }
    bool offsetDragActive() const noexcept { return offsetDragDepth_ > 0; }
    // The offset step of a source's ranges in time mode: its reported period, else activeSamplePeriod().
    Micros samplePeriodOf(const model::SourceId& source) const;
    void setSourceSamplePeriod(const model::SourceId& source, Micros periodUs);
    // Pending alignment offsets during a drag or key repeat (04 section 3.3 item 4), in domain units.
    const std::unordered_map<model::RangeRef, double>& pendingOffsets() const noexcept {
        return pendingOffsets_;
    }

public Q_SLOTS:
    void activate();                            // worksheet switch (06 section 4.1 item 4)
    void setDomain(esses::plot::Domain domain); // F9 rule
    void toggleDomain();
    void zoomIn();          // x0.5 about cursorT; clamp to >= 4 samples of the active trace
    void zoomOut();         // x2
    void zoomFull();        // F2: the whole Main outing
    void zoomToMainRange(); // W: the Main range; cursor pulled inside
    void zoomToDatumMain(); // Z
    void zoomTo(esses::plot::XSpan span);
    void extendZoom(double x);         // SHIFT+click in the ZoomSummaryBar
    void panBy(double fractionOfSpan); // SHIFT+LEFT/RIGHT +-0.10, F/B +-0.90; clamped to the outing
    void centreOnCursor();             // H
    void setCursor(esses::Micros tMainLap, esses::qt::CursorSource source);
    void stepCursor(int screenPixels); // LEFT/RIGHT +-1, CTRL +-10
    void cursorHome();
    void cursorEnd();
    void placeDatum();
    void toggleDatum();
    void swapDatum();            // X
    void setOffsetAxis(bool on); // O
    void offsetRange(const esses::model::RangeRef& r, int steps);
    void offsetRangeBy(const esses::model::RangeRef& r, double amount); // nudge; committed 250 ms later
    void dragOffset(const esses::model::RangeRef& r, double delta);
    void beginOffsetDrag();
    void endOffsetDrag(); // commits the pending offsets now
    void onSelectionChanged(const esses::plot::SelectionSnapshot& snapshot);
    void setAnimating(bool on);
    void setAnimationRate(double x);
    void setSize(int widthPx);

Q_SIGNALS:
    void changed(esses::plot::ChangeMask what);
    void cursorInfo(const QString& t, const QString& d, const QString& lap, const QString& dt,
                    const QString& dd);
    // The Main source has no Corr Dist while in distance mode (banner; AC-M1-09).
    void noDistanceData(bool missing);

private:
    struct MainGeom; // the Main range's geometry, outing and DistanceChain (alignment offsets excluded)
    MainGeom mainGeom(const model::RangeRef& main) const;
    MainGeom mainGeom() const;
    plot::XSpan zoomIn(plot::Domain domain) const; // the shared zoom expressed in `domain`
    double xOfT(const MainGeom& m, plot::Domain domain, Micros t) const;
    Micros tOfX(const MainGeom& m, plot::Domain domain, double x) const;
    double minSpan(const MainGeom& m, plot::Domain domain) const;
    Micros clampCursor(const MainGeom& m, Micros t) const;
    void applyZoom(plot::XSpan span);
    void convertSharedZoom(plot::Domain to);
    void compose(plot::ChangeMask what);
    void onWorkspaceChanged(plot::ChangeMask what);
    // M1 integration: the units of a distance zoom set while the Main lap's index is pending (the fraction
    // rule's normalised units) and their conversion to metres when the index arrives (keeping zoomFrac).
    bool coversMain(const MainGeom& m) const;
    void noteDistanceUnits(const MainGeom& m);
    void onMainIndexArrived();
    void updateBanner();
    void emitCursorInfo();
    void animationTick();
    void commitOffsets();

    project::WorksheetDef def_;
    WorkspaceViewModel& workspace_;
    DataSelectionModel& selection_;
    DistanceService& distance_;
    plot::ViewState state_;
    std::unordered_map<model::RangeRef, double> pendingOffsets_;
    int widthPx_ = 0;
    model::RangeRef lastMain_;
    Micros samplePeriodUs_ = 0;
    bool autoCentre_ = false;
    bool noDistance_ = false;
    int updating_ = 0;              // > 0 while a slot updates the workspace (one changed() per slot)
    plot::ChangeMask deferred_ = 0; // workspace bits seen while updating_
    QTimer* animationTimer_ = nullptr;
    QElapsedTimer animationClock_;
    double animationRate_ = 1.0;
    double animationCarryUs_ = 0.0;
    QTimer* offsetTimer_ = nullptr;
    OffsetCommit offsetCommit_;
    int offsetDragDepth_ = 0;
    std::unordered_map<model::SourceId, Micros> sourcePeriods_;
};

} // namespace esses::qt
