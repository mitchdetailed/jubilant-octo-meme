// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SourceLoader (ARCHITECTURE_OUTLINE section 2.1, 01 section 4.1, 06 section 6 "Changed files"; 07
// section 4.1: the open pipeline minus the MappingPanel). open(path) submits formats::ImporterRegistry::load
// on the TaskPool (task key open/<canonical path>/<size>/<mtime>/a, Interactive) with the profile's
// RuleRunner as the step-6 range rebuild (OpenContext::rangeRebuild); the published std::shared_ptr<const
// model::DataSource> reaches the GUI through Dispatcher::post and is announced by sourcePublished(), followed
// by the Import Log entries for the Messages dock. After publication it asks MathsService to register the
// built-in module and prefetch Corr Speed / Corr Dist at Background (AC-M1-10), and submits one
// summary/<sourceId> task (02 section 3.2 rule 10). A sidecar edit publishes a new snapshot through
// sourceReplaced(). M4 adds stale-file detection (MappedFile::changedOnDisk on application focus) and
// file.reload_source. GUI thread only.
// M1 details: the open task is submitted by this class (same key, so it coalesces with
// ImporterRegistry::load) and holds the registry and the unit table by shared_ptr, so a loader destroyed
// while an open is in flight leaves nothing dangling; a published source is registered with the
// MathsService when it is set (setMathsService) before sourcePublished() fires, and handed to the
// DistanceService (setDistanceService) for its lap indexes right after it, so that no channelReady() of the
// new source reaches a listener before sourcePublished(); once Corr Speed is evaluated the phase-2 range
// annotations (stint, pitStop; task ranges/<sourceId>/<layer>/<markerSetHash>/<Corr Speed version>, older
// runs of the layer cancelled first) run and their annotations are merged by (range id, start, end) into the
// newest snapshot with the same marker set, published through sourceReplaced() (a snapshot with another
// marker set is annotated anew).
// commitSidecar() is the sidecar edit path (DataSelectionModel::setTrusted in M1): phase-1 rules re-run on
// the GUI thread (01 section 5.2), sourceReplaced() fires, and the sidecar is written next to the log within
// 500 ms by a Background task (02 section 4 sidecar rule 4; edits in between coalesce into one write, a later
// document never loses to an earlier write), or at once when the source closes or the loader goes (D-190).
// connectSourceLoader() wires the loader to the selection and the Messages dock.
// M3 (the track work, D-195, D-199): with setTrackMaps, phase 2 also builds the Track Sections groups
// (RuleRunner::runPhase2 with the source's venue map and the laps' DistanceIndexes from the DistanceService,
// each lap measured forward from a BCN start or backwards from its end beacon) and merges them into the
// newest snapshot with the same marker set; it runs again when the venue's map changes
// (TrackMapService::mapChanged), when more lap indexes are ready (DistanceService::distanceReady) and when
// setRangeStretch publishes new lap-stretch factors. Its task key gains /t<map generation>-<the bound map's
// stamp>/i<indexed laps>-<digest of the indexes' D-140 keys> while a map is bound, so a replaced index (a new
// stretch, a newer Corr Dist) or a venue bound to another map (D-210) changes it at the same count. A layer's
// request is deduplicated against its latest run only, and only the latest run publishes (D-204).
//
// M2 (07 section 5.1, 05 sections 2 and 8.1, 06 section 13.8): the MappingPanel hand-off. With the hand-off
// on, open() submits task A through ImporterRegistry::load with a MappingHook; an open whose step 4 leaves
// units unresolved ends task A with mappingRequired(id, pending) instead of sourcePublished(); the panel
// answers with resolveMapping (task B, ImporterRegistry::resume, the decision by value; applyMappingDecision
// in undo_commands.hpp also records the decision as undoable project edits) or cancelMapping (the pending
// open is dropped and an Import Log row says so). No worker waits for the user, and an open without
// unresolved units publishes as before. setChannelDocuments hands step 4 the project's mappings and unit maps
// (the aliases are kept for the resolver); registerTextProfiles adds the user's text profiles (the shipped
// ones are built in, D-168). setRangeStretch publishes the lap-stretch factors of LapStretchService
// (Range::stretch, derived, never persisted; D-016). Opens are cancellable (D-190): cancelOpens() fires the
// token of every open in flight (the text importers' pass 1 stops at its next check) and drops its result
// and every open waiting for the Mapping panel; closeAll() and a project change (connectProjectSession) call
// it, so an open started for one project never publishes, or writes its mapping, into the next.
//
// M4 contract (D-206, docs/milestones/M4.md section 2): three marked sections below, one per M4 package. The
// contract froze this header: each package implemented its section in its own file (budget
// src/m4_budget/source_loader_budget.cpp, reload src/m4_reload/source_loader_reload.cpp) and keeps
// its private state in its nested struct (M4Budget, M4Reload: members of SourceLoader, so they reach its
// private members without a header edit); the followups package owns source_loader.cpp and
// source_loader_m2.cpp and keeps the hook calls the contract put there (published(): budgetHolds first and
// reloadPublished last; close(): budgetForget and reloadForget; cancelOpens(): budgetDropPending).
#pragma once

