# esses_project

Layer 2, Qt-free (01 §2.1, 02 §6). The persisted project documents and the shipped defaults. M1 (07 §4.1):
`ProjectFile`, `WorkbookFile`, `ComponentSpec`, `Scheme`, `Keymap`, `Profile` (Circuit) read/write paths, and the
shipped `data/workbooks/circuit-default.workbook.json`, `data/schemes/light.scheme.json`,
`data/schemes/dark.scheme.json`, `data/keymaps/default.json` (the full 06 §14.4 table; hotkey batch 1
`implemented`/M1, every other row `planned` with its milestone or `reserved`) and
`data/profiles/circuit.profile.json`. `SchemaRegistry`, `Migrations`, `Journal`, `ChannelOverrides`, `Aliases`,
`Mappings`, `TrackMapFile` and the configuration importers arrive later (M2+).

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
| `project_file.hpp` | `SourceKey`, `RangeKey`, `SelectionKeys`, `RecentSource`, `BuiltinMathsEntry`, `ProjectFile`, `saveWithBackup` | 02 §6.2, 06 §5.5, D-070 |
| `scheme.hpp` | `FontSpec`, `OverlayStyle`, `kSchemeRoles`, `kSchemeFontRoles`, `Scheme` (`role`, `font`, `channelColour`, `overlayColour`, `fillMissingFrom`) | 06 §16, 02 §6.9 |
| `keymap.hpp` | `KeymapStatus`, `kKeymapContexts`, `kKeymapMilestones`, `KeyBinding`, `KeyUnbind`, `Keymap` (`merged`, `conflicts`, `rowsFor`), `normaliseKeySequence` | 06 §14.3-14.4 |
| `profile.hpp` | `DataView`, `ProfileDetail`, `Profile` (`defaultView`, `findDetail`, `circuitDefault`) | 02 §6.9, D-036 |
| `shipped_data.hpp` | `kCircuitDefaultWorkbook`, `kLightScheme`, `kDarkScheme`, `kDefaultKeymap`, `kCircuitProfile`, `kRulesDir`, `shippedFile` | 02 §6.1 |

## Reading and writing (02 §6.5-6.6)

- Every document kind has `parse(text, [fileName,] log)`, `load(path, log)`, `toJson()` and `save(path)`
  (`ComponentSpec`: `parse`/`toJson` of one compact object). Errors: `Format` (syntax, with `context.offset`),
  `Invalid` (with `context.detail` = the JSON pointer of the offending member), `Unsupported` for another major
  of the schema (`context.schemaId`), `Io`; `load` adds `context.path`.
- `"$schema"` is checked in the `urn:esses:`, `esses:` and https forms and always written as `urn:esses:<id>`; a
  missing `"$schema"`/`"schemaVersion"` is read as version 1 and logged (02 §6.6 rule 4); a newer minor is
  logged and read (no migrations before M2). Unknown top-level members are preserved and re-emitted sorted;
  typed sub-records are closed, as in the schemas.
- `toJson()`/`save()` produce the canonical text of `esses_base`'s writer (`JsonDoc`, 02 §6.5 rule 1); `save`
  writes atomically (`AtomicFile`). `ProjectFile::save` and `saveWithBackup` keep the previous bytes in
  `<Project>/backup/<file>.<yyyymmdd-hhmmss>.json`, last 5 per file (D-070).
- Written forms: workbook headers in full (`id, type, version, rect, z, caption, links, legend,
  activeChannel`; `body`/`x-i2` when present); project typed members in full (`options`/`dataWindow` when
  present); scheme roles/fonts/palettes when not empty, `strikeOut` only when true; keymap rows write `args`,
  `milestone`, `doc` when set and `status` when not implemented or a milestone is set (every shipped row, no user
  row); profile `default` only when true, `outingDetail`/`unit` when not empty, `dps`/`default` when set.
- Round trip (02 §6.5 rule 3, `tests/round_trip_tests.cpp`): the shipped documents are byte-identical after
  load and save; every valid schema example saves idempotently to a schema-valid document; the loaders reject
  the invalid examples except those of members M1 keeps opaque (`options`, `dataWindow`, `print`).
- Ids: a workbook without `id` gets `uuidFromHash(hash128({"workbook", fileName}))`, a worksheet
  `uuidFromHash(hash128({"worksheet", workbookId, index}))`, a component
  `uuidFromHash(hash128({"component", worksheetId, index}))`; each sets `dirty` and logs.

Schemas (`schemas/`, D-072): `workbook-1`, `project-1`, `scheme-1`, `keymap-1`, `profile-1`, with valid and
invalid examples under `schemas/examples/<id>/` (`tools/check_schemas.py`, `tests/schema_examples_tests.cpp`).
Component bodies are not validated by `workbook-1` (their `schemas/components/<type>-<version>.schema.json`
belong to the component owners). `tools/check_keymap_coverage.py` is the AC-M1-06 lint (`--rows-only` is
registered in ctest; the full check needs `KEYMAP_TEST("<command.id>")` rows under `tests/ui/keymap/`).

## Shipped documents

- `circuit-default.workbook.json` (name "Circuit"): worksheet 0 "Overlay" (time domain, `esses-render
  --worksheet 0`): a `time_distance` graph on `[0, 0, 0.75, 1]` with panels Speed (`Corr Speed`, `SPEED`),
  Pedals (`THROTTLE`, `BRAKE`), Steering (`STEERANGLE`), Engine (`RPMS`), Gear (`GEAR`), next to a `values_list`
  on `[0.75, 0, 1, 1]` (`Corr Speed`, `Corr Dist` and the same channels plus `FUEL`); worksheet 1 "Driver"
  (distance domain). Channel names are the ACC-style logged names of the M1 data path (D-044), resolved
  case-insensitively by `ChannelResolver`; missing channels draw as missing.
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
