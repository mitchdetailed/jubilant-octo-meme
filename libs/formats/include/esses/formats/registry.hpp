// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ImporterRegistry and the open pipeline (05 section 2): path -> sniff -> probe -> open -> resolve units ->
// merge the companion and the sidecar -> phase-1 ranges -> an immutable model::DataSource whose channels are
// Columns fed lazily by the reader's chunk providers (no sample is decoded on the open path).
//
// M0 scope (deviations from 05 section 2, recorded in libs/formats/README.md):
//   * open() runs steps 1 to 6 on the calling thread (a worker; the CLI's main thread); load() submits the
//     same work as one TaskPool task keyed open/<canonical path>/<size>/<mtime>/a, so equal opens coalesce.
//     Without a MappingHook (M2, below) unresolved units open unitless with the status text
//     Unit unknown: "<vendorUnit>"  as 05 section 2 step 4 prescribes for a headless open.
//   * Step 4 resolves each channel's vendor unit with the UnitRegistry of OpenContext and converts samples to
//     the quantity's base unit at decode (D-010). Without a registry the channels open unitless in vendor
//     units.
//   * Step 6 runs OpenContext::rangeRebuild: from M1 the profile's rules (esses_analysis' RuleRunner, passed
//     in by the caller because formats never links analysis); without one, the whole-outing rule
//     (model::rebuildWholeOutingRanges), the only range rule of M0.
//
// M2 (07 section 5.1): withBuiltins also registers the shipped text profiles motec.csv and generic.csv (with
// their dictionaries) and the two CSV writers; addProfile registers a text importer (TextEngine over a
// TextProfile, with its Dictionary); step 4 applies the reader's column hints and dictionary (4a), the unit
// map (4b) and the context's Mappings (4c); timestamped channels open as TimestampedSeries. The load overload
// with a MappingHook ends task A with a PendingResolution when step 4 leaves a unit unresolved (LoadResult),
// and resume runs task B with the user's MappingDecision (05 section 2, 05 section 8.1). An unresolved unit
// is a vendor unit the unit map and the registry do not know, or a quantity (from a hint or the dictionary)
// without a unit; a channel with neither opens unitless and is not unresolved.
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/future.hpp>
#include <esses/base/result.hpp>
#include <esses/formats/byte_source.hpp>
#include <esses/formats/dictionary.hpp>
#include <esses/formats/import_log.hpp>
#include <esses/formats/importer.hpp>
#include <esses/formats/mapping.hpp>
#include <esses/formats/probe.hpp>
#include <esses/formats/text_profile.hpp>
#include <esses/model/opaque_json.hpp>
#include <esses/model/source.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace esses {
class TaskPool;
} // namespace esses
namespace esses::store {
class ChunkCache;
} // namespace esses::store
namespace esses::units {
class UnitRegistry;
} // namespace esses::units

namespace esses::formats {

// What the open pipeline needs besides the file. Every pointer is borrowed and must outlive the call (and,
// for cache and pool, every Column of the opened source).
struct OpenContext {
    const units::UnitRegistry* units = nullptr; // step 4; null: no unit resolution, vendor units
    store::ChunkCache* cache = nullptr;         // null: store::ChunkCache::installed() (Invalid when none)
    TaskPool* pool = nullptr;                   // chunk tasks; null: TaskPool::installed() when needed
    bool loadSidecar = true;                    // step 5: <log>.<ext>.sidecar.json when it exists
    bool mergeCompanion = true;                 // step 5: the companion's details, markers, unknown elements
    // Step 6, phase-1 ranges: analysis::RuleRunner::rebuildFunction() from M1; empty: the whole-outing rule.
    model::DataSource::RangeRebuild rangeRebuild;
    // M2 step 4 (05 sections 2 and 8.2): the project's Mappings, applied at load (02 section 4.6 rule 16,
    // step 4c), and its unit maps by importer file stem (channels/unitmaps/<stem>.json,
    // UnitMappingProfile::fileStem; step 4b). ImportOptions::unitMap, when set, replaces the map of the
    // opened file's importer.
    model::MappingTable mappings;
    std::map<std::string, UnitMappingProfile, std::less<>> unitMaps;
};

struct OpenedSource {
    std::shared_ptr<const model::DataSource> source;
    ProbeResult probe;                    // the reader's probe (also serialised in source->prov.probe)
    std::vector<ImportLogEntry> log;      // sniff/probe/open entries, then steps 4-6, in canonical order
    std::shared_ptr<DecodeLog> decodeLog; // shared with every Column provider of the source
};

// What task A delivers when a MappingHook was given (05 section 2): the opened source, or the rows the user
// must resolve.
using LoadResult = std::variant<OpenedSource, PendingResolution>;

// Pipeline codes (Import Log entries raised by the registry rather than by a reader).
namespace open_code {
inline constexpr std::string_view kUnitsSkipped = "open.units_skipped"; // Info: no unit registry
inline constexpr std::string_view kUnitUnknown = "open.unit_unknown";   // Warning: vendor unit unresolved
inline constexpr std::string_view kUnitNotApplied =
    "open.unit_not_applied"; // Info: non-numeric kind kept raw
inline constexpr std::string_view kSidecarLoadFailed = "open.sidecar_load_failed"; // Warning
inline constexpr std::string_view kSourceChanged = "sidecar.source_changed";       // expectedHash mismatch
inline constexpr std::string_view kReadError = "open.read_error"; // DecodeLog Warning: a readBlock failed
inline constexpr std::string_view kUnitMapLoadFailed = "open.unitmap_load_failed"; // Warning (M2)
} // namespace open_code

class ImporterRegistry {
public:
    ImporterRegistry() = default;
    ImporterRegistry(const ImporterRegistry&) = delete;
    ImporterRegistry& operator=(const ImporterRegistry&) = delete;
    ImporterRegistry(ImporterRegistry&&) = delete;
    ImporterRegistry& operator=(ImporterRegistry&&) = delete;
    ~ImporterRegistry();

