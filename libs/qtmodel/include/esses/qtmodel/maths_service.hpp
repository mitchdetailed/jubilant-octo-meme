// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathsService (ARCHITECTURE_OUTLINE section 4 esses_qtmodel, 03 sections 1, 4.2-4.3, 7 and 11, 06 section
// 10; D-016, D-022): the GUI-side host of the per-(source, layer) maths::MathGraphs. On publication it
// registers the built-in modules esses.corrected_speed_distance (with the project's settings, project.json
// builtinMaths) and, for a layer that logs both GPS inputs, esses.gps_corrections (its zero-fix validity step
// always on, 03 section 9.2), defines the project's global modules (project order), the source's local maths
// (sidecar localMaths, re-read on every new snapshot) and its transient module, the project's mappings and
// aliases and the compat switches, and prefetches Corr Speed / Corr Dist at Background (AC-M1-10). Only the
// cones whose definition or inputs changed become Pending (AC-M2-04). require() never blocks: it returns the
// current handle when evaluated and otherwise schedules evaluation at the given priority; completions are
// posted to the GUI thread, applied to the graph (status is written on the GUI thread only, 01 section 5.3
// rule 10; finished upstream nodes are applied with them) and announced by channelReady()/statusChanged().
// Status per item follows 03 section 7.3: Pending, Evaluating (a require started it), Ok, Warning, Error,
// Disabled.
//
// Additions beyond 03: source(), sources(), units(), pool(), descriptor(), outputs(), the settings schemas
// (loadSettingsSchema, validateCorrectedSpeedSettings), setCorrectedSpeedEnabled, moduleMessage() for the
// Messages dock; M2: setResolverTables, setSelectionDistances, statusRows (Channel Status), check (the Maths
// Editor's live diagnostics against the Main source's scope: its maths outputs, logged channels through the
// project's mappings and aliases, details and the global modules' constants; a data-less scope with nothing
// open), invalidate (Recalculate), and connectMathsSelection (the selection and the Main/reference distance
// indexes for SelectionDependent nodes).
#pragma once

#include <esses/analysis/distance_index.hpp>
#include <esses/base/future.hpp>
#include <esses/base/priority.hpp>
#include <esses/base/result.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/maths/diagnostics.hpp>
#include <esses/maths/function_registry.hpp>
#include <esses/maths/math_module.hpp>
#include <esses/maths/math_selection.hpp>
#include <esses/maths/math_status.hpp>
#include <esses/maths/maths_compat.hpp>
#include <esses/model/channel.hpp>
#include <esses/model/ids.hpp>
#include <esses/model/opaque_json.hpp>
#include <esses/model/resolver.hpp>
#include <esses/model/source.hpp>
#include <esses/plotcore/selection_snapshot.hpp>
#include <esses/units/unit_registry.hpp>

#include <QObject>
#include <QString>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses {
class TaskPool;
} // namespace esses

namespace esses::maths {
class CorrectedSpeedDistanceModule;
class GpsCorrectionsModule;
class MathGraph;
} // namespace esses::maths

namespace esses::qt {

class DataSelectionModel;
class Dispatcher;
class DistanceService;

class MathsService final : public QObject {
    Q_OBJECT
public:
    MathsService(TaskPool& pool, Dispatcher& dispatcher, std::shared_ptr<const units::UnitRegistry> units,
                 QObject* parent = nullptr);
    ~MathsService() override;

    // Loads the built-in modules' settings schemas (builtin-esses.corrected_speed_distance-1 and
    // builtin-esses.gps_corrections-1) from the data directory (then the executable's data/schemas, then the
    // source tree): from then on settings are validated before use.
    Result<void> loadSettingsSchema(const std::filesystem::path& dataDir);
    // NotFound until the schema is loaded; Invalid (context.detail = JSON pointer) for bad settings.
    Result<void> validateCorrectedSpeedSettings(const model::OpaqueJson& settings) const;
    const maths::CorrectedSpeedDistanceModule& correctedSpeedModule() const;
    const maths::GpsCorrectionsModule& gpsCorrectionsModule() const;

    // Settings of esses.corrected_speed_distance for every source (project.json builtinMaths); null =
    // defaults.
    void setCorrectedSpeedSettings(model::OpaqueJson settings);
    void setCorrectedSpeedEnabled(bool enabled);
    // Registers the built-in module for every layer of `source` and prefetches its outputs at Background.
    // A source that is already registered is replaced.
    void addSource(std::shared_ptr<const model::DataSource> source);
    void replaceSource(std::shared_ptr<const model::DataSource> source);
    void removeSource(const model::SourceId& source);
    // Main and reference overlay for SelectionDependent nodes (ungated stat_*, variance_* and their
    // consumers, 03 sections 7.5-7.6): the maths::MathSelection built from the snapshot (geometry from the
    // registered snapshots) re-keys them; nodes whose key changed are announced by statusChanged().
    void setSelection(const plot::SelectionSnapshot& selection);
    // The DistanceIndexes of the Main range and the reference overlay (null while pending or absent) for
    // variance_* (EvalContext::mainDist, refDist).
    void setSelectionDistances(std::shared_ptr<const analysis::DistanceIndex> main,
                               std::shared_ptr<const analysis::DistanceIndex> reference);
    const maths::MathSelection& mathSelection() const;

