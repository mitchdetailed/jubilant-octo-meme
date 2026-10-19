# esses_formats

Layer 2, Qt-free. Importers, readers and writers (05_IMPORTERS_AND_PLUGINS.md). Links `esses_base`,
`esses_units`, `esses_store` and `esses_model`; pugixml and nlohmann-json are private dependencies.

## Public API (namespace `esses::formats`, umbrella `formats.hpp`)

| Header | Contents |
|---|---|
| `import_log.hpp` | `ImportLogEntry`, `ImportLog`, `DecodeLog` exactly as 05 section 1.1 declares them (coalescing after the third identical `(code, layer, channel)`, canonical snapshot order) |
| `byte_source.hpp` | `ByteSource` (05 section 1.1) with the D-130 identity: `quickHash(formatHeader)` = xxh3-128 over size, first and last MiB and the format header; `contentHash`; `siblings()`; `openByteSource` (mmap), `makeMemorySource`, `listSiblings`, `findCompanion`, `quickHashOf` |
| `probe.hpp` | `ChannelInfo`, `MarkerInfo`, `LayerInfo`, `EnumTableInfo`, `Timestamped`, `channel_flags`, `ProbeResult` with `toJson()`, `canonicalJson()`, `fromJson()`, `fromJsonText()` (probe-1, canonical through esses_model's writer) |
| `importer.hpp` | `SniffInput`, `SniffResult`, `ImportOptions`, `Block`, `BlockSink`, `Progress`, `ChannelConversion`, `IReader`, `IImporter`, `ExportView`, `WriterOptions`, `WriteReport`, `IWriter`, `kSniffHeadBytes` |
| `registry.hpp` | `ImporterRegistry` (`withBuiltins`, `add`, `sniff`, `find`, `writer`, `catalogue`, `probe`, `open`, `load`, `decodeLog`, `release`), `OpenContext`, `OpenedSource`, the pipeline codes `open_code::*` |
| `ld_importer.hpp` | `kLdImporterId` (`motec.ld`), `kLdImporterVersion`, `LdEncoding`, `LdSourceEncoding` (the .ld `model::SourceEncoding`), `makeLdImporter`, `makeLdWriter` |
| `ld/ld_layout.hpp` | `.ld` offsets, record sizes, type codes and block markers |

## Open pipeline (05 section 2, M0 scope)

`ImporterRegistry::open(path, ImportOptions, OpenContext, CancelToken)` on the calling thread, or `load()` as
one TaskPool task keyed `open/<canonical path>/<size>/<mtime>/a`:

1. sniff (64 KiB head, size, sibling list; best confidence, ties in registration order) unless
   `ImportOptions::importerId` names the importer;
2. `IImporter::open` (the reader parses the header, the channel list and the `.ldx` companion; no sample);
   `SourceId = ByteSource::quickHash(IReader::formatHeader())` (.ld: the 1,762 header bytes and every record);
3. one `model::DataLayer` per `LayerInfo`, one `model::Channel` per `ChannelInfo`: `ChannelId` from
   `channelIdLogged`, descriptor with `dps`, `interp` (`IReader::interp`), enumeration, rate and
   `sourceEncoding`; a `UniformSeries` over a `Column` fed by `IReader::makeChunkProvider` (motec.ld: the .ld
   chunk provider, gap blocks as validity) or the generic `ImporterChunkProvider` over `readBlock`; the log's
   markers (device events), SOL/EOL synthesised with prov Derived when neither file has them; enum tables;
4. units (with `OpenContext::units`): the vendor unit through `UnitRegistry::match`; the channel gets the
   quantity's base unit and samples are converted at decode (`IReader::setConversion`, applied in F64 before
   the one narrowing; integer channels become F32, or F64 from 32-bit storage). Unresolved: unitless with the
   status text `Unit unknown: "<u>"` and one `open.unit_unknown` per spelling (`strictUnits`: NotFound);
5. `model::mergeCompanion` with `IReader::companion()` (precedence companion > file), `deriveFileDetails`,
   detail units, `SidecarDocument::load(pathFor(log))` + `applySidecar` (sidecar > companion > file, D-015);
