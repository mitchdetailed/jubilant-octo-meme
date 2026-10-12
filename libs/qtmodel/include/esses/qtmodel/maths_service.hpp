// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathsService (ARCHITECTURE_OUTLINE section 4 esses_qtmodel, 03 section 1, 06 section 10; D-016, D-022), the
// minimal M1 facade over the per-(source, layer) maths::MathGraphs: on publication it registers the built-in
// module esses.corrected_speed_distance with the project's settings (project.json builtinMaths, the module
// defaults otherwise) and prefetches Corr Speed / Corr Dist at Background (AC-M1-10), so distance mode is
// populated without user action. require() never blocks: it returns the current handle when evaluated and
// otherwise schedules evaluation at the given priority; completions are posted to the GUI thread, applied to
// the graph (status is written on the GUI thread only, 01 section 5.3 rule 10) and announced by
// channelReady()/statusChanged(). Expressions, the Maths Editor and Channel Status rows arrive in M2.
// Additions (M1): source(), units(), pool(), descriptor(), outputs(), the settings schema
// (loadSettingsSchema, validateCorrectedSpeedSettings: AC-M1-10's "the module's settings validate against its
// schema"), setCorrectedSpeedEnabled (project.json builtinMaths enabled), and moduleMessage() for the
// Messages dock (E-CSD-SETTINGS at registration, the first evaluation error of an output). A require()
// repeated while an evaluation is in flight posts one completion only.
#pragma once

#include <esses/base/future.hpp>
#include <esses/base/priority.hpp>
#include <esses/base/result.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/maths/math_status.hpp>
#include <esses/model/channel.hpp>
#include <esses/model/ids.hpp>
#include <esses/model/opaque_json.hpp>
#include <esses/model/source.hpp>
#include <esses/plotcore/selection_snapshot.hpp>
#include <esses/units/unit_registry.hpp>

#include <QObject>
#include <QString>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace esses {
class TaskPool;
} // namespace esses

namespace esses::maths {
class CorrectedSpeedDistanceModule;
} // namespace esses::maths

namespace esses::qt {

class Dispatcher;

class MathsService final : public QObject {
    Q_OBJECT
public:
    MathsService(TaskPool& pool, Dispatcher& dispatcher, std::shared_ptr<const units::UnitRegistry> units,
                 QObject* parent = nullptr);
    ~MathsService() override;

    // Loads schemas/builtin-esses.corrected_speed_distance-1.schema.json from the data directory (then the
    // executable's data/schemas, then the source tree): from then on settings are validated before use.
    Result<void> loadSettingsSchema(const std::filesystem::path& dataDir);
    // NotFound until the schema is loaded; Invalid (context.detail = JSON pointer) for bad settings.
    Result<void> validateCorrectedSpeedSettings(const model::OpaqueJson& settings) const;
    const maths::CorrectedSpeedDistanceModule& correctedSpeedModule() const;

    // Settings of esses.corrected_speed_distance for every source (project.json builtinMaths); null =
    // defaults.
    void setCorrectedSpeedSettings(model::OpaqueJson settings);
    void setCorrectedSpeedEnabled(bool enabled);
    // Registers the built-in module for every layer of `source` and prefetches its outputs at Background.
    // A source that is already registered is replaced.
    void addSource(std::shared_ptr<const model::DataSource> source);
    void replaceSource(std::shared_ptr<const model::DataSource> source);
    void removeSource(const model::SourceId& source);
    // Main and reference overlay for SelectionDependent nodes (none in M1).
    void setSelection(const plot::SelectionSnapshot& selection);

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
    // The registered snapshot of a source, or nullptr.
    std::shared_ptr<const model::DataSource> source(const model::SourceId& id) const;
    const std::shared_ptr<const units::UnitRegistry>& units() const noexcept { return units_; }
    TaskPool& pool() const noexcept { return pool_; }

Q_SIGNALS:
    void channelReady(const esses::maths::ChannelKey& key);
    void statusChanged(const esses::maths::ChannelKey& key);
    // A maths diagnostic for the Messages dock (origin Maths): code (E-CSD-SETTINGS, E-CSD-NO-SPEED, ...).
    void moduleMessage(const esses::model::SourceId& source, const QString& code, const QString& text);

private:
    struct Impl;
    void registerSource(const std::shared_ptr<const model::DataSource>& source);
    void prefetchSource(const model::SourceId& source);
    void watch(const maths::ChannelKey& key, const maths::Version& version,
               Future<maths::ColumnHandle> future);
    void complete(const maths::ChannelKey& key, const maths::Version& version,
                  const Result<maths::ColumnHandle>& result);

    TaskPool& pool_;
    Dispatcher& dispatcher_;
    std::shared_ptr<const units::UnitRegistry> units_;
    std::unique_ptr<Impl> impl_;
};

} // namespace esses::qt
