# esses_project

Layer 2, Qt-free (01 §2.1, 02 §6). The persisted project documents and the shipped defaults. M1 (07 §4.1):
`ProjectFile`, `WorkbookFile`, `ComponentSpec`, `Scheme`, `Keymap`, `Profile` (Circuit) read/write paths, and the
shipped `data/workbooks/circuit-default.workbook.json`, `data/schemes/light.scheme.json`,
`data/schemes/dark.scheme.json`, `data/keymaps/default.json` (the full 06 §14.4 table; hotkey batch 1
`implemented`/M1, every other row `planned` with its milestone or `reserved`) and
`data/profiles/circuit.profile.json`. M2 (07 §5.1): the channel documents `ChannelOverrides` (`overrides-1`),
`Aliases` (`aliases-1`), `Mappings` (`mappings-1`), `SchemaRegistry`, `Migrations` with the first real migration
(`project-1` 1 -> 2, D-162), the write-ahead `Journal` (`journal-1`), and the load pipeline every document kind
shares (D-163; see "M2: persistence" below). M3: `TrackMapFile` (`trackmaps/<venue>.track.json`, below). The
configuration importers arrive later.

Namespace `esses::project`; public headers in `include/esses/project/` (umbrella `project.hpp`). Links
`esses_base`, `esses_units`, `esses_model`, `esses_analysis`, `esses_maths` (same-layer edges, D-122);
nlohmann-json and json-schema-validator are private (no JSON library type in a public header; opaque members are
`esses::JsonDoc`, of which `model::OpaqueJson` is an alias). Sources and tests are globbed. `CMakeLists.txt`
copies every JSON file under `data/{workbooks,schemes,keymaps,profiles,rules}` to `<build>/bin/data/...` (the
data directory of `Paths::dataDir()` in a build tree), so new shipped documents need no CMake edit.

| Header | Contents | Design |
|---|---|---|
| `rgba.hpp` | `Rgba` (0xRRGGBBAA), `rgba()`, `red/green/blue/alpha`, `parseRgba`, `formatRgba` | 04 §5.1, 06 §16 |
| `component_spec.hpp` | `Rect`, `Links`, `Caption`, `Legend`, `ComponentSpec` (`parse`, `toJson`), `isKnownComponentType`, `supportedComponentVersion` | 04 §7.3, 02 §6.4 |
| `workbook_file.hpp` | `WorksheetDef`, `WorkbookFile` (`parse`, `load`, `toJson`, `save`, `worksheet`) | 02 §6.4, 06 §4 |
| `project_file.hpp` | `SourceKey`, `RangeKey`, `SelectionKeys`, `RecentSource`, `BuiltinMathsEntry`, `ProjectFile` (M2: `kSchemaVersion` 2, `dirty`, `autosaveMinutes`, `lapStretchSettings`), `saveWithBackup` | 02 §6.2, 06 §5.5, D-070 |
| `scheme.hpp` | `FontSpec`, `OverlayStyle`, `kSchemeRoles`, `kSchemeFontRoles`, `Scheme` (`role`, `font`, `channelColour`, `overlayColour`, `fillMissingFrom`) | 06 §16, 02 §6.9 |
| `keymap.hpp` | `KeymapStatus`, `kKeymapContexts`, `kKeymapMilestones`, `KeyBinding`, `KeyUnbind`, `Keymap` (`merged`, `conflicts`, `rowsFor`), `normaliseKeySequence` | 06 §14.3-14.4 |
| `profile.hpp` | `DataView`, `ProfileDetail`, `Profile` (`defaultView`, `findDetail`, `circuitDefault`) | 02 §6.9, D-036 |
| `shipped_data.hpp` | `kCircuitDefaultWorkbook`, `kLightScheme`, `kDarkScheme`, `kDefaultKeymap`, `kCircuitProfile`, `kRulesDir`, `shippedFile` | 02 §6.1 |
| `channel_overrides.hpp` | `ChannelOverrides` (`overrides-1`; `Entry`, `find`, `set`, `clear`) | 02 §6.3, 06 §13.1 |
| `aliases.hpp` | `Aliases` (`aliases-1` over `model::AliasTable`; `cycleThrough`) | 02 §4.6, §6.3 |
| `mappings.hpp` | `Mappings` (`mappings-1`; `Row`, `toTable`) | 02 §4.6, §6.3 |
| `schema_registry.hpp` | `SchemaRef`, `SchemaRegistry` (`instance`, `findSchemasDir`, `loadDirectory`, `ids`, `validate`, `schemaOf`) | 02 §6.1, §6.6 |
| `migrations.hpp` | `Migrations` (`instance`, `add`, `upgrade`, `latest`) | 02 §6.6 |
| `journal.hpp` | `JournalEntry` (`of`), `JournalFile` (`journal-1`), `JournalReplay`, `JournalRecovery`, `Journal` (`append`, `pending`, `ownFileExists`, `replay`, `clear`, `recover`) | 02 §6.8, D-070 |