6. `model::rebuildWholeOutingRanges` (the only range rule of M0).

`OpenedSource` holds the `shared_ptr<const DataSource>`, the reader's `ProbeResult` (also in `prov.probe`),
the Import Log (reader entries, then steps 4-6, canonical order) and the `DecodeLog`.

## motec.ld

- Sniff (05 section 3.1): 95 for magic and plausible pointers (header-only included), 60 for magic with a
  failed structure check and the `.ld` extension, 50 without it, 25 for the extension alone; the `.ldx`
  sibling (case-insensitive on Windows) is the companion.
- Probe: header and detail-record details (numbers in base units `m`, `kg`, `m3`, `ratio`; weather values
  with the unit text as written), the layer's UTC anchor (`assumedLocal`), channels, device events (labels
  from `model::deviceEventName`), enumeration tables, dialect flags, the reader's Import Log; plus the `.ldx`
  companion's markers and details as found (05 section 4 rule 7; companion details keep prov Companion in
  memory, which probe-1 does not serialise).
- Options: `{"ld": {"shiftOrder": "afterMul" | "beforeMul", "enumScanFallback": bool}}`.
- `IReader::rawHeader()`: the 1,762 header bytes (0x5DE..0x5E1 held as 0, D-143), the tag `ESR1`, then the
  event, venue, vehicle and weather records, each as a u32 length and its bytes.
- `.ldx` in `DataSource::companionOpaque`: each string is a `<?esses-ldx ...?>` instruction followed by XML;
  `parent=`/`position=` for unknown elements (verbatim), `element=`/`group=`/`marker=`/`detail=` for the
  structure, groups, markers and details kept for the writer (`src/importer/ldx_companion.cpp`).
- Writer (`IWriter` `motec.ld`, options `{"ld": {"dialect", "dataAlignment", "verbatim"}}`): verbatim stored
  bytes, records and gap blocks for channels with an .ld origin; re-quantisation through the encoding (and
  the UnitRegistry back to the vendor unit) otherwise; default encodings for channels without one; device
  events, enumeration tables, the raw-header blob (or a fresh header from the details); the `.ldx` from the
  companion structure, Companion and User markers (an ignored BCN as IGRDBCN) and details, unknown elements
  byte-identical at their positions. Writer notes: `ld.channel_skipped`, `ld.detail_skipped`, plus the core's.

Pipeline codes: `open.units_skipped` (Info), `open.unit_unknown` (Warning, ctx.detail = the spelling,
repeats = channels), `open.unit_not_applied` (Info), `open.sidecar_load_failed` (Warning),
`open.read_error` (DecodeLog Warning), `sidecar.source_changed` (expectedHash mismatch); the model's notes
(`companion.*`, `sidecar.*`, `model.*`, `detail.*`) are copied under their codes.

Deviations from 05 sections 1-2 (M0): `JsonDoc` is not in esses_base yet, so JSON members are
`model::OpaqueJson`; `load()` returns `Future<OpenedSource>` (M2 adds the `MappingHook` overload and `resume`,
see "Text importers"); in M0 step 4 had no dictionary, unit-mapping profile or mappings, and timestamped channels
did not open (M2 opens them); `IReader` gains `interp`, `companion`, `formatHeader`, `setConversion` and
`makeChunkProvider`, `ByteSource` gains `mtimeUnixUs`, `mappedFile` and `buffer`; `ImporterRegistry::writer` has
a non-const overload because `IWriter::write` is non-const; the motec.ld writer's timestamped channels and
partial-range or resampled exports are Unsupported.

Tests: `esses_formats_tests` (byte sources, registry sniffing and opening, the importer mapping, probe_json
goldens in `tests/data/probe/` validated against `schemas/probe-1.schema.json`, the writer's round trips and
the `[oracle]` public-API smoke test over the private corpus). `ESSES_UPDATE_GOLDEN=1` rewrites the goldens;
review the diff.

## The .ld/.ldx core (`src/ld/`, namespace `esses::formats::ld`, internal)

It includes nothing above `esses_store` (plus pugixml for `.ldx`); `src/importer/` maps it onto the model.

