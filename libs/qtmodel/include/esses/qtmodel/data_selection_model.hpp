// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DataSelectionModel (06 sections 5.1-5.6, D-017; AC-M1-01, AC-M1-07 candidate rule): one per workspace; owns
// the live plot::SelectionSnapshot (Main, Overlay 0, Overlays 1..n, hidden flag, single-colour Main) and the
// per-source Data-window filters, plus the list of open sources in Data-window order. Every component, dock
// and report reads this snapshot; maths receives a MathSelection copy built by MathsService. Selection
// changes are not undoable (D-032). GUI thread only. Operations (06 section 5.2): setMain (ENTER),
// toggleOverlay0 (CTRL+ENTER), toggleOverlay (ALT+ENTER; compact renumbering), toggleOverlaysHidden (F4),
// clearOverlays (SHIFT+F4), swapMainOverlay0 (Q), nextRange/prevRange (N/P, filtered displayed order within
// the Main source, no wrap), selectFastest (CTRL+F, the analysis::fastestLap candidate rule), selectAllFast
// (CTRL+ALT+F), setSingleColourMain (J), setTrusted, closeSource, sourcePublished, sourceReplaced (re-resolve
// by RangeId, then lap number, else drop), restore. Invariants (asserted in debug, tested in
// tst_dataselection): Main never in overlays nor equal to Overlay 0; Overlay 0 never in overlays;
// numbers 1..n contiguous; after clearOverlays() overlaysHidden == false.
//
// Deviations: the constructor takes a nullable ProjectSession* (06 writes ProjectSession&) so the selection
// rules are testable without a project; with a session the model reads options.fastLapTolerancePct and
// options.general.selectFastestOnLoad on projectOpened and writes lastSelection back on every change.
// setTrusted (no undo before M2) hands the edited sidecar to the SidecarCommit hook (the shell passes
// SourceLoader::commitSidecar), whose new snapshot comes back through sourceReplaced. Filters are kept per
// session in M1 (persisted in project.json dataWindow from M2). Additions: hasMain, displayedRanges,
// filterAccepts, selectionKeys, parseLapNumbers, setSelectFastestOnLoad, setSidecarCommit.
#pragma once

#include <esses/model/ids.hpp>
#include <esses/model/range.hpp>
#include <esses/model/sidecar.hpp>
#include <esses/model/source.hpp>
#include <esses/plotcore/selection_snapshot.hpp>
#include <esses/project/project_file.hpp>

#include <QFlags>
#include <QObject>
#include <QString>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace esses::qt {

class ProjectSession;

struct FilterSpec { // 06 section 5.4; persisted in project.json dataWindow.filters
    enum class Kind : std::uint8_t { All, Selected, Fastest, Fastest3, WithinOnePercent, LapNumbers };
    Kind kind = Kind::All;
    std::string laps; // LapNumbers: "3,5-7"
    friend bool operator==(const FilterSpec&, const FilterSpec&) = default;
};

class DataSelectionModel final : public QObject {
    Q_OBJECT
public:
    enum class Change : std::uint8_t {
        Main = 1,
        Overlay0 = 2,
        Overlays = 4,
        Hidden = 8,
        Colour = 16,
        Sources = 32,
        Filter = 64
    };
    Q_DECLARE_FLAGS(Changes, Change)

    // Persists a sidecar edit and publishes the new snapshot (SourceLoader::commitSidecar).
    using SidecarCommit =
        std::function<void(std::shared_ptr<const model::DataSource> current, model::SidecarDocument doc)>;

    explicit DataSelectionModel(ProjectSession* session = nullptr, QObject* parent = nullptr);
    ~DataSelectionModel() override;

