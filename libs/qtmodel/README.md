# esses_qtmodel

Layer 4, Qt Core only (01 §2.1, 06 §1; D-017, D-018, D-030, D-031, D-123). The observable application model:
the worker -> GUI bridge, the runtime, selection, view models, services and the item models of the M1 docks. All
`QObject`s live on the GUI thread. Built only with `ESSES_BUILD_QT=ON`.

Namespace `esses::qt` (01 §1); public headers in `include/esses/qtmodel/` (umbrella `qtmodel.hpp`). Links every
Qt-free library and Qt Core; never Qt Gui or Qt Widgets (`cmake/qt-free-check.cmake` rule 4,
`tools/check_layering.py`). Compiled with `QT_NO_KEYWORDS` (PUBLIC): write `Q_SIGNALS`, `Q_SLOTS`, `Q_EMIT`.
Sources (`src/**`, `include/**`) are globbed with AUTOMOC; every `tests/**/tst_<name>.cpp` is one Qt Test
executable (`ctest -R qtmodel.tst_`), run offscreen.

| Header | Contents | Design |
|---|---|---|
| `dispatcher.hpp` | `Dispatcher` (`post(context, fn)`, `close`, `drain`, `assertGuiThread`) | 01 §5.3 rule 3 |
| `app_runtime.hpp` | `AppRuntimeOptions`, `AppRuntime` (pool, cache, dispatcher, sinks; `shutdown`) | 01 §5.3 rule 11 |
| `data_selection_model.hpp` | `FilterSpec`, `DataSelectionModel` (all D-017 slots, `Changes` flags, displayed order, filters, `selectionKeys`/`restore`) | 06 §5 |
| `workspace_view_model.hpp` | `ZoomFraction`, `WorkspaceViewModel` (shared zoom/zoomDomain/zoomFrac, cursor, datum with visibility, active worksheet) | 04 §2.1, D-133 |
| `worksheet_view_model.hpp` | `CursorSource`, `WorksheetViewModel` (`setDomain` F9 rules, `activate`, zoom/pan/cursor/datum/offsets/animation) | 06 §4.5, 04 §3.4, §8.5-8.6 |
| `distance_service.hpp` | `DistanceState`, `DistanceService` (`index`, `chain`, `prefetch`, `forgetSource`) | 06 §4.5, D-134, D-140 (added) |
| `render_service.hpp` | `ChannelRequest`, `componentChannels`, `RenderService` (`submitBuild` replace keys, `cancelPending`, `prepareInputs`) | 04 §6.3, 01 §5.4 |
| `project_session.hpp` | `ProjectSession` (shipped defaults, project new/open/save with backups; no undo in M1) | 06 §1, 02 §6 |
| `source_loader.hpp` | `SourceLoader` (async open pipeline, RuleRunner, built-in maths and distance registration, phase-2 annotations, `commitSidecar`), `connectSourceLoader` | 01 §4.1 |
| `maths_service.hpp` | `MathsService` (built-in module per source, Background prefetch, `require`, settings schema) | 03 §1, D-016 |
| `command_registry.hpp` | `CommandContext`, `CommandSpec`, `CommandRegistry` (`setHandler`, `dumpJson`) | 06 §14.1 |
| `builtin_commands.hpp` | `registerSelectionCommands`, `registerViewCommands` (data/zoom/pan/cursor/datum rows of hotkey batch 1) | 06 §14.4 (added) |
| `keymap_binder.hpp` | `KeyResolution`, `KeymapBinder` (`setKeymap`, `resolve`, `dispatch`, `keysFor`, `normalise`) | 06 §14.2 |
| `settings_store.hpp` | `SettingsStore` (settings-1, dotted keys, coalesced atomic write, MRU) | 06 §18, 02 §6.9 |
| `messages_model.hpp` | `Message`, `MessagesModel` (5,000-row cap, `appendImportLog`), `makeMessagesLogSink` | 06 §11, 01 §8.3 |
| `data_window_model.hpp` | `DataWindowModel` (M, O, 1, Name, Time, Δ Fastest, Offset, Trusted, Local maths) | 06 §6 |
| `channels_model.hpp` | `ChannelsModel` (source > category > channel, `filterAccepts`, `channelsMime`) | 06 §7 |
| `values_model.hpp` | `ValuesModel` (values at the cursor for Main and the reference overlay, `refresh`, `cellText`) | 06 §8, 04 §3.2 |

## Wiring a workspace

`MainWindow` (and `esses-render`) build one set per workspace on top of the process `AppRuntime`:
`SettingsStore` -> `ProjectSession` -> `DataSelectionModel(&session)`; `MathsService` (call
`loadSettingsSchema(paths.dataDir())`, `setCorrectedSpeedSettings(session.correctedSpeedSettings())`);
`DistanceService`; `SourceLoader` with `setRuleRunner(session.ruleRunner())`, `setMathsService`,
`setDistanceService` and `connectSourceLoader(loader, selection, &messages)`; one `WorksheetViewModel` per
worksheet over the shared `WorkspaceViewModel` (call `activate()` on a worksheet switch);
`registerSelectionCommands`/`registerViewCommands` on the `CommandRegistry`, then
`keymap.setKeymap(session.keymap())`; `runtime.addSink(makeMessagesLogSink(messages, runtime.dispatcher()))`.