| File | Content |
|---|---|
| `ld_strings` | fixed-width fields: cut at NUL, UTF-8 when valid else cp1252, right-trim; writer encoding (cp1252 when it reads back unambiguously, else UTF-8) and truncation on code-point boundaries |
| `ld_codes` | every Import Log code with its level and meaning; `sharedWithOracle` marks the names `tools/ld_ref_decode.py` uses with the same meaning |
| `ld_decode` | sample types, the scaling plan (afterMul default, beforeMul option; one correctly rounded division when exact), binary16, the plain decode kernel run through `MappedFile::guarded()` |
| `ld_file` | `LdFile`: header (every field of the port specification §1.2/§1.4), event/venue/vehicle/weather records through their u32 pointers, device events (0x688), enumeration block (0x690, scan fallback), channel walk (64-bit bounds, visited set, `size / 124` bound), gap blocks → sorted merged half-open invalid intervals, aux blocks (enumeration reference, long name, both padding forms), M1 terminator, unit/dps/interpolation rules, dialect flags, raw header and records for round trips; `decode`, `rawValues`, validity helpers |
| `ld_chunk_provider` | `LdChunkProvider : store::IChunkProvider`: AllValid / Bitmap / AllInvalid chunks from the gap intervals, NaN for invalid float samples, changed-file guard (`ld.source_changed`), page fault → AllInvalid (`ld.io_fault`); an optional `LdConversion` (the pipeline's conversion to base units, applied in F64 before the narrowing) and `decodeConverted` |
| `ld_writer` | `LdWriter`: fixture writer for every type pair, dialect (pc-layout, m1, c-series, sim-exporter, converter-legacy), gap/aux/enumeration/event blocks and details; 0x5DE always 0 (D-143); `inputFromFile`/`optionsFromFile` for read → write → read |
| `ldx` | `.ldx` DOM over pugixml: markers, details, attributes in order; unknown elements, unknown-class markers and unparsable markers kept as their verbatim source text and re-emitted byte-identically at their position |

Provenance: new code written from `docs/formats/ld-converter-port.md`, `docs/formats/ld.md` and the public MIT
layout notes (D-144). No code from the user's converter and nothing from gotzl/ldparser.

Deviations and decisions taken in this slice (for the documentation of 05 §3 and `docs/formats/ld.md`):

- Channel records with an undecodable width (not 1, 2, 4 or 8) and records with rate 0 but samples are not
  channels (`ld.unknown_dtype`, `ld.zero_rate`); the oracle decodes zero-rate channels anyway.
- Pairs beyond 05 §3.5 decode as guesses with `ld.dtype_guessed`: width 1 → i8, (0x06, 2) → i16,
  (0x00/0x03/0x05/0x06, 8) → i64 (stored F64), (0x07, 8) → f64, (0x08, 2/4) → i16/i32; 0x04 is raw unsigned
  (u8/u16/u32, `Bitfield`, `StatusBits` flag; u32 is stored F64 so 0xFFFFFFFF stays 4294967295), where the
  oracle reads it signed.
- Scaled integers are stored F64 when `dec + floor(log10 |scale|)` ≥ 5 or the record's raw range (0x54/0x58) of a
  32/64-bit integer exceeds 2^24, else F32 (05 §3.5 as amended 2026-10-11).
- High-volume per-channel Infos (`ld.unit_from_short_name`, `ld.dps_from_dec`, `ld.name_unterminated`,
  `ld.terminator_entry`, ...) are logged once per file with `ctx.repeats` = the number of channels.
- `ld.string_encoding` is logged once per file, always (`ascii`, `utf-8`, `cp1252`, `mixed`).
- Interpolation byte 0 in the C-series logger record layout and bytes 4 and above give the kind default with
  `ld.interp_default`.
- The C-series record rules (unit rule (b)/(c), interpolation byte 0) follow the logger layout (u16@0x040 = 0 and
  `event_ptr` > 0x10000) whatever the device type; the `c-series` label also needs device type `C<digits>`.
  `sim-exporter` is the gotzl (`ADL` + u16@0x054 = 0xADB0) or afonso360 writer (u32@0x05A = 0x00010064 with
  u16@0x040 = 0 outside the logger layout) default header; unsettled (synthetic only).
- Work budgets keep probe O(file) on hostile input: gap blocks parsed once per pointer, at most size / 6 gap
  events parsed and intervals materialised per file; aux blocks read up to 16 arguments and size bytes per file;
  the enumeration scan keeps referenced tables only, checks candidates without decoding and stops at 4 × the
  region. Reversed gap pairs read as [exit, entry); `{"ld": {"gapConvention": "shifted"}}` reads the converter's
  Python-writer pairs as [entry − 1, exit + 1).
- The unit rule's registry lookup for C-series rule (c) is `LdOptions::unitResolves` (no `esses_units` here);
  without it the string stays the short name.
- `ldx.marker_unsorted` is checked within each marker group (groups are written one after the other).

Tests: `esses_formats_ld_tests` (round trips of every synthetic family, hostile input and a seeded mutation
smoke test, the chunk provider through a `Column`, `.ldx`), the private `[oracle]` cases over the user's files
in `fixtures/private/s1` (skipped when absent), `formats_ld.oracle_synthetic` (the synthetic families decoded
by `tools/ld_ref_decode.py`) and `lint.check_ld_codes` (one code vocabulary with the oracle).

## Text importers (M2; 05 §6–§8, D-029, D-167, D-168, D-190)

The text importers live in `src/text/`, their tests in `tests/text/` (one Catch2 executable, `formats_text.<test>`).
`docs/formats/text.md` is the as-built description (rules, dialect flags, every Import Log code, departures).

| Header | Contents | Design |
|---|---|---|
| `text_profile.hpp` | `TextProfile` (importer-profile-1: `parse`, `load`, canonical `toJson`, `contentHash` = xxh3 of that text, `effectiveImporterId`) | 05 §7.2 |
| `text_engine.hpp` | `TextEngine::makeImporter` (an `IImporter` over a profile and its dictionary), `loadProfiles` | 05 §7.1 |
| `dictionary.hpp` | `Dictionary` (dictionary-1: `parse`, `load`, `toJson`, `match` by vendorId, then name, then nameRegex) | 05 §8.3 |
| `unit_mapping_profile.hpp` | `UnitMappingProfile` (unitmap-1: canonical round trip, atomic `save`, `resolve` most specific first, `fileStem`) | 05 §8.2 |
| `csv.hpp` | importer ids `motec.csv` and `text.profile:generic.csv`, writer ids `motec.csv` and `csv`, `kLegacyCsvFormatName` (vendor-mark allow-list class c), `makeLegacyCsvWriter`, `makeGenericCsvWriter` | 05 §6 |
| `mapping.hpp` | `OpenState` (opaque), `UnresolvedRow`, `PendingResolution`, `MappingDecision`, `MappingHook` | 05 §2, §8.1 |
| `registry.hpp` (M2) | `LoadResult`, `addProfile`, `load(…, MappingHook)`, `resume`; `OpenContext::mappings` and `unitMaps`; `open_code::kUnitMapLoadFailed` | 05 §2 |
| `importer.hpp` (M2) | `IReader::columnHint`, `dictionary`, `makeTimesProvider`, `timeRange`, `attachColumns` (all defaulted) | 05 §2 step 4a, §7.1 |

Sources: `text_scan` (encodings, lines, cells, fast_float numbers with either decimal point, the rational rate
search, bounded and portable regular expressions), `text_analyse` (pass 1: preamble, header, columns, time, row
sets and their chunk starts, enumeration labels, lap sources, hold carry-in, the `ProbeResult`), `text_decode`
(pass 2: one chunk of one row set for any set of columns), `text_engine` (the importer, the reader, the slab
decoder that shares one sweep between the columns of a source and seals siblings with `insertGroup` through
`store::WeakColumn`), `text_profile`, `dictionary`, `unit_mapping_profile`, `csv_writers`; the pipeline's M2 part
is in `registry.cpp`.

Shipped data: `data/profiles-text/motec.csv.json` and `generic.csv.json`, `data/dictionaries/motec.csv.json` and
`generic.csv.json`; the build embeds them (`generated/builtin_text_profiles.hpp`) and `withBuiltins` registers
both profiles (after `motec.ld`) and both writers. Schemas `importer-profile-1`, `dictionary-1`, `unitmap-1`
with examples. Fixtures (AC-M2-13): `fixtures/motec.csv/dialect-basic`, `fixtures/generic.csv/{semicolon-decimal-
comma, sparse-gps, timestamped-tab}`, self-authored by `tools/gen_text_fixtures.py` (the oracle of their channel
values). Fuzz targets `fuzz_csv_sniff`, `fuzz_text_profile` (`tests/fuzz/`); benchmark `bench/formats_bench`.

Open pipeline in M2 (05 §2 as built, D-168): step 4a applies the reader's column hint (the profile's `columns`) and
the importer's dictionary; 4b the importer's unit map (`ImportOptions::unitMap`, else `OpenContext::unitMaps`),
then the unit registry; 4c `OpenContext::mappings` and the unit maps' analysis names (`model::applyMappings`).
Unresolved = a vendor unit known to neither, or a quantity without a unit: with a `MappingHook` task A ends with a
`PendingResolution`, `resume` runs task B with the `MappingDecision`; without one the channel opens unitless with
`Unit unknown: "<unit>"` (`strictUnits`: NotFound). Timestamped channels open as `TimestampedSeries` (time
columns shared between channels whose reader returns the same provider).

