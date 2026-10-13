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
// the GUI thread (01 section 5.2), the sidecar is written next to the log, sourceReplaced() fires.
// connectSourceLoader() wires the loader to the selection and the Messages dock.
#pragma once

#include <esses/analysis/rule_runner.hpp>
#include <esses/base/result.hpp>
#include <esses/formats/import_log.hpp>
#include <esses/model/ids.hpp>
#include <esses/model/sidecar.hpp>
#include <esses/model/source.hpp>
#include <esses/units/unit_registry.hpp>

#include <QObject>
#include <QString>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

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
    void close(const model::SourceId& source);
    void closeAll();
    int pendingOpens() const noexcept;
    const formats::ImporterRegistry& registry() const;
    // The published snapshot of an open source, or nullptr; every open source in open order.
    std::shared_ptr<const model::DataSource> source(const model::SourceId& id) const;
    std::vector<std::shared_ptr<const model::DataSource>> sources() const;
    // A sidecar edit of `current` (an open source): the new snapshot (ranges rebuilt by the rule runner,
    // version + 1), the document written next to the log, sourceReplaced(). NotFound when the source is not
    // open; Io when the sidecar cannot be written (the snapshot is still published).
    Result<std::shared_ptr<const model::DataSource>>
    commitSidecar(std::shared_ptr<const model::DataSource> current, model::SidecarDocument doc);

Q_SIGNALS:
    void sourcePublished(std::shared_ptr<const esses::model::DataSource> source);
    void sourceReplaced(std::shared_ptr<const esses::model::DataSource> source);
    void sourceClosed(const esses::model::SourceId& source);
    void openFailed(const QString& path, const QString& message);
    void importLog(const QString& path, const std::vector<esses::formats::ImportLogEntry>& entries);

private:
    void published(const std::filesystem::path& path, std::shared_ptr<const model::DataSource> source,
                   std::vector<formats::ImportLogEntry> log);
    void startPhase2(const model::SourceId& source, const std::string& layer);
    void phase2Finished(const std::shared_ptr<const model::DataSource>& annotated,
                        const std::string& canonical, const std::string& markers, const std::string& run,
                        std::optional<model::RangeGroupTree> tree);
    void publishReplacement(std::shared_ptr<const model::DataSource> next);

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
};

// The standard wiring of a workspace: sourcePublished/sourceReplaced/sourceClosed into the selection, the
// Import Log into the Messages dock (when given), and setTrusted's sidecar edits back into the loader.
void connectSourceLoader(SourceLoader& loader, DataSelectionModel& selection, MessagesModel* messages);

} // namespace esses::qt