#include <esses/analysis/rule_runner.hpp>
#include <esses/base/cancel.hpp>
#include <esses/base/future.hpp>
#include <esses/base/result.hpp>
#include <esses/formats/import_log.hpp>
#include <esses/formats/mapping.hpp>
#include <esses/formats/registry.hpp>
#include <esses/formats/unit_mapping_profile.hpp>
#include <esses/model/ids.hpp>
#include <esses/model/resolver.hpp>
#include <esses/model/sidecar.hpp>
#include <esses/model/source.hpp>
#include <esses/qtmodel/memory_budget.hpp>
#include <esses/units/unit_registry.hpp>

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
#include <vector>

class QTimer;

namespace esses {
class TaskPool;
} // namespace esses

namespace esses::formats {
class ImporterRegistry;
} // namespace esses::formats

namespace esses::qt {

class DataSelectionModel;
class Dispatcher;
class DistanceService;
class MathsService;
class MessagesModel;
class TrackMapService;

class SourceLoader final : public QObject {
    Q_OBJECT
public:
    SourceLoader(TaskPool& pool, Dispatcher& dispatcher, std::shared_ptr<const units::UnitRegistry> units,
                 QObject* parent = nullptr);
    ~SourceLoader() override;

    // The profile's phase-1 rules (ProjectSession::ruleRunner()); the default is
    // RuleRunner::circuitDefault().
    void setRuleRunner(analysis::RuleRunner runner);
    const analysis::RuleRunner& ruleRunner() const noexcept { return rules_; }
    // Optional services a published source is registered with (built-in maths prefetch, AC-M1-10).
    void setMathsService(MathsService* maths);
    void setDistanceService(DistanceService* distance);
    // M3: the track maps phase 2 builds the Track Sections groups from (null: none, the M2 behaviour).
    void setTrackMaps(TrackMapService* maps);
    // Read <log>.<ext>.sidecar.json on open and write it on commitSidecar (default true; tests turn it off).
    void setUseSidecars(bool on) { useSidecars_ = on; }
    // Asynchronous; one sourcePublished() or openFailed() per call.
    void open(const std::filesystem::path& path);
    // Closes a source (its pending sidecar write is done first, on this thread).
    void close(const model::SourceId& source);
    // Cancels the opens in flight (cancelOpens()) and closes every source.
    void closeAll();
    // Cancels every open in flight and every open waiting for the Mapping panel: their tokens fire, their
    // results are dropped, pendingOpens() and pendingMappings() are 0 at once, and each logs an Import Log
    // row (open.cancelled, open.mapping_cancelled). Published sources stay open.
    void cancelOpens();
    int pendingOpens() const noexcept;
    const formats::ImporterRegistry& registry() const;
    // The published snapshot of an open source, or nullptr; every open source in open order.
    std::shared_ptr<const model::DataSource> source(const model::SourceId& id) const;
    std::vector<std::shared_ptr<const model::DataSource>> sources() const;
    // A sidecar edit of `current` (an open source): the new snapshot (ranges rebuilt by the rule runner,
    // version + 1), sourceReplaced(), and the document written next to the log within 500 ms (a failed write
    // is an Import Log row, sidecar.write_failed). NotFound when the source is not open.
    Result<std::shared_ptr<const model::DataSource>>
    commitSidecar(std::shared_ptr<const model::DataSource> current, model::SidecarDocument doc);