Tests: `formats_text.*`: the scanner; the three documents (shipped files parse and validate, canonical round
trips, errors with JSON pointers, matching order); the engine through the registry (legacy dialect, decimal comma,
units in names, sparse and text columns, timestamped files with last-wins and dropped rows, epoch times, BOMs,
UTF-16, cp1252, CR-only lines, quotes, trailing delimiters, duplicate and empty names, lap counter beacons, ragged
rows, sniff bands, refused `blocks` layout, `addProfile` rules); the Mapping hand-off (AC-M2-11 at registry level);
slabs (one sweep seals siblings, parallel decodes equal the sequential result, hold across slab boundaries, sparse
chunk indexes); the CSV writers (legacy round trip, plain table with resampling and ranges); seeded mutations of
the fixtures under both profiles (`[property]`); the private sample files named by `ESSES_PRIVATE_TEXT_SAMPLES`
(`[oracle]`, skipped when unset).

Deviations (D-167, D-168; details in `docs/formats/text.md`):

- `LoadResult` is `variant<OpenedSource, PendingResolution>` (05 §2 has the bare `DataSource`), `resume` returns
  `Future<OpenedSource>`; both take the M0 `OpenContext` where 05 §2 passes a `Progress`. `resume` works on a
  copy of the state; the context it is given replaces task A's when it names a registry or a cache.