## Reading and writing (02 §6.5-6.6)

- Every document kind has `parse(text, [fileName,] log)`, `load(path, log)`, `toJson()` and `save(path)`
  (`ComponentSpec`: `parse`/`toJson` of one compact object). Errors: `Format` (syntax, with `context.offset`),
  `Invalid` (with `context.detail` = the JSON pointer of the offending member), `Unsupported` for another major
  of the schema (`context.schemaId`), `Io`; `load` adds `context.path`.
- The load pipeline (02 §6.6 rule 2, D-163; `src/doc_io.hpp` `readDocument`): JSON syntax; `"$schema"` in the
  `urn:esses:`, `esses:` and https forms (another schema `Invalid`, another major `Unsupported`), always written
  as `urn:esses:<id>`; a missing `"$schema"`/`"schemaVersion"` is read as version 1 and logged (rule 4); an
  older minor runs the registered `Migrations` up to the kind's current minor (one log line per step; the
  document is marked dirty where the type has the flag, `ProjectFile::dirty`); the typed reader; then
  `SchemaRegistry::instance()` validates the document as it will be written (`Invalid` with the schema's JSON
  pointer), which checks the members the typed readers keep verbatim (`options`, `dataWindow`, `print`, unknown
  members) while their own leniencies stay accepted. Unknown top-level members are preserved and re-emitted
  sorted; typed sub-records are closed, as in the schemas.
- A newer minor (R-38): read as it is, its `schemaVersion` kept on save (never written back as an older minor);
  when the typed reader or the schema refuses it (a member this build does not know in a closed record), the
  error is `Unsupported` ("written by a newer build ... left untouched"), so it is never overwritten with a
  loss.
- `toJson()`/`save()` produce the canonical text of `esses_base`'s writer (`JsonDoc`, 02 §6.5 rule 1); `save`
  writes atomically (`AtomicFile`). `ProjectFile::save` and `saveWithBackup` keep the previous bytes in
  `<Project>/backup/<file>.<yyyymmdd-hhmmss>.json`, last 5 per file (D-070).
- Written forms: workbook headers in full (`id, type, version, rect, z, caption, links, legend,
  activeChannel`; `body`/`x-i2` when present); project typed members in full (`options`/`dataWindow` when
  present); scheme roles/fonts/palettes when not empty, `strikeOut` only when true; keymap rows write `args`,
  `milestone`, `doc` when set and `status` when not implemented or a milestone is set (every shipped row, no user
  row); profile `default` only when true, `outingDetail`/`unit` when not empty, `dps`/`default` when set.