## Threading

Every `QObject` here lives on the GUI thread. Worker results arrive through `Dispatcher::post`; the services
that can be destroyed while their tasks run (`MathsService`, `DistanceService`, `SourceLoader`,
`RenderService`, the Messages sink) post with the Dispatcher itself as the context and carry a `QPointer` made
on the GUI thread at submit time (a destructor's `cancelAll` runs continuations inline, and a `QPointer` must
never be created from a worker for an object that is being deleted). The GUI path never calls `Future::get`,
`Future::take` or `Column::require`: `require`, `index`, `chain`, `prepareInputs` and `ValuesModel` use
`tryGet`, `tryPin` and `Resampler::sampleAt`; `tst_maths_distance` and `tst_source_loader` run the whole path
on a `ThreadRole::NoBlock` thread with a recording assertion handler (AC-M1-12).

## Tests (`ctest -R qtmodel.tst_`, Qt Test, offscreen)

| Test | Covers |
|---|---|
| `tst_dispatcher` | FIFO delivery, dead contexts, posts from 8 threads, `close`, `drain`, the NoBlock `Future::get` assertion (AC-M1-12), `AppRuntime` install/shutdown, shutdown with 20 builds and a 3-level chain in flight |
| `tst_dataselection` | every 06 §5.2 row with its `changed()` bits, compact renumbering, D-109, N/P without wrap in the filtered order, the TT 2485 candidate rule (out/in never), CTRL+ALT+F tolerance, trust through the sidecar, `sourceReplaced` (by id, by lap number, dropped), close, restore keys, filters, lap-number syntax, invariants over 2,000 random operations |
| `tst_worksheetviewmodel` | F9 through the Main lap's DistanceIndex and back within one sample, the fraction rule and the banner without Corr Dist (AC-M1-09), N/P zoom and cursor in both domains, W, Z, F2, zoom limits, pan clamping, cursor steps and edges, datum, animation (Cursor-only), the shared view across worksheets, offsets |
| `tst_maths_distance` | Background prefetch observed in the pool's band (AC-M1-10), the settings schema examples, E-CSD-SETTINGS, no speed -> NoDistance, versions kept across a sidecar snapshot (D-140), enable/disable, the per-lap index (AC-M1-08 through the service), outing chains, the no-blocking path |
| `tst_source_loader` | open/publish of a written .ld + .ldx, Import Log, maths and distance without a request, phase-2 stint annotation, a second open, failures to Messages, trust through `commitSidecar` read back on reopen, close, the no-blocking path, and the public ACC stand-in when its input is generated |
| `tst_project_session` | shipped defaults, user keymap and scheme, the new-project layout, save/reopen with D-070 backups, skipped workbooks, recent sources, the options the selection reads |
| `tst_settings_messages` | settings-1 defaults, dotted keys, coalesced write, unknown members, MRU caps, load errors; the Messages cap, first error, Import Log rows, the Log sink |
| `tst_commands_keymap` | registry semantics, the shipped keymap without conflicts, focus-chain resolution, normalisation, rejected rows, facade commands equal to their keymap rows, every qtmodel-bound batch-1 key driving its post-condition |
| `tst_render_items` | replace-key delivery and `cancelPending`, `componentChannels`, `prepareInputs` feeding `TimeDistanceBuilder::buildStatic`, the Data window tree, the Channels tree, the Values rows of Main and Overlay 1 in both domains |

`tests/qtmodel_test_support.*` builds synthetic circuit sources in memory (out lap, flying laps, in lap; wheel
speeds whose integral is exactly the track length) and writes them as .ld + .ldx through the motec.ld writer.

## Deviations from the design text and open issues

- `CommandSpec::defaultKeys` is a `QStringList` of portable key text and `CommandRegistry::action(id)` lives in
  `esses_widgets` (`widgets::CommandActions`): `QKeySequence` and `QAction` are Qt Gui types in Qt 6 and this
  library links Qt Core only (01 §2.2 rule 4). Likewise `KeymapBinder` holds the table and resolution; the
  `QKeyEvent` filter is `widgets::KeyEventRouter`.
- `DataSelectionModel` takes a nullable `ProjectSession*` (06: `ProjectSession&`) for testability; with a
  session it reads `options.fastLapTolerancePct` and `options.general.selectFastestOnLoad` and keeps
  `lastSelection` current. `setTrusted` hands the edited sidecar to a `SidecarCommit` hook
  (`SourceLoader::commitSidecar`); no undo before M2. Data-window filters are per session in M1 (persisted in
  `project.json` `dataWindow` with the M2 filter UI).
- One shared view per workspace (D-133): only the active worksheet converts the shared cursor and datum on a
  Main change and the shared zoom on F9; an inactive worksheet shows the shared zoom converted on the fly. N/P
  keep the zoom span (relative to the range start) and recompute `zoomFrac` for the new Main extent.
