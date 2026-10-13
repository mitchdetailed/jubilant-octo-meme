// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// IImporter, IReader and IWriter (05 section 1.3, D-028): the contracts every format implements. Errors cross
// the boundary as Result<T>, never exceptions (rule 8); every heuristic leaves an ImportLogEntry with a
// stable code (rule 9).
//
// Additions to the 05 text, all with defaults so other importers need not implement them:
//   * IReader::interp(): the interpolation policy the file states (probe-1 has no member for it).
//   * IReader::companion(): what the companion files say (.ldx details, markers and unknown elements) for
//     pipeline step 5. markers(layer) returns the log's own markers; probe() lists both, as found.
//   * IReader::formatHeader(): the bytes the SourceId covers besides the two MiB windows (D-130).
//   * IReader::setConversion() and makeChunkProvider(): pipeline step 4 hands the conversion to the
//   quantity's
//     base unit back to the reader (rule 6, D-010), and the reader supplies the provider that feeds a Column
//     (motec.ld: the .ld chunk provider). Without one the pipeline wraps readBlock in a generic provider.
//   * M2: columnHint/dictionary (pipeline step 4a), makeTimesProvider/timeRange (timestamped channels) and
//     attachColumns (the shared slab sweep of the text engine, 05 section 7.1).
// JsonDoc does not exist in esses_base yet, so JSON-valued members are model::OpaqueJson.
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/rate.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/formats/byte_source.hpp>
#include <esses/formats/dictionary.hpp>
#include <esses/formats/import_log.hpp>
#include <esses/formats/probe.hpp>
#include <esses/model/channel.hpp>
#include <esses/model/opaque_json.hpp>
#include <esses/model/source.hpp>
#include <esses/store/chunk_provider.hpp>
#include <esses/store/column.hpp>
#include <esses/store/types.hpp>
#include <esses/units/converter.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace esses::formats {

using LayerIx = std::uint32_t;
using ChannelIx = std::uint32_t;

// sniff() sees at most this many leading bytes (05 section 1.3 rule 1).
inline constexpr std::size_t kSniffHeadBytes = std::size_t{64} * 1024;

struct SniffInput { // everything sniff may look at; no ByteSource, so no read beyond the head
    std::filesystem::path path;
    std::span<const std::byte> head; // the first kSniffHeadBytes bytes (fewer for a shorter file)
    std::uint64_t size = 0;
    std::span<const std::filesystem::path> siblings;
};

struct SniffResult {    // D-028
    int confidence = 0; // 0..100; 0 = not mine; >= 90 magic and structure; 50-89 magic only; 20-49 extension
    std::vector<std::filesystem::path> companions; // companion files found among the siblings (full paths)
    std::string reason;                            // one line for the Open dialog and the Import Log
};

struct ImportOptions {
    std::string importerId;                       // chosen by the registry, or the user's override
    std::string importerVersion;                  // filled by the registry
    std::optional<std::string> profileId;         // text.profile:* only
    std::optional<ProbeResult> probe;             // a prior probe the reader may reuse
    model::OpaqueJson options;                    // importer-specific, validated against optionsSchema()
    bool applyDictionary = true;                  // false: raw vendor view (no dictionary exists in M0)
    std::optional<std::filesystem::path> unitMap; // unit-mapping profiles arrive in M2; ignored in M0
    bool strictUnits = false;                     // fail with NotFound instead of opening a channel unitless
    std::optional<Hash128> expectedHash; // sidecar source check: another SourceId logs sidecar.source_changed
    // The open's cancellation (D-190): ImporterRegistry sets it to the open task's token before calling
    // IImporter::open, so an importer's long pass (the text engine's pass 1) stops when it fires (Cancelled).
    CancelToken cancel;
};

// Host-owned output window inside a chunk under construction (D-011).
struct Block {
    store::Dtype dtype = store::Dtype::F32;
    std::uint64_t first = 0;
    std::uint32_t count = 0;
    std::span<std::byte> values; // count * dtypeSize(dtype) bytes
    std::span<std::uint8_t>
        validBits;             // empty: the reader asserts all valid; else 1 bit per sample, LSB first
    std::span<Micros> timesUs; // timestamped channels only
};