- Round trip (02 §6.5 rule 3, `tests/round_trip_tests.cpp`, AC-M2-07 in `tests/roundtrip_m2_tests.cpp`): the
  shipped documents are byte-identical after load and save; every valid schema example saves idempotently to a
  schema-valid document; the loaders reject every invalid example (those of the opaque members with the
  schema's pointer).
- Ids: a workbook without `id` gets `uuidFromHash(hash128({"workbook", fileName}))`, a worksheet
  `uuidFromHash(hash128({"worksheet", workbookId, index}))`, a component
  `uuidFromHash(hash128({"component", worksheetId, index}))`; each sets `dirty` and logs.

Schemas (`schemas/`, D-072): `workbook-1`, `project-1` (minor 2 since M2), `scheme-1`, `keymap-1`, `profile-1`,
and from M2 `overrides-1`, `aliases-1`, `mappings-1`, `journal-1` and `maths-1` (03 §4.1; the module type and its
reader are `esses_maths`' `MathModuleIo`), with valid and invalid examples under `schemas/examples/<id>/`
(`tools/check_schemas.py`, `tests/schema_examples_tests.cpp`, `tests/schema_registry_tests.cpp`). The M2 examples
are written in the canonical form, so the valid ones double as round-trip goldens.
Component bodies are not validated by `workbook-1` (their `schemas/components/<type>-<version>.schema.json`
belong to the component owners). `tools/check_keymap_coverage.py` is the AC-M1-06 lint (`--rows-only` is
registered in ctest; the full check needs `KEYMAP_TEST("<command.id>")` rows under `tests/ui/keymap/`).

## Shipped documents

- `circuit-default.workbook.json` (name "Circuit"): worksheet 0 "Overlay" (time domain, `esses-render
  --worksheet 0`): a `time_distance` graph on `[0, 0, 0.75, 1]` with panels Speed (`Corr Speed`, `SPEED`),
  Pedals (`THROTTLE`, `BRAKE`), Steering (`STEERANGLE`), Engine (`RPMS`), Gear (`GEAR`), next to a `values_list`
  on `[0.75, 0, 1, 1]` (`Corr Speed`, `Corr Dist` and the same channels plus `FUEL`); worksheet 1 "Driver"
  (distance domain). Channel names are the ACC-style logged names of the M1 data path (D-044), resolved
  case-insensitively by `ChannelResolver`; missing channels draw as missing. M3 (07 §6.1 "Data", D-202) appends
  worksheets 2-6: "Overview" (distance: graph, `track_position`, two `gauge_numeric`, two `gauge_bar`),
  "Braking" (distance: graph with F3 ready, brake `histogram`, throttle-vs-brake `scatter` with a polynomial-3
  trend), "Corners" (distance: graph, lateral-vs-longitudinal G `scatter`, `track_position`), "Reports" (time:
  `channel_report` Venue › Driver › Lap with Δ vs Overlay 0, `time_report` of the Default sections with bands
  0.5/1/2 % and a hidden 3 % band) and "Track" (distance: `track_position`, `outing`, graph); with M1's
  `values_list` every MVP component type appears. The new throttle and brake traces carry `displayUnit` `%`
  (logged percentages are stored in ratio, D-010); lateral and longitudinal G are `G Force Lat`/`G Force Long`.
- `light.scheme.json` ("Esses Light") and `dark.scheme.json` ("Esses Dark"): every role and font role of 06 §16
  (`kSchemeRoles`, `kSchemeFontRoles`), Noto Sans throughout, 12-colour channel palettes and 9-colour overlay
  palettes per background (every entry >= 3:1 contrast with `cptBackground`), `overlayStyle` plain channel
  colours (D-100), a print variant.
- `keymaps/default.json`: all 151 (command, context) rows of 06 §14.4 (178 rows with the per-argument rows of
  `view.worksheet_n`, `view.workbook_n`, `component.colour_band`); 48 rows / 39 commands `implemented` in M1
  (views: data/channels/values windows, return focus, worksheet n, previous/next worksheet; data: domain,
  overlays, single colour, next/previous range, fastest, all fast, swap, the Data-window selection keys, close
  file; zoom: in/out/full/default range/datum-main; pan: left/right/back/forward/centre; cursor: left/right/fast/
  home/end; datum: toggle/place/swap); the rest `planned` with M2, M3, M4 or v1.0, three `reserved`.
- `circuit.profile.json`: 02 §6.9 verbatim (`Profile::circuitDefault()` builds the same document).

## Deviations from the design text

- The data file names follow 02/06/08 (`light.scheme.json`, `circuit.profile.json`), not the shorter names of the
  M1 task text.
- The `Document<T>` operations of 02 §6.1 are static members of each document type rather than a template;
  validation through `SchemaRegistry` and `Migrations` joins them in M2.
- `Keymap` keeps key sequences and contexts as strings (keymap-1 spelling); `esses_qtmodel` converts them.
- `Rgba` is defined here (plot::Rgba aliases it) because `Scheme` sits below `esses_plotcore`.
- `view.channel_status` carries no key: 06 §14.4 gives Ctrl+U to both it and `tools.channel_status` in the
  global context, which 06 §14.2 rule 7 rejects as a conflict; the alias row documents the key of its target.
- Keymap rows for mouse gestures (`zoom.rubber_band`, `zoom.wheel`, `cursor.place`, ...) have empty `keys` and
  are `planned` M3, so every M1 `implemented` row can be tested by sending its key (AC-M1-06); the M1 graph's
  mouse cursor and zoom are exercised by the walkthrough tests instead. Keyless menu rows of batch 1
  (`view.messages_window`, `view.reset_docks`) are `planned` M2 for the same reason.
- `data.select_main`, `data.toggle_overlay0` and `data.toggle_overlay` also bind the keypad Enter (`Enter`,
  `Ctrl+Enter`, `Alt+Enter`) next to `Return`, as 06 §14.3's example does for `data.select_main`.
- Additions to the M1 contract (additive): `FontSpec::strikeOut` (06 §16 `keyStrikeout`), `Scheme::overlayColour`,
  `Scheme::fillMissingFrom` (06 §16 rule 8), `kSchemeRoles`/`kSchemeFontRoles`, `Keymap::unknown`,
  `Keymap::rowsFor`, `kKeymapContexts`/`kKeymapMilestones`, `normaliseKeySequence`, `Profile::findDetail`,
  `Profile::save`, `WorkbookFile::worksheet`, `isKnownComponentType`/`supportedComponentVersion`,
  `saveWithBackup`.

## M2: persistence (07 §5.1; `docs/milestones/M2.md`)

The persistence work lives in `src/persistence/` (headers in `include/esses/project/`); no `M2-STUB` is left in
this library.

- **Channel documents** (02 §6.3): `ChannelOverrides` keyed by channel name as written (case-insensitive lookup,
  names differing only in case kept and logged); every `{v, unit}` is a `model::Quantityd` with the written unit
  symbol, so a canonical document round-trips byte-identically; unknown members of an override entry are kept in
  `Entry::unknown`. `Aliases` read into `model::AliasTable` (`enabled` has one flag per candidate, all enabled
  when absent, always written); names are unique case-insensitively; `cycleThrough(name)` follows the resolver's
  expansion (02 §4.6 rule 17: a candidate spelled like its own alias is that channel, an alias already being
  expanded closes the cycle; disabled candidates count, so enabling one later cannot create a cycle the editor
  allowed); a cycle is logged on load, not refused. `Mappings` rows with each logged name at most once
  (case-insensitively); `unitlessOnly` written only when true.
- **`SchemaRegistry`**: every `*.schema.json` of `findSchemasDir()` (`<dataDir>/schemas`, `Paths::resolve()`'s data
  directory, `<exe dir>/data/schemas`, then the source tree, `ESSES_PROJECT_SOURCE_DIR`) compiled once with
  json-schema-validator; read-only afterwards, so concurrent `validate` calls need no lock. `validate` accepts the
  id in any of the four forms; `Invalid` names every error with its pointer and puts the first in
  `context.detail`; `schemaOf` returns the name, the major and the document's minor.
- **`Migrations`**: `upgrade` stamps `schemaVersion` after each step and logs it; `latest(schema)` is one above
  the highest registered step. The shipped step `project-1` 1 -> 2 (D-162): Data-window filter keys become
  `"default"` or `"xxh3:<32 lower-case hex digits>"` (`model::hashText` of the source's quick hash, the form of
  every other hash in project-1); a key of 32 bare hex digits gains the prefix (lower-cased), any other key
  moves with its filter to `x-esses-legacy.dataWindowFilters`. `ProjectFile::kSchemaVersion` is 2.
- **`Journal`** (02 §6.8, D-070): `append` writes `.journal/<seq>.json` (`JournalFile`, `journal-1`) atomically,
  `seq` one above every file on disk and every seq this object wrote; entry paths must stay inside the project
  folder; an entry's hash is computed when zero and checked otherwise (`JournalEntry::of` builds one from a
  document's `toJson()`). Every file is its session's complete unsaved state, so `append` deletes the files the
  object wrote before (D-187). `replay` takes each session's newest file (a damaged entry there falls back to the
  same path in the session's next older file), merges the sessions newest entry per path by (seq, at, file name)
  and logs a path two sessions changed, reports a `baseHash` that no longer matches the bytes on disk (or a file
  that appeared where `baseHash` is null) as a conflict, and skips (and logs) a file that is not a readable
  `journal-1` document: a kill mid-write leaves only an `AtomicFile` temporary file, which is ignored and removed by
  `clear`. `pending`, `replay` and `clear` take `JournalSessions` (`All`, this object's session `Own`, `Others`).
  `recover(prompt)` is the recovery hook of AC-M2-06 (`Recover` returns the replay, `Discard` clears it, `Later`
  leaves it). Project files are
  never written by the journal; the 2-minute timer and the prompt are `ProjectSession`'s (`esses_qtmodel`), which
  reads the interval from `ProjectFile::autosaveMinutes()` (`options.general.autosaveMinutes`, 0 = off, default 2)
  and the lap-stretch toggle and tolerance from `ProjectFile::lapStretchSettings()` (`options.lapStretch`).
- **R-38** (M1 §6 item 7): every document type carries `schemaVersion` (the minor written on save) and a newer
  minor is never downgraded; see "Reading and writing".

Tests (`project.*`): `channel_documents_tests` (the 02 §6.3 examples, edits, loader-only rules, the alias cycle
rule), `schema_registry_tests` (every shipped schema, every example at its pointer, id forms, `schemaOf`, a
directory with unusable schemas), `migrations_tests` (step mechanics, the shipped step, migrate-on-load with
dirty and the backup on the first save, R-38), `journal_tests` (append, refusals, replay across sessions and
conflicts, a simulated kill and damaged files, the recovery hook, `journal-1` round trip),
`roundtrip_m2_tests` (AC-M2-07: byte-identical canonical documents of every kind, idempotent saves of every valid
example, an unknown component type and the `x-i2` bags; `maths-1` through `esses_maths`' `MathModuleIo`, skipped
with the stub's message only while a reader is still a stub). `channel_documents_contract_tests` keeps the M2-contract checks.

Consumers: `esses_qtmodel` (`ProjectSession`, `undo_commands.hpp`: `ChannelOverrides`, `Aliases`, `Mappings`,
`Journal`), `esses-cli` (`Migrations` in `validate`).

### Deviations of the M2 work from the design text

- `{v, unit}` members of `overrides-1` are `model::Quantityd` (the written unit symbol kept, so a canonical
  document round-trips byte-identically, 02 §6.3); `warn`/`fault` stay `JsonDoc` (checked on load) until a typed
  form is needed. The unit conversion to base and the D-010 family check are the host's, which has the registry.
- `Migrations::instance()` is mutable (steps register before the first load), `upgrade` also stamps the version,
  and `latest()` is an addition (`esses-cli validate` migrates to it).
- The journal adds `JournalReplay` (newest entry per path, `baseHash` conflicts, the paths whose file already holds
  the entry (`applied`, not replayed), the skipped count, log), `JournalFile` (the `journal-1` document, so the kind
  has its own round trip), `JournalEntry::of`, `JournalRecovery` and `Journal::recover` (the Qt-free recovery hook;
  it deletes a journal holding only applied entries without asking). Journal files are named
  `<seq>-<session tag>.json` (a decimal seq, the session id's first eight hex digits; untagged `<seq>.json` files of
  older journals are still read); `clear(All)` renames the folder away before deleting it and every clear deletes
  oldest first, and only the files it listed when it began (a file another session writes meanwhile stays;
  `src/persistence/journal_hooks.hpp` is the test seam between the listing and the deletions);
  `ownFileExists()` tells a session whether its newest file is still there (D-190).
- `saveWithBackup` copies the previous version to `backup/` before it writes and takes an optional `backupError`:
  without it an error means the document was not written; with it a failed copy is reported there and the
  document written all the same (D-190).
- `SchemaRef` adds `schemaVersion` (the document's minor) next to the name and the major.
- Load order (D-163): the typed reader runs before `SchemaRegistry`, which validates the written form, where 02
  §6.1 writes "validate -> migrate -> parse"; without a schemas directory the typed reader is the only check.
- `project-1` minor 2 (D-162) fixes the Data-window filter keys that 02 §6.2's key table calls "SourceId hex".

## M3 contract (07 §6.1; `docs/milestones/M3.md` §2; D-193)

- `track_map_file.hpp`, `src/track/track_map_file.cpp` (track work): `TrackMapFile`, the project document
  `trackmaps/<venue>.track.json` (`track-1`, D-061) around `analysis::TrackMap`. As built (AC-M3-04): `parse`
  runs the shared load pipeline (header and migrations, `TrackMap::fromJson`, then `TrackMap::validate`, so a
  file with a section under 10 m, out of bounds or with a duplicate id is Invalid at its JSON pointer, then the
  schema check of what `save` writes); `toJson`/`save` write the canonical text (lengths in metres, a newer
  minor kept), `save` atomically and creating `trackmaps/` when needed; the copy of the previous file to
  `backup/` on the Track Editor's Accept is the caller's. `fileNameFor` lower-cases ASCII letters and digits,
  turns every other ASCII run into one `-`, keeps non-ASCII UTF-8 and suffixes `-track` to a Windows device
  name (`con`, `lpt1`, ...). Tests: `tests/track_map_file_tests.cpp` (schema examples, byte-for-byte round
  trip, user sections saved into a new `trackmaps/` and reloaded, the pipeline's checks, file names).
- Schemas: `track-1` (`schemas/track-1.schema.json`, loaded by `SchemaRegistry` with the other document schemas)
  and the component body schemas `schemas/components/<type>-1.schema.json` (fragments, not loaded by
  `SchemaRegistry` yet; D-193 (a)).