- Zoom minimum: 4 samples of the active trace (`setActiveSamplePeriod`, default the fastest channel of the
  Main layer) in time, 4 x 0.25 m in distance; cursor steps are whole sample periods in time.
- Alignment offsets: `pendingOffsets` and the 250 ms commit exist; the commit is a callback
  (`setOffsetCommit`). The undoable sidecar edit and the O-axis UI are M3 (AC-M3-09).
- `DistanceService` task keys append the Corr Dist version and a hash of the whole `DistanceIndexKey` (lap
  bounds, anchor markers, stretch) to `distidx/<src>/<layer>/<rangeId>`, so a request with other bounds never
  joins a stale build; a pending build asked for again at a higher priority is raised to that band
  (`TaskPool::raise`), so F9 or W right after an open does not wait behind the Background prefetch; a lap without
  a valid Corr Dist sample stays without an index; `state()` is NoDistance when Corr Dist is in Error or no
  lap could be indexed.
- `MathsService` adds `source`, `units`, `pool`, `descriptor`, `outputs`, the settings-schema calls,
  `setCorrectedSpeedEnabled` and `moduleMessage`.
- `SourceLoader` submits the open task itself (the same key as `ImporterRegistry::load`) so that the task holds
  the registry by `shared_ptr`; the summary pass is one `summary/<sourceId>` task; phase 2 (task
  `ranges/<src>/<layer>/<markerSetHash>/<Corr Speed version>`, older runs of the layer cancelled first) merges
  its stint/pitStop annotations by (range id, start, end) into the newest snapshot with the same marker set, so a
  sidecar commit while it runs keeps both (another marker set is annotated anew); phase 2 runs once Corr Speed
  is evaluated (speed = Corr Speed); a second open of an open file adds an `open.already_open` Import Log row;
  a first sidecar edit fills the document's source block (`newSidecarFor`).
- The item models are Qt-Core-only: styling travels in roles (`UntrustedRole`, `SlotRole`, `FilteredInRole`,
  `StatusRole`) and glyph text instead of `QFont`/`QColor`/`QIcon`. The Channels tree has Logged and Maths
  categories (the others arrive with their documents) and no level-3 groups (no M1 importer delivers them).
  `ValuesModel` recomputes every row on refresh and retries every 50 ms while a cell waits for data.
- `DistanceService` is declared here: 06 §4.5 names it as a `WorksheetViewModel` constructor argument without
  declaring it.
- Open issue for M2: `ProjectSession`'s `QUndoStack` (D-032) is a Qt Gui class in Qt 6, so it cannot live in this
  Qt-Core-only library as 06 §1 places it; M2 must either move the undo stack to `esses_widgets` or use a small
  Qt-Core command stack here.
- `AppRuntime` restores the constructing thread's previous `ThreadRole` on `shutdown()` (several runtimes in one
  test process) and adds `removeSink` (a window removes its Messages sink when it closes). The Messages log sink
  skips the `store.cache` category: the budget warning is posted once, as a Budget row, by the shell's
  `ChunkCache` budget hook (owned by the newest window). `ValuesModel::setWorksheet` tracks both its
  connections (no accumulation per worksheet switch). `DataSelectionModel::selectionKeys` caches each source's
  project-relative path (no file-system call per selection change). The `SidecarCommit` hook guards the loader
  with a `QPointer`.
- `AppRuntime` installs the pool and cache it owns and uninstalls them on `shutdown()`; `AppRuntimeOptions`
  selects the worker count, cache budget and the NoBlock marking (only `esses` sets it).
- M1 integration: F9, W or F2 before the Main lap's distance index has arrived (Corr Dist evaluated, the lap's
  index still being built, another lap of the outing already indexed) produced a garbage span (`[-4868 m, 0]` on
  the ACC stand-in, found by `tests/ui/tst_walkthrough_m1`). Besides the `plot::outingExtentIn` fix,
  `WorkspaceViewModel` records whether the shared distance zoom is in the fraction rule's normalised units
  (`zoomNormalised`, set by the active worksheet after F9/W/F2/the first Main), and `WorksheetViewModel`
  re-expresses it in metres with the same `zoomFrac` when the Main lap's index arrives (`distanceReady`); N/P to
  a lap whose index is pending keep the metre zoom and refresh `zoomFrac` on arrival; Main moved to a source
  without distance data switches the zoom to the fraction rule. Tests: `f9WhileTheMainIndexIsPending`,
  `wWhileTheMainIndexIsPending` (workers parked so the index stays pending).
- `tst_source_loader` opens the generated ACC stand-in, so ctest runs it after `fixtures.generate_inputs`
  (`FIXTURES_REQUIRED esses_fixture_inputs`): run concurrently with the generator it read a file being rewritten.
- Resolved in the M1 integration (was an open issue): the public ACC stand-in
  (`fixtures/motec.ld/synthetic-acc-dialect`) logs its road speed as `SPEED` only, which no entry of 03 §9.1's
  default channel lists named, so it had no Corr Speed and distance mode showed the banner. The module's
  default `alternateList` now ends with `"Speed"` (case-insensitive: `SPEED`), see `libs/maths/README.md`;
  `tst_source_loader` asserts the distance state Ready on that file.