    // The built-in importers and writers of this build: motec.ld (importer and writer), the shipped text
    // profiles motec.csv and text.profile:generic.csv with their dictionaries, and the motec.csv and csv
    // writers (M2). `units` is borrowed by the writers (re-quantising, unit labels) and may be null.
    static std::unique_ptr<ImporterRegistry> withBuiltins(const units::UnitRegistry* units = nullptr);

    void
    add(std::unique_ptr<IImporter> importer); // a second importer with the same id replaces nothing: asserts
    void add(std::unique_ptr<IWriter> writer);

    // Best first; ties keep registration order; only confidences > 0. Reads the first 64 KiB and the sibling
    // listing once. Io when the file cannot be read.
    Result<std::vector<std::pair<const IImporter*, SniffResult>>>
    sniff(const std::filesystem::path& path) const;
    std::vector<std::pair<const IImporter*, SniffResult>> sniff(const SniffInput& input) const;

    const IImporter* find(std::string_view id) const;
    const IWriter* writer(std::string_view id) const;
    IWriter* writer(std::string_view id);
    std::vector<const IImporter*> importers() const;
    std::vector<const IWriter*> writers() const;
    // {"importers":[{"id","version","extensions","optionsSchema"?}],"writers":[{"id","extensions",
    // "optionsSchema"?}]}: the docs generator and the Open dialog's filters.
    model::OpaqueJson catalogue() const;

    // Steps 1-2 without opening: the chosen importer (options.importerId overrides sniffing) and its probe.
    // NotFound when no importer claims the file or the id is unknown.
    Result<std::pair<const IImporter*, ProbeResult>> probe(const std::filesystem::path& path,
                                                           const ImportOptions& options = {}) const;

    // Steps 1-6 on the calling thread. Errors: Io (file), NotFound (no importer, unknown importerId, an
    // unresolved unit with strictUnits), Invalid (bad options, no chunk cache), Format/Unsupported from the
    // reader, Cancelled.
    Result<OpenedSource> open(const std::filesystem::path& path, ImportOptions options = {},
                              const OpenContext& context = {}, const CancelToken& cancel = {}) const;
    // The same from an existing ByteSource (in-memory sources, fuzzing); a sidecar is read only for a source
    // that is a file on disk.
    Result<OpenedSource> open(const std::shared_ptr<const ByteSource>& source, ImportOptions options = {},
                              const OpenContext& context = {}, const CancelToken& cancel = {}) const;
    // open() as one TaskPool task at Interactive, keyed open/<canonical path>/<size>/<mtime>/a. The registry,
    // the context's pointers and the pool must outlive the Future.
    Future<OpenedSource> load(const std::filesystem::path& path, ImportOptions options, TaskPool& pool,
                              const OpenContext& context = {}, CancelToken cancel = {}) const;

    // ---- M2 contract (05 sections 2, 7 and 8)
    // --------------------------------------------------------------- Registers a text importer for `profile`
    // (TextEngine::makeImporter). A profile whose effective importer id collides with a registered importer
    // is refused (Invalid).
    Result<void> addProfile(TextProfile profile, std::shared_ptr<const Dictionary> dictionary = nullptr);
    // Task A with the Mapping-panel hand-off: as load() above, but when step 4 leaves a unit or name
    // unresolved the Future resolves with the PendingResolution built by `hook` (never blocks).
    Future<LoadResult> load(const std::filesystem::path& path, ImportOptions options, TaskPool& pool,
                            const OpenContext& context, CancelToken cancel, MappingHook hook) const;
    // Task B (steps 5-7) with the user's decision, submitted by the GUI by value.
    Future<OpenedSource> resume(PendingResolution pending, MappingDecision decision, TaskPool& pool,
                                const OpenContext& context = {}, CancelToken cancel = {}) const;

    // The DecodeLog registered for an opened source (05 section 1.1); null after release() or when unknown.
    std::shared_ptr<DecodeLog> decodeLog(const model::SourceId& id) const;
    void release(const model::SourceId& id); // on source close

private:
    // Task A (steps 1-4: sniff, open, skeleton, dictionary, unit map, units) and task B (the leftovers, the
    // Columns, mappings, companion, sidecar, phase-1 ranges) of 05 section 2.
    Result<std::shared_ptr<OpenState>> beginOpen(const std::shared_ptr<const ByteSource>& source,
                                                 ImportOptions options, const OpenContext& context,
                                                 const CancelToken& cancel) const;
    Result<OpenedSource> finishOpen(OpenState& state, const CancelToken& cancel) const;

    std::vector<std::unique_ptr<IImporter>> importers_;
    std::vector<std::unique_ptr<IWriter>> writers_;
    mutable std::mutex logsMutex_;
    mutable std::map<model::SourceId, std::weak_ptr<DecodeLog>> decodeLogs_;
};

} // namespace esses::formats