    // ---- M2: the MappingPanel hand-off (05 sections 2 and 8.1)
    // ---------------------------------------------- Off by default (a headless open resolves unknown units
    // to unitless, 05 section 2 step 4); the shell turns it on.
    void setMappingHandOff(bool on) { mappingHandOff_ = on; }
    bool mappingHandOff() const noexcept { return mappingHandOff_; }
    // Step 4 inputs: the project's Mappings and Aliases (the resolver's tables) and its unit maps by importer
    // stem (channels/unitmaps/<stem>.json).
    void setChannelDocuments(model::MappingTable mappings, model::AliasTable aliases,
                             std::map<std::string, formats::UnitMappingProfile, std::less<>> unitMaps);
    // Registers the text profiles of <data>/profiles-text and <config>/profiles-text with their dictionaries
    // (<data>/dictionaries/<stem>.json, else <config>/dictionaries/<stem>.json). A profile whose importer is
    // already registered (the shipped profiles are built in, D-168) is skipped silently when it comes from
    // the data directory and logged otherwise. Call it before the first open: while an open is in flight the
    // registry is in use on a worker and nothing is registered (one log line). Returns the log lines.
    std::vector<std::string> registerTextProfiles(const std::filesystem::path& dataDir,
                                                  const std::filesystem::path& configDir);
    // Answers a mappingRequired(): task B with the decision, then sourcePublished() (or openFailed()).
    void resolveMapping(int pendingId, formats::MappingDecision decision);
    // Drops the pending open (no openFailed(); the Messages dock logs the cancellation).
    void cancelMapping(int pendingId);
    int pendingMappings() const noexcept { return static_cast<int>(pendingMappings_.size()); }
    // The pending open of a mappingRequired() id, or nullptr.
    const formats::PendingResolution* pendingMapping(int pendingId) const;
    const model::MappingTable& mappingTable() const noexcept { return mappings_; }
    const model::AliasTable& aliasTable() const noexcept { return aliases_; }

    // Publishes the lap-stretch factors of one layer (LapStretchService, D-016): every range of the layer's
    // "Outings:Laps" group gets Range::stretch = the factor of its id, or none when absent or 1. Returns
    // whether a new snapshot was published (sourceReplaced(); nothing when the factors are already in place);
    // a new one starts phase 2 for the layer, so its Track Sections follow the stretched indexes (D-204).
    bool setRangeStretch(const model::SourceId& source, std::string_view layer,
                         const std::map<model::RangeId, double>& stretch);

    // ---- M4 budget (package budget; src/m4_budget/source_loader_budget.cpp) -------------------------
    // The open-time budget warning (07 section 7.1, AC-M4-02; 01 section 7.2 "a warning at open, with
    // continue or cancel"). With a gate set, an open whose resident estimate (formats::estimateResident of
    // the opened source) exceeds the cache's budget stops before publication, so no chunk of it is decoded
    // and no maths is registered: budgetExceeded(pendingId, check) announces it, continueOverBudget publishes
    // it as any open, cancelOverBudget drops it (an Import Log row, open.budget_cancelled). cancelOpens()
    // drops the waiting ones too. nullptr (the default) turns the check off: headless opens never wait.
    void setBudgetGate(const store::ChunkCache* cache);
    const store::ChunkCache* budgetGate() const noexcept { return budgetGate_; }
    void continueOverBudget(int pendingId);
    void cancelOverBudget(int pendingId);
    int pendingBudgetOpens() const;
    // The check of a waiting open, or nullptr.
    const OpenBudgetCheck* pendingBudgetCheck(int pendingId) const;

