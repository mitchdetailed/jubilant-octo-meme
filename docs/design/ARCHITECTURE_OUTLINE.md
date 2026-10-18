# ARCHITECTURE_OUTLINE.md — unified architecture (contract for section writers)

Codename **Esses** (D-003). This outline is binding on the detailed sections that follow it: every section must use the target names, type names, interface names, JSON `type` strings and milestone ids written here. Where a detail is not fixed here, the section writer expands **parity-first** (`background/design-panel/parity-first.md`) and, for the data/render path, the corresponding interface in `DECISIONS.md`. Decision references are `D-nnn` in `DECISIONS.md`.

Conventions: C++20; namespace `esses`; CMake targets `esses_<lib>`; executables `esses` (GUI), `esses-cli` (Qt-free), `esses-render` (Qt Gui offscreen); C ABI prefix `esses_`; JSON schema ids `<schema>-<version>` (e.g. `sidecar-1`), written in `$schema` as `urn:esses:<schema>-<version>` until the project domain exists (D-072); time is `Micros` (int64 µs); rates are `Rate{num,den}`; canonical data-layer name is `"Normal"`; components are named by their JSON `type` string.

---

## 1. Module map and dependency rules

### 1.1 Targets

| # | Target | Kind | Responsibility | Depends on | Qt |
|---|---|---|---|---|---|
| 0 | `esses_base` | static lib | `Result<T,Error>`, `Micros`, `Rate`, `Uuid`, `Hash128` (xxh3), UTF-8 and case-fold helpers, `CancelToken`, `Future<T>`, `TaskPool` (priority thread pool), `Executor` interface (test-only inline executor; `TaskPool` is the only production implementation, D-049), `ThreadRole` (no-block flag of the GUI thread), `MappedFile` (share-read/delete mapping with change detection, 02 §1), `ByteReader` (LE/BE), `ILogSink`, `JsonDoc` (nlohmann) | STL, nlohmann-json, xxhash | free |
| 0 | `esses_units` | static lib | `UnitRegistry`, `Quantity`, `Unit`, `Dimension`, `Converter`; loads `data/units.json` (own table + i2 symbol aliases) | base | free |
| 1 | `esses_store` | static lib | `Chunk`, `Validity`, `MinMaxPyramid`, `SummaryPyramid`, `ChunkCache`, `ChunkHandle`, `IChunkProvider`, `Column`, `UniformSeries`, `TimestampedSeries`, `Series`, `Resampler`, `Interp`, `ResampleView` (02 §3.3) | base | free |
| 1 | `esses_model` | static lib | `DataSource`, `DataLayer`, `Channel`, `ChannelDescriptor`, `ChannelId`, `Marker`, `MarkerEdit`, `Range`, `RangeGroup`, `RangeRef`, `Detail`, `SidecarDocument`, `ChannelResolver`, `ResolutionTrace`, `EnumTable` | base, units, store | free |
| 2 | `esses_formats` | static lib | `IImporter`, `IReader`, `IWriter`, `ImporterRegistry`, `SniffResult`, `ProbeResult` (↔ `probe_json`), `ImportOptions`, `ImportLog`, `ImportLogEntry`, `DecodeLog`, `ByteSource` (contracts declared in 05 §1.1–1.3); readers `motec.ld` (+`.ldx` companion), `motec.csv`, `text.profile:*` via `TextEngine` + `TextProfile`; writers `.ld`+`.ldx` (fixtures from M0, export UI v1.0), MoTeC CSV, generic CSV; `Dictionary`, `UnitMappingProfile`; C-ABI host + adapters (v1.0) | base, units, store, model, pugixml, zlib, fast_float | free |
| 2 | `esses_analysis` | static lib | `IRangeRule`, `BeaconPairRule`, `WholeOutingRule`, `TrackSectionsRule`, `RuleRunner`; `DistanceIndex`, `LapStretch`; `CorrectedSpeed`, `GpsCorrections`, `DamperVelocity` algorithms; `TrackMap`, `TrackGenerator` (Speed+LatG, GPS), `SectionDetector`; `VarianceTable`; `ReportEngine`, `ReportSpec`, `StatCache`; `HistogramBinner`, `ScatterBinner`, `TrendFit`, `Spectrum` (pocketfft) | base, units, store, model, Eigen, pocketfft | free |
| 2 | `esses_maths` | static lib | `Lexer`, `Parser`, `Ast`, `TypeChecker`, `Plan`, `Kernel` classes, `PlanProvider`, `MathInputGatherer` (per-input wrapper over `esses_store`'s `ResampleView`, 03 §6.3), `MathGraph`, `MathsScheduler`, `MathModule`, `MathItem`, `EvalContext`, `IMathFunction`, built-in modules (`esses.corrected_speed_distance`, `esses.gps_corrections`, `esses.damper_velocity`), `MathsCompat` switches, C-ABI function host (v1.0) | base, units, store, model, analysis, Iir1 | free |
| 2 | `esses_project` | static lib | Qt-free documents and schemas: `ProjectFile`, `WorkbookFile`, `WorksheetDef`, `ComponentSpec`, `ChannelOverrides`, `Aliases`, `Mappings`, `Scheme`, `Keymap`, `Profile`, `TrackMapFile`, `SchemaRegistry`, `Migrations`, `Journal` (write-ahead autosave), `Archive` (v1.0); validates `maths-1` documents against `SchemaRegistry` and loads them into `esses_maths::MathModule` through `esses_maths::MathModuleIo` (`MathsService` receives the loaded modules); hosts every v1.0 i2 configuration-file importer (`CfgWorkbookImporter`, `CfgSchemeImporter`, `CfgMathsImporter`) under `libs/project/src/cfgimport/` (D-122) | base, units, model, analysis, maths (same-layer edge, D-122), json-schema-validator; pugixml (private, v1.0 `cfgimport/`) | free |
| 3 | `esses_plotcore` | static lib | `ViewState`, `ChangeMask`, `SelectionSnapshot`, `DomainMapper`, `DistanceChain`, `AutoscaleCache`, `TraceBuilder`, `M4Column`, `AxisLayout`, `BandLayout`, `KeyLayout`, `HitTester`, `DrawList`, `DrawCmd`, `TextRun`, `Bitmap`, `LayerKind`, `IRenderBackend`, `ITextMetrics`, geometry builders (`ScatterGeometry`, `HistogramGeometry`, `TrackGeometry`, `OutingGeometry`) | base, store, model, analysis, project | free |
| 3 | `esses_cli` | executable `esses-cli` | `info`, `channels`, `laps`, `eval`, `convert`, `export`, `validate`, `fixture add/verify/diff`, `bench`; all `--json` | all Qt-free libs, CLI11 | free |
| 3 | `esses_capi` (v1.1) | static/shared | stable `extern "C"` surface over formats + model + maths for bindings | all Qt-free libs | free |
| 3 | `esses_script` (v1.1) | static lib | Lua 5.4 + sol2 sandbox, object model, scripted `IMathFunction`/`IRangeRule`/`IImporter` | all Qt-free libs, lua, sol2 | free |
| 4 | `esses_qtmodel` | static lib | `DataSelectionModel`, `WorkspaceViewModel` (shared zoom, cursor, datum; D-133), `WorksheetViewModel`, `RenderService` (keyed build submission, 04 §6.3), `AppRuntime` (owns pool, cache, dispatcher, sinks), `ProjectSession` (+`QUndoStack` commands), `SourceLoader`, `MathsService`, `Dispatcher`, `CommandRegistry`, `KeymapBinder`, `SettingsStore`, `MessagesModel` | all Qt-free libs, Qt::Core, Qt::Gui undo framework (D-161) | Core (+ Gui undo, D-161) |
| 4 | `esses_qtrender` | static lib | `RasterBackend : IRenderBackend` (QPainter → `QImage` layers), `LayerCache` (Static `QImage` + directly painted Dynamic/Chrome lists, D-137), `OffscreenRenderer` (goldens, print, clipboard), `SchemeFonts`, `QtTextMetrics`; later `GlBackend` | plotcore, Qt::Gui | Gui |
| 4 | `esses_render` | executable `esses-render` | `esses-render <workbook.json> <log…> --worksheet N -o out.png [--dpr 2]` | qtmodel, qtrender | Gui (offscreen) |
| 5 | `esses_widgets` | static lib | `DockHost`, `WorksheetCanvas`, `ComponentWidget` base and all components, docks, dialogs/editors, `PropertiesDialog`, `MappingPanel`, `ZoomSummaryBar` | qtmodel, qtrender, Qt::Widgets, ADS | Widgets |
| 5 | `esses_app` | executable `esses` | `MainWindow`, menus/toolbars, keymap binding, session restore, file associations, `-w <project> -l <log>` | widgets | Widgets |
| 5 | `esses_video` (later) | static lib | `IVideoPlayer` over Qt Multimedia, PTS/log-time offset model | qtmodel, Qt::Multimedia | Multimedia |
| — | `plugins/formats/*`, `plugins/mathfn/*` (v1.0+) | shared libs | C-ABI plugins; may privately link any non-Qt library; Qt only under the 05 §10.3 `qt` modes (`none \| static \| infix \| dynamic`), disabled (never unloaded) after `ESSES_E_INTERNAL` (D-131) | `sdk/esses_abi.h` only | n/a |

### 1.2 Dependency rules (enforced by `target_link_libraries` scoping and `cmake/qt-free-check.cmake`)

```
base ← units ← store ← model ← { formats, analysis } ← maths ← project ← plotcore ← cli
                                   (formats never depends on maths or analysis; analysis never depends on maths)
qtmodel  ← all Qt-free libs + Qt::Core (+ Qt::Gui for QUndoStack/QUndoCommand only, D-161)
qtrender ← plotcore + Qt::Gui                (no dependency on qtmodel)
widgets  ← qtmodel + qtrender + Qt::Widgets + ADS
app      ← widgets
```

- The chain above is a **layer ordering**, not a list of link edges. Link edges are the "Depends on" column of §1.1: `esses_store` links `esses_base` only; `esses_project` links `esses_maths` as a same-layer edge because it loads `maths-1` documents and hosts the v1.0 `CfgMathsImporter` (D-122, amending D-048), and `esses_maths` never links `esses_project`; `esses_plotcore` does not link `esses_maths` (maths outputs reach the plot as `Column`s through the shared `ChunkCache`), D-048.
- Targets 0–3 contain no `#include <Q…>` and link no `Qt6::` target; `ESSES_BUILD_QT=OFF` configures and builds them, `esses-cli` and their tests with no Qt installed (D-007).
- Strings are UTF-8 `std::string` below layer 4; `QString` appears only in layers 4–5.
- No layer calls upward; layer 4 observes layer ≤ 3 objects by polling versions or through `Dispatcher`-posted completions, never through callbacks registered by lower layers into Qt objects.
- Persisted documents are JSON with `$schema` + `schemaVersion`, validated by `SchemaRegistry` on load and by `esses-cli validate` (D-019).

---

## 2. Data flow

### 2.1 File → data source (open)

1. **Sniff.** `ImporterRegistry::sniff(path)` reads the first 64 KiB, calls every `IImporter::sniff`, ranks by confidence, collects companions (`.ldx` beside `.ld`, video beside both). User override is remembered per `(extension, preamble fingerprint)` in `SettingsStore`.
2. **Probe** (worker, `Priority::Interactive`). `IImporter::probe(ByteSource&)` returns `ProbeResult` (details, layers, channel descriptors with rate/kind/counts/vendor unit strings, markers as found, enum tables, warnings) without decoding bulk samples (contract: ≤ 200 ms on a 500 MB file). `ProbeResult` serialises to `probe_json` (`probe-1`), used by the Open dialog's Details pane, by fixtures and later by the catalogue.
3. **Open.** `IImporter::open(std::shared_ptr<const ByteSource>, ImportOptions)` returns an `IReader` that holds the source; the `DataSource` id is the quick content hash (D-130); `SourceLoader` builds a `DataSource` whose `Channel`s hold `Column`s fed by an `ImporterChunkProvider` (or `LdChunkProvider` for `.ld`, which mmaps the file and decodes per chunk). Nothing beyond the header is read yet.
4. **Resolve.** Importer `Dictionary` → `UnitMappingProfile` (vendor name and unit strings → quantity, unit) → `ChannelResolver` (project `Mappings` → `Aliases` → case-fold), in 05 §2 step 4's order, so unit maps match the vendor's names before project mappings rename them; unknown units/names end task A with a `PendingResolution`; the GUI shows the `MappingPanel` and submits task B (steps 5–7) with the decision, so no worker waits for the user (01 §4.1, D-123); every step is a `ResolutionTrace` row.
5. **Merge edits.** `SidecarDocument` is read from `<file>.<ext>.sidecar.json`; the `.ldx` companion's markers/details are parsed tolerantly; merge precedence sidecar > companion > file; `MarkerEdit` ops are applied to logged/companion markers (D-015).
6. **Ranges.** `RuleRunner` executes the profile's `IRangeRule`s (`BeaconPairRule`, `WholeOutingRule`, `TrackSectionsRule` once a track map exists) → `RangeGroup` tree (`Outings`, `Outings:Laps`, `Outings:Laps:Track Sections:Default`, …) with trust flags from the rule and overrides from the sidecar.
7. **Built-in maths.** `MathGraph` for the source registers global modules, local (sidecar) maths and the built-in `esses.corrected_speed_distance` module; `MathsScheduler` prefetches `Corr Speed`/`Corr Dist` at `Priority::Background`. After publish, one `summary/<sourceId>` task builds every logged channel's `SummaryPyramid` through `Column::buildSummary` without inserting chunks into `ChunkCache` (02 §3.2 rule 10).
8. **Publish.** `Dispatcher::post` hands the finished `std::shared_ptr<const DataSource>` to the GUI; `DataSelectionModel` adds it to the Data window; Messages dock shows `ImportLog` warnings; auto-generation of the circuit track map runs if no `trackmaps/<Venue>.track.json` exists and the required channels exist.

### 2.2 Selection and view → pixels (draw)

1. The user selects ranges in the Data window (ENTER / CTRL+ENTER / ALT+ENTER) → `DataSelectionModel` emits a new `SelectionSnapshot`; `WorksheetViewModel` updates `ViewState` and emits `changed(ChangeMask::Selection)`.
2. Each `ComponentWidget` maps the mask to passes (D-018). For `time_distance`: a Static rebuild is requested.
3. **Trace build** (worker, `Priority::Visible`, one task per component, parallel per trace): for each `(trace, range)`: `DomainMapper` (time: affine on `Rate`; distance: `DistanceIndex` for that lap, plus the range's alignment offset and stretch) maps the visible `XSpan` to a sample span of the trace's `Column`; `TraceBuilder` pins the chunks (`ChunkHandle`), computes exact `M4Column`s per device pixel (D-012) applying the display-unit `Converter` while writing vertices, splits at validity gaps, and appends polylines to the Static `DrawList`; `AxisLayout`, `BandLayout` (range/section/marker bands) and `KeyLayout` add ticks, bands and text runs; `VarianceTable` adds the variance panel polylines when F3 is on.
4. **Paint** (GUI thread): `RasterBackend` renders the Static `DrawList` into the component's `QImage` layer (device pixels, DPR applied once); the Dynamic layer (cursor, datum, measurement hover line, key values via `Column::tryPin` and `Resampler::valueAt`) is a `DrawList` rebuilt on `Cursor|Datum` masks only and painted directly; Chrome likewise on focus/caption changes; `LayerCache::paint` blits the Static image and paints both lists in `paintEvent` (04 §6.2, D-137).
5. **Values/labels**: key column values, the Values window and gauges read `Resampler::valueAt(series, cursorT, interp)` for Main and `Resampler::valueAt` through each overlay's mapper; pending maths rows show "…" and queue at `Background`.
6. Summary components (`scatter`, `histogram`, later `spectrum`) recompute their geometry on `Zoom|Selection|Data` masks on a worker (`ScatterBinner`, `HistogramBinner`, `TrendFit`) and paint on completion.

### 2.3 Maths request → result

`MathsService::require(ChannelKey, SampleRange, Priority)` → `MathGraph::require` resolves the node (shadowing order D-022), computes the version, hits the `ChunkCache` or schedules a `PlanProvider` materialisation on the `TaskPool` (chunk-independent plans in parallel per chunk with halos; whole-extent plans on one worker); the result chunks are pyramided and cached like logged data; `Dispatcher` posts `ChannelReady(key, version)`; components holding Pending traces rebuild. Editing an item bumps its definition hash → dependents become Pending → only the edited cone recomputes.

### 2.4 Reports

`channel_report` and `time_report` build a `ReportSpec` from their `ComponentSpec` and the current selection scope; `ReportEngine` computes cells on workers through `StatCache` keyed `(channelId, version, rangeId, stat)` (5 s inward probe for start/end; untrusted excluded unless included; Δ vs Overlay 0 via `VarianceTable`/paired stats); `QTableView` models update incrementally; export CSV/TSV with quoting.

---

## 3. Threading and ownership

- **GUI thread** owns: all `QObject`s (`esses_qtmodel`, widgets), `QImage` layers, `ProjectSession` and its `QUndoStack`, every model mutation (selection, view state, sidecar/marker edits, project documents). Lap rebuilds after a marker edit run synchronously here for phase 1 (markers and details, O(markers)); phase 2 (stint/pit annotations, track sections) is a worker task `ranges/<sourceId>/<layer>/<markerSetHash>` that publishes a new snapshot (02 §4.4 rule 13, D-123).
- **`TaskPool`** (`esses_base`; `std::thread` workers with an atomic stop flag, joined on destruction, × (hardware threads − 1, min 2), D-123; bands `Interactive > Visible > Background`; `CancelToken` checked per chunk/block; coalescing by request key; shared `Future<T>`) runs: chunk decode, pyramid and `SummaryPyramid` builds, `DistanceIndex` builds, `TraceBuilder`, maths `PlanProvider`, text parsing, report cells, summary-component geometry, exports.
- **Snapshot rule**: workers receive only immutable inputs — `std::shared_ptr<const DataSource>` (a new `DataSource` version is published when the sidecar changes), `ChunkHandle`-pinned chunks (never evicted while pinned), copied `ViewState`/`SelectionSnapshot`, `ComponentSpec` by value.
- **Completion**: exactly one `Dispatcher` (`QObject` with a queued signal) posts closures to the GUI thread through `post(QObject* context, fn)`, skipping closures whose context is gone; `TaskPool`, `ChunkCache`, `Dispatcher` and sinks are owned by an explicit runtime (`AppRuntime`) with a fixed shutdown order; no pool thread waits for a task that is not running (Rule W; 01 §5, D-123); no worker touches a `QObject`; no model object registers callbacks that could fire on a worker.
- **Never block**: the GUI never waits on a `Future`; components render Pending hatching/"…" and repaint on completion; the cursor pass never waits for a trace build.
- **Priorities in practice**: cursor-driven trace rebuilds and Values rows at `Interactive`/`Visible`; `SummaryPyramid`, `Corr Dist` prefetch, Values-window scrolling, report cells at `Background`.
- **Memory**: `ChunkCache` budget 25 % of RAM (min 1 GB); M0–M4 (release 0.1) track the budget and warn, including a warning at open from the probe estimate; M5 adds LRU eviction of unpinned chunks and `OutOfBudget` when pinned bytes alone exceed the budget (D-011, D-043, D-053).
- **CLI**: same `TaskPool`, synchronous waits allowed.

---

## 4. Key interfaces (name → one-line contract)

### `esses_base`
- `Micros` — int64 microseconds since the data layer origin; `UtcInstant` optional anchor per layer.
- `Rate{num,den}` — rational Hz; `timeAt(i)`/`indexAt(t)` exact in integer arithmetic; `hz()` for display.
- `Result<T,Error>` — value-or-error with a stable `Error{code, message, context}`; no exceptions across library boundaries.
- `TaskPool` — `submit(Priority, key, CancelToken, fn) → Future<T>`; coalesces equal keys; `cancelAll(key prefix)`.
- `Executor` — abstract submit used by maths/plot so tests can run inline.
- `CancelToken` — cooperative cancellation checked at chunk/block granularity.
- `MappedFile` — read-only mmap/MapViewOfFile with `std::span<const std::byte>`; `FILE_SHARE_READ | FILE_SHARE_DELETE`, `changedOnDisk()`, `guarded()` decode (02 §1).
- `ThreadRole` — `thread_local` no-block flag set only on the GUI thread; blocking calls assert `!ThreadRole::isNoBlock()` (01 §5.3 rule 4).
- `Hash128` — xxh3-128; `SourceId` is the quick content hash (size, first/last MiB, format header), the full-content hash is a Background provenance field (D-130).

### `esses_units`
- `UnitRegistry::find(symbol_or_alias) → optional<UnitId>` — case-insensitive, vendor-normalised (`kph`, `KPH`, `km/h`).
- `UnitRegistry::converter(from,to) → Converter{a,b}` — affine `y = a·x + b`; errors when quantities (families) differ; delta vs absolute enforced.
- `Quantity{id, name, dim[7], family, base}`; `Unit{id, symbol, display, qty, scalar, offset}`.

### `esses_store`
- `Chunk` — 65,536-sample immutable block: samples | optional validity bitmap | factor-4 `MinMaxPyramid`; `version`; `samples<T>()`, `level(k)`, `validity()`.
- `Validity{AllValid, Bitmap, AllInvalid}` — per-chunk mode; `validAt(i)`.
- `IChunkProvider` — `size()`, `dtype()`, `materialise(chunkNo, Chunk&, CancelToken)`, `chunkIndependent()`; implemented by `LdChunkProvider`, `ImporterChunkProvider`, `PlanProvider`, later `DcfProvider`, `LiveProvider`.
- `Column` — `tryPin(no, prio) → optional<ChunkHandle>` and `prefetch(no, prio)` non-blocking (any thread), `require(first,last,prio,cancel) → ChunkHandle` (workers; materialises inline, Rule W), `at(i)` (= tryPin/read/unpin; NaN if absent/invalid), `summary() → SummaryPyramid&`, `buildSummary(cancel)`, `version()` (02 §3.2).
- `ChunkHandle` — move-only RAII pin over a chunk range; a pinned chunk is never evicted; chunk memory is freed only under the cache lock at pin count zero.
- `ChunkCache` — process-wide; `setBudgetBytes`, `allocate`, `touch`, `stats()`; LRU eviction of unpinned chunks (M5, D-043).
- `SummaryPyramid` — levels with bucket ≥ 256 for a whole column; always resident; `minmax(span)`.
- `UniformSeries{Rate rate; Micros t0; Column values}`; `TimestampedSeries{Column timesUs; Column values}`; `Series = variant<…>`.
- `Resampler` — `valueAt(series, t, interp)`, `block(series, Rate out, first, n, interp) → ResampleView`; never called by importers.

### `esses_model`
- `ChannelDescriptor` — identity (`ChannelId`, `name`, `loggedName`, `shortName`), unit/quantity, dtype/kind, interp, dps, colourIndex, enums, source, statusText (D-013).
- `Channel{desc, Series series, Status status}` — status `Ok | Pending | Error`.
- `DataLayer{name, channels, byNameCI, markers, ranges}`; `DataSource{id, provenance, details, layers, sidecar, version}`.
- `Marker` / `MarkerEdit{ref, op: offset|ignore|restore, dtUs}` / `Range` / `RangeGroup{path, ranges, children, version}` / `RangeRef` — per D-014; `RangeGroupTree::rangeVersion(path)` is kept per group path (D-140, 02 §4.4).
- `Detail{id, value: string|double+unit|datetime, prov, original}` — numeric details are maths constants, text details compare.
- `SidecarDocument` — load/merge/save of `sidecar-1`; `apply(DataLayer&)`; atomic write.
- `ChannelResolver::resolve(name, DataLayer&) → {Channel*, ResolutionTrace}` — mappings → aliases → case-fold → logged name; traces feed Channel Status.

### `esses_formats`
- `IImporter` (const, thread-safe) — `id() → string_view`, `extensions() → span<const string>`, `sniff(const SniffInput&) → SniffResult{confidence, companions, reason}`, `probe(const ByteSource&) → ProbeResult`, `open(shared_ptr<const ByteSource>, const ImportOptions&) → unique_ptr<IReader>` (05 §1.3).
- `IReader` — `readBlock(layer, channel, first, count, Block&, CancelToken)`, `readMany(layer, channels, first, count, BlockSink&, Progress, CancelToken)`, `readTimestamps(...)`, `markers(layer)`, `enumTable(id)`, `warnings()`, and the defaulted `rawHeader()` and `sourceEncoding(layer, channel)` that carry a format's verbatim header and per-channel encoding to its writer (05 §1.3).
- `IWriter` — `write(path, ExportView, options) → Result`; `.ld`+`.ldx`, MoTeC CSV, generic CSV.
- `ImporterRegistry` — `sniff(path)` ranked results; `load(path, options, TaskPool&, Progress, CancelToken, MappingHook) → Future<LoadResult>` (task A; a `PendingResolution`, carrying the opaque `OpenState` skeleton, when the user must map units) and `resume(PendingResolution, MappingDecision, …) → Future<shared_ptr<const DataSource>>` (task B) (05 §2).
- `ProbeResult` ↔ `probe_json` (`probe-1`) — the same bytes serve the Open dialog, fixtures (`expected.probe.json`) and the v1.0 catalogue.
- `TextEngine` + `TextProfile` (`importer-profile-1`) — two-pass line index then column decode into chunks; profiles declare `sniff`, `preamble`, `header`, `time`, `sentinels`, `sparse`, `columns`, `lapSource`.
- `Dictionary` (`dictionary-1`) — vendor name/id → canonical name, unit, quantity, interp, status-bit expansion.
- `UnitMappingProfile` — per-importer rules `nameOnly | unitOnly | nameAndUnit → (quantity, unit, analysis name)`.
- `sdk/esses_abi.h` (v1.0) — `esses_host_v1`, `esses_importer_v1`, `esses_writer_v1`, `esses_mathfn_v1`, `esses_block_v1`, `esses_plugin_desc_v1`, `esses_plugin_entry_v1`; rules per D-028.

### `esses_analysis`
- `IRangeRule::run(const DataLayer&, const Profile&) → RangeGroupTree` — declarative JSON state machine (`rules/*.rule.json`); `BeaconPairRule` implements i2 beacon semantics plus stint/pit classification.
- `DistanceIndex` — per `(source, lap)`: `tAtD[]` at 0.25 m, `lengthM`, `stretch`; `timeAt(d)`, `distAt(t)`, `indexAt(d, rate)` O(1); keyed by `(Corr Dist column version, lap start/end marker ids and times, stretch settings)` (D-140).
- `LapStretch::apply(DistanceIndex&, trackLength, tolerance)` — per-lap scalar; report rows for the Lap Editor.
- `CorrectedSpeed::compute(settings, layer) → Series` and `CorrectedDistance::integrate(speed) → Series(F64)` — algorithms behind the built-in module.
- `TrackGenerator::fromSpeedLatG(lap, speed, latG) → TrackMap`; `TrackGenerator::fromGps(lap, lat, lon[, innerLap, outerLap]) → TrackMap`; `SectionDetector::defaults(TrackMap&)` (Turns/Straights, ≥ 10 m).
- `TrackMap` (`track-1`) — centreline `[x,y,cumDist]` ENU metres about `origin{lat,lon,alt}`, optional inner/outer, `length`, `direction`, `closed`, `rotation`, `sections[{name,startDist,endDist,category,userDefined}]`.
- `VarianceTable::build(mainIndex, overlayIndex) → Δt(d) / Δd(t)` — O(pixels) sampling for the variance panel and `variance_*` functions.
- `ReportEngine::cells(ReportSpec, selection, StatCache&) → Table` — scope, ordered headings, stats, 5 s probe, highlight, Δ vs Overlay 0, transposed; `TimeReportEngine` adds eclectic and rolling minimum.
- `HistogramBinner`, `ScatterBinner`, `TrendFit` (linear/poly ≤ 6, through zero, equation text), `Spectrum` (window functions, Bartlett averaging).

### `esses_maths`
- `Parser::parse(text) → Ast | Diagnostics` — Pratt parser, precedence per D-020, integer vs decimal literal nodes.
- `TypeChecker::check(Ast, Scope) → Type + warnings` — dimension tracking, never blocking (D-021).
- `compile(Ast, Scope) → Plan{kernels, out type, chunkIndependent, halo, inputs, defHash}`.
- `Kernel::run(BlockCtx&, inputs, out)` — 4,096-sample blocks; classes Elementwise / Windowed / IIR / Stateful / Range / Variance / Table.
- `PlanProvider : IChunkProvider` — evaluates a plan into cache chunks (parallel per chunk or whole-extent).
- `MathInputGatherer` — one per plan input; pins source chunks, gathers a `store::ResampleView` (`Resampler::block`, 02 §3.3) at the result rate and converts it into a block (03 §6.3).
- `MathGraph` — `define/remove/setEnabled/setModuleEnabled`, `version(key)`, `require(key, range, prio) → Future<ColumnHandle>`, `status(key)`, `consumersOf(key)`; i2 shadowing order; cycle errors.
- `MathModule` (`maths-1`) — `{id, enabled, condition?, constants[], enumerations[], items[]}`; `MathItem` kinds `expression | filter | scale_offset | table2d | table3d | builtin | plugin | regression`.
- `EvalContext` — selection (Main, reference overlay), `DistanceIndex`es, math window `[startUs, endUs)`, result rate; passed to variance/stat kernels and plugin functions.
- `IMathFunction{manifest(), evaluate(EvalContext&, args) → Value}` — one interface for C-ABI (v1.0) and Lua (v1.1) functions.
- `MathsCompat` — `unaryMinusAbovePower`, future switches; documented in `docs/maths-compat.md`.

### `esses_project`
- `ProjectFile` (`project-1`), `WorkbookFile` (`workbook-1`), `WorksheetDef`, `ComponentSpec{id, type, version, rect[l,t,r,b], z, caption{show,auto,text}, links{zoom,cursor,datum}, legend, activeChannel, body, xI2}` (header per 04 §7.3; JSON key of `xI2` is `x-i2`), `ChannelOverrides` (`overrides-1`), `Aliases`, `Mappings`, `Scheme` (`scheme-1`), `Keymap` (`keymap-1`), `Profile` (`profile-1`), `TrackMapFile`.
- `SchemaRegistry::validate(json, schemaId)`; `Migrations::upgrade(json) → json`.
- `Journal` — append-only autosave entries under `.journal/`; `replay()` on open; cleared by an atomic Save.
- `Archive` (v1.0) — workspace zip with `manifest.json{requiresChannels, requiresMaths, minVersion}`.

### `esses_plotcore`
- `ViewState{domain, zoom, cursorT, datumT, offsetAxis, version}` — composed per worksheet from the workspace-shared zoom/cursor/datum (`WorkspaceViewModel`) and the worksheet's own `domain`/`offsetAxis` (D-133); cursor/datum in Main-lap time.
- `ChangeMask{Cursor, Datum, Zoom, Domain, Selection, Offsets, Data, Scale, Size, Scheme}`.
- `SelectionSnapshot{main, overlay0, overlays, overlaysHidden, singleColourMain}`; `referenceOverlay()`.
- `DomainMapper` — `xOf(t)`, `timeAt(x)`, `sampleAt(x)` for `(range, domain, offset, stretch)`; time affine, distance via a `std::shared_ptr<const DistanceChain>` over the outing's per-lap `DistanceIndex`es (04 §3.1, D-134).
- `DistanceChain` — outing-level piecewise distance from per-lap `DistanceIndex` + stretch; `AutoscaleCache` — per-component autoscale over trusted laps and overlay sources (04 §8.3).
- `TraceBuilder::build(TraceSpec, DomainMapper, XSpan, widthPx, Converter) → polyline(s)` — exact M4 (D-012), gap splitting, O(width).
- `AxisLayout` (ticks in project time format or metres/km/miles), `BandLayout` (range/section/marker bands), `KeyLayout` (labels, value@main/value@datum/Δ, measurements row).
- `HitTester` — pixel → `{component, group, trace, band, marker, handle}` for clicks, hover, drag.
- `DrawList{widthPx, heightPx, dpr, cmds, verts, texts, text}` with `DrawCmd{kind: CmdKind{Clear|Scissor|Polyline|Points|Lines|Quads|FilledRect|TextRun}, vertexOffset, vertexCount, colour, widthPx, dash, pointSizePx, rect, textIndex}` and `TextRun{byteOffset, byteLen, x, y, style, anchor, quarterTurns, colour, maxWidthPx, background}`; `LayerKind{Static, Dynamic, Chrome}`; `Bitmap{w, h, rgba}` (04 §5.1, §5.3).
- `IRenderBackend{beginLayer(kind, widthPx, heightPx, dpr, clear), draw(DrawList), endLayer() → LayerHandle, compose(handles, target), readback(handle) → Bitmap, release(handle), textMetrics() → shared_ptr<const ITextMetrics>}` (04 §5.3).
- `ITextMetrics` — `advancePx`, `lineHeightPx`, `ascentPx`, `generation()`; every const member safe from any thread (04 §5.4).
- `ScatterGeometry`, `HistogramGeometry`, `TrackGeometry`, `OutingGeometry` — component geometry builders returning `DrawList`s.

### `esses_qtmodel`
- `DataSelectionModel` (QObject) — owns `SelectionSnapshot`; slots for ENTER/CTRL+ENTER/ALT+ENTER/F4/SHIFT+F4/Q/N/P/CTRL+F/CTRL+ALT+F/J; filters; `changed(SelectionSnapshot)`.
- `WorkspaceViewModel` (QObject) — owns the workspace-shared zoom (with `zoomDomain`, `zoomFrac`), cursor and datum (D-133).
- `RenderService` — keyed replace-key build submission over `TaskPool` with `Dispatcher` delivery; widgets never call `TaskPool` (04 §6.3).
- `WorksheetViewModel` (QObject) — owns its `domain`, `offsetAxis` and size, composes `ViewState`; `setDomain` (F9 rule D-016), zoom/pan/cursor/datum/offset ops, `zoomToMainRange` (W changes zoom and cursor, not selection, D-108); `changed(ChangeMask)`.
- `ProjectSession` (QObject) — open/save/autosave of a project folder; `QUndoStack`; documents; `SourceLoader` ownership; selection restore.
- `SourceLoader` — async open pipeline §2.1; `MappingPanel` hand-off; publishes `DataSource` snapshots; marks a source stale when `MappedFile::changedOnDisk()` reports a change (checked on application focus; providers check before each decode batch) and reopens it on `file.reload_source` (06 §6, §14.4; M4, 02 §8 AC-4a).
- `MathsService` — Qt facade over `MathGraph`; `require(...) → QFuture-like handle`; `channelReady(key)`; `invalidate(module)`; status model for Channel Status.
- `Dispatcher::post(QObject* context, fn)` — the only worker → GUI bridge; skips the closure when `context` is gone (01 §5.3 rule 3).
- `CommandRegistry` — command ids → actions; drives menus, `Keymap`, command palette, later scripting.
- `KeymapBinder` — binds `keymap.json` rows to `CommandRegistry` with i2 scope rules (worksheet focus vs dock focus; ESC returns focus).
- `SettingsStore` — per-user settings, MRU, importer overrides, dock perspectives.
- `MessagesModel` — Import Log / maths diagnostics / budget warnings.

### `esses_qtrender`
- `RasterBackend` — `IRenderBackend` on QPainter into `QImage` layers at device pixels; antialiased polylines; text via `QStaticText` with scheme fonts.
- `LayerCache` — per component: one Static `QImage` plus Dynamic/Chrome `DrawList`s painted directly under a damage region; invalidation by `ChangeMask`; `paint(QPainter&, exposed)`; hidden-worksheet images released after 30 s (D-058, D-137).
- `QtTextMetrics` — `ITextMetrics` over immutable font descriptions with per-thread `QFont`/`QFontMetricsF` caches (04 §5.4).
- `OffscreenRenderer::render(WorkbookFile, worksheet, sources, ViewState, sizePx, dpr) → QImage` — goldens, print, clipboard, `esses-render`.
- `GlBackend` (only if D-025 triggers) — same interface; granularity per benchmark.

### `esses_widgets` / `esses_app`
- `DockHost` — facade over ADS: `addDock`, `savePerspective/restorePerspective`, `setAutoHide`, `resetToDefault`.
- `WorksheetCanvas` — free-placement rectangles (0..1 fractions, 1 % snap, overlap/z-order, CTRL+click multi-select, maximise, splitters CTRL+drag), focus routing, worksheet hotkey scope.
- `ComponentWidget` — base: `ComponentSpec` binding, `links`, active channel, context menu, F5 properties, L/K/G/S/ALT+n, export, print to clipboard, `onViewChanged(ViewState, ChangeMask)`.
- Components (MVP): `TimeDistanceGraph` (`time_distance`), `OutingGraph` (`outing`), `ScatterPlot` (`scatter`), `Histogram` (`histogram`), `ChannelReport` (`channel_report`), `TimeReport` (`time_report`), `TrackPosition` (`track_position`), `ValuesList` (`values_list`), `NumericGauge` (`gauge_numeric`), `BarGauge` (`gauge_bar`).
- Docks: `DataWindow` (F8), `ChannelsWindow` (C), `ValuesWindow` (V), `TrackMapWindow`, `ChannelStatusWindow` (CTRL+U), `MessagesWindow`.
- Dialogs/editors: `PropertiesDialog` (F5; tabbed, modeless, Apply), `ChannelEditor` (CTRL+E), `MathsEditor` (CTRL+M), `LapEditor`, `DetailsEditor` (CTRL+D), `TrackEditor`, `CorrectedSpeedDialog`, `OptionsDialog`, `MappingPanel`, `LayoutEditor` (CTRL+F7), `ZoomSummaryBar`.
- `MainWindow` — menus mirroring i2's vocabulary, toolbar (open, save, time/distance, overlays, single-colour, lap stretch, animate, scheme selector, filter-all-sources), status bar (t, d, lap, Δt/Δd, maths queue), `-w/-l` args, file associations.

### Names added during documentation (D-073)

Registered here under the §10 rule; the defining document is in brackets.
- `esses_base`: `ErrorCode` mirrored 1:1 by the C ABI's `esses_status` (D-051), `TaskKey` prefixes with share/replace semantics (D-052) [01].
- `esses_store`: `MinMax` (`float` for every dtype, rounded directionally for wider dtypes, D-054), `ChunkBuilder`, `ColumnKey`, `BitProvider` (shared-column bitfield expansion), `Dtype::I64` for time columns only (D-062) [01, 02, 05].
- `esses_model`: `RangeGroup::version` and `RangeGroupTree::rangeVersion(path)` (per group path, D-140) [02]; `Marker::group`, `MarkerClass::DeviceEvent`, `MarkerClass::Other` (D-064), `ChannelStatus` (the enum of `Channel::status`, D-060), `ChannelDescriptor::sourceEncoding` with its opaque base `SourceEncoding`, `EnumEntry` (value, label, `severity`), `RangeOverride`, `Annotation`, `VideoLink`, `SidecarLayer`, `SidecarDocument::companionOpaque`, `Document<T>`, `ResolutionStep`, `Provenance_Source` (with `rawHeader`) [02, 05].
- `esses_formats`: `Block`, `BlockSink`, `MappingHook`, `OpenState` (the opaque state task A hands to task B: reader, probe, skeleton `DataSource`, `ResolutionTrace`s; 05 §2), `PendingResolution`, `MappingDecision`, `LoadResult`, `SniffInput`, `DecodeLog` (device events travel as `DeviceEvent` markers in `probe_json` `markers[]`; the `EventInfo`/`LayerInfo::events[]` proposal of D-064 is withdrawn, D-132), `PluginImporterAdapter`, `ExportedImporter`, `esses_sink_v1`, `ExportView`, `WriterOptions`, `WriteReport` [05].
- `esses_maths`: `MathState {Pending, Evaluating, Ok, Warning, Error, Disabled}` (D-060), `ExtentMode::Ordered` (D-077), `MathsCompat::functionAliases` (D-080), `Type::scaleToBase` and `Dimension::unknown()` (D-085) [03]; `MathSelection`, `MathModuleIo`, `MathInputGatherer` (per-input wrapper over `esses_store`'s `ResampleView`, 02 §3.3), `MathTaskKey` (the scheduler's struct task key; string form for logs only, 01 §5.4) [03]; `esses_project::CfgMathsImporter` (v1.0, D-122, replacing D-089's `I2MathsImporter`, whose name the vendor-mark lint rejects).
- `esses_plotcore`: `ITextMetrics` with the `FixedTextMetrics` test double (implemented by `esses_qtrender` as `QtTextMetrics`), `DistanceChain`, `AutoscaleCache`, `TextRun`, `Bitmap`, `CmdKind` [04].
- `esses_qtmodel`: `WorkspaceViewModel`, `RenderService`, `AppRuntime` [01, 04, 06]; stale-source handling in `SourceLoader` and the command `file.reload_source` (Reload File in the Data window and on the Messages row; M4, 02 §1 behaviour 5, §8 AC-4a) [02, 06]. `esses_base`: `ThreadRole` [01, 04].
- `esses_cli`: subcommands `maths check`, `maths functions`, `units scan`, `bench formats` (D-090) [03, 05].

### Names added by the M2 contracts (2026-10-12)

Registered under the §10 rule when the M2 headers were laid down (`docs/milestones/M2.md` §2); each library's
`README.md` lists them with their files and their departures from the design text.
- `esses_maths`: `TokenKind`, `Token`; `kBlockSamples`; `NodeId` (03 §4.3 rule 9); `Stage::Ref`, `PlanInput::unit`,
  `Plan::rangeGroups`, `evaluateWhole`; `intrinsicName`, `intrinsicClass`, `registerIntrinsics`,
  `makeIntrinsicKernel`; `FunctionRegistry::intrinsicId`, `external`, `builtin`; `MathsScheduler::submit`;
  `GpsCorrectionsModule` (`esses.gps_corrections`); `TableSpec::Axis`, `TableSpec::Interp`,
  `TableSpec::OutOfRange`; `MathModule::kLocalId`, `kTransientId`, `unknown`.
- `esses_formats`: `TextProfile::Sniff`, `Preamble`, `Header`, `Time`, `Column`, `LapSource`,
  `effectiveImporterId`, `contentHash`; `Dictionary::Match`, `Entry`, `Bit`, `Device`, `Bitfield`;
  `UnitMappingProfile::Rule`, `Resolution`, `resolve`, `fileStem`; `kLegacyCsvImporterId`, `kGenericCsvProfileId`,
  `kGenericCsvImporterId`, `makeLegacyCsvWriter`, `makeGenericCsvWriter`; `UnresolvedRow`, `ImporterRegistry::resume`
  and `addProfile` (05 §2 names them; the declarations are new).
- `esses_model`: `SidecarDocument::setAnnotation`, `removeAnnotation`, `setLocalMaths`, `restoreLogged`.
- `esses_analysis`: `LapStretchRow`, `LapStretch::factor`.
- `esses_project`: `ChannelOverrides::Entry`, `Mappings::Row`, `Mappings::toTable`, `Aliases::cycleThrough`,
  `JournalEntry`, `JournalReplay`.
- `esses_qtmodel`: `ChannelStatusModel` (06 §1), `MathsService::StatusRow`, `statusRows`, `check`, `functions`,
  `setModules`, `setCompat`, `setLocalModule`, `setTransientModule`; `SourceLoader::mappingRequired`,
  `resolveMapping`, `cancelMapping`, `setMappingHandOff`, `setChannelDocuments`, `registerTextProfiles`;
  `ProjectSession::undoStack` and its document setters and journal members; `UndoMergeId`, `DocumentCommand`,
  `SidecarCommand`, `WorkbookCommand`, `SetOverrideCommand`, `EditAliasesCommand`, `EditMappingsCommand`,
  `EditMathModulesCommand`, `SetProjectOptionsCommand`, `InsertMarkerCommand`, `RemoveMarkerCommand`,
  `MarkerEditCommand`, `RestoreLoggedLapsCommand`, `SetAnnotationCommand`, `RemoveAnnotationCommand`,
  `SetLocalMathsCommand` (with 06's `SetRangeTrustCommand`, `OffsetRangeCommand`, `ClearOverrideCommand`,
  `SetDetailCommand`, `MoveWorksheetCommand`).
- `esses_widgets`: `SchemeSelector` (07 §5.1 "scheme selector").

### Names added by the M2 persistence work (2026-10-12)

- `esses_analysis`: `LapStretchSettings`, `LapStretchRow::withinTolerance` and `candidate`, `LapStretch::row` and
  `report`, `DistanceIndex::setStretch` (D-016, D-140).
- `esses_project`: `SchemaRef::schemaVersion`, `Migrations::latest`, `JournalEntry::of`, `JournalFile` (the
  `journal-1` document), `JournalRecovery`, `Journal::recover` (the recovery hook); `ProjectFile::kSchemaVersion`,
  `ProjectFile::dirty`, `ProjectFile::autosaveMinutes` and `lapStretchSettings`, and `schemaVersion` on every
  document type (D-162, D-163, R-38).
- `esses-cli`: `validate --all`, `--root`, `--exclude` (08 §5).

### Names added by the M2 maths language work (2026-10-12)

- `esses_maths`: `TypeChecker::Options` (`requester`, `resultUnit`) and the overload of `TypeChecker::check` that
  takes it; `ScopeEntry::interp`, `dtype`, `number`, `text`, `diags`; `Scope::dataBacked` (03 §3.2-§3.3, D-164,
  D-165).
- `esses-cli`: `eval`, `maths check`, `maths functions` implemented as 03 §13 names them (D-166).

### Names added by the M2 CSV import work (2026-10-12)

- `esses_formats`: `kLegacyCsvFormatName`, `kLegacyCsvWriterId`, `kGenericCsvWriterId` (writer ids `motec.csv` and
  `csv`); `IReader::columnHint`, `dictionary`, `makeTimesProvider`, `timeRange`, `attachColumns` (pipeline step 4a,
  timestamped channels, the shared slab sweep); `OpenContext::mappings`, `unitMaps`; `open_code::kUnitMapLoadFailed`
  (`open.unitmap_load_failed`) (05 §2, §7, §8; D-167, D-168).
- `esses_store`: `WeakColumn` (a non-owning Column reference for `ChunkCache::insertGroup`, D-167).
- Tools and data: `tools/gen_text_fixtures.py`, `bench/formats_bench` (`esses-formats-bench`), the fuzz targets
  `fuzz_csv_sniff` and `fuzz_text_profile`; fixture-1 oracle kind `generator` (D-168).

### Names added by the M2 qtmodel work (2026-10-12)

- `esses_qtmodel`: `LiveDiagnostics` (the Maths Editor's debounced live check, 03 §11 item 2), `LapStretchService`
  (the lap-stretch toggle and the per-lap factors, D-016, D-174), `SessionRestore` and `connectProjectSession`
  (06 §18 steps 5-6), `connectMathsSelection`; `WorkbookSet`, `ProjectSession::workbookSet`, `setWorkbookSet`,
  `workbookNames`, `setUnits`, `units`, `mappingTable`, `aliasTable`, `mathsCompat`, `lapStretchSettings`,
  `looseChannelSearch`, `setDataWindow`, `setLapStretchEnabled`, `journalReplay`, `setRecoveryPrompt`,
  `setAutosaveInterval`, `autosaveInterval`, `documentTexts`, `lapStretchChanged`, `dirtyChanged`;
  `WorkbookSetCommand`, `AddWorkbookCommand`, `RemoveWorkbookCommand`, `RenameWorkbookCommand`,
  `MoveWorkbookCommand`, `AddWorksheetCommand`, `RemoveWorksheetCommand`, `RenameWorksheetCommand`,
  `AddComponentsCommand`, `RemoveComponentsCommand`, `componentsMime`, `componentsFromMime`,
  `kComponentsMimeType`, `SetUnitMapCommand`, `applyMappingDecision` (D-172); `MathsService::setResolverTables`,
  `setSelectionDistances`, `mathSelection`, `modules`, `compat`, `sources`, `gpsCorrectionsModule` (D-173);
  `ChannelStatusModel::ModuleRole`, `refreshLater`, `rows`; `SourceLoader::pendingMapping`, `mappingTable`,
  `aliasTable`, `setRangeStretch`; `DataSelectionModel::lapNumbersError`, `defaultFilter`, `session`,
  `filterRejected`, `filterKindName`, `parseFilterKind`; `ChannelsModel::searchWords` and the `loose` argument of
  `filterAccepts` (D-175); `registerEditCommands`, `registerLapStretchCommand`.

### Names added by the M2 options and shell work (2026-10-12)

- `esses_widgets`: `RecoveryDialog` (the 06 §18 step 3 prompt, AC-M2-06); `OptionsDialog::reload`,
  `hasPendingEdits`, `errorText`, `schemeSelector`, `setRevealHandler`, `RevealHandler`, `importKeymap`,
  `exportKeymap`, `revealKeymap`, `applied`; `SchemeSelector::scheme`, `restoreActive`, `followsSystem`,
  `setFollowSystem`, `applySystemScheme`, `userSchemeDir`, `duplicateActive`, `importScheme`, `exportActive`,
  `palette`, `logMessage`, `kToolbarKey`, `kFollowOsKey`, `kActiveKey`; `MainWindow::ExitAnswer`, `setExitPrompt`,
  `confirmLeaveProject`, `lapStretch`, `schemeSelector`, `optionsDialog`, `recoveryDialog`, `sessionRestore` and
  the `logs` argument of `openProject`; `LayoutFile::dialogGeometry`, `setDialogGeometry` (D-178).

### Names added by the M2 maths editors work (2026-10-12)

- `esses_widgets`: `ActiveChannelCommands` (with `Host`, `FilterChoice`, `ScaleOffsetChoice`, `FilterPrompt`,
  `ScaleOffsetPrompt`; the handlers of the 06 §14.4 "Active channel" rows), `AliasesDialog` and `MappingsDialog`
  (06 §13.1 item 7); `MathsEditor::ModuleRef`, `kItemsMimeType`, `setMainSource`, `mainSource`, `currentModule`,
  `currentItemName`, `revert`, `isPageDirty`, `refusalText`, the module and item operations (`addModule`,
  `removeModule`, `moveModule`, `setModuleEnabled`, `setModuleCondition`, `importModule`, `exportModule`, `addItem`,
  `addConstant`, `removeSelected`, `copySelected`, `cutSelected`, `paste`, `setSelectedEnabled`, `recalculate`), the
  assistance members (`unitPopupVisible`, `unitPopupEntries`, `unitsAt`, `acceptUnitPopup`, `rangePaths`,
  `insertRange`, `channelNames`, `insertChannel`, `hoverText`), the pane accessors and `message`;
  `ChannelStatusWindow::errorsOnlyBox`, `detailPane`, `filterModel`, `selectRow`, `currentSourceRow`, `copyTable`,
  `openRow`; `ChannelEditor::Column`, `filterEdit`, `channelModel`, `currentChannel`, `setDisplayUnit`, `setLimit`,
  `setDps`, `setColourIndex`, `setInterp`, `clearOverride`, `lastRefusal`, `aliasesDialog`, `mappingsDialog`,
  `editRefused` (D-177).

### Names added by the rest of the M2 review (2026-10-13, D-190)

- `esses_formats`: `ImportOptions::cancel` (the open's token for `IImporter::open`).
- `esses_project`: `JournalReplay::applied`, `JournalReplay::skipped`, `Journal::ownFileExists`; the `backupError`
  argument of `saveWithBackup`.
- `esses_qtmodel`: `ProjectSession::saveMathsModules` (the Maths Editor's save on close); `SourceLoader::cancelOpens`.

### Names added by the M3 contracts (2026-10-14, D-193)

Registered under the §10 rule when the M3 headers were laid down (`docs/milestones/M3.md` §2); each library's
`README.md` lists them with their files, their owners and the `M3-STUB`s still to replace.
- `esses_analysis`: `AlternateListChoice`, `CorrectedSpeed::alternateList`, `CorrectedSpeed::gCorrection`;
  `VarianceMode`, `VarianceRequest`, `varianceTimeAt`, `varianceDistanceAt`; `TrackDirection`, `TrackMethod`,
  `TrackOrigin`, `TrackPoint`, `TrackSection`, `TrackGeneratedFrom`; `LapWindow`, `GpsLap`,
  `TrackGeneratorOptions`, `TrackGenerationReport`, `GeneratedTrack`, `TrackGenerator::toEnu`;
  `SectionDetectorOptions`; `TrackSectionsSettings`, `LapIndex`, `LapIndexLookup`, `TrackSectionsRule::sections`,
  `lapDistanceOfTrackDistance`, `trackDistanceOfLapDistance`; `ReportScope`, `ReportStat`, `ReportLayout`,
  `ReportChannel`, `ReportOptions`, `reportStatName`, `parseReportStat`, `reportScopeName`, `parseReportScope`;
  `StatKey`, `StatValue`; `ReportSlotRole`, `ReportSource`, `ReportChannelData`, `ReportChannelLookup`,
  `ReportInputs`, `ReportCellState`, `ReportCell`, `ReportHeading`, `ReportColumn`, `ReportTable`,
  `ReportEngine::statistic`; `TimeReportBand`, `TimeReportSummaries`, `TimeReportSpec`, `TimeReportCell`,
  `TimeReportRow`, `TimeReportTable`, `RollingMinimum`; `UnitValue`, `BandAxisMode`, `BandMode`, `BandDef`,
  `BandAxis`, `Band`, `generateBands`, `SampleSpan`; `HistogramYMode`, `HistogramInput`, `HistogramRequest`,
  `HistogramResult`; `ScatterChannel`, `ScatterInput`, `ScatterSeries`, `IntervalAverage`, `DensityGrid`;
  `TrendType`, `TrendSpec`, `TrendResult`; `TrackGeneratorOptions::invertLatAccel`,
  `SectionDetectorOptions::minTurnDeg`, `TrackSectionsSettings::supportsTrackGeneration`/`supportsReporting` and
  the `RuleRunner::runPhase2` overload that takes a track map and a `LapIndexLookup` (D-195); the trailing
  `store::Interp` parameter of `ReportEngine::statistic` (D-197).
- `esses_project`: `TrackMapFile` members (`parse`, `load`, `toJson`, `save`, `fileNameFor`); `legacyFileNameFor`,
  `loadFor`, `claimFor` and `kMaxSlugBytes` (D-205).
- `esses_plotcore`: `HitKind` values `GroupHeading` … `Caption`, `HandleKind`, `PointPx`, `HitResult::handleKind`
  /`point`/`bin`/`section`/`cell`, `HitTester::hitAll`/`addPoints`/`nearestPoint`; `KeyMeasurement`, `KeyColumns`,
  `KeyCell`, the column-aware `KeyLayout::layout`, `emitDatumValues`, `emitMeasurements`; `BandCell`,
  `BandMarkerKind`, `BandMarker`, `BandLayoutInput`, `BandColours`, `BandGeometry`; `ChannelUse`,
  `componentChannelUses`, `ScaleSpec`, `BandOpen`, `ColourBand` and their JSON helpers; `ScatterTrace`,
  `ScatterBody`; `HistogramTrace`, `HistogramBody`; `TrackPositionBody`, `TrackFit`; `OutingBody`;
  `GaugeNumericBody`, `GaugeBarBody`, `GaugeGeometry`; `ComponentBuildInputs::trackMap`; `KeyGeometry::valueLines`
  and the per-point tags of `HitTester::addPoints`/`nearestPoint` (D-196); `TextRun::scale`, `ScatterExportSet`,
  `ScatterAxes`, `ScatterGeometry::exportSets`/`axes`, `HistogramExportColumn`, `HistogramExport`,
  `HistogramGeometry::exportTable` (D-198); `TdScaleLine`, `TdVariance`, `TdStatusChannel`, `TdGroupZoom`,
  `TdLocalState`, `TdBandEntry`, `TdMarkerEntry`, `TdMeasurementEntry`, `TdStaticInfo`, the `buildStatic`/
  `buildDynamic` overloads with them, `TimeDistanceBuilder::zoomedScale`/`offsetText`, the M3 members of
  `TimeDistanceBody`, `TdGroup`, `TdGroupLayout` and `TdLayout`, `ResolvedChannel::limitMin`/`limitMax` (D-201);
  `rangeKeyColour`, the key colour of a range of 06 §5.3 item 3 (D-202).
- `esses_qtmodel`: `ReportModel` (04 §12 names it) with its styling roles and `Highlight`, `setHighlight`, `setTimeFormat`,
  `setProjectTimeFormat`, `setDistanceService`, `busy`, `deliveries`, `lastError`, `quoteField`, `channelTable`,
  `timeTable`; `TrackMapService`, `TrackMapCommand`, `TrackGenerationRequest`, `TrackGenerationState`,
  `TrackGenerationStatus`; `RenderService::pool`/`dispatcher`/`statCache`/`setTrackMaps`/`trackMaps`;
  `SourceLoader::setTrackMaps` (D-199); `WorksheetViewModel::beginOffsetDrag`/`endOffsetDrag`/`offsetDragActive`/
  `offsetRangeBy`/`setSourceSamplePeriod`/`samplePeriodOf`, `DataSelectionModel::commitSidecar`/
  `sidecarCommitText` (D-201).
- `esses_widgets`: `OutingGraph`, `ScatterPlot`, `Histogram`, `ChannelReport`, `TimeReport`, `TrackPosition`,
  `NumericGauge`, `BarGauge` (the §4 component classes), `TrackMapWindow`, `TrackEditor`, `PropertiesDialog`
  (`openFor`, `find`), `FieldKind`, `FieldBinding`, `FieldBinder`, `ZoomSummaryBar`, `CorrectedSpeedDialog`,
  `LayoutEditor`, `CommandPalette`, `AnimationController` (07 §6.1 "animation"), `ComponentClipboard` (07 §6.1
  "clipboard"), `LayeredComponent` (the layer pipeline of the drawn components of the plots work, D-198);
  `ChannelReport::copyTsv`/`toTsv`, `TimeReport::toggleBand`, `TrackPosition::zoomBy`/`panByLogical`/`fit`/`commitView`,
  the `TrackMapWindow` actions and `TrackEditor`'s generation, section and file members (D-199);
  `TimeDistanceGraph`'s commands (`toggleVariance` ... `cycleStyle`, `cursorValuesText`/`copyCursorValues`, the
  vertical zoom members, `offsetStep`, the annotation members), `staticInfo`, `localState`, `hoverLineX`,
  `toolTipAt` (D-201); `ComponentWidget::handleCommand`/`captionBarHeight`, `genericComponentEdit`/
  `hasGenericComponentEdit`, the `FieldBinder` editors (`FieldEditor`, `NumberEdit`, `ColourEdit`, `UnitValueEdit`,
  `FieldForm`, `RecordListEditor`, `FixedArrayEdit`, `OptionalEditor`, `jsonptr`, `SchemaTree`),
  `SelectWorksheetDialog`, `UnitsListDialog`, `OpenLogDialog`, `MainWindow::Prompts` and the M3 shell accessors,
  `WorksheetCanvas`'s selection, geometry, z-order and lock members with `componentSpecApplied`/`layoutEdited`/
  `selectionChanged` (D-200).

---

## 5. Persisted files and schemas

| File | Schema | Owner | Notes |
|---|---|---|---|
| `<log>.<ext>.sidecar.json` | `sidecar-1` | `esses_model` | per layer: details, markers, markerEdits, rangeOverrides (keyed by group path + start marker id), localMaths, annotations, videoLinks; `source{path,size,hash,importer}` |
| `<Project>/project.json` | `project-1` | `esses_project` | profile, options, MRU, last selection, filters, maths order |
| `channels/overrides.json`, `aliases.json`, `mappings.json`, `unitmaps/<importer>.json` | `overrides-1`, `aliases-1`, `mappings-1`, `unitmap-1` | `esses_project` | numbers as `{v, unit}` |
| `maths/<module>.maths.json` | `maths-1` | `esses_maths` | modules, items, constants, enumerations, condition |
| `workbooks/<name>.workbook.json` | `workbook-1` | `esses_project` | worksheets, components (`type`+`version`+`rect`+`links`+body), `x-i2` bag, unknown types preserved |
| `trackmaps/<venue>.track.json` | `track-1` | `esses_analysis` | ENU geometry + sections |
| `.local/layout.json` | `layout-1` | `esses_qtmodel` | per-user dock perspectives keyed by workbook id (not shared) |
| `.journal/*.json` | `journal-1` | `esses_project` | write-ahead autosave |
| `backup/` | — | `esses_project` | previous copy on explicit save |
| `~/.config/esses/schemes/*.scheme.json`, `keymap.json`, `settings.json` | `scheme-1`, `keymap-1`, `settings-1` (D-055; the `settings-1` document is defined in 02 §6.9) | `esses_qtmodel` | per user; directories per OS, `ESSES_*_DIR` overrides, portable mode via `portable.marker` |
| `data/units.json`, `data/profiles/*.profile.json`, `data/rules/*.rule.json`, `data/dictionaries/*.json`, `data/profiles-text/*.json`, `data/keymaps/default.json`, `data/schemes/*.json`, `data/workbooks/circuit-default.workbook.json` | respective schemas | repo | shipped data, all our own |
| `fixtures/<formatId>/<name>/…` | `fixture-1` (defined in 05 §9.1), `probe-1`, `markers-1`, `laps-1` | repo / private submodule | D-033, D-040, D-041, D-127 |
| Schema ids added during documentation (D-073) | `rule-1`, `units-1`, `common-1` (`BandAxis`/`ReportSpec`/`ColourBands`), `schemas/components/<type>-<version>.schema.json`, `importer-profile-1`, `dictionary-1`, `plugin-1`, `maths-case-1`, `mathfn-manifest-1`, `builtin-<id>-<version>`; `project.json` key `builtinMaths` | owners per 02, 03, 05 | shipped under `schemas/` |

---

## 6. MVP scope (release 0.1)

A circuit engineer can replace i2 for a Saturday:

- Open several `.ld` (+`.ldx` merged) and CSV (`motec.csv`, generic profiles) files; Import Log; inline Mapping panel for unknown units/names.
- Project folder with autosave journal and backup; project reopens with the previous selection; Options (General/Analysis/Colours/Fonts/Keys); two shipped schemes (light/dark), default circuit workbook.
- Data window with Main / Overlay 0 / Overlays 1..n, filters (incl. lap-number syntax), lap time, Δ to fastest, offset amount, editable trust flag; N/P, Q, F4, SHIFT+F4, CTRL+F, CTRL+ALT+F, J, W, DELETE.
- Workbooks and worksheets (tabs, 0–9, CTRL+0–9, F7, F11/F12, CTRL+F11/F12), free-placement components, Layout Editor basics, zoom summary bar with SHIFT+click extend.
- `time_distance` with everything in parity-first §5.4 (groups/panels/overlapped, like-units grouping, scale modes and lines, key column, cursor/datum/zoom/pan, measurements, variance, offset axis, bands, annotations, status strip, device events, SHIFT+F7, CTRL+SHIFT+C, Export CSV), `outing`, `scatter`, `histogram`, `channel_report`, `time_report`, `track_position`, `values_list`, `gauge_numeric`, `gauge_bar`; animation (A) basic.
- Channels window with i2 search semantics; Channel Editor; Channel Status with resolution traces; overrides/aliases/mappings; Values window; Track Map dock.
- Maths: full i2-compatible language, global + local maths, constants, enumerations, filter/scale-offset/2D tables, Pending status, multi-core evaluation (parallel across nodes in 0.1; chunk-parallel at v1.0, D-139), Maths Editor with live diagnostics, `[unit]` popup, Insert Range, auto-save on close; transient vs local maths semantics from the channel label.
- Corrected speed/distance built-in module with dialog; lap-distance stretching with toolbar toggle and per-lap report; Lap Editor (insert by time or lap time, offset distance with Apply, trust, restore logged; auto-insert by distance / GPS gate at v1.0, 06 §13.3); Details editor; Track Editor (Speed+LatG and GPS generation, default and user sections).
- Full hotkey table shipped as `keymap.json` with a behaviour test per row; command palette.
- Raster renderer meeting the MVP budgets on 20 traces; HiDPI correct at 1× and 2×.
- `esses-cli info/channels/laps/eval/convert/export/validate/fixture`.
- Windows installer + portable zip; Linux and macOS builds green in CI.

Explicitly **not** in 0.1 (first v1.0 items): i2 workbook/workspace/scheme/maths importers; indexed four-pane Open dialog; GL backend; `.ld/.ldx/.mat` export UI; FFT, suspension histogram, mixture map, track report, rainbow, GPS track, dial/light/wheel gauges, notes/picture; Corrected GPS dialog; Drag/Rally; print; scripting; plugins.

---

## 7. Milestones and acceptance criteria

Effort = one developer with AI assistance, ~25 focused hours per calendar week. Each milestone ends with its CI jobs green and its acceptance list checked in `docs/milestones/Mn.md`.

| Id | Name | Scope | Acceptance criteria | Weeks |
|---|---|---|---|---|
| **M0** | Foundations | Repo, licence/governance/cleanroom docs (D-002..D-004), CMake presets, vendored dependencies under `third_party/` (D-147), three-OS CI (`core`, `lint`), `esses_base` (`TaskPool`, `Rate`, `MappedFile`), `esses_units` + `data/units.json`, `esses_store` (chunks, pyramid, `SummaryPyramid`, `ChunkCache` without eviction, `Resampler`), `esses_model`, `esses_formats` with `motec.ld` + `.ldx` reader and `.ld`+`.ldx` writer (CSV engine moved to M2, D-044), `SidecarDocument`, JSON schemas, `esses-cli info/channels/laps/validate/fixture add/verify`, synthetic fixture generator, independent reference decoder `tools/ld_ref_decode.py` (D-041), fixture corpus layout | `esses-cli info --json` matches `expected.probe.json` for ≥ 20 `.ld/.ldx` fixtures (synthetic dtype/scale/M1-header/sim-dialect families including `synthetic-acc-dialect`; a real ACC export only in the private submodule for now, as an ordinary third-party fixture (Q-04; ACC is not MoTeC-licensed, answered 2026-10-10), and third-party sim exports only where their licence permits redistribution; D-040, D-127); per-channel decode equals an oracle independent of our writer (bit-exact ints, ≤ 1 ulp floats); `.ld` write → read round-trips bit-exact; pyramid and M4 property tests pass; shift-vs-mul order recorded in `CLEANROOM.md` from a real MoTeC-logger file (spike S-1, 2026-10-10: `afterMul`, now the default, D-037); fuzz 60 s clean on `.ld/.ldx`; `ESSES_BUILD_QT=OFF` builds on three OSes with no Qt | 4 |
| **M1** | Walking skeleton | `esses_analysis` (`BeaconPairRule`, `WholeOutingRule`, `DistanceIndex`, `CorrectedSpeed` fastestWheel with clamps, `CorrectedDistance`), minimal `esses_maths` (`MathGraph`, `PlanProvider`, `EvalContext` and the built-in module only; parser and `eval` in M2, D-044), `esses_project` documents, `esses_plotcore` (`ViewState`, `DomainMapper`, `TraceBuilder`, `AxisLayout`, `DrawList`), `esses_qtrender` `RasterBackend` + `LayerCache`, `esses_qtmodel` (`DataSelectionModel`, `WorksheetViewModel`, `ProjectSession`, `SourceLoader`, `MathsService` minimal, `Dispatcher`, `CommandRegistry`), `esses_widgets` shell: `DockHost` (ADS; `QDockWidget` fallback only if spike S-3 fails), `DataWindow` (filters in M2), `ChannelsWindow` (plain filter; i2 search in M2), `ValuesWindow`, `WorksheetCanvas`, `TimeDistanceGraph` (groups, like-units grouping, cursor, zoom, pan, F9 with both preservation rules, overlays in time and distance, Main/O0/overlay colours), `ValuesList`, hotkeys batch 1 (views, data, zoom, pan, cursor), `esses-render` | Open the `synthetic-acc-dialect` `.ld` (a real ACC `.ld` where the private submodule is present), overlay fastest vs chosen lap in distance mode with cursor-linked Values; cursor move ≤ 4 ms, zoom/pan ≤ 16 ms with 20 traces × 3,800 px on the reference machine (median of ≥ 5 runs, D-042); first paint of a 100 MB `.ld` ≤ 500 ms; golden images for 8 worksheet cases at DPR 1 and 2; Qt Test rows for every implemented hotkey pass; `esses-render` reproduces the on-screen image | 8 |
| **M2** | Channels, maths, persistence | `Parser`/`TypeChecker`/`esses-cli eval` (from M1), `TextEngine` + `motec.csv` + `generic.csv` + `Dictionary` + `UnitMappingProfile` (from M0), `LapStretch`, Data-window filters and Channels-window i2 search (D-044); full maths language and all built-in functions, modules/items/constants/enumerations/filter/scale-offset/2D tables, `MathGraph` shadowing + cycles + versions + `SelectionDependent`, `PlanProvider` into the chunk cache, `MathsEditor` with live diagnostics, `ChannelStatusWindow`, `ChannelEditor`, overrides/aliases/mappings/unit maps + `MappingPanel`, `DetailsEditor`, `LapEditor` (all functions incl. stretch report), sidecar `MarkerEdit`s, workbook/project JSON with `x-i2` slot and unknown-type preservation, `QUndoStack` commands, journal autosave + backup, schemes (two shipped) + `OptionsDialog`, `docs/maths-compat.md` + `*.case.json` corpus | `esses-cli maths check` over the user's real i2 maths modules: 0 parse errors, ≥ 95 % of items OK or warnings only, every remaining error listed as a known gap (counts committed, no names); CSV fixtures pass; ≥ 150 maths cases pass (all documented functions/arities, window rules, filters incl. zero-phase, `range_is/range_change`, tables); shadowing/cycle tests; beacon insert/offset rebuilds laps < 5 ms and trust/offset overrides survive it; project reopens with previous selection and single-colour state; canonical round-trip of every file kind byte-identical; `esses-cli validate` passes on all shipped data | 9 |
| **M3** | The 80 % components | `PropertiesDialog` (first appearance, D-044); corrected-speed `alternateList`/`gCorrection` with `CorrectedSpeedDialog`; `time_distance` completion (overlapped mode, datum, measurements, variance via `VarianceTable`, offset axis, range/section/marker bands, annotations, status strip, device events, SHIFT+F7, CTRL+SHIFT+C, Export CSV), `outing`, `scatter`, `histogram`, `channel_report` (`ReportEngine`, `StatCache`), `time_report` (eclectic, rolling minimum, % bands), `TrackEditor` (Speed+LatG, GPS, default/user sections) + `track_position` + `TrackMapWindow`, `gauge_numeric`, `gauge_bar`, `PropertiesDialog` tabs for all MVP components, `ZoomSummaryBar`, animation (A), Print to Clipboard, remaining hotkeys, command palette, `LayoutEditor` basics | Behaviour test for every row of `keymap.json`; golden images for all MVP components at DPR 1 and 2; Time Report eclectic/rolling-minimum and Channel Report cells verified against hand-computed fixtures; track shape measured against ground truth (GPS method against a known centreline with simulated noise; Speed+LatG against the GPS map after rigid alignment); variance equals a brute-force reference ± 1 ms; scatter trend equation matches a least-squares reference | 11 (9 with the D-046 cut list) |
| **M4** | Hardening and 0.1 | Resident-budget warnings incl. a warning at open from the probe estimate (eviction moved to M5, D-043), HiDPI sweep (mixed-DPI floating docks), crash-free soak under ASan (30 min nightly, 2 h before tags, LSan suppressions for Qt/fontconfig/glib), installer (Inno Setup + portable zip, `.ld` association, `-w/-l`), AppImage and DMG unsigned, `NOTICES.md` + Qt source offer, user docs skeleton, `CLEANROOM.md` complete for 0.1, default circuit workbook, changed-file detection with the stale badge and `file.reload_source` (02 §8 AC-4a), tag **0.1** | Every MVP scope item mapped to a named test (Qt walkthrough, CLI golden JSON, VM checklist) and passing; budget warning shown before loading a file above the budget; soak with 10 files under ASan: no crash and no leak with an `esses_*` frame; packages launch on clean Windows 10/11 VMs, AppImage and DMG launch-tested and labelled "builds green, not supported" unless the user decides otherwise; all MVP budgets green on the reference machine | 4 |
| **M5** | v1.0 parity | `ChunkCache` LRU eviction + RW-1 stress fixture (from M4, D-043); `.i2wkb` v101/v200, `.i2wsp`, `.mcs/.mfs`, i2 maths XML importers with `x-i2` bag and import report; `spectrum`, `suspension_histogram`, `mixture_map`, `track_report`, `rainbow_track`, `gps_track`, `gauge_dial/light/wheel`, `notes`, `picture`; Corrected GPS dialog; `.ld/.ldx` + `.mat` export UI, KML export; 3D tables, regression item; indexed four-pane Open dialog (sqlite3 amalgamation); Drag/Rally rules + Run/Stage editors; print; workspace `Archive`; `GlBackend` **only if** the v1.0 budgets fail on the user's hardware (granularity chosen by benchmark); C ABI frozen and proven by moving `motec.csv` out of tree + one Clang/MinGW example plugin; tag **1.0** | Feature inventory "core" + "important" rows closed except video/telemetry/setup sheets; the user's real workbooks import with ≥ 90 % attribute coverage and a complete import report; Kunos ACC workspace (private fixture, D-040) imports; RW-1 stays within the budget under eviction and a 1 GB `.ld` reaches first paint ≤ 3 s cold; exported `.ld` re-imports and decodes identically with i3rs/motec-i2 (i2 is never run on Esses output, D-038); v1.0 budgets (D-025) green nightly; `abi-compat` job green | 13 |
| **M6** | Formats and SDK | Converter harvest wrap-then-port (XRK, MDF4+DBC, IBT, PDS, …), text profiles for Pi/EFI/RacePak/VBO/TrackAddict/RaceChrono/RaceBox/MSL/RaceCapture/Haltech/Link/MaxxECU/Syvecs, dictionaries, fixtures repo (CC0 synthetic + donated), `OWNERS` per format, SDK packaging (`sdk/` headers + cmake config + example plugins), on-disk column cache if CSV reopen is a measured complaint | ≥ 15 formats with ≥ 2 fixtures each and oracle diffs; a plugin built out of tree with `find_package(EssesSDK)` loads on three OSes; CSV reopen budget met | 8 |
| **M7** | v1.1 | `esses_script` (Lua sandbox: user functions, lap rules, scripted importers, report templates), `esses_video` (Qt Multimedia, sync points, camera views), alarms (basic), setup-sheet CSV/JSON timeline, JSON-RPC automation, `esses_capi` (+ Python wheel on demand), `RhiBackend` evaluation | A Lua importer for a new CSV dialect written from the tutorial in < 1 h; video within one frame of the cursor at 1× playback; RPC drives a headless export | 8 |

Milestone time to 0.1 = 36 weeks (900 h); with the 15 % contingency line ≈ 42 calendar weeks (≈ 39–40 with the D-046 cut list); to 1.0 ≈ 57; to 1.1 ≈ 75 (D-045, superseding the earlier ≈ 30 / 42 / 58). Tests are 30 % of every milestone's hours.

---

## 8. Budgets (CI gates)

| Budget | MVP gate (from M1; absolute limit, median of ≥ 5 runs on the reference machine, D-042) | v1.0 gate (nightly, same machine) |
|---|---|---|
| Cursor move, 20 traces × 3,800 px | ≤ 4 ms (Dynamic layer only) | RW-4 (10 laps × 12 ch × 10 components) ≤ 2 ms CPU |
| Zoom/pan step, 20 traces | ≤ 16 ms | RW-4 full rebuild ≤ 33 ms hard limit, ≤ 8 ms typical |
| Full relayout / resize | ≤ 50 ms, coalesced to 30 Hz | same |
| Open 100 MB `.ld` → first paint (8 channels) | ≤ 500 ms | ≤ 300 ms |
| Open 1 GB `.ld` cold → first paint | — | ≤ 3 s |
| `SummaryPyramid` for all channels of a 1 GB file (background) | — | ≤ 6 s |
| Values window refresh, 250 channels | ≤ 2 ms | ≤ 1 ms |
| Elementwise maths, 2.16 M samples, one core | ≤ 10 ms | ≤ 3 ms |
| `filter_lp` order −2 (zero-phase), 21.6 M samples, one core | — | ≤ 150 ms |
| Channel Report 50 laps × 30 channels × 8 stats, cold | ≤ 300 ms | ≤ 200 ms |
| CSV 300 MB (100 columns) → first paint | ≤ 10 s | ≤ 6 s |
| Resident memory, RW-2 + 20 maths channels | ≤ 1 GB | ≤ 1 GB |
| Resident memory, RW-1 (6 h, 250 ch) | — | ≤ budget (25 % RAM) honoured |
| Startup to empty workspace (warm) | ≤ 1 s | ≤ 1 s |

Reference workloads RW-1 … RW-5 are as defined in `background/design-panel/performance-first.md` §1.2 and are reproduced in `bench/workloads.md`, together with the reference machine's CPU, core count, RAM and OS and the 20 maths expressions of the readiness row. GitHub-hosted runners run the same benchmarks as a non-blocking ±30 % regression alarm. "Cold" = fresh process with the OS file cache flushed. The RW-1 and 1 GB cold rows gate from M5 (D-043); per-milestone placement is 07_ROADMAP.md §12.

---

## 9. Risks the sections must design around

| Risk | Mitigation already in the design |
|---|---|
| Numbers differ from i2 (window edges, derivative, filters, stretch, corrected speed) and users distrust the tool | `docs/maths-compat.md` with explicit semantics, `*.case.json`, compat switches, oracle cases (permitted since 2026-10-10, D-005), documented "method" selectors with "results will not be bit-identical" messaging |
| `.ld` dialect zoo (shift/mul, unit placement, gap and aux blocks, sim exporters) | fixture families per dialect, dialect flag per file, Import Log surfaces every heuristic |
| Raster ceiling reached earlier than planned | renderer seam + layer cache from M1; budgets gate every milestone; GL is an additive backend (D-025) |
| MVP scope creep toward 271 features | inventory priorities and D-035 drive milestones; "nice" lands post-1.0 or via contributors |
| Clean-room slip | `CLEANROOM.md`, CI grep, no vendor templates/schemes/icons, provenance checkbox in PRs |
| Threading bugs | one pool, one bridge, immutable snapshots, TSan on maths/plotcore |
| Solo developer bus factor | Qt-free libs with fixtures and schemas make contributions small; ADRs; `CLAUDE.md`/`AGENTS.md` describing the architecture for AI-assisted contributors |
| Qt version churn | minimum 6.7, releases on 6.12, yearly bump task; Qt confined to layers 4–5 |
| i2 EULA field of use and reverse-engineering clause | i2 is run only on files from MoTeC devices, never on Esses output; behaviour observation black-box only, never disassembly, with the legal check the user's own responsibility (permitted 2026-10-10, Q-03); `CLEANROOM.md` states the rule and the research-phase use of user-written files (D-038, D-039) |
| Effort optimism | 30 % test share and a 15 % contingency line; optional 0.1 cut list; R-01 fires when M0 exceeds 130 h (D-045, D-046) |

---

## 10. Instructions to section writers

- Expand, do not redesign: when a behaviour is unspecified here, take it from `background/design-panel/parity-first.md` (§5.4–§5.10, §7.1–§7.6, §3.3); when a data-path detail is unspecified, take the interface from `DECISIONS.md` and the closest proposal it was grafted from (performance-first for store/decimation/distance, ecosystem-first for ABI/fixtures/profiles, core-first for CLI/testing/governance).
- Use the names in §1 and §4 verbatim; introduce a new name only by adding it to this outline.
- Every section that persists data must cite its schema id from §5; every section that computes must say which `TaskPool` priority it uses and what snapshot it reads.
- Every section that touches i2 behaviour must add a `CLEANROOM.md` provenance line (help page, shipped XML, public project) and must not reproduce MoTeC text.
- Open questions go to the consolidated list at the end of `DECISIONS.md`; sections do not decide licence, codename, oracle policy or MVP scope.