    const plot::SelectionSnapshot& snapshot() const noexcept { return snapshot_; }
    // A Main range exists (exactly one exists while any open source has a range).
    bool hasMain() const noexcept;
    std::optional<model::RangeRef> referenceOverlay() const;
    std::vector<model::RangeRef> visibleOverlays() const; // empty while overlaysHidden
    int overlayNumber(const model::RangeRef& r) const;    // 0 = Overlay 0, 1..n, -1 = none
    FilterSpec filter(const model::SourceId& source) const;
    const std::vector<std::shared_ptr<const model::DataSource>>& sources() const noexcept { return sources_; }
    std::shared_ptr<const model::DataSource> source(const model::SourceId& id) const;
    // The Range a RangeRef names in the current snapshot of its source, or nullptr.
    const model::Range* resolve(const model::RangeRef& r) const;
    // The group path the Data window shows (the worksheet's linkTo view; "Outings:Laps" in M1).
    void setDisplayedGroupPath(std::string groupPath);
    const std::string& displayedGroupPath() const noexcept { return displayedGroupPath_; }
    // The displayed, filtered order of a group (06 section 5.2 N/P, Data window rows): the ranges of
    // `groupPath` in `layer` of `source` in start-time order that pass the source's filter; ranges in a
    // selection slot always stay (06 section 5.4). The default group is displayedGroupPath() in "Normal",
    // falling back to "Outings" when the source has no such group.
    std::vector<model::RangeRef> displayedRanges(const model::SourceId& source) const;
    std::vector<model::RangeRef> displayedRanges(const model::SourceId& source, std::string_view layer,
                                                 std::string_view groupPath) const;
    // The source's filter accepts `r` (slots are always accepted).
    bool filterAccepts(const model::RangeRef& r) const;
    bool applyToAllSources() const noexcept { return applyToAllSources_; }
    double fastLapTolerancePct() const noexcept { return fastLapTolerancePct_; }
    void setFastLapTolerancePct(double pct) { fastLapTolerancePct_ = pct < 0.0 ? 0.0 : pct; }
    // Options > General "select fastest lap on load": Main of a new source / after close = the fastest.
    bool selectFastestOnLoad() const noexcept { return selectFastestOnLoad_; }
    void setSelectFastestOnLoad(bool on) { selectFastestOnLoad_ = on; }
    void setSidecarCommit(SidecarCommit commit) { sidecarCommit_ = std::move(commit); }
    // The project.json lastSelection of the current snapshot (06 section 5.5); paths are written relative to
    // `projectFolder` when they lie under it.
    project::SelectionKeys selectionKeys(const std::filesystem::path& projectFolder = {}) const;
    // Lap-number filter syntax (06 section 5.4): expr := term (',' term)*; term := int | int '-' int;
    // whitespace ignored, descending ranges normalised. nullopt on a parse error.
    static std::optional<std::vector<std::pair<int, int>>> parseLapNumbers(std::string_view text);

public Q_SLOTS:
    void setMain(const model::RangeRef& r);
    void toggleOverlay0(const model::RangeRef& r);
    void toggleOverlay(const model::RangeRef& r);
    void toggleOverlaysHidden();
    void clearOverlays();
    void swapMainOverlay0();
    void nextRange();
    void prevRange();
    void selectFastest();
    void selectAllFast();
    void setSingleColourMain(bool on);
    void setTrusted(const model::RangeRef& r, bool trusted);
    void setFilter(const model::SourceId& source, const FilterSpec& spec);
    void applyFilterToAllSources();
    void sourcePublished(std::shared_ptr<const model::DataSource> source);
    void sourceReplaced(std::shared_ptr<const model::DataSource> source);
    void closeSource(const model::SourceId& source);
    void closeAllSources();
    void restore(const project::SelectionKeys& keys);

Q_SIGNALS:
    void changed(const esses::plot::SelectionSnapshot& snapshot, esses::qt::DataSelectionModel::Changes what);
    void filterChanged(const esses::model::SourceId& source);
    void sourcesChanged();
    // Status-bar and Messages hints (no-op commands: CTRL+F without candidates, CTRL+ENTER on Main, ...).
    void hint(const QString& text);

private:
    struct Candidates; // fastest-lap candidates of a source (analysis::fastestLap rule)
    void commit(Changes what);
    void checkInvariants() const;
    void readProjectOptions();
    bool removeFromSlots(const model::RangeRef& r, Changes& what);
    std::optional<model::RangeRef> firstRangeOf(const model::DataSource& src) const;
    std::optional<model::RangeRef> fastestOf(const model::DataSource& src) const;
    std::optional<model::RangeRef> initialMainOf(const model::DataSource& src) const;
    std::optional<model::RangeRef> reResolve(const model::DataSource& src, const model::RangeRef& r) const;
    void stepMain(int direction);

    ProjectSession* session_ = nullptr;
    plot::SelectionSnapshot snapshot_;
    std::vector<std::shared_ptr<const model::DataSource>> sources_;
    std::map<model::SourceId, FilterSpec> filters_;
    std::string displayedGroupPath_ = "Outings:Laps";
    double fastLapTolerancePct_ = 1.0;
    bool applyToAllSources_ = false;
    bool selectFastestOnLoad_ = false;
    SidecarCommit sidecarCommit_;
    // selectionKeys: the project.json path of a source per (source id, source path, project folder).
    mutable std::map<std::string, std::string> relativePaths_;
};

} // namespace esses::qt

Q_DECLARE_OPERATORS_FOR_FLAGS(esses::qt::DataSelectionModel::Changes)
