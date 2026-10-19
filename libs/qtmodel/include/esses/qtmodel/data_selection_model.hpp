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
// setTrusted hands the edited sidecar to the SidecarCommit hook; connectSourceLoader makes it an undoable
// SidecarCommand on the session's stack (D-032) committed through SourceLoader::commitSidecar, whose new
// snapshot comes back through sourceReplaced.
// Data-window filters (06 section 5.4, M2): with a session they are project.json dataWindow (view state,
// written without the undo stack, D-032): "default" (the filter of a source without its own entry) and one
// entry per source by quick hash ("xxh3:<hex>", D-162), restored unconditionally when the project opens and
// when a source publishes, kept for sources that are closed; applyToAllSources is the toolbar flag. A
// lap-number filter that does not parse is refused (filterRejected(), the previous filter stays).
// Additions: hasMain, displayedRanges, filterAccepts, selectionKeys, parseLapNumbers, lapNumbersError,
// filterKindName/parseFilterKind, defaultFilter, session, setSelectFastestOnLoad, setSidecarCommit; M3 (the
// time_distance completion): commitSidecar hands any sidecar edit of an open source (the marker band's
// annotation Edit / Remove) to the same hook with its undo text (sidecarCommitText, read by the hook).
// M4 reload (06 section 6 "Changed files", AC-M4-10; D-208): closeSource parks what it takes from the
// selection (the source's slots, its own filter, its Data-window position, and which range it moved into
// Main) for the most recent closes, and sourceReloaded(previous, source) gives it to the reopened file
// (SourceLoader::reload, wired by connectSourceReload).
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
// The project.json spelling of a filter kind (02 section 6.2): "all", "selected", "fastest", "fastest3",
// "withinOnePercent", "lapNumbers"; nullopt for anything else.
std::string_view filterKindName(FilterSpec::Kind kind) noexcept;
std::optional<FilterSpec::Kind> parseFilterKind(std::string_view name) noexcept;

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
    // The filter of an open source: its own, else the persisted one of its hash, else defaultFilter().
    FilterSpec filter(const model::SourceId& source) const;
    const FilterSpec& defaultFilter() const noexcept { return defaultFilter_; }
    ProjectSession* session() const noexcept { return session_; }
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
    // Commits `doc` as the sidecar of the open source `source` through the SidecarCommit hook (with a
    // session: one undoable command named `undoText`); false without a hook or when the source is not open.
    bool commitSidecar(const model::SourceId& source, model::SidecarDocument doc, const QString& undoText);
    // The undo text of the commit in progress (the hook reads it; "" outside a commit).
    const QString& sidecarCommitText() const noexcept { return sidecarCommitText_; }
    // The project.json lastSelection of the current snapshot (06 section 5.5); paths are written relative to
    // `projectFolder` when they lie under it.
    project::SelectionKeys selectionKeys(const std::filesystem::path& projectFolder = {}) const;
    // Lap-number filter syntax (06 section 5.4): expr := term (',' term)*; term := int | int '-' int;
    // whitespace ignored, descending ranges normalised. nullopt on a parse error.
    static std::optional<std::vector<std::pair<int, int>>> parseLapNumbers(std::string_view text);
    // The filter editor's message for text that does not parse ("expected a lap number at 4"), else "".
    static std::string lapNumbersError(std::string_view text);
    // M4 reload: how many of the newest closes are parked for sourceReloaded.
    static constexpr std::size_t kParkedCloses = 8;

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
    // M4 reload: `source` is the reopened file of `previous`, closed by closeSource (SourceLoader::reload;
    // the id differs when the content changed). The slots, the filter and the Data-window position the close
    // took from `previous` go to `source`: each slot by RangeId, else by the same lap number and class (06
    // section 5.6, as sourceReplaced), else it is dropped with a hint; a range the close moved into Main goes
    // back to the slot it had. Publishes `source` first when the list does not hold it; nothing else happens
    // when no close of `previous` is parked (closeAllSources since, or more than kParkedCloses closes later).
    void sourceReloaded(const model::SourceId& previous, std::shared_ptr<const model::DataSource> source);

Q_SIGNALS:
    void changed(const esses::plot::SelectionSnapshot& snapshot, esses::qt::DataSelectionModel::Changes what);
    void filterChanged(const esses::model::SourceId& source);
    // A lap-number filter that does not parse was refused; the previous filter stays (06 section 5.4).
    void filterRejected(const esses::model::SourceId& source, const QString& error);
    void sourcesChanged();
    // Status-bar and Messages hints (no-op commands: CTRL+F without candidates, CTRL+ENTER on Main, ...).
    void hint(const QString& text);

private:
    // M4 reload: a slot a close took, with what the lap-number fallback needs from its old range.
    struct ParkedSlot {
        model::RangeRef ref;
        model::RangeKind kind = model::RangeKind::Outing;
        model::LapClass lapClass = model::LapClass::None;
        int number = 0;
    };
    // What closeSource took from the selection for one source (sourceReloaded gives it back).
    struct ParkedClose {
        enum class From : std::uint8_t { None, Overlay0, Overlay };
        model::SourceId id;
        std::optional<ParkedSlot> main;
        std::optional<ParkedSlot> overlay0;
        std::vector<std::pair<std::size_t, ParkedSlot>> overlays; // with their position among the overlays
        std::optional<FilterSpec> filter;                         // the source's own filter (filters_)
        std::optional<model::SourceId> before; // the source after it in the Data window (none: it was last)
        // The range this close moved into Main, and the slot it held before.
        std::optional<model::RangeRef> fallbackMain;
        From fallbackFrom = From::None;
        std::size_t fallbackIndex = 0;
    };
    static ParkedSlot parkedSlot(const model::DataSource& src, const model::RangeRef& r);
    std::optional<model::RangeRef> reResolveParked(const model::DataSource& src,
                                                   const ParkedSlot& slot) const;

    struct Candidates; // fastest-lap candidates of a source (analysis::fastestLap rule)
    void commit(Changes what);
    void checkInvariants() const;
    void readProjectOptions();
    void readFilters();
    void writeFilters();
    std::optional<FilterSpec> storedFilter(const model::SourceId& source) const;
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
    // project.json dataWindow.filters by key ("xxh3:<hex>"), also of sources that are not open, and
    // "default".
    std::map<std::string, FilterSpec, std::less<>> storedFilters_;
    FilterSpec defaultFilter_;
    std::string displayedGroupPath_ = "Outings:Laps";
    double fastLapTolerancePct_ = 1.0;
    bool applyToAllSources_ = false;
    bool selectFastestOnLoad_ = false;
    SidecarCommit sidecarCommit_;
    QString sidecarCommitText_;
    // selectionKeys: the project.json path of a source per (source id, source path, project folder).
    mutable std::map<std::string, std::string> relativePaths_;
    // M4 reload: the newest closes, oldest first (at most kParkedCloses).
    std::vector<ParkedClose> parked_;
};

} // namespace esses::qt

Q_DECLARE_OPERATORS_FOR_FLAGS(esses::qt::DataSelectionModel::Changes)