class BlockSink { // readMany: the reader asks for windows and commits them
public:
    BlockSink() = default;
    BlockSink(const BlockSink&) = delete;
    BlockSink& operator=(const BlockSink&) = delete;
    virtual ~BlockSink() = default;
    virtual Result<Block*> begin(LayerIx layer, ChannelIx channel, std::uint64_t first,
                                 std::uint32_t count) = 0;
    virtual void commit(Block* block) = 0; // seals the window; the pointer is invalid afterwards
};

using Progress = std::function<void(double fraction)>; // passed as const Progress&; may be empty

// The conversion pipeline step 4 resolved for one channel: base = toBase(vendor value), stored as `dtype`.
struct ChannelConversion {
    units::Converter toBase;
    store::Dtype dtype = store::Dtype::F32;
};

class IReader {
public:
    IReader() = default;
    IReader(const IReader&) = delete;
    IReader& operator=(const IReader&) = delete;
    virtual ~IReader() = default;

    virtual const ProbeResult& probe() const = 0;
    // Random access, workers only, thread-safe when chunkIndependent(). Writes vendor values, or base-unit
    // values in the conversion's dtype after setConversion(); block.dtype must be that dtype. Invalid
    // samples: NaN for float dtypes and a cleared bit in validBits; an integer channel with invalid samples
    // in the range needs validBits (Invalid otherwise). NotFound for an unknown layer or channel, Invalid for
    // a bad range.
    virtual Result<void> readBlock(LayerIx layer, ChannelIx channel, std::uint64_t first, std::uint32_t count,
                                   Block& out, CancelToken cancel) = 0;
    // One sweep over several channels (message-stream and text formats); random-access readers loop
    // readBlock.
    virtual Result<void> readMany(LayerIx layer, std::span<const ChannelIx> channels, std::uint64_t first,
                                  std::uint32_t count, BlockSink& sink, const Progress& progress,
                                  CancelToken cancel) = 0;
    virtual Result<void> readTimestamps(LayerIx layer, ChannelIx channel, std::uint64_t first,
                                        std::uint32_t count, std::span<Micros> out, CancelToken cancel) = 0;
    // The log's own markers of a layer (device events for .ld), in (tUs, stored order).
    virtual std::span<const MarkerInfo> markers(LayerIx layer) const = 0;
    virtual std::optional<EnumTableInfo> enumTable(std::uint32_t id) const = 0;
    virtual std::vector<ImportLogEntry> warnings() const = 0; // snapshot of the sniff/probe/open ImportLog
    virtual std::shared_ptr<DecodeLog> decodeLog() const = 0; // shared with this source's chunk providers
    virtual bool chunkIndependent() const = 0;
    // Verbatim header bytes for a writer round trip (.ld: the header and the detail records, 0x5DE..0x5E1
    // held as 0, D-143); empty when none. The layout is private to the importer that produced it.
    virtual std::vector<std::byte> rawHeader() const { return {}; }
    // The on-disk encoding of a channel (02 section 4.1; .ld: LdSourceEncoding); null when none.
    virtual std::shared_ptr<const model::SourceEncoding> sourceEncoding(LayerIx, ChannelIx) const {
        return nullptr;
    }

    // ---- additions (see the file comment) -----------------------------------------------------------------
    // Default: Linear for Real channels, Previous for every other kind.
    virtual store::Interp interp(LayerIx layer, ChannelIx channel) const;
    // Details (prov Companion), markers per layer name ("" = Normal) and unknown elements of the companions.
    virtual model::CompanionData companion() const { return {}; }
    virtual std::vector<std::byte> formatHeader() const { return {}; }
    // Called by the pipeline before any readBlock or provider, never concurrently with them. Default: accepts
    // an identity conversion that keeps the probe's dtype, Unsupported otherwise.
    virtual Result<void> setConversion(LayerIx layer, ChannelIx channel, const ChannelConversion& conversion);
    // The provider of one channel's chunks, sharing decodeLog(); nullptr: the pipeline uses readBlock.
    virtual std::shared_ptr<store::IChunkProvider> makeChunkProvider(LayerIx, ChannelIx) const {
        return nullptr;
    }