- `MappingDecision::unitMap` is a typed `std::optional<UnitMappingProfile>` (05 writes `JsonDoc`);
  `PendingResolution` adds `path` and `importerId`. `saveUnitMap` is the caller's to honour (the registry knows no
  project directory).
- `ImporterRegistry::addProfile` returns `Result<void>` (a colliding importer id, or an `importerId` other than the
  built-in `motec.csv`, is refused) and takes the profile's `Dictionary`.
- The rules of 05 §7 that differ (D-167): `@auto` names row at half numeric, decimal comma among separator tokens,
  checked rate candidates, column classification from spread samples, the portable multi-line sniff search, the
  `capture` direction, the slab decoder's in-flight table as the share key, `blocks` layout Unsupported.
- D-190: both passes read a file mapping through `GuardedLines` (guarded 4 MiB windows; rows over 16 MiB cut) and
  pass 2 checks `changedOnDisk()` before each chunk, so the page-fault guard covers text files too (D-167's
  departure withdrawn); `text::analyse` takes the mapping. Cells are capped at what a row can use, a first line
  over 1 MiB is Format; a ragged row is an invalid sample of a sparse column in both passes; a header-only CSV
  opens with no samples; an odd UTF-16 byte is dropped; quoted line breaks are reported (`text.quoted_newline`),
  not joined; nested-quantifier patterns are refused and dictionary patterns compiled once (`text::cachedRegex`);
  the sniff sees the first 16 lines. `ImportOptions::cancel` (an addition to 05 §1.1) carries the open's token to
  `IImporter::open`; `ImporterRegistry` sets it.
