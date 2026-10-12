# esses_maths

Layer 2, Qt-free (01 §2.1, 03 §1). M1 is the minimal subset of 07 §4.1 and D-044: a basic `MathGraph`
(define/require/version), `PlanProvider` into the `ChunkCache` on the 0.1 whole-node path (03 §6.0, D-139),
`EvalContext`, and the built-in module `esses.corrected_speed_distance` with its settings schema. The language
(`Lexer`, `Parser`, `TypeChecker`), kernels, `MathsScheduler`, `MathModuleIo`, shadowing chains and
`esses-cli eval` arrive in M2.

Namespace `esses::maths`; public headers in `include/esses/maths/` (umbrella `maths.hpp`; snake_case file names
per the repository convention where 03 §1 lists `Parser.h`-style names). Links `esses_base`, `esses_units`,
`esses_store`, `esses_model`, `esses_analysis` (same-layer edge); never `esses_formats` or `esses_project`.
nlohmann-json and json-schema-validator are private. Sources and tests are globbed.

| Header | Contents | Design |
|---|---|---|
| `channel_key.hpp` | `ChannelKey{src, layer, nameCf}` (+ `std::hash`), `Version` (= `Hash128`), `SampleRange`, `ColumnHandle` (with the evaluation's `diags`) | 03 §4.3 rule 9, §7.1-7.2 |
| `diagnostics.hpp` | `Span`, `Diagnostic`, `Diagnostics`, `hasError` | 03 §3.1, §2.5 |
| `math_status.hpp` | `MathState`, `MathStatus`, `mathStateName`, `toChannelStatus` | 03 §7.3, D-060 |
| `maths_compat.hpp` | `MathsCompat` | 03 §2.3, D-020 |
| `math_module.hpp` | `ItemKind`, `MathItem` (builtin fields), `MathModule` (M1 subset of maths-1) | 03 §4.1 |
| `eval_context.hpp` | `MathRangeGeom`, `MathSelection`, `EvalContext` | 03 §7.5-7.6, D-050 |
| `builtin_module.hpp` | `DisplayDefaults`, `WholeExtentFn`, `BuiltinOutput` (`upstream`, `inputs`), `IBuiltinModule`, `validateSettings` | 03 §9, D-016, D-024 |
| `whole_signal.hpp` | `readWholeSignal` (a uniform series into an `analysis::UniformSignal`, worker only) | 03 §6.0 |
| `corrected_speed_distance.hpp` | `CorrectedSpeedDistanceModule` (`kId`, `kVersion`, `kSettingsSchemaId`, `kSettingsSchemaFile`, `kSpeedOutput`, `kDistanceOutput`, `loadSchema`, `validateSettings`, `findSchemasDir`) | 03 §9.1 |
| `plan_provider.hpp` | `PlanSnapshot`, `PlanProvider : store::IChunkProvider` | 03 §6.0, §6.2, §7.2 |
| `math_graph.hpp` | `MathRequest`, `MathGraph` (pimpl; `defineBuiltin`, `setModuleEnabled`, `setSource`, `version`, `require`, `prefetch`, `applyCompletion`, `applyReadyCompletions`, `status`, `consumersOf`, `outputs`, `descriptor`, `find`, `taskKey`, `taskKeyPrefix`) | 03 §7.1-7.3, §8 |

## How it works (0.1 whole-node path)

- `defineBuiltin(module, settings)` expands the module (null settings: its defaults) into one node per output.
  `defHash` = xxh3(engine version, module id and version, canonical settings JSON, output definition, compat
  switches); `version` = xxh3(defHash, enabled, module enabled, sorted upstream versions, layer extent, and for
  each `BuiltinOutput::inputs` name present in the layer its column id, store version, unit, dtype, interp and
  time base). `DataSource::version` is never hashed (D-140): a sidecar commit that shares the columns keeps
  every version. A node whose version changes drops its cached column, failure and evaluation and becomes
  Pending (Disabled while its module is disabled).
- `require(key)` never blocks. A cached column of the current version (or a memoised non-cancelled failure)
  comes back as a ready `Future`; otherwise the upstream nodes are required first and one task is submitted at
  the requester's priority under the share key `maths/<src hex>/<layer>/<name>/<version hex>/all`, so a repeat
  joins the queued or running task (one more requester, priority raised). The task waits for its upstream
  Futures (Rule W: a still-queued one runs inline), reads their sealed columns back with `readWholeSignal`
  (Corr Dist therefore integrates Corr Speed as stored, F32), calls `BuiltinOutput::evaluate` over the whole
  extent, and seals the result into the `ChunkCache` through a `Column` over a `PlanProvider` seeded with the
  extent (chunk key prefix `chunk/<src hex>/<layer>/maths/<name>/<version hex>`: a new version's column never
  joins or raises a chunk task of the superseded one). The status becomes Evaluating.
- Versions (03 §7.2) hash each input as the evaluation resolves it (channel name, then logged name) with its
  channel id, status, unit, dtype, interpolation and column identity/version, so an input matched by its logged
  name only, or one whose status turns to Error, re-versions its readers. When a node's version changes while
  its evaluation is in flight, that evaluation and its chunk tasks are cancelled (`cancelAll` on the old keys).
- The host posts each completion to the owning thread and calls `applyCompletion(key, version, result)`, or
  calls `applyReadyCompletions()` (the CLI after waiting): Ok/Warning (diagnostics from the handle; warnings
  give the summary "first message (+n more)"), Pending after Cancelled, Error otherwise with the error's
  `context.detail` as the code when it is one (`E-CSD-NO-SPEED`), else `E-EVAL`. A completion for an older
  version is ignored. Upstream failures propagate with "input '<name>': " and their code.
- `prefetch(Priority::Background)` is the D-016 / AC-M1-10 hook: it requires every enabled output and returns
  `MathRequest{key, version, future}` for the host to post. `TaskPool::cancelAll(MathGraph::taskKeyPrefix(src))`
  stops a closing source's evaluations.
- `PlanProvider` slices chunks from the evaluated extent (NaN and a cleared valid bit at invalid samples, no
  bitmap for a chunk without one), releases the extent once every chunk has been served, and evaluates the
  whole extent again through `PlanSnapshot::evaluate` if a chunk is asked for later (buildSummary, M5 eviction).
  A cancelled evaluation is retried; any other failure is memoised.

Settings schema: `schemas/builtin-esses.corrected_speed_distance-1.schema.json` (`$id`
`urn:esses:builtin-esses.corrected_speed_distance-1`), every member optional (absent = module default), with
valid/invalid examples under `schemas/examples/builtin-esses.corrected_speed_distance-1/`. `apps/cli` copies
`schemas/*.schema.json` next to the executables (`<exe dir>/data/schemas`); `findSchemasDir(dataDir)` tries
`<dataDir>/schemas`, then `<exe dir>/data/schemas`, then the source tree (`ESSES_MATHS_SOURCE_DIR`). The host
calls `loadSchema(findSchemasDir(paths.dataDir()))` once; `expand()` then validates settings first
(`E-CSD-SETTINGS`, `context.detail` = JSON pointer) and only then parses them with
`analysis::CorrectedSpeedSettings::fromJson`.

## Tests (`maths.*`, Catch2)

- `plan_provider_tests.cpp`: chunks sealed into a `ChunkCache` with validity and NaN, F32/F64/I32, extent
  release and bit-identical recomputation, cancelled vs memoised failures; `readWholeSignal`.
- `math_graph_tests.cpp` (with `FakeModule`, no analysis needed): definition and descriptors, require through
  `PlanProvider`, version-keyed caching (no re-evaluation, shared columns), invalidation by settings, by a new or
  bumped input column, by an input appearing/disappearing and by module enable (never by a sidecar commit),
  stale/cancelled completions, warnings, error propagation and memoisation, the Background prefetch hook (tasks
  queued in the Background band, raised by a Visible require through the share key), `cancelAll` on the source
  prefix, rejected output sets.
- `corrected_speed_distance_tests.cpp`: AC-M1-10 core part (defaults equal the shipped example and validate;
  every schema example through `validateSettings`; invalid settings rejected before any node exists), schema
  loading, `expand` wiring and a four-wheel fixture through `MathGraph` + `PlanProvider` (Corr Speed =
  fastestWheel, Corr Dist = trapezoid integral of stored Corr Speed within 1e-9). The last two skip with the
  stub's message while `analysis::CorrectedSpeed` is an M1 stub.

AC-M1-12 runs this suite under TSan (CI `sanitize`, Linux/Clang); the GUI-side prefetch test is
`tests/ui/corr_dist_prefetch` (esses_qtmodel).

## Deviations from the design text

- Default settings (M1 integration; D-154, proposed in `DECISIONS.md` A.12, with 03 §9.1 and 07 §4.1 updated):
  `alternateList.channels` is 03 §9.1's list with `"Speed"` appended (11 entries). The sim-exporter dialect (`synthetic-acc-dialect`, ACC-style exports)
  logs its road speed as `SPEED`, which no other entry names; without it the 07 §4.1 M1 data path (ACC `.ld`
  -> Corr Speed/Corr Dist -> distance mode, AC-M1-01) shows "no distance data". Lookups are case-insensitive and
  a channel whose unit is not a speed is skipped, so the entry is inert for other files; the importer
  `Dictionary` (M2) or project Aliases can supersede it.
- JSON values are `model::OpaqueJson` where 03 writes `JsonDoc` (`esses_base`'s `JsonDoc` is still reserved).
- `IBuiltinModule::expand(settings, UnitRegistry, Diagnostics)` instead of `(settings, Scope, Diagnostics)`; the
  maths `Scope` arrives with the parser (M2).
- `BuiltinOutput` carries `upstream` names, `inputs` (the logged channels it may read, which the node version
  hashes in place of a compiled `Plan`'s inputs) and a whole-extent `evaluate(ctx, upstream, diags)` instead of
  `variant<expression, unique_ptr<Kernel>>` (Kernel classes M2; 03 §6.0 whole-node path).
- `EvalContext::getChannel` (a `MathInputGatherer` factory) is omitted until `MathInputGatherer` exists (M2).
- `MathGraph` takes a `TaskPool*` and a `ChunkCache*` (null: the installed ones at require time) and adds
  `applyCompletion`/`applyReadyCompletions` (the owning-thread status write of 03 §7.3 / 01 §5.3 rule 10),
  `prefetch`, `descriptor`, `find` and `taskKey`; `setSource` and `setModuleEnabled` return the re-versioned keys.
  `define(MathModule, MathItem)`, `remove`, `setEnabled`, `setModules`, `setSelection` and `trace` arrive in M2.
- `SampleRange` (half-open, `all()`) and `ColumnHandle{key, version, descriptor, series, diags}` are defined
  here; 03 names them without declaring them. `require`'s range is accepted but every 0.1 evaluation covers the
  whole extent; `MathStatus::progress` is 0 while Evaluating and 1 when done (no per-chunk progress in 0.1).
- The output geometry (rate, t0, size) is the evaluated signal's, not computed beforehand from 03 §6.4.
- A failure other than Cancelled is memoised per node version (03 §7.3 Error until something changes).
- Output names may not repeat across modules (`E-DUPLICATE-OUTPUT`) until the shadowing rules of 03 §4.3 (M2).
- `tools/check_schemas.py` accepts `builtin-<module id>-<version>.schema.json` (the id may contain `.` and `_`)
  as a settings fragment without the document `$schema`/`schemaVersion` envelope (03 §9, 02 §6.2
  `builtinMaths`).
