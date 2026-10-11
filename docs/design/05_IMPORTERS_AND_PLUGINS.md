# 05 — Importers, Text Engine and Plugins (`esses_formats`, `sdk/esses_abi.h`)

**Scope.** This document specifies the `esses_formats` library (layer 2, Qt-free, D-007): the `IImporter` / `IReader` / `IWriter` contracts, `ImporterRegistry` and the open pipeline that turns a path into a published `DataSource`, the clean-room MoTeC `.ld`/`.ldx` reader and export-only writer, the MoTeC CSV reader/writer, the `TextEngine` with JSON importer profiles, dictionaries and unit-mapping profiles with the inline Mapping panel, the fixture corpus and fuzzing, the plugin C ABI designed now and frozen at v1.0, the plan to harvest the user's converter, and the format and writer roadmaps. It does not specify the chunk store (`esses_store`), the sidecar document (`esses_model`), lap rules (`esses_analysis`) or the Open dialog widgets; it names the interfaces it consumes from them. Everything about i2 behaviour here comes from the public sources named in §3 and §13 and from the four Conversion Wizard sample files; no MoTeC binary was inspected.

**Related decisions.** D-003 (naming, vendor-mark grep), D-004 (`CLEANROOM.md` provenance), D-005 (oracle policy), D-007 (Qt-free line), D-008 (`Micros`, `Rate`), D-009 (series kinds, validity), D-010 (base units at decode, `.ld` re-quantisation on write), D-011 (`IChunkProvider`, `ChunkCache`, DCF deferred), D-013 (identity, dictionaries, `ChannelResolver`), D-014 (markers, provenance), D-015 (sidecar, `.ldx` merge and export-only writer), D-019 (no index DB in MVP; `probe_json` catalogue rows at v1.0), D-028 (importer interface, C ABI, harvest), D-029 (text engine, profiles, dictionaries), D-030 (scripted importers at v1.1), D-031 (threading), D-033 (fixtures, fuzzing), D-034 (dependencies; vendored under `third_party/` since D-147), D-035 (MVP scope), D-142 (`.ld` reader and writer ported from the user's converter).

---

## 1. Contracts

### 1.1 Primitive types

This section is the single declaration of the `esses_formats` contract types; 01 §4.1 and §6.2 summarise them and point here. `Result<T>`, `Error` and `ErrorCode` are 01 §6.1's.

```cpp
namespace esses::formats {
using LayerIx = std::uint32_t; using ChannelIx = std::uint32_t;

class ByteSource {                       // read-only view of a file (MappedFile) or an in-memory buffer (fuzz, companions)
public:
  virtual ~ByteSource() = default;
  virtual const std::filesystem::path& path() const = 0;     // may be empty for buffers
  virtual std::uint64_t size() const = 0;
  virtual std::span<const std::byte> span(std::uint64_t offset, std::uint64_t len) const = 0;   // zero-copy for mmap; throws nothing: clamps and reports via size()
  // Identity (SourceId): xxh3-128 over (size, first 1 MiB, last 1 MiB, format header). Final before pipeline step 3;
  // reads at most ~2 MiB plus the header, never the whole file. mtime is deliberately excluded so a copied or moved
  // log keeps its id, sidecar match and cache keys.
  virtual Hash128 quickHash(std::span<const std::byte> formatHeader = {}) const = 0;
  // Full-content xxh3-128: computed once at Background after first paint (sequential read), cached; never on the
  // open path. Stored in provenance and the sidecar; a later mismatch logs "source.changed" and invalidates caches.
  virtual Result<Hash128> contentHash(CancelToken) const = 0;
  virtual std::vector<std::filesystem::path> siblings() const = 0;   // directory listing with the same stem (companion discovery)
};

struct SniffInput {                      // everything sniff may look at; no ByteSource access, so no read beyond the head
  std::filesystem::path path;
  std::span<const std::byte> head;       // first 64 KiB
  std::uint64_t size;
  std::span<const std::filesystem::path> siblings;
};
struct SniffResult {                     // D-028
  int confidence = 0;                    // 0..100; 0 = not mine
  std::vector<std::filesystem::path> companions;   // .ldx beside .ld, video beside both, .itssn beside .itlog
  std::string reason;                    // one line for the Open dialog and ImportLog ("magic 0x40, pointers plausible")
};

struct ImportLogEntry {                  // the one diagnostic record of esses_formats (01 §6.2 ImportMessage is this type)
  enum class Level { Info, Warning, Error } level;   // Info records a heuristic decision (01 P-9)
  std::string code;                      // stable id, e.g. "ld.count_mismatch", "text.ragged_row"
  std::string message;                   // UTF-8, human readable, no vendor marks outside format names
  struct Context { std::optional<std::string> layer, channel, detail;   // detail: the value chosen, e.g. "afterMul"
                   std::optional<std::uint64_t> line, offset; std::uint32_t repeats = 1; } ctx;
};
class ImportLog {                        // thread-safe; one per reader for sniff/probe/open entries
  mutable std::mutex m_; std::vector<ImportLogEntry> items_; std::uint64_t version_ = 0;
public:
  void add(ImportLogEntry e);            // coalesces identical (code, layer, channel) by bumping ctx.repeats after the 3rd occurrence
  std::vector<ImportLogEntry> snapshot() const;      // copy under the lock; canonical order (code, layer, channel, offset)
  std::uint64_t version() const;
  bool hasErrors() const;
};
class DecodeLog : public ImportLog {};   // per source: entries raised by chunk decodes on workers (code, channel coalesced)

struct ImportOptions {
  std::string importerId;                // chosen by the registry or the user override
  std::string importerVersion;           // filled by the registry
  std::optional<std::string> profileId;  // text.profile:* only
  std::optional<ProbeResult> probe;      // a prior probe the reader may reuse instead of re-parsing the header
  JsonDoc options;                       // importer-specific, validated against the importer's options schema (e.g. ld.shiftOrder)
  bool applyDictionary = true;           // false: raw vendor view (fixtures, "Open as raw")
  std::optional<std::filesystem::path> unitMap;   // channels/unitmaps/<importerStem>.json (headless use)
  bool strictUnits = false;              // CLI: fail instead of opening unitless
  std::optional<Hash128> expectedHash;   // sidecar source check (quick hash); mismatch → warning "sidecar.source_changed"
};
```

**Where diagnostics go.** Entries raised during sniff, probe and open go to the reader's `ImportLog`; they are deterministic (single thread, file order) and are the only entries in `probe_json.warnings[]`, so `expected.probe.json` compares byte-for-byte. Entries raised while chunks decode on workers (a bad row in slab 7, a page fault) go to the source's `DecodeLog`, which is shared by all of that source's chunk providers, coalesced per `(code, channel)` and never serialised into `probe_json`. `ImporterRegistry::decodeLog(SourceId)` hands it to `esses_qtmodel::SourceLoader`, which drains `snapshot()` into `MessagesModel` whenever `version()` has moved, checked inside the chunk-completion closures it already receives through `Dispatcher` (01 §2.2 rule 2: no callback from layer 2 into a QObject). `esses-cli` prints the drained entries sorted by `(code, layer, channel, offset)`.

### 1.2 `ProbeResult` and `probe_json` (schema `probe-1`)

`ProbeResult` is the complete metadata of a file without any bulk sample decode. Its canonical JSON serialisation (`probe_json`, canonical form of 02 §6.5 rule 1: sorted keys with `$schema`/`schemaVersion` first, UTF-8, shortest round-trip floats, `Micros` as integers, no non-finite numbers) is the Open dialog's Details pane, the fixture expectation `expected.probe.json` (D-033) and the v1.0 catalogue row (D-019). The contract is one schema, three uses; nothing may be added to the fixture expectation that is not in `probe_json`. This section is the canonical `probe-1`; 01 §8.2 points here. The catalogue's key `(path, size, mtime, quick hash)` is stored beside the row, not inside it, so a fixture's expectation does not depend on where the file lives.

```cpp
struct ChannelInfo {
  std::uint32_t index;                   // position in the layer, stable for the life of the reader
  std::string loggedName, shortName, vendorUnit;   // raw strings as found (trimmed, NUL-stripped, transcoded to UTF-8)
  std::optional<std::string> vendorId;   // e.g. ld counter, DBC signal, ibt var name
  std::string vendorDtype;               // on-disk type as the format names it: "i16", "i32", "f16", "f32", "f64", "u16", "text"
  Dtype dtype; SampleKind kind;          // D-009 storage dtype after decode (F32 | F64 | I32 only)
  std::variant<Rate, Timestamped> timing; // uniform rate or timestamped (then timesUs come from readTimestamps)
  std::uint64_t sampleCount; Micros t0Us; int dps;
  std::optional<std::uint32_t> enumTable;
  std::uint32_t flags;                   // ChannelFlags::VendorDerived | HasDisplayRange | StatusBits | UnitFromShortName | DtypeGuessed
};
// Markers, including device events (cls = DeviceEvent, group = "DeviceEvents", comment = event message; 02 §4.3):
// one record type, mapped 1:1 onto model::Marker. JSON keys: class, tUs, name, group, comment, flags, ordinal.
struct MarkerInfo { MarkerClass cls; Micros tUs; std::string name, group, comment; std::uint32_t flags; std::uint32_t ordinal; };
struct LayerInfo  { std::string name /*"Normal"*/; Micros durationUs; std::optional<UtcInstant> utc; std::vector<ChannelInfo> channels;
                    std::vector<MarkerInfo> markers; };
struct ProbeResult {
  std::string importerId, importerVersion, dialect;   // dialect: comma-separated §3.8 flags ("m1", "c-series", "pc-layout",
                                                      // "converter-legacy", "sim-exporter", "pro-logging"), "" when none
  std::vector<Detail> details;           // D-013 Detail{id, value, prov=Logged}
  std::vector<LayerInfo> layers;
  std::vector<EnumTableInfo> enumTables; // {id, entries[{value,label,severity}]}; severity 0 normal … 3 fault (02 §4.1)
  std::vector<std::filesystem::path> companions;   // file names only (relative to the source's directory)
  std::vector<ImportLogEntry> warnings;  // sniff/probe/open entries only (§1.1)
  JsonDoc toJson() const;  static Result<ProbeResult> fromJson(const JsonDoc&);
};
```

Example `probe_json` (abridged, an ACC-generated `.ld`; written for reading, not in canonical key order):

```json
{ "$schema": "urn:esses:probe-1", "schemaVersion": 1,
  "importerId": "motec.ld", "importerVersion": "0.1.0", "dialect": "sim-exporter",
  "companions": ["bathurst_r1.ldx"],
  "details": [ {"id": "Venue", "value": "Bathurst"}, {"id": "Driver", "value": "M. Minton"}, {"id": "Vehicle Id", "value": "GT3-07"},
               {"id": "Log Date", "value": "09/10/2026"}, {"id": "Log Time", "value": "14:32:11"}, {"id": "Device Type", "value": "ADL"} ],
  "layers": [ { "name": "Normal", "durationUs": 1834560000,
    "channels": [ { "index": 0, "loggedName": "Engine RPM", "shortName": "RPM", "vendorUnit": "rpm", "vendorId": "12001",
                    "vendorDtype": "i16", "dtype": "F32", "kind": "Real",
                    "timing": {"rate": {"num": 50, "den": 1}}, "sampleCount": 91728, "t0Us": 0, "dps": 0, "flags": 0 } ],
    "markers": [ {"class": "BCN", "tUs": 83412340, "name": "Manual.1", "group": "Beacons", "comment": "", "flags": 77, "ordinal": 0} ] } ],
  "enumTables": [],
  "warnings": [ {"level": "Warning", "code": "ld.count_mismatch", "message": "header declares 42 channels, list has 44",
                 "ctx": {"detail": "44"}} ] }
```

### 1.3 `IImporter`, `IReader`, `IWriter`

```cpp
struct Block {                           // host-owned output window inside a Chunk under construction (D-011)
  Dtype dtype; std::uint64_t first; std::uint32_t count;
  std::span<std::byte> values;           // count * sizeof(dtype) bytes, 64-byte aligned
  std::span<std::uint8_t> validBits;     // empty => reader asserts all valid; else 1 bit per sample, 1 = valid
  std::span<Micros> timesUs;             // non-empty only for timestamped channels
};
class BlockSink {                        // used by readMany: the reader asks for windows and commits them (zero-copy into chunks)
public:
  virtual Result<Block*> begin(LayerIx layer, ChannelIx channel, std::uint64_t first, std::uint32_t count) = 0;
  virtual void commit(Block*) = 0;       // seals the window; after commit the pointer is invalid
};
using Progress = std::function<void(double fraction)>;   // passed as const Progress&; may be empty

class IReader {
public:
  virtual ~IReader() = default;
  virtual const ProbeResult& probe() const = 0;
  virtual Result<void> readBlock(LayerIx, ChannelIx, std::uint64_t first, std::uint32_t count,
                                 Block& out, CancelToken) = 0;                           // random access; workers only
  virtual Result<void> readMany(LayerIx, std::span<const ChannelIx> channels, std::uint64_t first, std::uint32_t count,
                                BlockSink&, const Progress&, CancelToken) = 0;           // one sweep; message-stream and text formats
  virtual Result<void> readTimestamps(LayerIx, ChannelIx, std::uint64_t first, std::uint32_t count,
                                      std::span<Micros> out, CancelToken) = 0;
  virtual std::span<const MarkerInfo> markers(LayerIx) const = 0;
  virtual std::optional<EnumTableInfo> enumTable(std::uint32_t id) const = 0;
  virtual std::vector<ImportLogEntry> warnings() const = 0;   // snapshot by value of the sniff/probe/open ImportLog (§1.1)
  virtual std::shared_ptr<DecodeLog> decodeLog() const = 0;   // shared with this source's chunk providers
  virtual bool chunkIndependent() const = 0;  // true: readBlock may run in parallel across chunks (ld, text table layout); false: serialise
  virtual std::vector<std::byte> rawHeader() const { return {}; }   // verbatim header bytes for a writer round trip (.ld: header and
                                                                   // detail records, 0x5DE..0x5E1 held as 0, D-143); empty when none
  virtual std::shared_ptr<const SourceEncoding> sourceEncoding(LayerIx, ChannelIx) const { return nullptr; }  // on-disk encoding of a
                                                                   // channel (02 §4.1; .ld: the §3.4 record); null when none
};

class IImporter {                        // D-028 shape; every member is const and thread-safe (the registry hands out const IImporter*)
public:
  virtual ~IImporter() = default;
  virtual std::string_view id() const = 0;                  // "motec.ld", "motec.csv", "text.profile:generic.csv"; storage owned by the importer
  virtual std::string_view version() const = 0;             // semver; part of every cache key
  virtual std::span<const std::string> extensions() const = 0;   // lower-case with dot
  virtual SniffResult sniff(const SniffInput&) const = 0;                                // head + size + siblings only
  virtual Result<ProbeResult> probe(const ByteSource&) const = 0;                        // ≤ 200 ms on a 500 MB file; no bulk decode
  virtual Result<std::unique_ptr<IReader>> open(std::shared_ptr<const ByteSource>, const ImportOptions&) const = 0;  // reader keeps the source alive
  virtual std::optional<JsonDoc> optionsSchema() const { return std::nullopt; }
};

struct ExportView {                      // what a writer sees; built by the GUI/CLI from a DataSource snapshot
  std::shared_ptr<const DataSource> source; std::string layer = "Normal";
  std::vector<ChannelId> channels; Micros start, end;      // [start,end)
  std::optional<Rate> resampleRate;      // writers may resample (importers never do, D-009)
  bool includeMarkers = true, includeDetails = true;
};
struct WriterOptions { JsonDoc options; };
struct WriteReport { std::uint64_t bytes; std::vector<ImportLogEntry> notes; };
class IWriter {
public:
  virtual ~IWriter() = default;
  virtual std::string id() const = 0;  virtual std::vector<std::string> extensions() const = 0;
  virtual Result<WriteReport> write(const std::filesystem::path&, const ExportView&, const WriterOptions&, const Progress&, CancelToken) = 0;
};
}
```

Numbered contract rules:

1. `sniff` sees only `SniffInput` (first 64 KiB, size, sibling list); it cannot read beyond the head. It returns `confidence` 0 for "not mine"; ≥ 90 only when magic *and* structure checks pass; 50–89 for magic-only; 20–49 for extension-only; the generic CSV profile caps itself at 30 so any specific profile wins.
2. `probe` never decodes samples. The fixture suite enforces ≤ 200 ms on the 500 MB synthetic `.ld` and ≤ 2 s on the 300 MB CSV (pass 1 only).
3. `open` is O(header) for binary formats. Text formats may run pass 1 (line index) inside `open`; the budget is the CSV first-paint budget (outline §8). `open` receives the source as `std::shared_ptr<const ByteSource>`; the returned `IReader` and every `LdChunkProvider`/`ImporterChunkProvider` built from it hold that `shared_ptr`, so no reader or provider outlives its bytes (01 §5.3 rule 9).
4. `readBlock`/`readMany` run on `TaskPool` workers only (D-031; `Visible` for component demand, `Background` for prefetch); `CancelToken` is checked at least once per 65,536 samples.
5. A `chunkIndependent()` reader must tolerate concurrent `readBlock` calls on different chunks (the `.ld` reader is stateless over an mmap); other readers are called by one thread at a time.
6. Readers convert samples to the quantity's SI base unit while writing the `Block` (D-010) using the `Converter` resolved in pipeline step 4; the raw vendor unit string stays in `ChannelInfo::vendorUnit`.
7. Invalid samples: float dtypes write NaN *and* clear the bit when a bitmap is present; integer dtypes must use the bitmap (D-009). Sentinels and parse failures are invalid, never errors.
8. Errors cross the boundary as `Result<T>` (01 §6.1), never exceptions. Readers use `ErrorCode::Io` (read or mmap failure, page fault), `Format` (malformed content), `Unsupported` (valid but not implemented), `NotFound` (missing companion, unknown layer/channel index), `Invalid` (bad options), `Cancelled`, `OutOfMemory` and `Internal`; `esses_status` in the C ABI mirrors `ErrorCode` 1:1 by value (§10.2).
9. Every heuristic a reader applies (count mismatch, guessed dtype, unit taken from the short name, shift order chosen) produces an `ImportLogEntry` with a stable `code` (decode-time ones in the `DecodeLog`, §1.1); the Import Log is how tolerant parsing stays honest (outline §9).
10. `IWriter::write` writes to `<path>.tmp` and renames on success; a cancelled write leaves no partial file.

---

## 2. `ImporterRegistry` and the open pipeline

```cpp
class ImporterRegistry {
public:
  void add(std::unique_ptr<IImporter>);  void add(std::unique_ptr<IWriter>);
  void addProfile(TextProfile);                               // builds a text.profile:<id> IImporter
  std::vector<std::pair<const IImporter*, SniffResult>> sniff(const std::filesystem::path&) const;   // best first, ties by registration order
  // Task A (steps 1–4). A resolved open continues into task B on the same worker and yields the DataSource; an open
  // with unresolved units/names and a non-null hook ends with the PendingResolution the hook built (never blocks).
  Future<LoadResult> load(const std::filesystem::path&, ImportOptions, TaskPool&, const Progress&, CancelToken,
                          MappingHook /*null in CLI*/);
  // Task B (steps 5–7), submitted by the GUI with the user's decision by value.
  Future<std::shared_ptr<const DataSource>> resume(PendingResolution, MappingDecision, TaskPool&, const Progress&, CancelToken);
  const IImporter* find(std::string_view id) const;  const IWriter* writer(std::string_view id) const;
  std::shared_ptr<DecodeLog> decodeLog(const SourceId&) const;   // registered at open, released on source close (§1.1)
  JsonDoc catalogue() const;                                  // ids, versions, extensions, options schemas → docs generator, Open dialog filters
};
struct OpenState;                        // opaque state task A hands to task B: reader, probe, skeleton DataSource, ResolutionTraces
struct UnresolvedRow { std::string loggedName, vendorUnit; std::optional<std::string> inferredQuantity; std::vector<double> sampleValues; };
struct PendingResolution { std::vector<UnresolvedRow> rows; std::shared_ptr<const OpenState> skeleton; };   // end of task A
struct MappingDecision { std::vector<model::MappingRow> mappings; JsonDoc unitMap; bool saveUnitMap = false, openAnyway = false; };
using LoadResult  = std::variant<std::shared_ptr<const DataSource>, PendingResolution>;
using MappingHook = std::function<PendingResolution(std::vector<UnresolvedRow>, std::shared_ptr<const OpenState>)>;   // packages; never waits
```

Pipeline (`SourceLoader` in `esses_qtmodel` calls `load`; `esses-cli` calls it directly). Each step names its thread and the snapshot it reads.

1. **Sniff** (worker, `Interactive`). Read `head` (64 KiB) and the sibling listing once; call every importer; rank. A user override is remembered in `SettingsStore` per `(extension, preambleFingerprint)` where `preambleFingerprint = xxh3-64(first non-empty line)`. Companions are deduplicated and attached to `ImportOptions.probe->companions` later. The source's **quick hash** (§1.1: size, first and last MiB, plus the format header that probe locates in step 2; for `.ld` the 1,762-byte header and every channel header) is final before step 3; it is the `SourceId`, the cache-key and sidecar-match identity, and no step before first paint reads more of the file than the header, the channel-header list and these two windows.
2. **Probe** (worker, `Interactive`). `IImporter::probe` → `ProbeResult`; serialise to `probe_json` for the Open dialog's Details pane; with D-019's catalogue (v1.0) the row is written here keyed by `(path, size, mtime, quick hash)`.
3. **Open** (worker, `Interactive`). `IImporter::open` → `IReader`. Build a `DataSource` (`esses_model`): `id = SourceId` (the quick hash), `provenance {path, size, hash = quick hash, contentHash = empty, mtime, importerId, importerVersion, dialect, companions, probe = probe_json, rawHeader = IReader::rawHeader()}`, one `DataLayer` per `LayerInfo` (device-event markers included, 02 §4.3), one `Channel` per `ChannelInfo` (with `desc.sourceEncoding = IReader::sourceEncoding(layer, channel)`) whose `Column` is fed by `LdChunkProvider` (motec.ld) or `ImporterChunkProvider` (everything else), all sharing the source's `DecodeLog`. No sample is decoded.
4. **Resolve** (worker). In order: (a) importer `Dictionary` (§8.3) rewrites `loggedName → name, unit, quantity, interp, kind` and expands status bitfields into derived Bool channels; (b) `UnitMappingProfile` (§8.2) maps `(name, vendorUnit)` pairs to `(quantity, unit)`; (c) `ChannelResolver` (`esses_model`, D-013) applies project Mappings → Aliases → case-fold and records a `ResolutionTrace` per channel. If any channel is left with an unknown unit or name and `MappingHook` is non-null, **task A ends here**: the hook packages the unresolved rows and the partially built skeleton into a `PendingResolution`, the `Future<LoadResult>` resolves with it, and `SourceLoader` posts it to the GUI (`Dispatcher::post(context, …)`), which shows the Mapping panel (§8.1). When the user resolves (or chooses Open anyway), the GUI submits **task B** through `resume` with the `MappingDecision` by value; Cancel drops the `PendingResolution`. No worker ever waits on user input (01 §5.1 rule W (d)); headless behaviour is in §8.1. Without a hook, unknown units become unitless with status `Unit unknown: "<vendorUnit>"`.
5. **Merge edits** (worker). `SidecarDocument::load(<file>.<ext>.sidecar.json)` and the `.ldx` companion (§4) are merged with precedence sidecar > companion > file (D-015): companion markers get `prov = Companion`, `MarkerEdit` ops are applied, details with user provenance override logged ones. A `source.hash` mismatch in the sidecar logs `sidecar.source_changed` and still applies.
6. **Ranges, phase 1** (worker). `RuleRunner` (`esses_analysis`) runs the profile's phase-1 rules (beacon-pair transitions and numbering, whole-outing, detail groups) on the merged marker set and details → `RangeGroup` tree with sidecar `rangeOverrides` applied (02 §4.4 rule 13). No sample is read.
7. **Built-in maths** (worker). `MathGraph` registration; `Corr Speed`/`Corr Dist` prefetch is submitted at `Background` (D-016).
8. **Publish** (GUI). `Dispatcher::post` hands `std::shared_ptr<const DataSource>` to `DataSelectionModel`; `MessagesModel` receives the sniff/probe/open `ImportLog` snapshot.
9. **After publish** (workers, each its own task, none on the first-paint path): the phase-2 range task `ranges/<sourceId>/<layer>/<markerSetHash>` (`Interactive`; stint/pit annotations, track sections; publishes a new snapshot, 02 §4.4 rule 13); one summary task `summary/<sourceId>` (`Background`) that builds every logged channel's `SummaryPyramid` chunk-sequentially through `Column::buildSummary`, never inserting chunks into `ChunkCache` (02 §3.2 rule 10; for text sources it rides the shared slab sweep of §7.1); and the full-content hash `ByteSource::contentHash` (`Background`), stored in provenance and the sidecar, whose mismatch with a sidecar's `contentHash` logs `source.changed`.

Steps 1–4 run as task A keyed `open/<canonical path>/<size>/<mtime>/a` and steps 5–7 as task B keyed `open/<canonical path>/<size>/<mtime>/b` (01 §4.1, §5.4); the key is known before any hashing, so a second open of the same file coalesces onto the same `Future`. A resolved task A continues into task B on the same worker; the `SourceId` is a result of the tasks, not their key.

**Column cache key.** Every decoded chunk lives in the process-wide `ChunkCache` (D-011) under

```cpp
struct ColumnKey { Hash128 sourceHash /*SourceId, quick hash*/; std::string importerId, importerVersion; Hash128 profileHash; std::string layer; std::uint32_t channel; };
// profileHash = xxh3-128(text profile JSON ∥ dictionary version ∥ unit-map JSON ∥ ImportOptions.options) — anything that changes decoded base-unit values
```

and chunk number. The deferred on-disk column cache (DCF, D-011, post-v1.0) names its file `<sourceHash>-<importerId>-<importerVersion>-<profileHash>.dcf` and stores chunk payloads in the in-memory `Chunk` layout, so no importer changes when it arrives. Reopening a file with the same key is a cache hit; a dictionary or unit-map edit changes `profileHash` and invalidates exactly that source.

**Failure modes.** Sniff finds nothing → the Open dialog offers "Open as…" with every importer and the generic CSV profile. `probe` fails → the file is listed with the error and no Details. `open` fails → `Error` to Messages, nothing published. A reader error inside `readBlock` marks that chunk `AllInvalid`, logs once per channel, and the component draws a gap; the source stays open.

---

## 3. MoTeC `.ld` reader (`motec.ld`): independent implementation from public documentation

Since 2026-10-10 this reader follows the user's own Log File Converter (D-142): `docs/formats/ld-converter-port.md` specifies the converter's `.ld` reader and writer (layout, quirks, the inverse of each writer step, type mapping), and the reader is new code written from that specification, `docs/formats/ld.md` and the public notes below, because the converter's licence and lineage are not yet confirmed (D-144); the heading is kept for stable references. Spike S-1 (`docs/spikes/S-1.md`) decoded the user's 87 real `.ld` files with the independent `tools/ld_ref_decode.py`, and together with the port specification's §6 and §8 it amended §3.1 to §3.11 on 2026-10-10. The reader is checked against the public documentation, the user's own files and the vendor-software CSV exports listed in S-1, and the oracles of the synthetic fixtures come from `tools/ld_ref_decode.py` (D-041), which is never changed to agree with the reader. It is not a two-team clean room (the same developer and AI read the sources and write the code). Primary public source: Friss/i3rs `docs/ld-file-format.md` (MIT); corroborating: afonso360/motec-i2 (MIT) and tobi/motorsport-telemetry-rs `motec-telemetry` (MIT); writer references for sim dialects: the *outputs* of GeekyDeaks/sim-to-motec and stevendaniluk/MotecLogGenerator. gotzl/ldparser (GPL-3) was consulted early and is superseded by the MIT documentation: nothing from it is needed, and its struct format strings are not reproduced in anything committed (`docs/formats/ld.md` restates offsets in prose and tables only, as below). All integers little-endian. Strings are fixed-width NUL-padded fields bounded by their width (a full field has no NUL); each is decoded as UTF-8 when its bytes are valid UTF-8 and as cp1252 otherwise (amended 2026-10-10: the converter writes UTF-8, the public notes assume a single-byte code page), then transcoded to UTF-8 and right-trimmed of NUL and spaces. The probe records the encoding used once per file as Info `ld.string_encoding` with `ctx.detail` = `ascii`, `utf-8`, `cp1252` or `mixed` (S-1 found ASCII everywhere except one channel name).

### 3.1 Sniff

Confidence 95 when all hold: `size ≥ 0x6E2`; `u32@0x00 == 0x00000040`; `meta_ptr`, `data_ptr` ∈ `[0x6E2, size)`, or both equal to `size` (a header-only file, §3.4); `event_ptr == 0` or `∈ [0x6E2, size − 0x488]` (the event-record fields the reader uses, §3.3); and, when the channel record at `meta_ptr` lies inside the 64 KiB head, `prev_meta_ptr == 0` and `next_meta_ptr ∈ {0} ∪ [0x6E2, size − 124]` (logger files place the list beyond the head; probe checks it there). The u16 at 0x040 is never tested: it is 0 on C125/C127 logger files (S-1). Confidence 60 when only the magic matches and the extension is `.ld`. Companions: `<stem>.ldx` if it exists (case-insensitive on Windows). Reason strings name which check failed.

### 3.2 File header (1,762 bytes = 0x6E2)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0x000 | u32 | magic | 0x40 on all 87 S-1 files |
| 0x004 | u32 | unknown | non-zero on C125 logger files (offset of an unknown block); kept in the raw header |
| 0x008 | u32 | `meta_ptr` | offset of the first channel record; equal to `size` in a header-only file (§3.4) |
| 0x00C | u32 | `data_ptr` | offset of the first sample array (informational; each channel carries its own) |
| 0x024 | u32 | `event_ptr` | offset of the event record (§3.3); 0 = none |
| 0x040 | u16 + u32 | unknown, then 1,000,000 | the u16 is 2 on M1 and PC-layout files and 0 on C125/C127 logger files and is never tested; the u32 is 1,000,000 on every file; both kept in the raw header |
| 0x046 | u32 | device serial | detail `Device Serial` |
| 0x04A | 8s | device type | `M1`, `C125`, `C127`, `C185` on logger files, `MTCA2LD` on PC-layout files with the generic identity → detail `Device Type`; drives dialect detection (§3.8) |
| 0x052 | u16 | device version | detail `Device Version` |
| 0x054 | u16 | unknown | 0 on logger files as downloaded, 0x0080 on PC-layout files and header-modified copies; fingerprint only (§3.8) |
| 0x056 | u16 | channel count | the number of records including an M1 terminator record (§3.4); compared with the walk (`ld.count_mismatch`), never used to bound it |
| 0x058 | u16 | channel count again, or 0 | the count on M1 and PC-layout files, 0 on C125/C127; read together with 0x056 as one u32 it is what i3rs calls "unreliable"; informational |
| 0x05A | 2 × u16 | unknown | varies on M1 files; kept in the raw header |
| 0x05E | 16s | date `DD/MM/YYYY` | detail `Log Date`; layer UTC anchor if parseable (`DD/MM/YYYY` on every S-1 file) |
| 0x07E | 16s | time `HH:MM:SS` | detail `Log Time` (24 h on every S-1 file) |
| 0x09E | 64s | driver | detail `Driver` |
| 0x0DE | 64s | vehicle id | detail `Vehicle Id` |
| 0x11E | 64s | engine id | detail `Engine Id` (S-1 D11; the vehicle record's copy wins when non-empty, §3.3) |
| 0x15E | 64s | venue | detail `Venue` |
| 0x1DE | 1,024 B | unknown | free text on one S-1 file; kept in the raw header, not exposed |
| 0x5DE | u32 | "pro logging" field | zero or non-zero → dialect flag `pro-logging` only; never interpreted, validated or derived, and held as 0 in the raw header (D-143) |
| 0x5E4 | 64s | session | detail `Session` (the event record's copy wins when non-empty) |
| 0x624 | 64s | short comment | detail `Short Comment` |
| 0x687 | u8 | unknown | non-zero on logger files; kept in the raw header |
| 0x688 | u32 | log-event block pointer | device events (§3.7); 0 = none |
| 0x68C | u32 | unknown block pointer | non-zero on logger files; kept in the raw header |
| 0x690 | u32 | enumeration block pointer | enumeration definitions (§3.9); 0 = none |
| 0x694 | 32s | team | detail `Team` |

Bytes not listed are kept opaque. The reader keeps the header and the detail records of §3.3 as one opaque blob, `Provenance_Source::rawHeader` (02 §4.2; `IReader::rawHeader`, §1.3), with 0x5DE to 0x5E1 held as 0 (D-143), so the writer can re-emit unknown fields on round-trip. **S-1 amendments (2026-10-10):** the channel count is a u16 at 0x056 (S-1 D5); 0x040, 0x054, 0x11E and the pointer slots 0x687 to 0x690 come from the port specification §1.2 and §1.4 and were confirmed on the user's files.

### 3.3 Pointer-linked event, venue and vehicle blocks

| Block | At | Layout | Details produced |
|---|---|---|---|
| Event (3,156 B = 0xC54) | `event_ptr` | name 64s @0x000, session 64s @0x040, long comment 1024s @0x080, `venue_ptr` u32 @0x480, `weather_ptr` u32 @0x484 | `Event`, `Session`, `Long Comment` |
| Venue (3,102 B = 0xC1E) | `venue_ptr` | name 64s @0x000, length u32 mm @0x042, `vehicle_ptr` u32 @0x44A, category 32s @0x44E | `Venue` (overrides header venue only when header venue is empty), `Venue Length` (mm → m), `Venue Category` |
| Vehicle (3,316 B = 0xCF4) | `vehicle_ptr` | id 64s @0x000, description 64s @0x040, engine id 64s @0x080, weight u16 kg @0x0C0, fuel tank u16 L × 10 @0x0C2, type 32s @0x0C4, drive type 32s @0x0E4, diff ratio u16 × 1000 @0x104, gears 1 to 10 u16 × 1000 @0x106 to @0x118, track u16 mm @0x11A, wheelbase u16 mm @0x11C, comment 1024s @0x120, number 32s @0x524 | `Vehicle Id` (same rule), `Vehicle Desc`, `Engine Id`, `Vehicle Weight` `{v, "kg"}`, `Fuel Tank Capacity`, `Vehicle Type`, `Vehicle Drive Type`, `Diff Ratio`, `Gear 1` to `Gear 10`, `Vehicle Track`, `Vehicle Wheelbase`, `Vehicle Comment`, `Vehicle Number` |
| Weather (2,048 B = 0x800) | `weather_ptr` | sky 64s @0x000; air temperature, track temperature, barometric pressure, relative humidity and wind speed as value 16s + unit 8s pairs @0x040/0x050, @0x058/0x068, @0x070/0x080, @0x088/0x098, @0x0A0/0x0B0; wind direction 64s @0x0B8; comment 1024s @0x0F8 | `Sky`, `Air Temp`, `Track Temp`, `Baro Pressure`, `Rel Humidity`, `Wind Speed` (a `Quantityd` when the unit text resolves, else text), `Wind Direction`, `Weather Comment` |

Rules: records are reached only through their pointers, never at fixed offsets (logger files place them after a device-configuration region of about 1.4 MB on C125 and 3 MB on M1; PC-layout files at 0x6E2). A pointer of 0 means the record is absent; a record whose last used field does not fit in the file is skipped with `ld.block_out_of_range` (Info); a numeric field of 0 produces no detail. **S-1 amendments (2026-10-10):** every pointer is a u32 (S-1 D1: on logger files the event record sits above 1.4 MB, so the earlier u16 reading of `venue_ptr`/`vehicle_ptr` fails on 35 files; i3rs agrees). The record sizes, the weather record and the vehicle layout come from the port specification §1.3 and were confirmed on the user's M1 and C125 files (E-11 to E-14; i3rs's u32 weight at vehicle +0x0C0 is contradicted by the M1 values u16 279 and u16 76).

### 3.4 Channel header (124 bytes = 0x7C), doubly linked list

| Offset | Type | Field | Use |
|---|---|---|---|
| 0x00 | u32 | `prev_meta_ptr` | consistency check only |
| 0x04 | u32 | `next_meta_ptr` | 0 terminates |
| 0x08 | u32 | `data_ptr` | sample array offset |
| 0x0C | u32 | `data_len` | sample count |
| 0x10 | u16 | `counter` | a channel id on logger files, 20101 + index on PC-layout files, never 0x2EE1 + index on any S-1 file (D8); `vendorId` only |
| 0x12 | u16 | `dtype_a` | type class (§3.5) |
| 0x14 | u16 | `dtype` | byte width |
| 0x16 | u16 | `freq` | Hz, integer → `Rate{freq,1}` |
| 0x18 | i16 | `shift` | scaling (§3.6) |
| 0x1A | i16 | `mul` | scaling |
| 0x1C | i16 | `scale` | scaling; 0 → 1 |
| 0x1E | i16 | `dec_places` | scaling; `dps` only as a fallback (§3.6) |
| 0x20 | 32s | name | `loggedName` unless the aux block holds a long name; may fill the field with no NUL |
| 0x40 | 8s (or 20s) | short name or unit | unit rule below |
| 0x48 | 12s | unit | unit rule below |
| 0x54 | i32 | raw maximum | of the logged data in stored units; kept in the encoding record (never-logged channels carry 1,000,000) |
| 0x58 | i32 | raw minimum | kept in the encoding record (never-logged channels carry 1,000,000,000) |
| 0x5C | f32 | display range bound | with 0x60: flag `HasDisplayRange` unless both are 0; the lower of the two is the minimum |
| 0x60 | f32 | display range bound | |
| 0x64 | u8 | display decimals | `dps` (§3.6) |
| 0x65 | u8 | interpolation | `interp`: 1 Previous, 2 Next, 3 Linear; 0 Closest except on `c-series` files, where 0 means unset and the kind default applies (to verify); 4 and 5 → kind default (Info); `Bool` channels Previous regardless |
| 0x66 | 14 B | unknown | opaque, round-tripped (C125: a non-zero u32 at 0x6C) |
| 0x74 | u32 | gap block pointer | 0 = none; validity (below, §3.11) |
| 0x78 | u32 | aux block pointer | 0 = none; long name and enumeration reference (below) |

The reader keeps each record's 124 raw bytes and decoded fields (offsets, counter, type pair, scaling, raw max/min, display range, the two bytes at 0x64/0x65, the gap and aux pointers, the 32-byte name as stored) in an `.ld` encoding record, a `SourceEncoding` (02 §4.1) that reaches the writer through `ChannelDescriptor::sourceEncoding` (§5 item 2). Byte layouts beyond the public notes come from the port specification §1.7 to §1.9.

**Unit rule** (S-1 D6, E-19; port specification R5). Where the unit text sits depends on the producer: (a) when the file is not `c-series` and the 20 bytes at 0x40 have no NUL within the first 8, they are one 20-byte unit (converter output) and the short name is empty; (b) otherwise, when the 12-byte field at 0x48 is non-empty, the short name is the 8 bytes at 0x40 and the unit the 12 bytes at 0x48 (C-series logger files); (c) otherwise the 8-byte string at 0x40 is the unit, with flag `UnitFromShortName` and Info `ld.unit_from_short_name` (M1 and PC-layout files, sim exporters), unless the file is `c-series` and the string does not resolve in `UnitRegistry`, in which case it stays the short name, the unit is empty and Info `ld.unit_from_short_name_candidate` is logged.

**Names.** The 32-byte name may fill its field without a NUL (S-1 D7: 24 files, truncated names); Info `ld.name_unterminated`. i3rs's continuation rule (name + short name) is never applied, because the short-name field often holds the unit (`…Secondary F` + `ul`, where `ul` is µL). A long name from the aux block replaces the 32-byte name as `loggedName`.

Walk: start at `meta_ptr`; every bounds check widens each u32 operand to `uint64_t` before adding or multiplying (`u32 + u32 × u16` overflows 32 bits on hostile or large files); for each record check `offset + 124 ≤ size`, `data_ptr + data_len × dtype ≤ size` (else `ld.data_out_of_range`, channel kept with `sampleCount` clamped to what fits), `freq > 0` (else channel skipped with `ld.zero_rate`), `dtype ∈ {2,4,8}`; stop at `next_meta_ptr == 0`; abort with `ld.list_cycle` on a revisited offset; hard bound `size / 124` iterations. Records are not a fixed distance apart: each may be followed by its gap block, its aux block and other per-record data, so the next record is found only through `next_meta_ptr` and no stride is ever derived (S-1 D2: distances of 124 to 2,487 bytes). The `m1-212` stride rule is withdrawn: no M1 file has fixed 212-byte records, and a 212-byte distance is a record followed by an aux block that holds only an enumeration reference (124 + 88 bytes), whose index-size and index fields are what i3rs reads at 0xD0 and 0xD2 (S-1 D3). The u16 count at 0x056 is compared with the records walked, the terminator included; a difference → `ld.count_mismatch` (Warning). Duplicate names → the later one gets suffix ` (2)` and `ld.duplicate_name`.

**M1 terminator** (S-1 D4, E-23). M1 lists end with a record of rate 0, count 0, type pair 0/0 and an empty name. It is dropped with Info `ld.terminator_entry`, is not a channel and is never reported as a nameless one, and it counts in the header channel count.

**Header-only file** (S-1 §11 item 5). `meta_ptr == size` with `data_ptr == size` (no channel list; one converter test output in S-1) is a valid empty source: layer `Normal`, no channels, duration 0, details from the header and detail records, and Warning `ld.header_only`. It is not an error.

**Gap block** (at record u32@0x74; port specification §1.8):

| +Off | Type | Field |
|---|---|---|
| +0x00 | u16 | 0 |
| +0x02 | u16 | channel rate in Hz (Info `ld.gap_rate_mismatch` when it differs from the record's) |
| +0x04 | u32 | number of 6-byte events, two per gap |
| +0x08 | u32 | event array pointer (block + 44 on every file seen) |
| +0x0C | 32 B | zero |
| +0x2C | 6 B each | u16 kind (0 = invalid from this sample, 1 = valid again from this sample) + u32 sample index |

**Aux block** (at record u32@0x78; port specification §1.9):

| +Off | Type | Field |
|---|---|---|
| +0x00 | u16 | 0 |
| +0x02 | u32 | content pointer (block + 70 on every file seen) |
| +0x06 | 64 B | zero |
| +0x46 | u32 | 1 |
| +0x4A | u32 | argument count |
| +0x4E | arguments | type 5, enumeration reference (10 B): u32 5, u16 0, u16 2, u16 table index (§3.9); type 4, long name: u32 4, u16 0, u16 `len` (bytes + 1), `len` bytes including the NUL, followed by one pad byte in converter output and none in M1 files |

An unknown argument type stops the scan with Info `ld.aux_unknown_arg`; a block or argument beyond the file logs `ld.aux_out_of_range` (Info); `len` must be at least 1 and trailing NULs are stripped; the next argument starts at `pos + len`, or at `pos + len + 1` when a valid type sits there. These two blocks are the per-record extension data S-1 found between records (D2); with the records they form the channel list for the O(header) rule (§14 item 4), and bytes between records that no pointer names are ignored.

### 3.5 Data types

| `dtype_a` | `dtype` | Decoded as | Notes |
|---|---|---|---|
| 0x07 | 2 | F16 → F32 chunk | half-precision; i3rs reads via `half` |
| 0x07 | 4 | F32 | |
| 0x00, 0x03, 0x05 | 2 | I16 → F32 after scaling (I32 if scaling is identity and `kind` Integer) | gotzl writes 0x03 for int16; the user's converter writes 0x00/2 only for Bool channels, but the reader keeps 0x00 Integer unless a dictionary says Bool, because header evidence cannot tell converter output from vendor PC-software output (S-1 §2) |
| 0x00, 0x03, 0x05 | 4 | I32 → F32/F64 after scaling | gotzl writes 0x05 for int32 |
| 0x06 | 4 | plain signed integer, treated like 0x05/4 (`SampleKind::Integer`, `Interp::Previous` when the scaling is identity) | S-1 D9: GPS date as DDMMYY, satellite counts and warning counters on M1 files; not a bitfield (amended 2026-10-10) |
| 0x08 | 8 | F64 | only on GPS channels in S-1 (7 files); kept F64 (D-009) |
| 0x04 | 2 / 4 | raw unsigned integer, `kind = Bitfield`, flag `DtypeGuessed` | public sources: "state channels", undecoded; not seen in S-1 |
| 0x00 | 0 | none | only in the M1 terminator record (§3.4) |
| other | any | raw integer of `dtype` width, flag `DtypeGuessed`, `ld.unknown_dtype` | motec-i2 maps some exporter pairs to Invalid; we decode and warn |

Scaling targets: a channel whose scaling is identity (`scale ∈ {0,1}`, `dec = 0`, `mul = 1`, `shift = 0`) and whose integer dtype has no dictionary `kind` override is stored I32 (`SampleKind::Integer`, `Interp::Previous`); every other integer channel becomes F32 (F64 when `dec_places ≥ 5`, so that lat/lon-like scaled integers keep precision). *Amended 2026-10-11 (M0 review):* an identity-scaled u32 (0x04/4) is stored F64, like the 8-byte integers, so 0xFFFFFFFF reads 4,294,967,295; a scaled integer is stored F64 when F32 would lose raw steps, that is when the effective number of decimals `dec_places + floor(log10 |scale|)` is 5 or more (M1 files carry decimals in `scale = 10000` with `dec_places` 1–4, E-24) or when the record's raw range (0x54/0x58) of a 32- or 64-bit integer exceeds 2^24 (`docs/formats/ld.md` §Data types). Decoding dispatches on the `(dtype_a, dtype)` pair, never on `dtype_a` alone (0x07/2 is F16). Scaling applies to every dtype, float ones included, unless it is identity; a float channel with non-identity scaling logs `ld.float_scaling_nonidentity` (Warning; none in S-1). A NaN sample in a float channel is invalid (S-1 D13, §3.11).

### 3.6 Scaling — the shift-vs-mul ambiguity

Public sources disagree:

- gotzl/ldparser: `value = (raw / scale × 10^−dec + shift) × mul`
- Friss/i3rs: `value = (raw / scale) × 10^−dec × mul + shift`
- afonso360/motec-i2: asserts `shift == 0` (unsupported otherwise)

`scale == 0` is treated as 1 by all three. The reader implements both as `ImportOptions.options.ld.shiftOrder ∈ {"beforeMul", "afterMul"}`, default **`afterMul`** (D-037 as amended 2026-10-10; originally `beforeMul`, gotzl's reading), and logs `ld.shift_nonzero` (Info, once per file) whenever a channel has `shift ≠ 0` and `ld.shift_mul_order` (Info, `ctx.detail` = the order used) when a channel has `shift ≠ 0 ∧ mul ≠ 1`. **Settled by spike S-1** (`docs/spikes/S-1.md` §7): the two logger channels with `shift ≠ 0 ∧ mul ≠ 1` both carry `shift 32, mul 9, scale 5, dec 1`, unit °F and raw counts in 0.1 °C, the °C to °F conversion that only afterMul expresses; on `20251009-1552000.ld` (C185 logger data) `Gbox Oil Temp` reads 75.2 °F (24.0 °C) at a cold start where coolant reads 24.0 °C, against 331.2 °F (166 °C) under beforeMul. The i2 CSV export of the same file, which this section originally required (permitted since Q-03 (1) was answered yes on 2026-10-10), is still welcome as confirmation and is recorded in `CLEANROOM.md` when made; the option stays for dialects. `mul == 0` is treated as 1 with `ld.zero_mul` (Warning), and a `scale` of 0 never drops `mul` (the converter's C++ reader did). Arithmetic is F64 then narrowed.

**Display decimals** (amended 2026-10-10; port specification §1.7, E-20): `dps` is the byte at record 0x64 when it is non-zero or when `dec_places` is 0, else `max(0, dec_places)` with Info `ld.dps_from_dec`. The byte differs from `dec_places` on many M1 channels; C-series files write 0 there with `dec_places` 2 (to verify).

### 3.7 Time base and layers

One layer `"Normal"`. Every channel is `UniformSeries{Rate{freq,1}, t0 = 0}`; `timeAt(i)` per D-008 and 02 §1 behaviour 1. Channel lengths differ; `LayerInfo::durationUs = max over channels of rate.timeAt(sampleCount, 0)` (128-bit safe). The layer UTC anchor is `Log Date` + `Log Time` parsed as local time without zone (flagged `utcAssumedLocal`). The `.ld` carries no lap beacons; those come from `.ldx` (§4), and S-1 found no `.ldx` with markers among the user's 42 (D14), so marker parsing is tested synthetically until such a file exists. The `.ld` does carry **device events** (amended 2026-10-10; port specification §1.5): the log-event block at the header pointer u32@0x688 holds a u32 block type (must be 1; any other type is ignored with Info `ld.event_block_unknown`, as on some C125 files), a u32 record count, a u32 record-array pointer and 32 reserved bytes, then 20-byte records of u16 code, f64 time in µs from log start and 10 reserved bytes. Each record becomes `MarkerInfo{cls = DeviceEvent, group = "DeviceEvents", tUs = llround(time), name = Esses' own label for the code (codes 0 to 5; `Event <code>` otherwise), comment = "", flags = code}` (02 §4.3, D-132 (d)); markers are ordered by `(tUs, stored order)`, because the stored order groups events by restart, and `ordinal` follows 02 §4.3 rule 5. Codes 0 to 52 occur on the user's M1 files. A record array beyond the file logs `ld.event_block_malformed` (Warning) and keeps the records that fit. The block is metadata and is read in probe. An I32 channel named like a lap counter (`Lap`, `Lap Number`, `Lap Count`) or a 5 Hz `Lap Beacon`-style channel on M1 logs is *not* turned into markers by the reader; `BeaconPairRule` may consume it through a `lapSource` option in a later milestone (listed in §15 item 5). The converter's `Beacon (Internal)` channel (1 Hz int16 lap words, port specification §1.12) is likewise an ordinary channel in M0, named once by Info `ld.beacon_channel`.

### 3.8 Dialects

| Dialect | Detection | Tolerance |
|---|---|---|
| `m1` | device type `M1` | unit at 0x40 (unit rule (c)); terminator record expected; enumeration definitions through 0x690 (§3.9) |
| `c-series` | device type `C` followed by digits, in the logger-file layout (u16@0x040 = 0, event record after the device-configuration region) | short name at 0x40 and unit at 0x48 (unit rule (b)); interpolation byte 0 read as unset. *Amended 2026-10-11:* these record rules follow the logger layout alone, whatever the device type, because header-modified copies replace the device type but keep the logger's records; the device type decides only the label |
| `pc-layout` | event record at 0x6E2 directly after the header and u16@0x054 = 0x0080 | none; written by PC software, the vendor's or the user's converter, which header evidence cannot tell apart (S-1 §2); counters 20101 + index |
| `converter-legacy` | channel list at 0x1336 with 132-byte records and zero 0x74/0x78 slots (port specification Q-W30) | Info `ld.legacy_layout`; no long-name recovery |
| `sim-exporter` | the header defaults of a public writer (device type, serial, version and the u16 at 0x054, or the u32 at 0x05A together with the writer's 0 at 0x040 outside the logger layout, as listed in `docs/formats/ld.md` "Producer fingerprints"; the u32 at 0x05A alone also occurs on PC-software files, amended 2026-10-11). Unsettled: synthetic families only | unit rule (c); names of exactly 32 bytes matched by prefix in the dictionary; odd dtype pairs per §3.5 |
| `pro-logging` | u32@0x5DE ≠ 0 | none; recorded as zero or non-zero only (D-143) |

`ProbeResult::dialect` lists the flags that apply in this table's order, comma-separated (for example `"m1,pro-logging"`), and is `""` when none applies; it is in `probe_json` so fixture families (D-033) assert it. **S-1 amendments (2026-10-10):** the `m1-212` stride dialect is withdrawn (§3.4), and so is `sim-exporter` detection by a counter outside `0x2EE1 + i`, by a unit in the short-name field or by 32-character names, which matched real logger files (S-1 D6 to D8); those are per-channel rules now (§3.4). No producer fingerprint uses the value at 0x5DE (D-143).

### 3.9 Enum tables (best effort, M1 only)

Amended 2026-10-10 (S-1 D12, port specification §1.6; the heading is kept for stable references): the tables come from the enumeration block at the header pointer u32@0x690, on every dialect, in M0. The block holds u32 1 and a u32 table count; each table holds u32 2, a u16 value count, u16 2 (index size) and a u16 table index; each value holds u32 3, a u16 extended flag, a u16 item size (label bytes + 3), an i16 value and the label with its NUL, and, when the flag is set, a 10-byte severity block (u32 6, u16 0, u16 2, u16 severity: 0 normal, 1 info, 2 warning, 3 fault). A channel names its table through the type-5 argument of its aux block (§3.4), by table index and never by position, and becomes `SampleKind::Enum` with `Interp::Previous`. Values 0xFFFF/0xFFFE are the i16 values −1/−2. Severity is kept per entry (`EnumTableInfo`, §1.2; `EnumTable`, 02 §4.1). A marker mismatch or an item size below 3 ends that table and keeps the earlier ones (`ld.enum_block_malformed`, Warning); a reference to a missing index logs `ld.enum_ref_missing` (Info) and the channel stays Integer. Only when 0x690 is 0 and a channel references an index does the reader fall back to i3rs's scan of the region between the last channel record and the first data array for `u16 count, u16 type == 0x0002, u16 id` followed by `count` entries (the same grammar seen from inside a table), logging `ld.enum_scan_failed` when it finds nothing. S-1's scan found tables on 17 files, M1 and PC layout alike.

### 3.10 Must be verified against the user's real files (recorded in `CLEANROOM.md` as "observed on user-owned files")

Status after spike S-1 and the port specification (2026-10-10; details in `docs/spikes/S-1.md` §3 and `docs/formats/ld.md`):

1. Magic value 0x40: confirmed on all 87 files; no other header version seen.
2. Width of `venue_ptr`/`vehicle_ptr`: u32 (§3.3).
3. Shift-vs-mul order (§3.6): `afterMul`, by physical evidence on two logger channels; a vendor-software CSV export of one of the two files would add confirmation.
4. The 212-byte M1 entry: not an M1 layout; a 124-byte record followed by an aux block (§3.4).
5. Enum tables: read through the 0x690 pointer; the scan is only a fallback (§3.9).
6. `dtype_a` 0x06: plain integers (§3.5); 0x04 not seen.
7. `counter`: a channel id on logger files, 20101 + index on PC-layout files, never 0x2EE1 + index.
8. The "pro logging" u32: recorded as zero or non-zero only (D-143). F64 (0x08, 8) only on GPS channels.
9. Date/time: `DD/MM/YYYY` and 24 h `HH:MM:SS` in every `.ld` header; the vendor CSV export of the same data writes `M/D/YYYY` and a 12-hour time, so the CSV reader must not reuse the `.ld` order.

Still open (port specification §6.3, S-1 §11): the encoding of non-ASCII text in logger and vendor-software files; the meaning of interpolation byte 0 and display-decimals byte 0 on C-series files; the unit rule on C-series channels that have a short name and no unit; the unknown blocks at 0x004, 0x02C, 0x68C and record 0x6C; the raw max/min sentinel pair; event codes 6 to 52; the beacon channel's fraction units; F16 on any real file; and real sim-exporter files and `.ldx` files with markers, none of which exist on the development machine.

### 3.11 `LdChunkProvider`

Implements `IChunkProvider` (D-011) over the `MappedFile`: `materialise(chunkNo)` decodes samples `[chunkNo × 65536, …)` of one channel from `data_ptr` with the scaling of §3.6 into the chunk, sets the chunk's validity from the channel's gap intervals (below), then builds the colocated pyramid. `chunkIndependent() = true`; the open path is O(header) and first paint of a 100 MB file touches only the chunks the first worksheet needs (outline §8).

**Validity** (amended 2026-10-10; port specification §1.8, E-16). The format has validity after all: probe turns each record's gap block (§3.4) into sorted, merged, half-open invalid intervals `[s, e)` clamped to `[0, count)`. Each event pair `(0, s), (1, e)` marks samples `s` to `e − 1` invalid, `e` being the first logged sample after a restart. An entry without an exit runs to the end of the data (Info `ld.gap_unpaired`); unsorted or overlapping intervals are merged (Info `ld.gap_unsorted`); an index beyond the count is clamped (Info `ld.gap_out_of_range`). The stored values inside a gap are stale held values, so validity comes from this block, never from the samples. A chunk that no interval touches is `AllValid`, a chunk an interval covers is `AllInvalid`, any other is `Bitmap`; F32/F64 chunks also write NaN for invalid samples (§1.3 rule 7), and a NaN already stored in a float channel is invalid too (S-1 D13). `tools/ld_ref_decode.py` does not decode gap blocks and marks every sample valid, so its oracles leave validity unverified on files with gap blocks (S-1 §11 item 2): validity is checked against the vendor-software CSV exports (invalid cells export empty) and, for synthetic families, against the generator's own input, never against the reader (D-041).

Changed or vanishing file (02 §1 behaviour 5): the mapping is opened `FILE_SHARE_READ | FILE_SHARE_DELETE`; before each decode batch the provider calls `MappedFile::changedOnDisk()` and, when the file changed, fails the chunk with `Io`, logs `ld.source_changed` once in the `DecodeLog` and lets the GUI offer Reload; the decode loop itself is a plain C routine run through `MappedFile::guarded()`, so a page fault (truncated file, dropped network share) seals that chunk `AllInvalid` with `ld.io_fault` instead of terminating the process.

---

## 4. `.ldx` companion reader

Structure derived as facts from the user's own logger and ACC `.ldx` files and from the MIT projects that handle the format (Friss/i3rs reads `.ldx`; tobi/motorsport-telemetry-rs and afonso360/motec-i2 write it). GeekyDeaks/sim-to-motec (`ldx.py`, no licence) and the SARUSySLab reader notes (licence not stated) were consulted for confirmation only: no text, code or template is copied from them. Everything not listed is tolerated and preserved.

```xml
<LDXFile locale="English_United Kingdom.1252" DefaultLocale="C" Version="1.6">
  <Layers>
    <Layer>
      <MarkerBlock>
        <MarkerGroup Name="Beacons" Index="N-1">
          <Marker Version="100" ClassName="BCN" Name="Manual.1" Flags="77" Time="123456789.00"/>
        </MarkerGroup>
      </MarkerBlock>
    </Layer>
    <Details>
      <String Id="Total Laps" Value="12"/>
      <String Id="Fastest Time" Value="2:03.541"/>
      <String Id="Fastest Lap" Value="7"/>
    </Details>
  </Layers>
</LDXFile>
```

Rules (pugixml, `parse_default | parse_declaration`, any encoding attribute honoured):

1. `Marker/@Time` is cumulative microseconds from log start written with two decimals → `MarkerInfo::tUs = llround(Time)`. `@ClassName` maps to `MarkerClass` for `SOL EOL BCN SPLTBCN IGRDBCN RESET STGSTRT STGEND`. Any other class (`SOR` and others seen in real files) is **opaque-only**: the whole `<Marker>` element is kept in `DataSource::companionOpaque` (rule 5), never becomes a `MarkerInfo`, and is reported once per class as `ldx.unknown_marker_class` (Info). There is no `MarkerClass::Other` (02 §4.3).
2. `@Flags` is stored verbatim in `MarkerInfo::flags`; its bits are undocumented (77 observed) and are never interpreted.
3. `MarkerGroup/@Name` → `group` (`"Beacons"` is the common group); `@Index` kept opaque. Markers are ordered by `Time`; `ordinal` is the marker's index among markers of the same `(class, group, Time)` in file order (0 for the first), exactly 02 §4.3 rule 5, so inserting a marker elsewhere never changes another marker's `MarkerId` (D-014).
4. `Details/String` → `Detail{id, value, prov = Companion}`; `Details` is accepted both as a child of `Layers` (observed) and of `Layer` (defensive).
5. Unknown elements and attributes anywhere are serialised back to strings and stored on the `DataSource` as `companionOpaque[]` (02 §4.2, §5.3 rule 3; never in the sidecar) with their XPath-like position so the writer (§5) re-emits them (D-015 round-trip); nothing is dropped and nothing unknown is interpreted.
6. The reader is called with a `ByteSource` (companion bytes come through the same abstraction, and through `host->read_companion` for plugins); a malformed `.ldx` logs `ldx.parse_error` and the `.ld` still opens.
7. Merge precedence sidecar > companion > file is implemented in `SidecarDocument::merge` (`esses_model`), not here; the reader only reports what the file says.

---

## 5. `.ld` + `.ldx` writer (export only)

The `.ld` part follows the user's converter's writer as specified in `docs/formats/ld-converter-port.md` and is new code written from that specification until the converter's licence and lineage are confirmed (D-142, D-144); the `.ldx` part (item 7) is new code, because the converter writes no `.ldx`. Exists from M0 as the synthetic fixture generator (D-033) and as `esses-cli export --format motec.ld`; the File → Export Data UI is v1.0 (D-015, D-035). Never written automatically.

1. **Layout emitted:** header 1,762 B, then the event, venue, vehicle and weather records at their fixed sizes (§3.3), contiguous, every pointer a u32; the enumeration block (pointer at 0x690) when a channel has an enumeration table and the log-event block (pointer at 0x688) for `DeviceEvent` markers; channel records of 124 B, `prev/next` linked, `counter = 20101 + i`, each followed by its gap block when the channel has invalid samples and by its aux block when the encoded name exceeds 32 bytes or the channel has an enumeration table; then sample arrays 64-byte aligned; `data_ptr` = first array; the u16 count at 0x056 (and its copy at 0x058 where the dialect has one) = records written; 0 at 0x5DE (D-143). Device type, serial and version are Esses' own neutral constants, kept beside `cmake/naming.cmake`. Writer option `ld.dialect ∈ {"pc-layout" (default), "m1", "c-series", "sim-exporter", "converter-legacy"}` sets the dialect-specific fields (unit placement, terminator record, u16@0x040, the 0x058 copy) for fixtures; `converter-legacy` is for fixtures only. The earlier option `ld.entrySize = 212` is withdrawn (§3.4).
2. **Dtype selection:** a channel that originated from `.ld` is re-quantised with its original `(dtype_a, dtype, scale, dec, mul, shift)` from `ChannelDescriptor::sourceEncoding` (02 §4.1; for `.ld` the encoding record of §3.4) so read → write → read is bit-exact (D-010). Otherwise: `Real` F32 → (0x07, 4) scale 1, dec 0, mul 1, shift 0; F64 → (0x08, 8); `Integer/Enum/Bitfield/Bool` → (0x05, 4) or (0x03, 2) when the range fits; `Text` channels are skipped with a note.
3. **Units:** values are converted from base to the channel's display unit (project override) and the unit symbol written in the i2 ASCII spelling from the registry alias table (12-char limit → truncated with a note). Scaling fields express `dec = min(dps, 6)` for integer encodings.
4. **Rates and resampling:** `.ld` is uniform-only. A `TimestampedSeries` is resampled by the writer with the channel's `Interp` to `ExportView::resampleRate` (default: the nearest integer Hz to the median interval, logged as `ld.resampled`). Uniform channels keep their rate when it is an integer Hz; a rational rate (e.g. 20.48 Hz from MoTeC CSV) is resampled to the nearest integer Hz with a note.
5. **Validity:** invalid samples are written as gap-block intervals for every encoding (§3.4, §3.11); float encodings also write NaN, and integer encodings hold the previous valid value inside a gap, as loggers do. `ld.validity_lost` is retired (amended 2026-10-10).
6. **Strings:** cp1252 when every character is representable, else UTF-8 with note `ld.string_encoding` (§3); NUL padding; truncation at field width on code-point boundaries with `ld.truncated`; a channel name longer than 32 encoded bytes also goes into the aux block as a long name.
7. **`.ldx`:** builds the document node by node with pugixml from the §4 structure facts (no string template, nothing taken from another project's emitter), with layer name `""` (D-015 alias), `Beacons` group with every marker of class `BCN` (user and companion markers after `MarkerEdit` application; ignored markers are written as `IGRDBCN`), `Time` as cumulative µs with two decimals, `Details` for `Total Laps`, `Fastest Time` (`m:ss.mmm`), `Fastest Lap` from the `Outings:Laps` group, plus every `companionOpaque` element at its original position.
8. **Round-trip acceptance (M0):** for every synthetic family (each dtype pair, multi-rate, shift/mul variants, aux-chained records with long names and enumeration references (the 212-byte case included), gap blocks, device events, truncated names, sim-exporter dialects) `write → read` reproduces `probe_json` and every sample and validity bit bit-exactly; `read(real) → write → read` reproduces the real file's samples and validity bit-exactly and its unknown header bytes verbatim, except 0x5DE to 0x5E1, which are always written as 0 (D-143).

---

## 6. MoTeC CSV (`motec.csv`): read and write

Dialect evidence (layout facts only; the vendor file is never committed, D-004, 07 R-06): `conv_samples/motec_sample_data.csv` (11 quoted key/value metadata lines, two blank lines, names row, units row, two blank lines, data; every field double-quoted; CRLF; single rate `Sample Rate` 20.480 Hz; `Time` in s; `Corr Dist` exported as a channel). The committed fixture `fixtures/motec.csv/dialect-basic/` is a self-authored file generated by our own writer (§6 Writer) that reproduces this structure.

**Reader** = the `TextEngine` with the shipped profile `data/profiles-text/motec.csv.json`. Shipped profiles may declare an explicit `importerId`; this one keeps the D-028 built-in id `motec.csv` rather than becoming `text.profile:motec.csv` (§7.2):

```json
{ "$schema": "urn:esses:importer-profile-1", "schemaVersion": 1, "id": "motec.csv", "importerId": "motec.csv", "name": "MoTeC CSV", "version": "1.0.0",
  "sniff": { "preambleRegex": "^\"Format\",\"MoTeC CSV File\"", "extensions": [".csv"], "confidence": 95 },
  "encoding": "auto", "delimiter": ",", "decimal": ".", "quote": "\"", "trim": true, "layout": "table",
  "preamble": { "untilBlank": true,
    "keyValueColumns": [[0, 1, 2], [4, 5, 6]],
    "capture": { "Venue": "Venue", "Vehicle": "Vehicle Id", "Driver": "Driver", "Device": "Device Type", "Comment": "Comment",
                 "Log Date": "Log Date", "Log Time": "Log Time", "Session": "Session", "Vehicle Desc": "Vehicle Desc", "Engine ID": "Engine Id",
                 "Workbook": "Export Workbook", "Worksheet": "Export Worksheet", "Range": "Export Range",
                 "Sample Rate": "@rate", "Duration": "@duration", "Origin Time": "@originTime", "Start Time": "@startTime", "End Time": "@endTime",
                 "Start Distance": "@startDist", "End Distance": "@endDist" } },
  "header": { "skipBlank": true, "namesRow": 0, "unitsRow": 1, "dataRow": 2, "unitInName": null },
  "time": { "column": "Time", "unit": "s", "rate": "fromDetail:@rate" },
  "sentinels": ["", "NaN", "-###", "+###"], "sparse": "hold", "duplicateNames": "suffix",
  "columns": { "Corr Dist": { "name": "Corr Dist", "quantity": "Distance", "dtype": "F64" } },
  "lapSource": { "kind": "lap_table", "detailsKey": "Beacon Markers", "optional": true, "fallback": { "kind": "lap_counter_edge", "column": "Lap", "optional": true } } }
```

Behaviours: `@`-prefixed captures are engine inputs, not details (`@rate` becomes `Rate` from the printed value: `20.480` → `{20480, 1000}` reduced to `{512, 25}`); the time column is validated against the rate (`|t_i − i/rate| ≤ 0.6/rate` after printed rounding) and a violation logs `text.time_rate_mismatch` and switches the source to timestamped. Vendor unit strings are i2 spellings and resolve through the registry aliases (D-010), so the Mapping panel normally does not appear. Lap information: the sample carries none; the `lap_table` source is a placeholder for the rows an i2 export may add when laps are included and must be **verified against the user's own i2 CSV exports** (D-005); until then a `Lap` column edge rule is the fallback and otherwise the file is one `whole-outing` range.

**Writer** (`motec.csv`, M0 for CLI/fixtures, MVP UI through Export Data): emits exactly the observed skeleton so i2 users can import it:

1. Eleven metadata lines in the observed key order with values from details (`Format` = the literal format name, `Venue`, `Vehicle`, `Driver`, `Device`, `Comment`, `Log Date`, `Log Time`, `Sample Rate` Hz 3 dp, `Duration` s, `Range` text; right-hand pairs `Workbook`, `Worksheet`, `Vehicle Desc`, `Engine ID`, `Session`, `Origin Time`, `Start Time`, `End Time`, `Start Distance`, `End Distance` with units).
2. Two blank lines, names row, units row (display units in i2 spelling), two blank lines, data.
3. Every field double-quoted, including numbers; CRLF; `.` decimal regardless of locale.
4. Single rate: all channels are resampled by the writer to `ExportView::resampleRate` (default: highest rate in the view, capped at 1 kHz) with each channel's `Interp`; `Time` starts at 0 at `ExportView::start`; `Corr Dist` is included when the view has it.
5. Invalid samples are written as empty quoted fields `""`.
6. The first line must contain the literal format name, which includes a vendor mark; D-003's CI allow-list must be extended to the writer's format-name constant (proposed decision, §15).

---

## 7. `TextEngine` and importer profiles (`importer-profile-1`)

### 7.1 Engine

Own light two-pass engine (D-029), ~1,500 lines, `fast_float` for numbers, no SIMD, no DCF.

**Pass 1 — index** (`open`, worker `Interactive`): (1) encoding: BOM → UTF-8/UTF-16LE/BE; else valid UTF-8; else cp1252 (transcoded line by line into a scratch buffer); (2) line index `std::vector<std::uint64_t>` via `memchr('\n')`, CR/LF/CRLF tolerated; (3) apply the profile's `preamble` → details and `@` inputs; (4) locate header rows; parse names, units (`unitsRow` or `unitInName` regex), build `ChannelInfo`s, apply `duplicateNames`; (5) for `layout: table` count data rows, parse the time column's first, last and every 1,000th value to infer rate and monotonicity; for `layout: blocks` index each section's line span and do the same per `ChannelBlock`; (6) classify each column `Real | Integer | Text` from the first 256 non-sentinel values unless `columns` says otherwise (Text columns become `Enum` with a table built in pass 2); (7) for `sparse: hold` profiles, build the **carry-in table**: for every slab boundary `k ≥ 1` and every hold column, the last valid value before the boundary, found by scanning slab `k − 1` backwards line by line from the boundary until every hold column has a value (columns still empty inherit slab `k − 1`'s own carry-in); cost is a few lines per boundary in practice and at most one extra parse of the file for a column that is empty for long stretches; (8) produce `ProbeResult`.

**Rate inference:** `step = (t_last − t_first) / (n − 1)`; candidate `Rate` = the rational with the smallest denominator ≤ 1,000 within 0.1 % of `1/step` (0.02 → {50,1}; 0.001 → {1000,1}; 0.0488 → {512,25}); accepted as `Uniform` when `max_i |t_i − (t_0 + i/rate)| ≤ 0.6/rate` over the sampled rows, else `Timestamped` (D-009; never resampled). A printed-rounding tolerance is why the MoTeC CSV at 20.48 Hz still reads as uniform.

**Pass 2 — decode** (`readMany`, worker `Visible`/`Background`): per 65,536-row slab, one sweep parses **every column of the source** (Text columns as enum ids) into `Block`s from the `BlockSink` (one per column per slab): quoted fields with doubled quotes, `trim`, `trailingDelimiter`, decimal comma via `fast_float`'s decimal-point option, sentinels → invalid bit + NaN, ragged rows → missing cells invalid (`text.ragged_row`, coalesced in the `DecodeLog`), Text columns → enum ids, unit conversion to base. Slabs run in parallel (`chunkIndependent() = true` for `table` layout). `sparse: timestamped` drops empty cells and emits `timesUs` from the time column; `sparse: hold` forward-fills, seeding each slab from the pass-1 carry-in table, so hold columns stay chunk-independent: any slab decodes alone, in any order, and re-decodes identically after eviction.

**One sweep per slab, shared by all columns.** All `ImporterChunkProvider`s of a text source share one `SlabDecoder`. Materialising `(channel c, slab k)` claims slab `k` for the whole source under the share-key `slab/<sourceId>/<k>` (01 §5.4 coalescing), decodes all columns in that one sweep, fills `c`'s `ChunkBuilder` and seals every sibling column's chunk into `ChunkCache` with `insertGroup` (02 §3.2; members already resident are skipped; the group is evicted as a unit). A slab is therefore parsed once whether one or a hundred columns of it are drawn, and the summary task of 02 §3.2 rule 10 consumes the same sweep (it folds every column's levels and discards the samples instead of inserting them). Without this, a 300 MB / 100-column CSV would be re-parsed once per column (≈ 30 GB of text for the summaries alone).

**Lap sources** are evaluated in pass 1 (cheap column scans) so `probe_json` carries markers: `lap_counter_edge` emits a `BCN` at every change of the column's value (and `SOL`/`EOL`); `beacon_event` emits `BCN` for matching event rows and a `DeviceEvent` marker (group `DeviceEvents`, `comment` = the row's message, category and source appended in brackets) for the rest; `lap_table` reads lap times from a details row or section; `none` leaves one outing.

### 7.2 Profile schema (`schemas/importer-profile-1.schema.json`)

| Key | Type / values | Default | Meaning |
|---|---|---|---|
| `$schema`, `schemaVersion` | `"urn:esses:importer-profile-1"`, int | required | 02 §6.6 rule 1, D-072 |
| `id`, `name`, `version` | string | required | profile id (also the file stem: `data/profiles-text/<id>.json`); `version` is the profile's content version and enters the cache key |
| `importerId` | string | `text.profile:<id>` | the importer id the profile registers under. Only shipped profiles may set it, and only to a D-028 built-in id (`motec.csv`); user profiles always get `text.profile:<id>`. The registry refuses a profile whose resulting id collides with another importer. The id's file stem (without `text.profile:`) names the unit map, dictionary and fixture directory: `generic.csv` → `channels/unitmaps/generic.csv.json`, `data/dictionaries/generic.csv.json`, `fixtures/generic.csv/` |
| `sniff.preambleRegex` | regex on the first 64 KiB, multiline | — | magic for text files |
| `sniff.extensions`, `sniff.confidence` | string[], 0..100 | `[]`, 50 | |
| `encoding` | `auto`, `utf-8`, `utf-16le`, `utf-16be`, `cp1252` | `auto` | |
| `delimiter` | `,` `;` `\t` `|` ` ` `auto` | `auto` | `auto` picks the delimiter whose count is constant over the first 50 data rows |
| `decimal` | `.` `,` `auto` | `auto` | `auto`: `,` only when delimiter ≠ `,` and ≥ 90 % numeric tokens contain one `,` |
| `quote`, `trim`, `trailingDelimiter` | char, bool, bool | `"`, true, auto | |
| `layout` | `table` \| `blocks` | `table` | `blocks` = sectioned files (Pi); **proposed extension to D-029** |
| `preamble` | `{lines: n}` \| `{untilRegex}` \| `{untilBlank: true}` plus `capture{detailId|@input: regex-with-group | keyName}`, `keyValueColumns[[k,v,u]…]`, `unmatchedTo` | `{lines: 0}` | captures go to details with `prov = Logged` |
| `header.skipBlank` | bool | true | rows counted among non-blank lines after the preamble |
| `header.namesRow`, `unitsRow`, `dataRow` | int, int\|null, int\|`auto` | 0, null, auto | `auto` = first row whose time column parses |
| `header.unitInName` | regex with one group | null | `Name (unit)` / `Name[unit]` |
| `header.ignoreEmptyNames` | bool | true | drops columns with empty header cells (EFI) |
| `time` | `{column, unit: s|ms|us|hhmmss|epoch_s|epoch_ms, rate?: "fromDetail:@x" | {num,den} | "infer", origin?: "first" | "zero"}` | infer | |
| `sentinels` | string[] | `["", "NaN", "-###", "+###"]` | matched after trim, case-sensitive |
| `sparse` | `timestamped` \| `hold` | `timestamped` | D-029 |
| `duplicateNames` | `suffix` \| `error` | `suffix` | ` (2)`, ` (3)` … |
| `columns` | map vendor name → `{name, quantity, unit, dtype, kind, interp, dps, sentinels}` | `{}` | per-column overrides applied before the dictionary |
| `lapSource` | `{kind: none | lap_counter_edge | beacon_event | lap_table, column?, section?, match?, detailsKey?, optional?, fallback?}` | `none` | |
| `blocks` | `{sectionRegex, metadata: {section, keyColumn, valueColumn, map}, events: {section, timeColumn, columns}, channels: {section, timeColumn, unitInName}}` | — | only with `layout: blocks` |

Profiles live in `data/profiles-text/<id>.json` (shipped: `motec.csv.json`, `generic.csv.json`) and `<config>/profiles-text/<id>.json` (user; `<config>` per 01 §9: `%APPDATA%\esses\`, `$XDG_CONFIG_HOME/esses/`, `~/Library/Application Support/esses/`); both are validated by `SchemaRegistry` and by `esses-cli validate`.

### 7.3 Example profiles

The layouts below were read as facts from the vendor's Conversion Wizard sample files, which are never committed (D-004). Each profile's committed fixture is a **self-authored** file under `fixtures/<id>/dialect-basic/` that reproduces the same structure (preamble shape, quoting, separators, sentinels, blocks) with our own values; the "Expected" notes give the structural expectations, and the vendor-sample figures are checked only locally as a private oracle when U-13 permits (tests marked `skipped (private)` in public CI).

**EFI ASCII** (layout facts: every line starts with an empty field; 8 free-text quoted lines; quoted names at odd columns with empty even columns; `.02` style numbers; 50 Hz; no units row):

```json
{ "$schema": "urn:esses:importer-profile-1", "schemaVersion": 1, "id": "efi-ascii", "name": "EFI ASCII dataset", "version": "1.0.0",
  "sniff": { "preambleRegex": "^,\"[^\"]*\"\\r?\\n,\"Driver : ", "extensions": [".asc"], "confidence": 85 },
  "encoding": "auto", "delimiter": ",", "decimal": ".", "quote": "\"", "trim": true, "layout": "table",
  "preamble": { "untilRegex": "^,\"Time\"",
    "capture": { "Source File": "^,\"([^\"]+\\.DAT)\"$", "Driver": "^,\"Driver : (.*)\"$", "Venue": "^,\"Track : (.*)\"$",
                 "Outing Number": "^,\"Outing : (\\d+)\"$", "Session Number": "^,\"Session : (\\d+)\"$",
                 "Total Laps": "^,\"Lap\\(s\\) : (\\d+)", "Fastest Time": "\\((\\d+:\\d+\\.\\d+)\\)\"$" },
    "unmatchedTo": "Comment" },
  "header": { "namesRow": 0, "unitsRow": null, "dataRow": 1, "ignoreEmptyNames": true },
  "time": { "column": "Time", "unit": "s", "rate": "infer" },
  "sentinels": ["", "NaN"], "sparse": "hold", "duplicateNames": "suffix",
  "columns": { "RPM": { "name": "Engine RPM", "quantity": "Angular Speed", "unit": "rpm" },
               "System Speed": { "quantity": "Speed" } },
  "lapSource": { "kind": "lap_counter_edge", "column": "Lap Count", "optional": true } }
```

Expected (self-authored fixture `fixtures/efi-ascii/dialect-basic/`): every quoted name becomes a channel at the inferred rate (`{50,1}` for a 0.02 s step), the empty even columns are dropped, the captures fill `Driver`, `Venue`, `Outing Number`, `Total Laps`, `Fastest Time` with the fixture's own values (leading zeros kept as text), `Comment` = the unmatched free-text lines; a column with no unit goes to the Mapping panel (or the shipped unit map `efi-ascii.json`, e.g. `nameOnly: "System Speed" → Speed, km/h`). Leading-dot numbers (`.02`) are a required parser test case.

**Pi Toolbox versioned ASCII** (layout facts: tab-separated; `{Section}` blocks separated by blank lines; one `{ChannelBlock}` per channel with its own absolute `Time` column; `Name[unit]`; `{EventBlock}` rows where `End of lap` are the beacons):

```json
{ "$schema": "urn:esses:importer-profile-1", "schemaVersion": 1, "id": "pi-ascii", "name": "Pi Toolbox versioned ASCII dataset", "version": "1.0.0",
  "sniff": { "preambleRegex": "^PiToolboxVersionedASCIIDataSet\\r?\\nVersion\\t2", "extensions": [".txt"], "confidence": 98 },
  "encoding": "auto", "delimiter": "\t", "decimal": ".", "quote": null, "trim": true, "layout": "blocks",
  "preamble": { "lines": 2, "capture": { "@formatVersion": "^Version\\t(\\d+)$" } },
  "blocks": { "sectionRegex": "^\\{(\\w+)\\}$",
    "metadata": { "section": "OutingInformation", "keyColumn": 0, "valueColumn": 1,
      "map": { "CarName": "Vehicle Id", "DriverName": "Driver", "TrackName": "Venue", "OutingNumber": "Outing Number", "SessionNumber": "Session Number",
               "FirstLapNumber": "Start Lap", "LongComment": "Long Comment", "ShortComment": "Short Comment", "EngineTest": "Engine Test" } },
    "events": { "section": "EventBlock", "timeColumn": "Time", "columns": { "name": "Name", "category": "Category", "source": "Source", "message": "Message" } },
    "channels": { "section": "ChannelBlock", "timeColumn": "Time", "unitInName": "\\[([^\\]]*)\\]$" } },
  "time": { "unit": "s", "origin": "first", "rate": "infer" },
  "sentinels": ["", "NaN"], "sparse": "timestamped", "duplicateNames": "suffix",
  "columns": { "Speed": { "quantity": "Speed" }, "Gear": { "kind": "Integer", "interp": "Previous" } },
  "lapSource": { "kind": "beacon_event", "section": "EventBlock", "match": { "Name": "^End of lap( beacon)?$" } } }
```

Expected (self-authored fixture `fixtures/pi-ascii/dialect-basic/`): layer origin = the earliest block time; a block with a regular 0.02 s step is uniform `{50,1}` (inferred), `Gear` Integer/Previous; one `BCN` marker per `End of lap` row; other event rows (e.g. `Burst Trigger`) become `DeviceEvent` markers in group `DeviceEvents`; `FirstLapNumber` fills the `Start Lap` detail; `kph` resolves through the registry alias. Blocks with irregular timing stay timestamped.

**RacePak DataLink CSV** (layout facts: comma-separated with space padding, trailing comma, no preamble, no units row, duplicate `Time`, sentinels `-###`/`+###`, text `none` in `LapMarkers`, 1 kHz):

```json
{ "$schema": "urn:esses:importer-profile-1", "schemaVersion": 1, "id": "racepak-csv", "name": "RacePak DataLink ASCII", "version": "1.0.0",
  "sniff": { "preambleRegex": "^Time\\s+,\\s*GPS_|^Time\\s+,[^\\r\\n]*,\\s*\\r?\\n[0-9.]+\\s+,", "extensions": [".csv", ".txt"], "confidence": 80 },
  "encoding": "auto", "delimiter": ",", "decimal": ".", "quote": null, "trim": true, "trailingDelimiter": true, "layout": "table",
  "preamble": { "lines": 0 },
  "header": { "namesRow": 0, "unitsRow": null, "dataRow": 1 },
  "time": { "column": "Time", "unit": "s", "rate": "infer" },
  "sentinels": ["", "-###", "+###", "NaN"], "sparse": "hold", "duplicateNames": "suffix",
  "columns": { "LapMarkers": { "kind": "Text" }, "GPS_MPH": { "name": "GPS Speed", "quantity": "Speed", "unit": "mph" },
               "GPS_LatG": { "quantity": "Acceleration", "unit": "g" }, "Lateral G": { "quantity": "Acceleration", "unit": "g" },
               "Accel G": { "quantity": "Acceleration", "unit": "g" }, "Battery Volt": { "quantity": "Voltage", "unit": "V" },
               "RPM Port": { "quantity": "Angular Speed", "unit": "rpm" }, "RPM Starboard": { "quantity": "Angular Speed", "unit": "rpm" } },
  "lapSource": { "kind": "none" } }
```

Expected (self-authored fixture `fixtures/racepak-csv/dialect-basic/`): one channel per header cell except the trailing empty one, at `{1000,1}`; second `Time` → `Time (2)`, `LapMarkers` an Enum channel (`none` → 0) that the user may promote to a `lap_counter_edge` source in the profile; `-###` cells invalid; units for unmapped columns come from the shipped unit map or the Mapping panel. i2's importer derives no laps for this format (help text); same here.

**Generic CSV** (profile id `generic.csv` → importer id `text.profile:generic.csv`, file `data/profiles-text/generic.csv.json`; the fallback; everything `auto`):

```json
{ "$schema": "urn:esses:importer-profile-1", "schemaVersion": 1, "id": "generic.csv", "name": "Delimited text (auto-detect)", "version": "1.0.0",
  "sniff": { "extensions": [".csv", ".txt", ".tsv", ".asc", ".log"], "confidence": 30 },
  "encoding": "auto", "delimiter": "auto", "decimal": "auto", "quote": "\"", "trim": true, "layout": "table",
  "preamble": { "untilRegex": "@auto" },
  "header": { "namesRow": 0, "unitsRow": "@auto", "dataRow": "auto", "unitInName": "[\\(\\[]([^\\)\\]]*)[\\)\\]]\\s*(\\*OBD)?$" },
  "time": { "column": "@auto", "unit": "@auto", "rate": "infer" },
  "sentinels": ["", "NaN", "nan", "#N/A", "N/A", "-###", "+###", "none", "null"], "sparse": "timestamped", "duplicateNames": "suffix",
  "columns": {}, "lapSource": { "kind": "lap_counter_edge", "column": "@auto:^(lap|lap number|lap count|lapno)$", "optional": true } }
```

`@auto` rules: preamble ends at the first line with ≥ 2 fields followed by a line ≥ 80 % numeric; a units row exists when the row after names has no numeric cell and every cell is ≤ 12 chars or empty; the time column is the first whose name matches `^(time|t|elapsed.*|secs?|timestamp)$` (case-insensitive), else the first monotone numeric column; the unit is `s` when the step < 10, `ms` when all values are integers with max > 10⁴, `hhmmss` by pattern, `epoch_s`/`epoch_ms` when the first value > 10⁹/10¹². Every `@auto` decision is logged (`text.auto_delimiter`, `text.auto_time_column`, …), and "save as profile" in the Mapping panel freezes the guesses into a user profile.

---

## 8. Mapping panel, unit-mapping profiles, dictionaries

### 8.1 Inline Mapping panel (`esses_widgets::MappingPanel`, D-028/D-029)

Replaces the modal Conversion Wizard page 2 with a non-modal panel docked above the Data window that appears only when pipeline step 4 leaves something unresolved. Table columns: logged name · vendor unit · inferred quantity · unit (combo from the registry, filtered by quantity) · analysis name (editable; writes a project Mapping) · interp · status. Actions keep i2's vocabulary: **Scan** (re-run step 4 on the selected files; lists unrecognised units), **Resolve** (for the selected rows choose match mode `Match Name Only` / `Match Unit Only` / `Match Name and Unit`, then quantity and unit), **Save** / **Save As** (writes `channels/unitmaps/<importer>.json`), **Load** (another unit map), **Open anyway** (unresolved rows open unitless with status text), **Cancel**. No worker waits for the panel: task A has already ended with a `PendingResolution` (§2 step 4); **Resolve** / **Open anyway** submit task B (`ImporterRegistry::resume`) with the `MappingDecision` by value, **Cancel** drops the pending open, and the GUI never blocks. Decisions are undoable (D-032) as project document edits. Headless: `esses-cli … --unitmap <file>` applies a map; `--strict-units` fails on leftovers; `esses-cli units scan <files>` prints the table as JSON.

### 8.2 `UnitMappingProfile` (`unitmap-1`, `channels/unitmaps/<importerStem>.json`)

This section owns `unitmap-1` (02 §6.3 uses the same keys). Keys: `match ∈ nameOnly | unitOnly | nameAndUnit`; `name` = the logged name; `vendorUnit` = the unit string as the file spells it; `quantity` and `unit` = the target quantity and unit symbol; optional `analysisName`. The file stem is the importer id without `text.profile:` (`efi-ascii.json`, `generic.csv.json`), because `:` is not allowed in Windows file names.

```json
{ "$schema": "urn:esses:unitmap-1", "schemaVersion": 1, "importer": "text.profile:efi-ascii",
  "rules": [ { "match": "nameOnly",    "name": "System Speed",                       "quantity": "Speed",    "unit": "km/h" },
             { "match": "unitOnly",    "vendorUnit": "KPH",                          "quantity": "Speed",    "unit": "km/h" },
             { "match": "nameAndUnit", "name": "Oil Pres", "vendorUnit": "PSI", "quantity": "Pressure", "unit": "psi", "analysisName": "Eng Oil Pres" } ] }
```

Application: rules are tried most specific first (`nameAndUnit` > `nameOnly` > `unitOnly`); names compare NFKC case-folded; a vendor unit that already resolves through `UnitRegistry::find` needs no rule; a profile is "complete" for a file when every channel has a quantity and unit afterwards, in which case the panel is skipped silently (D-028). `analysisName` writes a project Mapping (D-013) rather than renaming inside the importer, so the trace shows `'Oil Pres' → mapping → 'Eng Oil Pres' (psi)`.

### 8.3 Per-importer `dictionary.json` (`dictionary-1`, `data/dictionaries/<importerStem>.json`)

Generalises the pattern of i2's shipped id and bit-field tables (pattern only; no vendor table content is copied). This section owns `dictionary-1`; 02 §4.6 uses the same keys (`vendorId` as a string, `bits`, `device{type, minVersion, maxVersion?}`). `version` is the dictionary's content version (it enters `profileHash`); the schema version is `schemaVersion`.

```json
{ "$schema": "urn:esses:dictionary-1", "schemaVersion": 1, "importer": "motec.ld", "version": "0.1.0",
  "entries": [
    { "match": { "name": "Engine RPM" },         "name": "Engine RPM",   "shortName": "RPM",     "unit": "rpm",  "quantity": "Angular Speed", "kind": "Real",    "interp": "Linear" },
    { "match": { "nameRegex": "^GPS Lat" },      "name": "GPS Latitude", "unit": "deg",  "quantity": "Angle",         "dtype": "F64" },
    { "match": { "name": "Gear" },               "kind": "Integer", "interp": "Previous" },
    { "match": { "vendorId": "900" },            "name": "ECU Error Group 1", "kind": "Bitfield" } ],
  "bitfields": [
    { "match": { "name": "ECU Error Group 1" }, "device": { "type": "M800", "minVersion": 0 },
      "bits": [ { "mask": "0x0001", "name": "Error Throttle Pos" }, { "mask": "0x0002", "name": "Error Manifold Pres" } ] } ] }
```

Rules: entries match by `vendorId`, exact `name` or `nameRegex` (first match wins, in file order); matched fields overwrite `ChannelInfo` before `ChannelResolver` runs; `bitfields` produce derived `Bool` channels `name` = `(group & mask) ≠ 0` with `source = Mapped`, sharing the group's `Column` through a `BitProvider` (no copy); `device` filters by the `Device Type` detail and by `Device Version` within `[minVersion, maxVersion]` (`maxVersion` optional). Dictionaries are data PRs with one fixture; the shipped `motec.ld` dictionary starts from names observed in public sim exports and the user's own files.

---

## 9. Fixtures, CLI and fuzzing

### 9.1 Corpus layout (D-033)

This section is the single definition of the fixture directory and of `fixture-1`; 08 §6.7 refers here. `<formatId>` is the importer id's file stem (`motec.ld`, `motec.csv`, `generic.csv`, `efi-ascii`). Private fixtures use the same layout under `fixtures/private/<formatId>/<name>/` (submodule, D-005).

```
fixtures/<formatId>/<name>/
  input.<ext>                      or fixture.json input.remote {url, sha256, bytes} for files > 2 MB
  input.<companion ext>            companions sit beside the input with the same stem (input.ldx), because
                                   companion discovery is by sibling stem (§3.1, SniffInput.siblings)
  fixture.json                     schema fixture-1 (below)
  expected.probe.json              probe_json, canonical (02 §6.5 rule 1)
  expected.markers.json            schema markers-1: merged markers after .ldx, before sidecar
  expected.laps.json               schema laps-1: `esses-cli laps --json` (range-group tree: ids, kinds, classes, numbers,
                                   trust, stint/pitStop annotations, [start,end)) after .ldx and rules, no sidecar
  expected.channels/<channel>.csv.gz   header "index,timeUs,value,valid"; one file per channel (or a subset listed in fixture.json)
  README.md                        provenance, known quirks
```

```json
{ "$schema": "urn:esses:fixture-1", "schemaVersion": 1,
  "format": "motec.ld", "name": "acc-bathurst-r1",
  "input": { "file": "input.ld", "sha256": "…", "bytes": 10485760, "remote": null },
  "companions": [ { "file": "input.ldx", "sha256": "…" } ],
  "source": "ACC built-in export, Bathurst, 2026-10-01, own session", "licence": "LicenseRef-unknown",
  "permission": "private: third-party game output, nothing real is public for now (Q-04)", "private": true,
  "importOptions": { "ld": { "shiftOrder": "afterMul" } },
  "dialect": "sim-exporter",
  "oracle": { "kind": "public-project", "ref": "motec-i2 0.2.0 (MIT)", "permitted": true,
              "tolerance": { "F32": "1ulp", "F64": "1ulp", "I32": "exact", "timesUs": "exact" } },
  "channelsCompared": "all",
  "notes": "num_channs disagrees with the list by 2" }
```

`oracle.kind ∈ ref-decode | public-project | i2-export | converter | self` (`ref-decode` = `tools/ld_ref_decode.py`, written from the public layout notes independently of `esses_formats`; `self` is allowed only for round-trips of our own writer and never settles a dialect claim, D-041); `i2-export` and anything derived from vendor or user-private files require `private: true` (the fixture lives in `fixtures/private`, D-005) and `permitted` recording the U-03/U-13 answer. `dialect` is the string `probe_json` reports, so the fixture asserts it; dialect options the reader must be told go in `importOptions` (the `ImportOptions.options` object).

Synthetic families generated by the `.ld` writer at test time (inputs not committed) live under `fixtures/motec.ld/synthetic-*/` with only `fixture.json` and the expectations committed: one per dtype pair, multi-rate, `shift ≠ 0` × `mul ≠ 1` in both orders, aux-chained records (long names, enumeration references with severities; the 212-byte case included), gap blocks, a log-event block, an M1 terminator record, a header-only file, 32-char names, unit-in-short-name, `num_channs` off by one, zero-rate channel, truncated data array. The text dialects have committed, self-authored `dialect-basic` fixtures (`motec.csv` produced by our own MoTeC CSV writer; `efi-ascii`, `pi-ascii`, `racepak-csv` hand-written to the layout facts of §7.3) under CC0. The user's real logger files, workbooks, any i2 exports used as oracles and the vendor's Conversion Wizard samples (local oracle only, U-13) are in the private submodule and are never committed (D-004, D-005, D-033; Q-04 answered 2026-10-10: nothing real is public for now); tests that need them report `skipped (private)` in public CI.

### 9.2 CLI (`esses-cli`, Qt-free)

| Command | Behaviour |
|---|---|
| `info <file> [--json]` | sniff + probe; prints `probe_json` |
| `channels <file> [--json]` | channel table after dictionary + unit map (`--unitmap`) |
| `laps <file> [--json]` | markers + ranges after `.ldx`/sidecar merge and rules (`--json`: schema `laps-1`; `--no-sidecar` gives the `expected.laps.json` view) |
| `convert <in> --to <writerId> [-o out] [--rate] [--channels]` | reader → writer through `ExportView` |
| `export <file> --format csv\|motec.csv\|motec.ld` | alias of `convert` for scripts |
| `fixture add <file> --format <id> --oracle ref-decode\|<tool>\|<dir> [--remote <url>] [--channels a,b]` | writes the directory of §9.1; `--oracle` names where the expected channel values come from (`ref-decode`, a pinned i3rs/motec-i2 decode, or a directory of per-channel CSVs) and is recorded in `fixture.json`; expectations of synthetic families never come from the current reader (D-041) |
| `fixture verify [<dir>…] [--update]` | re-probes, re-decodes, compares with per-dtype tolerances; `--update` rewrites expectations after a reviewed change |
| `fixture diff <dir> [--channel x]` | explains mismatches: first differing sample, max ulp, missing markers, JSON pointer paths |
| `units scan <files…>` | the Mapping panel table as JSON |
| `validate <file>` | any schema'd JSON incl. profiles, dictionaries, unit maps, fixtures |
| `bench formats` | open/probe/decode timings against outline §8 |

`fixture verify` is the `core` CI job's main format test; a PR that changes a reader must either leave expectations byte-identical or update them with a `README.md` note.

### 9.3 Fuzzing (D-033)

libFuzzer targets in `tests/fuzz/`: `ld_header` (header + block chain), `ld_channels` (linked-list walk + decode of one chunk), `ldx` (pugixml + marker mapping), `text_sniff` (delimiter/encoding/preamble detection), `text_profile` (pass 1 + pass 2 of one slab under each shipped profile), `probe_json` (`fromJson` round-trip). Seeds: the fixture corpus heads (first 64 KiB) plus the synthetic families; dictionaries for the `.ld` magic and `.ldx` tags. Invariants: no crash, no UB (ASan/UBSan), per-input allocation cap 256 MiB, every loop bounded by file size, `readBlock` never reads outside the mapped span. Budget: 60 s per target per PR, 30 min nightly; new crashes become regression inputs under `tests/fuzz/regress/`.

---

## 10. Plugin ABI: `sdk/esses_abi.h` (designed now, frozen at v1.0)

### 10.1 Rules (D-028, ecosystem-first conventions)

1. C11 header, no C++ or Qt types; compiles with MSVC, Clang, MinGW; usable from Rust via bindgen. **One header**, `sdk/esses_abi.h`, holds the importer, writer *and* maths-function ABI (03 §10.2 uses the same file and the types below; there is no `esses_abi_math.h`). Every struct field is a fixed-width integer (`uint32_t`/`int32_t`/`uint64_t`/`int64_t`), `double`, a pointer or `esses_str`; C `enum`s define constants only and never appear as field or parameter types, because their size is implementation-defined (MinGW, bindgen).
2. **`struct_size`.** Every struct begins with `uint32_t struct_size`. Fields are append-only; nothing is reordered or removed. *In-structs* (written by the caller, read by the callee): the reader requires `struct_size >= ESSES_<T>_V1_0_SIZE` (offset of the last v1.0 field plus its size; the header defines the constant) and reads only fields inside `min(struct_size, sizeof(its own T))`. *Out-structs* (`channel_desc` out, blocks handed out by `sink->begin`): the **caller** zero-fills the struct and sets `struct_size = sizeof(its own T)`; the callee writes at most `min(out->struct_size, sizeof(its own T))` bytes and never writes `struct_size`. A plugin built against v1.1 therefore never overruns a v1.0 host's buffer.
3. Entry point `esses_plugin_entry_v1`; a breaking change adds `esses_plugin_entry_v2` and both coexist in one binary. `ESSES_ABI_MAJOR` changes only with a new entry symbol; `ESSES_ABI_MINOR` increments on appended fields. **Any minor is accepted in both directions:** the host checks a plugin struct's `struct_size` before using an appended member, and the plugin checks `host->struct_size` before calling an appended host service.
4. Strings are UTF-8 `esses_str {const char* ptr; size_t len;}`; not NUL-terminated by contract. **Descriptor strings** (everything reachable from `esses_plugin_desc_v1`: ids, names, versions, extensions, manifest JSON) are plugin-static, valid until process exit, and never freed by the host. **Out-parameter strings** (`probe_json`, `markers_json`, `warnings`, `reason`, `companions_json`, `err`) are allocated with `host->alloc` and freed by the host. Strings the host passes in (including `canonical_out` from `resolve_unit`) are valid only for the call. (Refines D-028's "all plugin-produced strings via `host->alloc`"; DECISIONS.md Addendum A.)
5. Sample buffers are **host-owned** (`esses_block_v1` points into a `Chunk` under construction); plugins fill, never allocate. `read_many` uses a sink so message-stream formats write all channels of a sweep directly into chunk memory (zero-copy). **Validity, stated once for importers and maths functions:** `valid_bits` is a bitmap, 1 bit per sample, bit `i % 8` of byte `i / 8` (LSB first), 1 = valid; `NULL` means all valid; invalid float samples also hold NaN (D-009). 03's `esses_math_array_v1` uses the same representation (no byte-per-sample mask, no conversion copy).
6. JSON (`esses_str`) only for metadata: `probe`, `markers`, `enum_table`, `warnings`, `options`, `companions`; validated by the host against `schemas/probe-1`, `markers-1`.
7. **Per-call context.** Cancellation and progress belong to a request, not to a plugin: every call that can take time (`probe`, `open`, `read_block`, `read_many`, `read_timestamps`, `markers`, `enum_table`, writer `write`, maths `evaluate`) receives a `const esses_call_v1*` whose `cancelled(ctx)` reflects that request's `CancelToken` and whose `progress(ctx, f)` feeds that request's `Progress`; a plugin polls `call->cancelled` at least every 65,536 samples. `esses_host_v1` keeps only process-wide services.
8. A plugin must support several concurrent sessions and, when it sets `ESSES_IMP_CHUNK_INDEPENDENT`, concurrent `read_block` calls on one session (each with its own `esses_call_v1`); otherwise the host serialises calls on a session. Maths functions: the host creates **one state per worker thread** with `create_state` and never calls `evaluate` concurrently on the same state, so an `ESSES_FN_BLOCK_CAPABLE` function on the parallel path runs on several states at once, never on one.
9. No exceptions, longjmp or thread-locals cross the boundary. Every function generated by `ExportedImporter` and every host callback is `noexcept` and wraps its body in `try { … } catch (...) { return ESSES_E_INTERNAL; }` (an exception unwinding through MinGW or Clang C frames into the MSVC host is undefined behaviour). Status codes mirror `ErrorCode` 1:1 by value (§10.2).
10. Host services: `log`, `resolve_unit` (registry lookup), `read_companion` (bytes via the host's `ByteSource`, so plugins never do their own I/O for companions), `alloc`/`free`.

### 10.2 Header excerpt

```c
#define ESSES_ABI_MAJOR 1
#define ESSES_ABI_MINOR 0
typedef struct esses_str { const char* ptr; size_t len; } esses_str;
typedef int64_t esses_time_us;

typedef int32_t esses_status;          /* values mirror esses::ErrorCode (01 §6.1) 1:1, in its order */
enum { ESSES_OK = 0, ESSES_E_IO = 1, ESSES_E_FORMAT = 2, ESSES_E_UNSUPPORTED = 3, ESSES_E_NOT_FOUND = 4,
       ESSES_E_INVALID = 5 /* bad argument; replaces 03's ESSES_E_ARG */, ESSES_E_CANCELLED = 6,
       ESSES_E_OUT_OF_BUDGET = 7, ESSES_E_OOM = 8, ESSES_E_CYCLE = 9, ESSES_E_PARSE = 10, ESSES_E_INTERNAL = 11 };
typedef uint32_t esses_dtype;          /* D-009 storage dtypes only; shared with 03 §10.2 */
enum { ESSES_DT_F32 = 1, ESSES_DT_F64 = 2, ESSES_DT_I32 = 3 };
enum { ESSES_UNIFORM = 1, ESSES_TIMESTAMPED = 2 };                                   /* esses_channel_desc_v1.series_kind */
enum { ESSES_INTERP_LINEAR = 1, ESSES_INTERP_CLOSEST = 2, ESSES_INTERP_PREVIOUS = 3, ESSES_INTERP_NEXT = 4 };
enum { ESSES_IMP_CHUNK_INDEPENDENT = 1, ESSES_IMP_NEEDS_WHOLE_FILE = 2, ESSES_IMP_PREFERS_READ_MANY = 4 };

typedef struct esses_host_v1 {               /* process-wide services; valid for the plugin's lifetime */
  uint32_t struct_size; void* ctx;
  void* (*alloc)(void* ctx, size_t bytes);  void (*free)(void* ctx, void* p);
  void  (*log)(void* ctx, int32_t level, esses_str code, esses_str msg);
  int32_t (*resolve_unit)(void* ctx, esses_str vendor_spelling, esses_str* canonical_out);
  esses_status (*read_companion)(void* ctx, esses_str path, const uint8_t** data, size_t* len);  /* valid until close */
} esses_host_v1;

typedef struct esses_call_v1 {               /* per request: one per probe/open/read/evaluate call */
  uint32_t struct_size; void* ctx;
  int32_t (*cancelled)(void* ctx);
  void    (*progress)(void* ctx, double fraction);
} esses_call_v1;

typedef struct esses_channel_desc_v1 {       /* out-struct: host zero-fills and sets struct_size */
  uint32_t struct_size; uint32_t index;
  esses_str logged_name, short_name, vendor_unit, vendor_id, vendor_dtype, layer;
  uint32_t dtype /* esses_dtype */; uint32_t sample_kind; uint32_t series_kind; uint32_t interp;
  uint32_t rate_num, rate_den; esses_time_us t0_us; uint64_t sample_count; int32_t dps; uint32_t flags; uint32_t enum_table;
} esses_channel_desc_v1;

typedef struct esses_block_v1 {              /* host-owned window inside a chunk */
  uint32_t struct_size; uint32_t layer, channel; uint64_t first; uint32_t count;
  void* values; uint8_t* valid_bits /* rule 5; NULL => all valid */; esses_time_us* times_us /* TIMESTAMPED only */;
} esses_block_v1;

typedef struct esses_sink_v1 {               /* zero-copy chunk sink for read_many */
  uint32_t struct_size; void* ctx;
  esses_status (*begin)(void* ctx, uint32_t layer, uint32_t channel, uint64_t first, uint32_t count, esses_block_v1** out);
  esses_status (*commit)(void* ctx, esses_block_v1* block);
} esses_sink_v1;

typedef struct esses_importer_v1 {           /* descriptor strings are plugin-static (rule 4) */
  uint32_t struct_size;
  esses_str format_id, display_name, version; const esses_str* extensions; uint32_t extension_count; uint32_t flags;
  uint32_t     (*sniff)(const esses_host_v1*, esses_str path, const uint8_t* head, size_t head_len, uint64_t file_size,
                        esses_str* reason, esses_str* companions_json);                         /* 0..100 */
  esses_status (*probe)(const esses_host_v1*, const esses_call_v1*, esses_str path, esses_str* probe_json);  /* session-less */
  esses_status (*open)(const esses_host_v1*, const esses_call_v1*, esses_str path, esses_str options_json, void** session);
  esses_status (*channel_count)(void* session, uint32_t layer, uint32_t* n);
  esses_status (*channel_desc)(void* session, uint32_t layer, uint32_t i, esses_channel_desc_v1* out);
  esses_status (*read_block)(void* session, const esses_call_v1*, esses_block_v1* out);         /* out.layer/channel/first/count set by host */
  esses_status (*read_many)(void* session, const esses_call_v1*, uint32_t layer, const uint32_t* channels, uint32_t n,
                            uint64_t first, uint32_t count, esses_sink_v1*);
  esses_status (*read_timestamps)(void* session, const esses_call_v1*, uint32_t layer, uint32_t channel, uint64_t first,
                                  uint32_t count, esses_time_us* out);
  esses_status (*markers)(void* session, const esses_call_v1*, uint32_t layer, esses_str* markers_json);
  esses_status (*enum_table)(void* session, const esses_call_v1*, uint32_t table, esses_str* json);
  esses_status (*warnings)(void* session, esses_str* json);
  void         (*close)(void* session);
} esses_importer_v1;

typedef struct esses_writer_v1 { uint32_t struct_size; esses_str format_id; const esses_str* extensions; uint32_t extension_count;
  esses_status (*write)(const esses_host_v1*, const esses_call_v1*, esses_str path, esses_str options_json,
                        const struct esses_export_view_v1* view); } esses_writer_v1;

typedef struct esses_plugin_desc_v1 {
  uint32_t struct_size; uint32_t abi_major, abi_minor;
  esses_str id, version, licence, homepage;
  uint32_t importer_count; const esses_importer_v1* const* importers;
  uint32_t writer_count;   const esses_writer_v1*   const* writers;
  uint32_t mathfn_count;   const struct esses_mathfn_v1* const* mathfns;    /* D-024; declared in this header, 03 §10.2 */
} esses_plugin_desc_v1;
ESSES_EXPORT const esses_plugin_desc_v1* esses_plugin_entry_v1(uint32_t host_abi_major, uint32_t host_abi_minor);
```

`esses_export_view_v1` exposes the host's `ExportView` as channel descriptors plus a pull callback (`read_block` in the opposite direction) so writers also stay zero-copy. The maths-function types of 03 §10.2 live in this header and reuse `esses_str`, `esses_status`, `esses_dtype`, `esses_call_v1` (cancellation) and the rule-5 validity bitmap; `esses_math_ctx_v1` carries a `const esses_host_v1*` for `alloc`/`free`/`log` instead of declaring its own copies.

### 10.3 `plugin.json` manifest

```json
{ "$schema": "urn:esses:plugin-1", "schemaVersion": 1, "id": "mitch.converter", "version": "0.1.0", "abi": "1.0", "licence": "MIT", "homepage": "…",
  "binary": { "win-x64": "esses_fmt_converter.dll", "linux-x64": "libesses_fmt_converter.so", "mac-arm64": "libesses_fmt_converter.dylib" },
  "formats": [ { "id": "aim.xrk", "name": "AiM XRK/XRZ", "extensions": [".xrk", ".xrz"], "magic": "…", "fixtures": "fixtures/aim.xrk" } ],
  "writers": [], "mathFunctions": [], "trust": "native",
  "qt": { "mode": "dynamic", "minVersion": "6.5" } }
```

`qt.mode ∈ none | static | infix | dynamic`: `none` (no Qt); `static` (Qt linked statically into the plugin); `infix` (a private Qt built with `-qtlibinfix` and `-qtnamespace`, e.g. `Qt6Core_espl.dll`, with `qt.infix`); `dynamic` (the plugin links the ordinary `Qt6Core` and therefore runs on the **host's** Qt in the GUI process: accepted only when `qt.minVersion` ≤ the host's Qt version, Qt 6 being backward binary compatible within the major; refused otherwise with `plugin.qt_mismatch`). In `esses-cli`, which loads no Qt, a `dynamic` plugin's own Qt DLLs load from its folder.

Read before any code is loaded: the Plugin Manager, the catalogue and the docs generator need only the manifest.

### 10.4 Loading and isolation rules

1. Directories: the Plugins row of 01 §9: `<exe dir>\plugins\`, `<config>/plugins/` (`%APPDATA%\esses\plugins\`, `$XDG_CONFIG_HOME/esses/plugins/`, `~/Library/Application Support/esses/plugins/`), `<prefix>/lib/esses/plugins/` (Linux), `Esses.app/Contents/PlugIns/esses/` (macOS), `--plugin-dir`. Each plugin is a folder with `plugin.json` and the binaries.
2. Load order: validate `plugin.json` against `plugin-1`; refuse `abi` major ≠ host major and a `qt` block the host cannot honour (§10.3); load with `LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)` so the plugin's own dependencies resolve from its folder (never `SetDllDirectory`, which is process-global); on Linux `dlopen(RTLD_NOW | RTLD_LOCAL)` (plus `RTLD_DEEPBIND` on glibc), on macOS `dlopen(RTLD_LOCAL)` with `@loader_path` install names. **This does not isolate a same-named DLL**: when a module named `Qt6Core.dll` is already loaded, Windows binds the plugin's import to it whatever the directory, and ELF default symbol lookup interposes the same way; hence the §10.3 `qt` rule. Then call `esses_plugin_entry_v1(host_major, host_minor)`; a NULL return logs `plugin.rejected` and unloads (nothing references the library yet). Any minor is accepted (§10.1 rule 3).
3. Native plugins are trusted code; the Plugin Manager says so and shows id, version, licence, formats; `trust: native` vs `signed` (first-party builds signed with the release certificate when the budget exists, D-034).
4. Isolation is by contract, not by process in v1.0: the host wraps every call in a watchdog that logs `plugin.slow` after 10 s without a progress callback and cancels through `esses_call_v1` on user request. After a status `ESSES_E_INTERNAL` the plugin is **disabled, never unloaded**: new sessions are refused, open sessions are closed when their sources close, and the library stays mapped until process exit, because published `DataSource`s (`ImporterChunkProvider`s), `FunctionRegistry` entries and in-flight calls on other workers still point into it (`FreeLibrary` at runtime would be a use-after-free). Plugin calls may additionally be guarded with SEH/signal handlers, but only to log and terminate cleanly: state after an access violation is undefined. An out-of-process host is a post-v1.1 option behind the same `IImporter` adapter.
5. The host never requires a plugin to link Qt; a harvested parser may link QtCore under the §10.3 `qt` modes (D-028) and must not create, assume or touch a `QCoreApplication` (none exists in `esses-cli`; the GUI's belongs to the host).
6. `plugins.disabled` in `SettingsStore` and `--safe-mode` skip loading; a plugin that crashed at load is auto-disabled with a Messages entry.
7. Adapters: `PluginImporterAdapter : IImporter` wraps a vtable; `ExportedImporter` generates a vtable from any `IImporter` (how `motec.csv` moves out of tree at v1.0, D-028). Both come from one table so the directions cannot drift. Every generated entry point and host callback is `noexcept` with a catch-all that returns `ESSES_E_INTERNAL` (§10.1 rule 9).
8. `abi-compat` CI (from v1.0): build `plugins/examples/c` with Clang and MinGW (Rust if cheap), load into the MSVC host, run `fixture verify` through the plugin path, diff `sdk/esses_abi.h` against the frozen copy for non-append changes.

---

## 11. Harvesting the user's converter (after v1.0, outline M6)

Preconditions (DECISIONS open question Q-15): licence MIT-compatible; each parser the user's own work with no GPL or vendor-SDK lineage (BLF parsers derived from vector_blf, AiM DLL wrappers and anything built from NDA documents cannot be ported; they may stay plugins if their own licence allows).

**Exception: the `.ld` reader and writer (D-142, D-144).** At the user's instruction of 2026-10-10 the converter's `.ld` knowledge enters `esses_formats` in M0 rather than through the harvest: `docs/formats/ld-converter-port.md` specifies `src/parsers/LdParser.cpp` and `src/formats/LdWriter.cpp`, and the `motec.ld` reader (§3, largely the inverse of the writer) and the `.ld` part of the fixture writer (§5) are new code written from that specification. Verbatim reuse of the converter's code waits for the user's confirmation that it is their own MIT work with no GPL lineage (Q-15, D-144). The new code follows step 2's Qt-free rules and the `qt-free-check`, but its oracle is `tools/ld_ref_decode.py` or a pinned public decoder (D-041), never the converter's own output, because a writer and a reader from one source can share an error. The converter has no `.ldx` code; the `.ldx` parser (§4) and writer (§5 item 7) are new.

**Step 1 — wrap as a plugin (days per format).** Build `plugins/formats/converter/` from the converter's tree: one `esses_importer_v1` per parser, `probe` serialises the parser's metadata to `probe_json`, `read_many` copies the parser's in-memory result into sink blocks, `markers` serialises laps/beacons. QtCore is linked under the §10.3 `qt` rule: `static` or `infix` when the converter's Qt differs from the host's, `dynamic` with `minVersion` only when it is not newer than the host's. The converter's existing CSV output for its sample files becomes the oracle: `esses-cli fixture add --oracle` imports them as `expected.channels/*.csv.gz`. Nothing in the core changes.

**Step 2 — port into `esses_formats` (highest value first).** Replace `QByteArray` → `std::span<const std::byte>`, `QDataStream` → `ByteReader`, `QString` → UTF-8 `std::string`, `QFile::map` → `MappedFile`; make `open` O(header) and reads chunk-granular. Acceptance: hash-pinned fixture, `probe_json` byte-identical to the plugin's, oracle diff (bit-exact integers, ≤ 1 ulp floats) against the step-1 plugin; `qt-free-check` blocks any Qt include.

| Converter parser | Format priority (§12) | Harvest path | Oracle |
|---|---|---|---|
| XRK/XRZ | P1 | port first (message-stream → `readMany`, timestamped series) | libxrk / xrk-js |
| MDF4 (+ DBC decode) | P1 (MDF4), P2 (DBC) | port; or replace by mdflib (MIT) if the user's parser is thinner | asammdf |
| IBT | P1 | port (uniform 60 Hz rows, YAML session info → details) | pyirsdk |
| PDS | P2 | port (SI base units inside match D-010 directly) | motorsport-telemetry-rs |
| BLF | P2 | wrap only until provenance is clear; port along ablf's approach if own work | python-can |
| RPK | P3 | wrap only (proprietary, key-locked) | converter itself |
| Emtron | P3 | wrap only | converter itself |
| Haltech, Link, Syvecs, MaxxECU CSV/tab exports | P1/P2 | **not ported**: each becomes a text profile + fixture (§7); the converter's parser output is the oracle | converter itself |

---

## 12. Format roadmap

Priorities from `background/research/formats.md`; the MVP cut follows D-029/D-035 (DECISIONS wins over the research P0 list — see §15).

| Phase | Readers | Mechanism |
|---|---|---|
| **P0 — MVP 0.1 (M0)** | MoTeC `.ld` + `.ldx` (ACC/sim dialects, M1 and C-series logger files, PC-layout files), MoTeC CSV, generic CSV/TSV (auto-detect) | `motec.ld` reader, `TextEngine` + two profiles |
| **P0 research list → v1.0 (M5/M6)** | Pi Toolbox ASCII, EFI ASCII, RacePak ASCII, Racelogic `.vbo` | profiles (`blocks` layout for Pi); VBO needs a small `[section]` layout profile and GPS normalisation |
| **P1 (M6)** | AiM `.xrk/.xrz` (libxrk spec port), AiM CSV, iRacing `.ibt`, RaceCapture `.LOG` (sparse → timestamped), RaceChrono CSV v3 (+VBO), TrackAddict CSV, McLaren ATLAS ASCII, MegaSquirt/TunerStudio `.msl`, MaxxECU tab log, Haltech/Link/ECUMaster CSV, GPX, ASAM MDF4 (mdflib or port), rF2/AMS2/Mu `.ld` dialect hardening | profiles where text; C++ readers or plugins where binary |
| **P2** | GoPro GPMF, Garmin FIT (own decoder), NMEA, Pi `.pds`, Bosch WinDarab ASCII, ECUMaster native, EFI Analytics `.mlg`, Harry's LapTimer CSV, RaceBox CSV, Syvecs/FuelTech/Hondata/Holley/HP Tuners/COBB CSV, DBC decoding, Vector ASC/BLF, PEAK TRC, candump, Parquet/Arrow, MATLAB `.mat`, GT7 via sim-to-motec | plugins / profiles |
| **P3** | AiM `.drk`, AiM MP4 `aimd`, RacePak `.rpk`, WinDarab `.bmsbin`, ATLAS `.ssn`, Link `.llgx`, Emtron `.elf`, AEM `.daq/.itlog`, Holley `.dl/.dlz`, Hondata `.fpdl/.kdl`, Race Technology `.RUN`, VBOX `.vbb`, Apex Pro, Unipro/Alfano/misc, Excel, KML/TCX, live UDP sims | wrap-only plugins; profiles as samples arrive |

Writer roadmap:

| Writer | Phase | Notes |
|---|---|---|
| Generic CSV (`csv`) | M0 CLI, MVP UI (Export Data on every component, D-027) | quoting, display units, selectable rate |
| MoTeC CSV (`motec.csv`) | M0 CLI, MVP UI | §6; interchange with i2 users |
| `.ld` + `.ldx` (`motec.ld`) | M0 for fixtures and CLI; export UI v1.0 (D-015, D-035) | §5 |
| MATLAB `.mat` (`mat`) | v1.0 (D-027) | matio (BSD-2); v5 by default |
| Parquet (`parquet`) | post-v1.0, optional plugin | Arrow C++ is heavy; never a core dependency |

---

## 13. `CLEANROOM.md` provenance lines added by this document

- `motec.ld`: reader and the `.ld` part of the writer written as new code from the port specification of the user's own converter (`docs/formats/ld-converter-port.md`; category user-contributed own work, D-145), with no `ported-from` row until the user confirms the converter's licence and lineage (D-142, D-144); layout facts confirmed on the user's own files by spike S-1 (category user-owned device logs); checked against public documentation (not a two-team clean room). Primary public source Friss/i3rs `docs/ld-file-format.md` (MIT); corroborated by afonso360/motec-i2 (MIT) and tobi/motorsport-telemetry-rs (MIT); gotzl/ldparser (GPL-3): *consulted, superseded*, no code or struct strings reproduced; sim-exporter dialects from the outputs of GeekyDeaks/sim-to-motec and stevendaniluk/MotecLogGenerator; shift-vs-mul order: *`afterMul`, settled 2026-10-10 by physical evidence on user-owned logger files (S-1, D-037), no internals inspected*; pro-logging field: *excluded, never computed or written* (D-143).
- `.ldx`: structure facts from the user's own logger and ACC `.ldx` files and the MIT projects i3rs (reader), motorsport-telemetry-rs and motec-i2 (writers); GeekyDeaks/sim-to-motec `ldx.py` (no licence) and the SARUSySLab reader notes (licence not stated): *consulted, unlicensed, no text or template copied*; the writer builds XML node by node with pugixml; `Time` = cumulative µs; unknown classes preserved opaquely.
- MoTeC CSV: layout facts from the vendor's sample file (read locally, never committed) and public forum statements that every field is quoted; the committed fixture is self-authored by our writer.
- EFI/Pi/RacePak: layout facts from the three vendor Conversion Wizard samples (read locally, never committed; committed fixtures are self-authored); lap derivation rules (EFI lap-count edge, Pi `End of lap` events, RacePak none) from the public help pages for the three importers, restated, not quoted.
- Mapping panel vocabulary (Scan / Resolve / Save; Match Name Only / Match Unit Only / Match Name and Unit): Conversion Wizard page 2 help; behaviour reimplemented.
- Dictionaries: the *pattern* of i2's shipped id and bit-field tables; no table content copied.

---

## 14. Acceptance criteria (consolidated; M0 unless marked; D-044 moved the text engine, MoTeC CSV, Mapping panel and dictionaries to M2)

1. `esses-cli info --json` equals `expected.probe.json` for ≥ 20 fixtures (synthetic families, including `synthetic-acc-dialect`); byte-identical after canonicalisation. Real ACC files and third-party sim exports count toward the 20 only where their licence permits redistribution; otherwise (ACC exports for now, like every real file under Q-04; sim-to-motec output always) they are private fixtures (D-040).
2. Per-channel decode equals the oracle: integers bit-exact, floats ≤ 1 ulp, timestamps exact; `fixture verify` green on three OSes with no Qt.
3. `.ld` write → read round-trips every synthetic family bit-exactly; real-file read → write → read preserves samples, validity and unknown header bytes (0x5DE to 0x5E1 excepted: always written as 0, D-143).
4. `probe` ≤ 200 ms on the 500 MB synthetic `.ld`; `.ld` open is O(header): before first paint a counting `ByteSource` sees reads only of the header, the channel-header list and the two 1 MiB quick-hash windows (no chunk decode, no full-content hash); the full hash and the summary pass start only after publish and never insert chunks into `ChunkCache`.
5. The `.ld` reader never trusts `num_channs`, survives every fuzz target 60 s clean, and reports each heuristic with a stable Import Log code.
6. Shift-vs-mul order recorded in `CLEANROOM.md` from a real file, with the default confirmed or flipped and a fixture pinning it (spike S-1, 2026-10-10: `afterMul`, default flipped by D-037; the pinning fixture is still due).
7. `.ldx` with unknown classes/elements round-trips through read → `.ldx` write byte-equivalent for the unknown parts; unknown marker classes never appear in the marker list; marker times are exact microseconds; `MarkerId`s are unchanged when a marker is inserted elsewhere in the group.
8. (M2) MoTeC CSV (self-authored fixture `fixtures/motec.csv/dialect-basic/`, generated by our writer with the observed preamble structure): channels at `{512,25}` with the fixture's row count, `@rate`/details captured, `indexAt(timeAt(i)) == i` on every row; writer output re-reads to the same `probe_json` (modulo export details). Our writer's output is verified by re-import and by a pinned i3rs/motec-i2 decode, never by running i2 on it (D-038). Private, local only (U-13): the same expectations on the vendor sample, reported `skipped (private)` in public CI.
9. EFI, Pi and RacePak profiles (v1.0) produce the §7.3 expectations on their self-authored `dialect-basic` fixtures; `generic.csv` opens all three with only Import Log notes (no errors) and lower confidence than the specific profiles. The vendor samples are private local oracles only (U-13), `skipped (private)` in public CI.
10. (M2) `TextEngine` pass 2 decodes 300 MB / 100 columns to first paint within the outline §8 budget (≤ 10 s MVP) on the reference machine (D-042), parsing each slab once for all columns (asserted by a slab-parse counter: one parse per slab after drawing ten columns and running the summary task); `sparse: hold` matches a reference forward-fill across slab boundaries when slabs are decoded in random order and after eviction.
10a. Concurrency: TSan clean while workers decode chunks that add `DecodeLog` entries and the drain reads `snapshot()`; `expected.probe.json` is byte-identical across 100 runs with 1 and N workers.
11. (M2) Mapping panel appears only when step 4 leaves unresolved units/names (task A ends with a `PendingResolution`, task B runs on the decision); a saved unit map makes the next open silent; `--strict-units` fails headless with a listing.
12. (M2) Dictionary bitfield expansion yields Bool channels sharing the group column (no extra chunk allocations).
13. `sdk/esses_abi.h` compiles as C11 under MSVC, Clang, MinGW; no enum type appears in a struct or signature; `ESSES_<T>_V1_0_SIZE` constants match `offsetof` on all three; at v1.0 `motec.csv` runs out of tree through the adapters with identical fixtures, the Clang/MinGW example plugin loads in the MSVC host (`abi-compat` green), a plugin compiled against a newer minor with an appended out-struct field writes nothing past the host's `struct_size`, cancelling one of two concurrent `read_block` requests on one session cancels only that one, and an exception thrown inside an `IImporter` behind `ExportedImporter` surfaces as `ESSES_E_INTERNAL` and disables, not unloads, the plugin.
14. All fuzz targets run per PR and nightly within D-033 budgets; the regress directory replays clean.

---

## 15. Unresolved items and proposed decisions (for `DECISIONS.md`)

1. **Pi ASCII needs a sectioned layout.** D-029's profile schema has no block/section concept; this document adds `layout: "table" | "blocks"` and a `blocks` object (§7.2). Proposed: accept as an amendment to D-029.
2. **MoTeC CSV writer emits a vendor mark.** The first line must contain the literal format name for i2 interchange; D-003's CI allow-list covers importer ids and dialog labels only. Proposed: extend the allow-list to the writer's format-name constant and the `data/profiles-text/motec.csv.json` sniff regex.
3. **Research P0 vs MVP cut.** `background/research/formats.md` lists Pi/EFI/RacePak/VBO as P0; D-029/D-035 ship only `motec.csv` + `generic.csv` in 0.1. DECISIONS wins; recorded as a research-vs-decision discrepancy (not outline-vs-decision).
4. **Shift-vs-mul default.** Resolved 2026-10-10: spike S-1 settled `afterMul` on real logger data and D-037 flipped the default (§3.6); `beforeMul` stays selectable.
5. **`.ld` lap-counter channels.** Whether an I32 lap counter / 5 Hz beacon channel in M1 logs should feed `BeaconPairRule` when no `.ldx` exists (via a `lapSource` option on `motec.ld`) is left to the lap-rules document; the reader exposes the channel unchanged.
6. **Device events are markers.** Device events (Pi `Burst Trigger`, later ibt/MDF events) are `DeviceEvent` markers in group `DeviceEvents` (02 §4.3, 04 §8.10), reported in `probe_json` `markers[]` with a `comment`; the earlier `LayerInfo::events`/`EventInfo` proposal is withdrawn. Proposed for D-014 in DECISIONS.md Addendum A (`MarkerClass::DeviceEvent` beyond D-014's eight classes; unknown `.ldx` classes stay opaque, no `Other`).
7. **Session-less `probe` in the C ABI** mirrors D-028's `probe(ByteSource&)` rather than ecosystem-first's `probe(session)`. Proposed: confirm.
8. **Pointer widths and the M1 entry** (§3.10): resolved 2026-10-10 by spike S-1 and the port specification: u32 pointers, and no 212-byte M1 entry (a record plus its aux block, §3.4).
9. **Converter provenance** (DECISIONS open question Q-15) gates §11; the harvest table assumes own-work parsers.
10. **Qt inside plugins.** A same-named Qt DLL cannot be isolated by search path on Windows (or by default ELF lookup), so `privateQt` is replaced by the §10.3 `qt` modes (`none | static | infix | dynamic` with `minVersion`); a wrapped parser that needs a `QCoreApplication` cannot run headless and must avoid it. Proposed for D-028 in DECISIONS.md Addendum A.
11. **ABI refinements** (DECISIONS.md Addendum A, D-028): per-call `esses_call_v1` for cancellation/progress; out-struct `struct_size` ownership and `ESSES_<T>_V1_0_SIZE`; any minor accepted; fixed-width fields only; descriptor strings plugin-static (only out-parameter strings via `host->alloc`); one maths state per worker thread; one header; `esses_status` = `ErrorCode` by value; bitmap validity for maths arrays; plugins disabled, never unloaded, after `ESSES_E_INTERNAL`.
12. **Source identity** (D-011/D-015, Addendum A): `SourceId` = quick content hash (size, first/last MiB, format header; no mtime so copies keep their id); full content hash at Background; open coalescing key `open/<canonical path>/<size>/<mtime>`.
13. **Diagnostics split** (01 §8.2): `probe_json.warnings[]` holds sniff/probe/open entries only; decode-time entries go to the per-source `DecodeLog`.
14. **Text profile naming** (D-028/D-029): shipped profiles may declare `importerId` (only `motec.csv` does); generic profile id `generic.csv` → `text.profile:generic.csv`; file stems drop `text.profile:` (`unitmaps/generic.csv.json`).
15. **Fixtures** (D-033, Addendum A): `fixture-1` is defined once in §9.1 (08 §6.7 refers to it); companions sit beside `input.<ext>` with the same stem; `expected.laps.json` (new schema `laps-1`, the `esses-cli laps --json` output) joins the expectation set; vendor sample files are private local oracles only and every committed dialect fixture is self-authored.
16. **One `$schema` form and one canonical JSON** (02 §6.5–6.6, D-072): every example here uses `urn:esses:<name>-<major>` + `schemaVersion`; `probe_json` uses the shared canonical algorithm.
