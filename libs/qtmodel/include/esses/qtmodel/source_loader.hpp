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
// MathsService and DistanceService when they are set (setMathsService/setDistanceService) before
// sourcePublished() fires; once Corr Speed is evaluated the phase-2 range annotations (stint, pitStop; task
// ranges/<sourceId>/<layer>/<markerSetHash>/<Corr Speed version>, older runs of the layer cancelled first)
// run and their annotations are merged by (range id, start, end) into the newest snapshot with the same
// marker set, published through sourceReplaced() (a snapshot with another marker set is annotated anew).
// commitSidecar() is the sidecar edit path (DataSelectionModel::setTrusted in M1): phase-1 rules re-run on
// the GUI thread (01 section 5.2), sourceReplaced() fires, and the sidecar is written next to the log within
// 500 ms by a Background task (02 section 4 sidecar rule 4; edits in between coalesce into one write, a later
// document never loses to an earlier write), or at once when the source closes or the loader goes (D-190).
// connectSourceLoader() wires the loader to the selection and the Messages dock.
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
#include <esses/units/unit_registry.hpp>

#include <QObject>
#include <QString>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
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
    void setDistanceService(DistanceService* distance) { distance_ = distance; }
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
    // whether a new snapshot was published (sourceReplaced(); nothing when the factors are already in place).
    bool setRangeStretch(const model::SourceId& source, std::string_view layer,
                         const std::map<model::RangeId, double>& stretch);

Q_SIGNALS:
    void sourcePublished(std::shared_ptr<const esses::model::DataSource> source);
    void sourceReplaced(std::shared_ptr<const esses::model::DataSource> source);
    void sourceClosed(const esses::model::SourceId& source);
    void openFailed(const QString& path, const QString& message);
    void importLog(const QString& path, const std::vector<esses::formats::ImportLogEntry>& entries);
    // M2: an open waits for the MappingPanel (setMappingHandOff(true)).
    void mappingRequired(int pendingId, const esses::formats::PendingResolution& pending);

private:
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
    void phase2Finished(const std::shared_ptr<const model::DataSource>& annotated,
                        const std::string& canonical, const std::string& markers, const std::string& run,
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
    bool useSidecars_ = true;
    int pending_ = 0;
    std::vector<std::shared_ptr<const model::DataSource>> open_; // open order
    std::set<std::string>
        phase2_; // phase-2 runs started or done: ranges/<src>/<layer>/<markers>/<speed version>
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