- Dictionary bitfields are read and written but not expanded into Bool channels yet (05 §8.3; no shipped
  dictionary has one).

## M4 contract (07 §7.1; `docs/milestones/M4.md` §2-3; D-206)

- `resident_estimate.hpp` (budget package, `src/m4_budget/`): `ResidentEstimate` (final: the byte counts,
  `total()`, `+=`), `estimateChannel`, `estimateResident(ProbeResult)` and
  `estimateResident(DataSource)`: the resident bytes of a source's logged channels in the chunk layout of
  `esses_store`, what `SourceLoader`'s open-time budget gate compares with the budget (AC-M4-02).
- `source_file_check.hpp` (reload package, `src/m4_reload/`): `SourceFileState` and `sourceFileStateName` (final),
  `checkSourceFile(path, size, mtime)` and `checkSourceFile(SourceProvenance)`: the re-stat of an
  open source's file for every importer (02 §1 behaviour 5, AC-M4-10). The reload package also owns the
  providers' own checks (`src/importer_chunk_provider.*`, `src/ld/ld_chunk_provider.*`).
- Tests: `tests/m4/**` is one Catch2 executable (`formats_m4.<test>`); `m4_contract_tests.cpp` covers what is
  final, `budget_*.cpp` and `reload_*.cpp` belong to the packages. Every member above is implemented (D-207, D-208;
  the sections below); no contract stub is left (D-216).

## M4 budget: the open-time resident estimate (AC-M4-02; D-207)

- `estimateChannel`, `estimateResident(ProbeResult)` and `estimateResident(DataSource)`
  (`src/m4_budget/resident_estimate.cpp`) count every chunk as `store::ChunkBuilder::allocate` lays it out with a
  validity bitmap: samples padded to 64 bytes, the bitmap, the factor-4 pyramid of float pairs, the rounding of the
  layout to 64 bytes and the 64-byte header (`overheadBytes`); an I64 time column has no bitmap and no pyramid. It is
  the upper bound of what `ChunkCache::Stats::residentBytes` reports once every chunk of the source is resident
  (equal when every chunk carries a bitmap, the bitmaps above it when they are AllValid). The source form counts the
  logged channels only, a column several channels share once (`Column::id`), a timestamped channel's time column
  too; the probe form adds an I64 time column per timestamped channel. Nothing is decoded.
- Tests: `tests/m4/budget_resident_estimate_tests.cpp` (`formats_m4.budget: ...`): hand-worked chunk sizes of 01
  §7.1 for every dtype, full and partial chunks, a probe and a source, and the estimate held against the cache's
  resident bytes after every chunk of a synthetic source is materialised.

## M4 reload: changed source files (AC-M4-10; D-208)

- `checkSourceFile` (`src/m4_reload/source_file_check.cpp`): `esses::statFile` of the provenance's path against its
  size and mtime: Changed when either differs, Missing when the path cannot be stat'ed, Unchanged otherwise and for
  a source without a path. It never reads the file.
- The providers' own check before each decode batch is as built (`ld.source_changed` from `LdChunkProvider`,
  `text.source_changed` from the text engine's decode): no further chunk is decoded from a changed file. Every
  built-in reader supplies its own provider; `ImporterChunkProvider` (a reader without one) holds no mapping and
  stays unchanged.
- `LdChunkProvider` seals a chunk `AllInvalid` on a page fault and logs `ld.io_fault` once per source (its
  DecodeLog; it was once per fault), also when channels of the source fault together on several workers (the check
  and the add under one lock). `setDecoder` (private header) lets the AC-4a test raise an in-page error inside
  `MappedFile::guarded()` on Windows, where the truncation that would cause a real fault is refused; on POSIX the
  test's fault is real.
- Tests: `tests/m4/reload_source_file_check_tests.cpp`, `tests/m4/reload_provider_tests.cpp`
  (`formats_m4.reload: ...`) with `tests/m4/reload_test_support.hpp`; `ld provider: a file changed on disk ...`
  moves the mtime through the attributes only.