    // ---- M2 additions (text importers; 05 section 2 step 4a, section 7.1)
    // ---------------------------------- Step 4a: the importer's own knowledge of a channel, applied before
    // the unit lookup: a text profile's column override (name, quantity, unit, interp, kind, dtype) and the
    // importer's dictionary (05 section 8.3), matched by the pipeline. Defaults: none.
    virtual const Dictionary::Entry* columnHint(LayerIx, ChannelIx) const { return nullptr; }
    virtual const Dictionary* dictionary() const { return nullptr; }
    // Timestamped channels: the provider of the I64 time column (channels whose readers return the same
    // provider share one Column), nullptr: the pipeline wraps readTimestamps; and the first and last
    // timestamp when the reader knows them without a decode (nullopt: the pipeline reads them).
    virtual std::shared_ptr<store::IChunkProvider> makeTimesProvider(LayerIx, ChannelIx) const {
        return nullptr;
    }
    virtual std::optional<std::pair<Micros, Micros>> timeRange(LayerIx, ChannelIx) const {
        return std::nullopt;
    }
    // Called once per channel after the pipeline built its Columns (before publication, never concurrently
    // with a decode): a reader that decodes several columns in one sweep seals the siblings' chunks into
    // `cache` with ChunkCache::insertGroup (05 section 7.1).
    virtual void attachColumns(LayerIx, ChannelIx, const store::Column& /*values*/,
                               const store::Column* /*timesUs*/, store::ChunkCache& /*cache*/) {}
};

class IImporter { // every member is const and thread-safe (the registry hands out const IImporter*)
public:
    IImporter() = default;
    IImporter(const IImporter&) = delete;
    IImporter& operator=(const IImporter&) = delete;
    virtual ~IImporter() = default;

    virtual std::string_view id() const = 0;                     // "motec.ld"
    virtual std::string_view version() const = 0;                // semver; part of every cache key
    virtual std::span<const std::string> extensions() const = 0; // lower case with the dot
    virtual SniffResult sniff(const SniffInput& input) const = 0;
    virtual Result<ProbeResult> probe(const ByteSource& source) const = 0; // no bulk decode
    virtual Result<std::unique_ptr<IReader>> open(std::shared_ptr<const ByteSource> source,
                                                  const ImportOptions& options) const = 0;
    virtual std::optional<model::OpaqueJson> optionsSchema() const { return std::nullopt; }
};

// What a writer sees; built by the CLI (and later the GUI) from a DataSource snapshot.
struct ExportView {
    std::shared_ptr<const model::DataSource> source;
    std::string layer = "Normal";
    std::vector<model::ChannelId> channels;            // empty: every channel of the layer, in layer order
    Micros start = std::numeric_limits<Micros>::min(); // [start, end); the default is the whole layer
    Micros end = std::numeric_limits<Micros>::max();
    std::optional<Rate> resampleRate; // writers may resample (importers never do, D-009)
    bool includeMarkers = true, includeDetails = true;
};

struct WriterOptions {
    model::OpaqueJson options; // writer-specific (motec.ld: {"ld": {"dialect": ..., ...}})
};

struct WriteReport {
    std::uint64_t bytes = 0;                  // over every file written
    std::vector<std::filesystem::path> files; // the files written (.ld, then .ldx when one was written)
    std::vector<ImportLogEntry> notes;
};

class IWriter {
public:
    IWriter() = default;
    IWriter(const IWriter&) = delete;
    IWriter& operator=(const IWriter&) = delete;
    virtual ~IWriter() = default;

    virtual std::string id() const = 0;
    virtual std::vector<std::string> extensions() const = 0;
    // Writes through a temporary file and a rename (rule 10): a failed or cancelled write leaves no partial
    // file.
    virtual Result<WriteReport> write(const std::filesystem::path& path, const ExportView& view,
                                      const WriterOptions& options, const Progress& progress,
                                      CancelToken cancel) = 0;
    virtual std::optional<model::OpaqueJson> optionsSchema() const { return std::nullopt; }
};

} // namespace esses::formats