    // ---- M4 reload (package reload; src/m4_reload/source_loader_reload.cpp) -------------------------
    // Changed files (06 section 6 "Changed files", 02 section 1 behaviour 5, AC-M4-10; E-5: until AC-M4-10
    // passes, a changed file is detected and marked stale). checkChangedFiles() runs formats::checkSourceFile
    // for every open source (the shell calls it when the application becomes active; the providers check
    // before each decode batch on their own); a changed or missing file marks its source stale:
    // staleChanged(id, true) and one Import Warning row (code open.file_changed, "file changed on disk")
    // whose action is file.reload_source with the source id. reload() closes the source (its sidecar flushed
    // first) and opens its path again through open(); when it publishes, sourceReloaded(previous id, source)
    // fires and the selection slots, filters and Data-window state that named the old source are restored by
    // RangeId where the ranges still exist (06 section 5.6 sourceReplaced).
    std::vector<model::SourceId> checkChangedFiles(); // the sources newly marked stale
    bool isStale(const model::SourceId& source) const;
    std::vector<model::SourceId> staleSources() const;
    Result<void> reload(const model::SourceId& source);
    int pendingReloads() const;

Q_SIGNALS:
    void sourcePublished(std::shared_ptr<const esses::model::DataSource> source);
    void sourceReplaced(std::shared_ptr<const esses::model::DataSource> source);
    void sourceClosed(const esses::model::SourceId& source);
    void openFailed(const QString& path, const QString& message);
    void importLog(const QString& path, const std::vector<esses::formats::ImportLogEntry>& entries);
    // M2: an open waits for the MappingPanel (setMappingHandOff(true)).
    void mappingRequired(int pendingId, const esses::formats::PendingResolution& pending);
    // M4 budget: an open waits for Continue or Cancel (setBudgetGate).
    void budgetExceeded(int pendingId, const esses::qt::OpenBudgetCheck& check);
    // M4 reload: a source became stale (or fresh again: false, when it was reloaded or closed).
    void staleChanged(const esses::model::SourceId& source, bool stale);
    // M4 reload: reload() published the reopened file (its id differs when the content changed).
    void sourceReloaded(const esses::model::SourceId& previous,
                        std::shared_ptr<const esses::model::DataSource> source);
    // M4 followups (M3.md section 6 item 18): the summary/<sourceId> pass finished, so every logged channel
    // of the source has its SummaryPyramid; components built before it rebuild their extents. Emitted by the
    // followups package (source_loader.cpp); never before sourcePublished of the source.
    void summariesReady(const esses::model::SourceId& source);

private:
    // ---- M4 contract: the packages' hooks (called from source_loader.cpp) and private state ----
    struct M4Budget; // src/m4_budget/source_loader_budget.cpp
    struct M4Reload; // src/m4_reload/source_loader_reload.cpp
    // published(): true when the budget gate holds the open (it publishes later or never).
    bool budgetHolds(const std::filesystem::path& path,
                     const std::shared_ptr<const model::DataSource>& source,
                     const std::vector<formats::ImportLogEntry>& log);
    void budgetDropPending();                                                     // cancelOpens()
    void budgetForget(const model::SourceId& id);                                 // close()
    void reloadPublished(const std::shared_ptr<const model::DataSource>& source); // end of published()
    void reloadForget(const model::SourceId& id);                                 // close()
    const store::ChunkCache* budgetGate_ = nullptr;
    std::shared_ptr<M4Budget> m4Budget_; // created by the budget package on first use
    std::shared_ptr<M4Reload> m4Reload_; // created by the reload package on first use

    struct InFlight {
        CancelSource cancel;
        std::filesystem::path path;
    };
    struct SidecarDue {
        model::SidecarDocument doc;
        std::filesystem::path path;
        std::uint64_t generation = 0;
    };
    struct SidecarSlot; // per source: the generation on disk, under a mutex (source_loader.cpp)