    // Non-blocking: the evaluated output, or nullopt (evaluation scheduled at `priority`; channelReady
    // follows).
    std::optional<maths::ColumnHandle> require(const maths::ChannelKey& key, Priority priority);
    // Whether `name` is a maths output of the source's layer (Corr Speed, Corr Dist in M1).
    bool isMathsChannel(const model::SourceId& source, std::string_view layer, std::string_view name) const;
    maths::MathStatus status(const maths::ChannelKey& key) const;
    // The output's descriptor (statusText from the status), or nullopt for an unknown key.
    std::optional<model::ChannelDescriptor> descriptor(const maths::ChannelKey& key) const;
    // Every output of the source's layer in definition order.
    std::vector<maths::ChannelKey> outputs(const model::SourceId& source, std::string_view layer) const;
    // The registered snapshot of a source, or nullptr; every registered source in registration order.
    std::shared_ptr<const model::DataSource> source(const model::SourceId& id) const;
    std::vector<model::SourceId> sources() const;
    const std::shared_ptr<const units::UnitRegistry>& units() const noexcept { return units_; }
    TaskPool& pool() const noexcept { return pool_; }

    // ---- M2 (03 sections 4.2-4.3, 7 and 11) -------------------------------------------------------------
    // Global modules in project order (ProjectSession::mathsModules, project.json mathsOrder); an equal set
    // is a no-op.
    void setModules(std::vector<maths::MathModule> globalInOrder);
    const std::vector<maths::MathModule>& modules() const;
    // options.maths.compat (part of every defHash: flipping a switch re-versions the affected nodes; the live
    // check parses with it).
    void setCompat(maths::MathsCompat compat);
    const maths::MathsCompat& compat() const;
    // The project's mappings and aliases (02 section 4.6, 03 section 3.3 steps 1-2) for name resolution.
    void setResolverTables(model::MappingTable mappings, model::AliasTable aliases);
    // The local maths of a source: by default its sidecar's localMaths of each layer (re-read on every new
    // snapshot); a module set here replaces it for every layer until nullopt hands it back to the sidecar.
    // The transient module (filters from a channel label, never saved, dropped on close or reload; nullopt
    // removes it).
    void setLocalModule(const model::SourceId& source, std::optional<maths::MathModule> local);
    void setTransientModule(const model::SourceId& source, std::optional<maths::MathModule> transient);
    // Recalculate (03 section 11 item 11): every output of `moduleId` (in every source) drops its column and
    // becomes Pending, to be evaluated again on demand (built-in outputs are prefetched again).
    void invalidate(std::string_view moduleId);

    // One Channel Status row (06 section 10) per maths output of every open source.
    struct StatusRow {
        model::SourceId source;
        std::string layer;
        maths::ChannelKey key;
        std::string name;     // output name as defined
        std::string moduleId; // "user", "local", "~transient", "esses.corrected_speed_distance", ...
        maths::ItemKind kind = maths::ItemKind::Expression;
        maths::MathStatus status;
        model::ResolutionTrace trace;
        std::optional<model::ChannelDescriptor> descriptor; // rate and unit once evaluated
    };
    std::vector<StatusRow> statusRows() const;
    // Parse and type check of `expression` against the scope of `source` (the Main source; nullopt or a
    // source that is not registered: the data-less scope, where unknown names are I-UNRESOLVED), for the
    // Maths Editor's live diagnostics (03 section 11 items 2 and 12) with the project's compat switches.
    // Never evaluates and never blocks.
    maths::Diagnostics check(std::string_view expression, const std::optional<model::SourceId>& source) const;
    const maths::FunctionRegistry& functions() const;

Q_SIGNALS:
    void channelReady(const esses::maths::ChannelKey& key);
    void statusChanged(const esses::maths::ChannelKey& key);
    // A maths diagnostic for the Messages dock (origin Maths): code (E-CSD-SETTINGS, E-CSD-NO-SPEED, ...).
    void moduleMessage(const esses::model::SourceId& source, const QString& code, const QString& text);

public:
    struct Impl; // src/maths/maths_service_impl.hpp

private:
    void registerSource(const std::shared_ptr<const model::DataSource>& source);
    void applyModules(const model::SourceId& source);
    void emitChanged(const std::vector<maths::ChannelKey>& keys);
    void prefetchSource(const model::SourceId& source);
    void watch(const maths::ChannelKey& key, const maths::Version& version,
               Future<maths::ColumnHandle> future);
    void complete(const maths::ChannelKey& key, const maths::Version& version,
                  const Result<maths::ColumnHandle>& result);
    void applyResult(maths::MathGraph& graph, const maths::ChannelKey& key, const maths::Version& version,
                     const Result<maths::ColumnHandle>& result);

    TaskPool& pool_;
    Dispatcher& dispatcher_;
    std::shared_ptr<const units::UnitRegistry> units_;
    std::unique_ptr<Impl> impl_;
};

// The selection wiring of a workspace (06 section 5.1: maths receives a MathSelection copy built from the
// snapshot): every DataSelectionModel change reaches MathsService::setSelection, and with a DistanceService
// the Main and reference laps' indexes reach setSelectionDistances (again when they arrive).
void connectMathsSelection(DataSelectionModel& selection, MathsService& maths,
                           DistanceService* distance = nullptr);

} // namespace esses::qt