    formats::OpenContext openContext() const;
    // Registers an open in flight; its id is handed to deliver() and checked when the result arrives.
    int track(const std::filesystem::path& path, CancelSource cancel);
    void deliver(const std::filesystem::path& path, Future<formats::OpenedSource> future, int openId);
    void openWithHandOff(const std::filesystem::path& path, formats::OpenContext context, int openId,
                         CancelToken cancel);
    void mappingPending(formats::PendingResolution pending);
    void published(const std::filesystem::path& path, std::shared_ptr<const model::DataSource> source,
                   std::vector<formats::ImportLogEntry> log);
    void startPhase2(const model::SourceId& source, const std::string& layer);
    // Phase 2 again for every layer of the open sources whose venue is `venue` (all of them when nullopt).
    void restartPhase2(const std::optional<std::string>& venue);
    bool hasTrackSectionsRule() const;
    void phase2Finished(const std::shared_ptr<const model::DataSource>& annotated,
                        const std::string& canonical, const std::string& markers, std::uint64_t run,
                        std::optional<model::RangeGroupTree> tree);
    void publishReplacement(std::shared_ptr<const model::DataSource> next);
    // The due sidecar documents: written by Background tasks (the timer), or here and now (a closing source,
    // the destructor; `report` emits sidecar.write_failed rows).
    std::shared_ptr<SidecarSlot> slotFor(const model::SourceId& source);
    void writeDueSidecars();
    void flushSidecar(const model::SourceId& source, bool report);

    TaskPool& pool_;
    Dispatcher& dispatcher_;
    std::shared_ptr<const units::UnitRegistry> units_;
    std::shared_ptr<formats::ImporterRegistry> registry_;
    analysis::RuleRunner rules_;
    MathsService* maths_ = nullptr;
    DistanceService* distance_ = nullptr;
    TrackMapService* trackMaps_ = nullptr;
    std::uint64_t trackGeneration_ = 1; // bumps on every map change (phase-2 key, with the map's stamp)
    bool useSidecars_ = true;
    int pending_ = 0;
    std::vector<std::shared_ptr<const model::DataSource>> open_; // open order
    // Per layer (ranges/<src>/<layer>/), its latest phase-2 run, running or done (D-204): the task key, which
    // names the run's inputs, and the serial of that start. A request with the same key is deduplicated, and
    // only the latest run of a layer publishes. close() drops the source's entries.
    struct Phase2Run {
        std::string key;
        std::uint64_t serial = 0;
    };
    std::map<std::string, Phase2Run, std::less<>> phase2_;
    std::uint64_t phase2Serial_ = 0;
    // M2: step 4 inputs and the MappingPanel hand-off
    model::MappingTable mappings_;
    model::AliasTable aliases_;
    std::map<std::string, formats::UnitMappingProfile, std::less<>> unitMaps_;
    bool mappingHandOff_ = false;
    int nextPendingId_ = 1;
    std::map<int, formats::PendingResolution> pendingMappings_;
    // D-190: cancellable opens and coalesced sidecar writes.
    std::map<int, InFlight> opens_;
    int nextOpenId_ = 1;
    std::map<model::SourceId, SidecarDue> sidecarsDue_;      // queued by commitSidecar, not yet submitted
    std::map<model::SourceId, SidecarDue> sidecarsInFlight_; // submitted, not known to be written
    std::map<model::SourceId, std::shared_ptr<SidecarSlot>> sidecarSlots_;
    std::uint64_t sidecarGeneration_ = 0;
    QTimer* sidecarTimer_ = nullptr;
};

// The standard wiring of a workspace: sourcePublished/sourceReplaced/sourceClosed into the selection, the
// Import Log into the Messages dock (when given), and setTrusted's sidecar edits back into the loader.
void connectSourceLoader(SourceLoader& loader, DataSelectionModel& selection, MessagesModel* messages);

} // namespace esses::qt
