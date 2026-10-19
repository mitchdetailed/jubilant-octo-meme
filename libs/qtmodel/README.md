# esses_qtmodel

Layer 4, Qt Core, plus Qt Gui for `QUndoStack`/`QUndoCommand` only (01 §2.1, 06 §1; D-017, D-018, D-030, D-031,
D-123, D-161). The observable application model: the worker -> GUI bridge, the runtime, selection, view models,
services, the item models of the docks, and (M2) the undo stack and its commands, the project's persistence and
journal, the full `MathsService` with Channel Status, the Mapping-panel hand-off and lap stretching. All `QObject`s
live on the GUI thread. Built only with `ESSES_BUILD_QT=ON`.

Namespace `esses::qt` (01 §1); public headers in `include/esses/qtmodel/` (umbrella `qtmodel.hpp`). Links every
Qt-free library, Qt Core and Qt Gui for `<QUndoStack>`/`<QUndoCommand>` only; never another Qt Gui header and
never Qt Widgets (`cmake/qt-free-check.cmake` rule 4, `tools/check_layering.py` rule 6; D-161). Compiled with `QT_NO_KEYWORDS` (PUBLIC): write `Q_SIGNALS`, `Q_SLOTS`, `Q_EMIT`.
Sources (`src/**`, `include/**`) are globbed with AUTOMOC; every `tests/**/tst_<name>.cpp` is one Qt Test
executable (`ctest -R qtmodel.tst_`), run offscreen.

| Header | Contents | Design |
|---|---|---|
| `dispatcher.hpp` | `Dispatcher` (`post(context, fn)`, `close`, `drain`, `assertGuiThread`) | 01 §5.3 rule 3 |
| `app_runtime.hpp` | `AppRuntimeOptions`, `AppRuntime` (pool, cache, dispatcher, sinks; `shutdown`) | 01 §5.3 rule 11 |
| `data_selection_model.hpp` | `FilterSpec`, `filterKindName`/`parseFilterKind`, `DataSelectionModel` (all D-017 slots, `Changes` flags, displayed order, filters persisted in project.json `dataWindow`, `lapNumbersError`, `filterRejected`, `selectionKeys`/`restore`) | 06 §5 |
| `workspace_view_model.hpp` | `ZoomFraction`, `WorkspaceViewModel` (shared zoom/zoomDomain/zoomFrac, cursor, datum with visibility, active worksheet) | 04 §2.1, D-133 |
| `worksheet_view_model.hpp` | `CursorSource`, `WorksheetViewModel` (`setDomain` F9 rules, `activate`, zoom/pan/cursor/datum/offsets/animation) | 06 §4.5, 04 §3.4, §8.5-8.6 |
| `distance_service.hpp` | `DistanceState`, `DistanceService` (`index`, `chain`, `prefetch`, `forgetSource`) | 06 §4.5, D-134, D-140 (added) |
| `render_service.hpp` | `ChannelRequest`, `componentChannels`, `RenderService` (`submitBuild` replace keys, `cancelPending`, `prepareInputs`) | 04 §6.3, 01 §5.4 |
| `project_session.hpp` | `WorkbookSet`, `ProjectSession` (shipped defaults; every project document loaded, saved with D-070 backups and retired files; `undoStack()`, the channel documents, unit maps, maths modules, document setters, view state written without the stack, journal autosave and recovery) | 06 §1, 02 §6, D-172 |
| `undo_commands.hpp` | the commands of every D-032 edit (workbooks, worksheets, components with the clipboard payload, channel documents, unit maps, modules, options, sidecar edits), `applyMappingDecision` | D-032, 02 §7, D-172 |
| `source_loader.hpp` | `SourceLoader` (async open pipeline, RuleRunner, built-in maths and distance registration, phase-2 annotations, `commitSidecar`; M2: the Mapping-panel hand-off, step-4 channel documents, user text profiles, `setRangeStretch`), `connectSourceLoader` | 01 §4.1, 05 §2, §8.1 |
| `maths_service.hpp` | `MathsService` (built-in modules per layer, the project's modules, local and transient maths, compat, resolver tables, selection, `require`, status per item, `invalidate`, `statusRows`, `check`), `connectMathsSelection` | 03 §1, §7, §11, D-016, D-173 |
| `channel_status_model.hpp` | `ChannelStatusModel` (Channel Status rows, roles, coalesced refresh, `rowOf`) | 06 §10, D-173 |
| `live_diagnostics.hpp` | `LiveDiagnostics` (the Maths Editor's 150 ms debounced check) | 03 §11 item 2 |
| `lap_stretch_service.hpp` | `LapStretchService` (toggle, track length, per-lap factors, report) | D-016, D-174 |
| `session_restore.hpp` | `connectProjectSession`, `SessionRestore` (previous data and the selection after a project opens) | 06 §18 steps 5-6 |
| `command_registry.hpp` | `CommandContext`, `CommandSpec`, `CommandRegistry` (`setHandler`, `dumpJson`) | 06 §14.1 |
| `builtin_commands.hpp` | `registerSelectionCommands`, `registerViewCommands` (data/zoom/pan/cursor/datum rows of hotkey batch 1), `registerEditCommands` (edit.undo/redo), `registerLapStretchCommand` | 06 §14.4 (added) |
| `keymap_binder.hpp` | `KeyResolution`, `KeymapBinder` (`setKeymap`, `resolve`, `dispatch`, `keysFor`, `normalise`) | 06 §14.2 |
| `settings_store.hpp` | `SettingsStore` (settings-1, dotted keys, coalesced atomic write, MRU) | 06 §18, 02 §6.9 |
| `messages_model.hpp` | `Message`, `MessagesModel` (5,000-row cap, `appendImportLog`), `makeMessagesLogSink` | 06 §11, 01 §8.3 |
| `data_window_model.hpp` | `DataWindowModel` (M, O, 1, Name, Time, Δ Fastest, Offset, Trusted, Local maths) | 06 §6 |
| `channels_model.hpp` | `ChannelsModel` (source > category > channel with Alias and Project/Constants, the word-prefix search `filterAccepts`/`searchWords`, `channelsMime`) | 06 §7, D-175 |
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
M2 adds: `connectProjectSession(session, &maths, &loader)` (modules, compat, mappings, aliases, unit maps and the
corrected-speed settings follow every document change; the session shares the maths unit table);
`connectMathsSelection(selection, maths, &distance)`; `loader.setMappingHandOff(true)` with the `MappingPanel`
on `mappingRequired` (it answers through `applyMappingDecision`); `loader.registerTextProfiles(dataDir,
configDir)` before the first open; a `LapStretchService(loader, distance, units, &session)` with
`registerLapStretchCommand`; `registerEditCommands(registry, session.undoStack())`; the recovery prompt through
`session.setRecoveryPrompt` or `recoveryAvailable()`; after a project opens, `SessionRestore(session, loader,
selection).start(logArguments)`. `connectSourceLoader` makes the Data window's trust edits undoable when the
selection was built with the session.

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
| `tst_source_loader` | open/publish of a written .ld + .ldx, Import Log, maths and distance without a request, phase-2 stint annotation, a second open, failures to Messages, trust through `commitSidecar` read back on reopen, close, the no-blocking path, and the public ACC stand-in when its input is generated; D-190: sidecar writes coalesced on a Background task and flushed on close, `closeAll()` cancelling an open in flight |
| `tst_project_session` | shipped defaults, user keymap and scheme, the new-project layout, save/reopen with D-070 backups, skipped workbooks, recent sources, the options the selection reads |
| `tst_settings_messages` | settings-1 defaults, dotted keys, coalesced write, unknown members, MRU caps, load errors; the Messages cap, first error, Import Log rows, the Log sink |
| `tst_commands_keymap` | registry semantics, the shipped keymap without conflicts, focus-chain resolution, normalisation, rejected rows, facade commands equal to their keymap rows, every qtmodel-bound batch-1 key driving its post-condition |
| `tst_render_items` | replace-key delivery and `cancelPending`, `componentChannels`, `prepareInputs` feeding `TimeDistanceBuilder::buildStatic`, the Data window tree, the Channels tree, the Values rows of Main and Overlay 1 in both domains |

| `tst_undo_contract` | `QUndoStack` under `QCoreApplication`, document commands, merge, worksheet move (the M2 contract) |
| `tst_undo_property` | AC-M2-09: 30 randomised sequences (seed `ESSES_TEST_SEED`) of every command kind, undo and redo, equal to an independent replay after every step; undo-all and redo-all; view-state operations (selection, zoom, cursor, F9, filters, the stretch toggle) never on the stack; file open/close not on the stack |
| `tst_project_recovery` | every document saved and reopened, renamed/removed files retired, unreadable files kept, autosave writing only changed documents and never project files, the kill-and-recover path of AC-M2-06, the prompt's three answers, the timer, the selection, single-colour state and filters restored through `SessionRestore`; D-187 and D-190: a failed or killed save recovers without false conflicts, a journal holding only saved content is not offered, a failed backup copy keeps the save, the prompt's replay is the one recovered, `saveMathsModules` leaves other edits unsaved |
| `tst_maths_service` | modules defined with only the edited cone re-versioned (AC-M2-04 through the service), every 03 §7.3 state, Recalculate, compat, mappings, local maths (explicit and from the sidecar), transient maths, GPS corrections, the live check and `LiveDiagnostics`, `connectProjectSession`, Channel Status rows with every trace kind and "(missing)" references (AC-M2-12) |
| `tst_mapping_handoff` | AC-M2-11 at the model level: two unknown units wait for the panel, the decision as an undoable macro, the saved unit map applied silently to the next file, Cancel, Open anyway, headless opens, user text profiles; D-190: a project change cancels the opens in flight and those waiting for the panel |
| `tst_filters_search` | the lap-number syntax and its messages, filters persisted by source hash with the default and the apply-to-all flag, the word-prefix search, the Channels tree's Alias and Constants |
| `tst_lap_stretch` | factors within the tolerance published and cleared by the toggle (view state), the tolerance as an undoable options edit, `Venue Length` as the track length, the report, `edit.undo`/`edit.redo` |

`tests/qtmodel_test_support.*` builds synthetic circuit sources in memory (out lap, flying laps, in lap; wheel
speeds whose integral is exactly the track length, optionally scaled per lap; optional GPS channels, a renamed
throttle, a `Venue Length` detail) and writes them as .ld + .ldx through the motec.ld writer.

## Deviations from the design text and open issues

- `CommandSpec::defaultKeys` is a `QStringList` of portable key text and `CommandRegistry::action(id)` lives in
  `esses_widgets` (`widgets::CommandActions`): `QKeySequence` and `QAction` are Qt Gui types in Qt 6 and this
  library uses no Qt Gui class but the undo framework (01 §2.2 rule 4, D-161). Likewise `KeymapBinder` holds the table and resolution; the
  `QKeyEvent` filter is `widgets::KeyEventRouter`.
- `DataSelectionModel` takes a nullable `ProjectSession*` (06: `ProjectSession&`) for testability; with a
  session it reads `options.fastLapTolerancePct` and `options.general.selectFastestOnLoad` and keeps
  `lastSelection` current. `setTrusted` hands the edited sidecar to a `SidecarCommit` hook; `connectSourceLoader`
  makes it an undoable `SidecarCommand` on the session's stack (M2, D-172). Data-window filters are persisted in
  `project.json` `dataWindow` (M2; per session in M1).
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
- Resolved in M2 by D-161: `ProjectSession`'s `QUndoStack` (D-032) is a Qt Gui class in Qt 6; this library now
  links Qt Gui and may include `<QUndoStack>`/`<QUndoCommand>` and no other Qt Gui header (`tools/check_layering.py`
  rule 6). The M1 note read: M2 must either move the undo stack to `esses_widgets` or use a small
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

## M2 as built (07 §5.1; `docs/milestones/M2.md`; D-161, D-172 to D-175)

Qt modules: Qt Core, plus Qt Gui for `QUndoStack`/`QUndoCommand` only (D-161). The M2 work lives in three source
roots beside the M1 files it extends (`project_session.cpp`, `maths_service.cpp`, `source_loader.cpp`,
`data_selection_model.cpp`, `channels_model.cpp`, `distance_service.cpp`, `builtin_commands.cpp`):

| Root | Contents | Design |
|---|---|---|
| `src/undo/` | the commands and `applyMappingDecision` (`undo_commands.cpp`); `ProjectSession`'s setters, view-state writes, journal, recovery and autosave timer (`project_session_m2.cpp`) | D-032, 02 §6.8, §7, D-172 |
| `src/maths/` | `MathsService::Impl` (`maths_service_impl.hpp`), the M2 members and the live-check scope (`maths_service_m2.cpp`), `ChannelStatusModel`, `LiveDiagnostics` | 03 §4.2-4.3, §7, §11, 06 §10, D-173 |
| `src/loader/` | the Mapping-panel hand-off, text profiles and `setRangeStretch` (`source_loader_m2.cpp`), `LapStretchService`, `connectProjectSession` and `SessionRestore` | 05 §2, §8.1, 06 §13.8, §18, D-174 |

No `M2-STUB` is left in this library.

### Deviations of M2 from the design text

- Undo and persistence (D-172): commands hold whole documents; undone options edits keep the view state
  (`lastSelection`, `recentSources`, `dataWindow`, `options.lapStretch.enabled`) and the members other documents own
  (`workbooks`, `mathsOrder`); the F9 domain stays view state (D-032 over 02 §7); the workbook list is a document
  (`WorkbookSet`); the Data window's trust edits are `SidecarCommand`s pushed by `connectSourceLoader`'s hook; a
  Mapping-panel decision is one "Resolve mapping" macro; `isDirty()` follows the stack's clean index except after a
  recovery or a migration. Save writes changed documents only, retires files the project no longer names to
  `backup/` (never a file a case-only rename writes again), never writes over an unreadable document, journals
  what it is about to write first and clears this session's journal only after every write succeeded; autosave
  writes nothing for an unchanged set, journals this session's edits beside a deferred recovery (Later), and
  Recover adopts the replayed journal as this session's own (D-187); `discardSessionJournal` drops this session's
  journal when the project is left without saving. Track-map
  section edits (M3) and scheme edits (v1.0) get their commands with their documents.
- `MathsService` (D-173): `esses.gps_corrections` is registered for a layer that logs both GPS inputs; local maths
  is the sidecar's `localMaths`, re-read on every snapshot (`setLocalModule` overrides it); the live check resolves
  the Main source's maths outputs, logged channels through the project's mappings and aliases, details and the
  global constants; `channelReady` fires once per (output, version), also for upstream nodes applied with a
  completion; `require()` announces Pending -> Evaluating. Additions: `setResolverTables`, `setSelectionDistances`,
  `mathSelection`, `modules`, `compat`, `sources`, `gpsCorrectionsModule`.
- Channel Status (D-173): kinds beyond 06 §10 for the other item kinds and "Reference" for missing workbook
  references; mappings no open source uses are one "Project" row; refreshes keep the rows (`dataChanged`) when
  their identities hold.
- Lap stretching (D-174): `LapStretchService` (an addition) publishes `Range::stretch` from the laps' unstretched
  indexes and the source's track length (explicit, else `Venue Length`); `DistanceService` copies an index whose key
  differs only by the stretch; the toggle is view state.
- Search (D-175): the query is split like the names; loose matching is a substring test per word, still in order.
- Data-window filters are view state persisted in `project.json` `dataWindow` (D-162 keys) and restored when a
  project opens and when a source publishes; a lap-number filter that does not parse is refused with
  `filterRejected` (the previous one stays).
- The hand-off's registry and unit table are kept alive by the continuation until the result reaches the GUI
  thread (task A and the skeleton borrow them); Import Log rows `open.mapping_resolved` and `open.mapping_cancelled`
  record the panel's outcome; `registerTextProfiles` registers the user's profiles (the shipped ones are built in,
  D-168) and refuses to touch the registry while an open is in flight.
- `SessionRestore` waits for the opens and for any pending Mapping panel, then calls `DataSelectionModel::restore`
  with the `lastSelection` captured at `start()` (every selection change rewrites it).
- D-190: `ProjectSession::saveMathsModules` (the Maths Editor's save on close: modules and their order only, the other
  edits stay unsaved and journalled); Save writes `project.json` only after every other document; the open's
  journal replay is reused by the prompt and Recover, and a journal holding only saved content is deleted at open;
  autosave builds entries only for a changed set (its write stays on the GUI thread: open issue). `SourceLoader`
  cancels opens (`cancelOpens()`, called by `closeAll()` and, through `connectProjectSession`, on a project change;
  `open.cancelled`, `open.mapping_cancelled`) and writes sidecars from a Background task 500 ms after the first
  edit (a per-source generation keeps the newest; flushed on close and destruction; `sidecar.write_failed`), so
  `commitSidecar` no longer returns Io.

### Open issues

- The shell adopts the M2 wiring above (`libs/widgets/README.md`, D-178) except the Mapping-panel hand-off, which
  stays off until the shell has the `MappingPanel` (unknown units then open unitless).
- `RenderService` resolves channels through the source's own `ChannelResolver` without the project's mappings and
  aliases (its M1 note); the Channels tree and Channel Status use them.
- The keymap rows `edit.undo`, `edit.redo` and `data.lap_stretch_toggle` are `implemented` with their
  `KEYMAP_TEST` rows (rows of the milestone in progress may be, `tools/check_keymap_coverage.py`, D-178).
- The Channels tree has no Details or Mapped category yet; enumerated maths outputs show numbers (maths open
  issue 11).
- Autosave and save serialise and write on the GUI thread (D-190 (d)): a background journal writer needs an
  asynchronous save first, since the save's write-ahead must land before its writes.

## M3 contract (07 §6.1; `docs/milestones/M3.md` §2; D-193)

- `report_model.hpp`, `src/reports/report_model.cpp` (reports work): `ReportModel`, the `QAbstractTableModel` of
  `channel_report` and `time_report` over `analysis::ReportEngine`/`TimeReportEngine` and a shared
  `analysis::StatCache`. Implemented (below, "M3: reports and track").
- `componentChannels` (final): time_distance and values_list as before, every other type through
  `plot::componentChannelUses`, so the M3 owners never edit `render_service.cpp`.

### M3: reports and track (D-199)

- `ReportModel` (`src/reports/`): `refresh()` coalesces per event-loop turn; the inputs are built on the GUI
  thread (the ranges in scope; every channel resolved once per source and layer through `detail::resolveChannel`,
  maths outputs requested at Background) and the engine runs on the pool under the replace key
  `report/<componentId>`, delivered through the Dispatcher (a QPointer made at submit; older deliveries dropped).
  Scopes, the zoom link, the two layouts, row labels, cell texts, time formats and export quoting are in the
  header. Styling is in plain roles (`BackgroundRgbaRole`, `ItalicRole`, `BoldRole`, `CellStateRole`,
  `RowKindRole`): D-161 keeps `QColor`/`QFont` out of this library, so the widget maps them. Added:
  `setHighlight`, `setTimeFormat`, `setProjectTimeFormat`, `setDistanceService`, `busy`, `deliveries`,
  `lastError`, `quoteField`, `channelTable`, `timeTable`.
- `TrackMapService` and `TrackMapCommand` (`track_map_service.hpp`, `src/track/`): the workspace's track maps by
  venue (`trackmaps/<slug>-<hash>.track.json`, `project::TrackMapFile::fileNameFor`, D-205; read on first use
  through `TrackMapFile::loadFor`, so never a file that holds another venue, and cached; written at once by
  `setMap` at `TrackMapFile::claimFor`'s file, which moves an earlier build's slug-only file of the venue and
  refuses a name that holds another venue's map; the previous file to `backup/` on Accept, a removal moved to
  `backup/<stem>.<stamp>.removed.json`; in memory without a project; a project change drops the cache) and the
  one undo command of every track-map edit (02 §7).
  `generate(TrackGenerationRequest, context, onGui)` is the generation path of 06 §13.5 item 1 (the inputs
  resolved here through `MathsService`, `analysis::TrackGenerator` on a worker, `geom/track/<source>/<lap>`).
- `RenderService`: `pool()`, `dispatcher()`, `statCache()` (the workspace's `StatCache`), `setTrackMaps()`;
  `prepareInputs` fills `ComponentBuildInputs::trackMap` from the Main source's venue.
- `SourceLoader::setTrackMaps`: phase 2 builds the Track Sections groups (open issue 4 of the milestone note):
  the `RuleRunner` overload with the venue's map and the lap indexes ready now; `distanceReady`, `mapChanged`
  and a `setRangeStretch` that publishes new factors run it again (D-204). The key gains
  `/t<map generation>/i<indexed laps>-<digest>`; the digest (xxh3-128, laps in `RangeId` order) covers each
  indexed lap's id, its index's D-140 key (Corr Dist version, bounding markers and times, stretch) and its
  beacon flags, so a replaced index (a new stretch, a newer Corr Dist) changes the key at the same count. A
  request is deduplicated against its layer's latest run only (running or done, per `ranges/<src>/<layer>/`);
  each start has a serial, so a finished run that is no longer its layer's latest publishes nothing, a latest
  run that was cancelled or failed is forgotten (a later request runs it again), and `close` forgets the
  source's runs.
- Wiring a workspace (M3): one `TrackMapService(&session, &maths, &runtime.dispatcher())`, then
  `render.setTrackMaps(&maps)` and `loader.setTrackMaps(&maps)` (MainWindow does, `main_window_m3.cpp`).
- Tests: `tests/reports/tst_report_model` (every display rule against hand-computed cells), `tests/track/
  tst_track_map_service` (load, cache, write, backup, remove, undo/redo, the `RenderService` binding, GPS
  generation within 2 m RMS of the AC-M3-04 synthetic circuit's true centreline, Speed+LatG, pending inputs,
  and `SourceLoader` building the Track Sections of an opened `.ld` at the hand-computed times),
  `tests/track/tst_track_sections_pass` (D-204: a lap stretch published at the same index count moves the
  out lap's Turn 1 to its stretched time and leaves a flying lap's in place, clearing it brings the
  unstretched time back; a file closed and opened again is annotated).

### M3: the time_distance completion (D-201)

- `WorksheetViewModel`: `beginOffsetDrag`/`endOffsetDrag` (a sub-axis drag holds the 250 ms commit and commits
  once at the release, `offsetDragActive`), `offsetRangeBy` (a nudge by a computed amount), and
  `setSourceSamplePeriod`/`samplePeriodOf`: the offset step of a source's ranges in time mode, reported by the
  focused time_distance graph for every visible slot (`offsetRange` steps a range by its own source's period; the
  Main source's `activeSamplePeriod()` when none was reported).
- `DataSelectionModel::commitSidecar(source, doc, undoText)`: any sidecar edit of an open source through the
  SidecarCommit hook (with a session one undoable `SidecarCommand` named `undoText`; `connectSourceLoader` reads
  `sidecarCommitText()`); `setTrusted` goes through it with its M2 text "Edit trust". The graph's annotation Edit /
  Remove use it.

## M4 contract (07 §7.1; `docs/milestones/M4.md` §2-3; D-206)

- `memory_budget.hpp` (budget package, `src/m4_budget/`): the budget setting (`kBudgetSettingKey`
  `memory.chunkCacheBudgetBytes`, `kMinBudgetBytes`), `OpenBudgetCheck` (`exceeds()`: estimate > budget, final),
  `PerformanceSnapshot` and `PerformanceMonitor` (final: polls `ChunkCache::stats()`, the budget and
  `TaskPool::stats()` on a GUI timer); `budgetFromSettings`, `budgetWarningText`, `formatBytes`,
  `performanceLine`, `performanceDetails`.
- `SourceLoader` (`source_loader.hpp` is frozen; one marked section per package): budget
  (`src/m4_budget/source_loader_budget.cpp`: the gate, Continue/Cancel, `budgetExceeded`, the hook `budgetHolds`
  at the start of `published()`), reload (`src/m4_reload/source_loader_reload.cpp`: `checkChangedFiles`,
  `isStale`, `reload`, `staleChanged`, `sourceReloaded`, the hooks at the end of `published()` and in `close()`),
  followups (`summariesReady`, emitted from `source_loader.cpp`, which that package owns with the hook calls
  kept). Each package keeps its state in its nested struct (`SourceLoader::M4Budget`, `M4Reload`).
- `Message::altAction`/`altActionArgs` and `MessagesModel::AltActionRole` (final): a row's second action.
- `DataWindowModel`: `StaleRole`, `setStaleQuery`/`isStale` (final), `refreshStale`
  (`src/m4_reload/data_window_model_stale.cpp`).
- Test: `tests/m4/tst_m4_contract.cpp` (`qtmodel.tst_m4_contract`); the packages' tests are in
  `tests/m4_<package>/`. Every member above is implemented by its package (D-207, D-208, D-210; the sections below);
  no contract stub is left (D-216).

## M4 budget: the open-time warning and the Stats (AC-M4-02; D-207; 07 §7.1 row 1, 01 §7.2)

- `memory_budget.hpp` (`src/m4_budget/memory_budget.cpp`): `budgetFromSettings` reads
  `memory.chunkCacheBudgetBytes` (an integer or a double above 0, raised to `kMinBudgetBytes`, 1 GiB; nullopt
  otherwise, so the runtime keeps its budget); `formatBytes` ("512 bytes", "1023 KB", "4.2 MB", "812 MB", "1.4 GB";
  units of 2^10, 2^20, 2^30 bytes); `budgetWarningText` ("needs about 4.2 MB of memory once loaded, above the 2.0 MB
  memory budget (0 bytes in use). Continue to open it anyway, or Cancel."); `performanceLine` ("Resident 412 MB of
  4.0 GB", " (over budget)" above it) and `performanceDetails` (memory, chunks, hits and misses, evictions, the
  pool's queues and workers); empty texts for an empty snapshot (the runtime shut down).
- `SourceLoader`'s budget gate (`src/m4_budget/source_loader_budget.cpp`, state in `SourceLoader::M4Budget`): with
  `setBudgetGate(&cache)`, `published()` holds an open whose `formats::estimateResident` exceeds the cache's budget
  before it registers anything (no chunk decoded, no maths, no summary pass, no Import Log yet) and announces
  `budgetExceeded(pendingId, check)` (the check carries the estimate, the budget and the resident bytes, reported,
  not added). `continueOverBudget` publishes the same snapshot as the open's own publication (a second open of the
  file published meanwhile gives `open.already_open`); `cancelOverBudget` and `cancelOpens()` drop it with an Import
  Log row `open.budget_cancelled` (Info) and release the registry's decode log unless another open or waiting open
  has the same source id; `close()` leaves the waiting opens of the file alone. A held open is not counted by
  `pendingOpens()`; without a gate (the default) no open ever waits.
- Test: `tests/m4_budget/tst_memory_budget.cpp` (`qtmodel.tst_memory_budget`): the texts and the setting by hand,
  and the gate over a synthetic `.ld` whose 4.2 MB estimate (worked out by hand from 01 §7.1) exceeds a 2 MiB budget:
  held with the cache never asked for a chunk, Continue, Cancel, `cancelOpens()`, two opens of one file, a close
  while an open waits, an open within the budget and an open without a gate.

## M4 followups: the loader, the track maps and the report model (D-210; `docs/milestones/M4.md` §5.1)

- `TrackMapService` with a `MathsService` and a `Dispatcher` (the application's) does every file operation on
  `MathsService::pool()` (Interactive) and none on the GUI thread (M3 item 23): `map()` on a cache miss gives
  nullptr and reads on a worker (`isLoading`; `mapChanged` when it lands, found or not; a project change drops
  the reads in flight); `setMap()` validates, replaces the cached map and announces it at once and queues the
  write, one per venue file in flight with the next one held and merged (newest map, backed up if any merged
  edit asked); a write that fails is a `logMessage` row, the venue is read back from its file, and the
  `TrackMapCommand`s it carried become failed and obsolete (QUndoStack drops them at the next undo or redo);
  writes still queued are made by the destructor; `venues()` answers from a scan on the pool
  (`venuesChanged`); a generated map reaches `onGui` only once the venue's own map is known. `mapStamp(venue)`
  identifies the cached map, `pendingIo()` counts what has not landed. Without a Dispatcher everything stays
  synchronous (headless tools, the M3 tests). Tests: `qtmodel.tst_track_map_io`.
- `SourceLoader`: the summary pass (`summary/<sourceId>/<first column id>`) posts `summariesReady(id)` when it
  has run, after `sourcePublished`, only while the source is open with the columns it summarised (M3 item 18;
  `qtmodel.tst_summaries_ready`). The phase-2 key holds the bound track map's identity
  (`/t<setTrackMaps generation>-<mapStamp>/i...`), `mapChanged` no longer bumps a global generation, no run
  starts while the venue's map is being read, and a sidecar edit that changes the Venue detail starts the pass
  (M3 item 31; `qtmodel.tst_track_sections_pass` `aVenueEditRunsThePassForTheNewVenue`). The serial guard of
  D-204 (d) has its own test, `aSupersededRunNeverPublishes` (M3 item 32).
- `ReportModel`: a delivery or a display change that keeps the table's rows and columns is one `dataChanged`
  over the table instead of a reset, so a view keeps its selection (M3 item 30;
  `qtmodel.tst_report_model` `aRefreshOfTheSameShapeKeepsTheSelection`); `ReportTable` (widgets) re-measures
  its columns on `dataChanged`.
- `SessionRestore::check` also waits while an open is held at the memory budget warning
  (`pendingBudgetOpens()`, D-207): Continue publishes it and Cancel announces it, and either runs the check
  again, so the restored selection includes a file continued later (`qtmodel.tst_restore_budget`).
- Not here: the components' rebuild on `summariesReady` (the widgets' Data path) belongs to the widgets
  follow-ups; the frozen `source_loader.hpp` comment still names the M3 key form `/t<map generation>`.

## M4 reload: changed source files and Reload (AC-M4-10; D-208; 06 §6 "Changed files")

- `SourceLoader` (`src/m4_reload/source_loader_reload.cpp`): `checkChangedFiles()` runs `formats::checkSourceFile`
  for every open source not yet stale; a Changed or Missing file marks it stale (until it is reloaded or closed)
  and `staleChanged(id, true)` follows, once, after the pass. `checkChangedFilesInBackground()` (the shell's
  activation check since D-217 (g)) runs the same re-stats on the pool and marks on the GUI thread
  (`fileChecksPending()`; `staleState(id)` says Changed or Missing). `reload(id)` (D-217 (e), amends D-208) keeps
  the source open: it writes its sidecar first (a failed write refuses the reload with `Io` and keeps the source)
  and refuses a Missing file (`NotFound`); it opens the path again beside the stale source, and when the new open
  publishes (`sourceReloaded(previous, source)`) the previous source is closed; a reload whose open fails or is
  cancelled leaves the stale source as it was (`pendingReloads()`). A reload of an unchanged file
  (`reloadSameContent`) is the same path. Every second the loader reads each open source's DecodeLog (D-217 (f)):
  new entries go out as `importLog` (Messages), and `ld.io_fault` marks the source Missing, `ld.source_changed` or
  `text.source_changed` Changed, at once rather than at the next activation.
- `source_reload.hpp` (new): `connectSourceReload(loader, selection, messages)` appends the stale source's Messages
  row (`staleSourceMessage`: Import, Warning, `open.file_changed`, the action `file.reload_source` with the id as
  32 hex digits, `sourceIdArgument`/`parseSourceIdArgument`) and restores the selection on `sourceReloaded`; the
  shell calls it beside `connectSourceLoader`, whose file another package owns.
- `DataSelectionModel`: `closeSource` parks what it takes from the selection for the `kParkedCloses` (8) newest
  closes, and `sourceReloaded(previous, source)` gives it to the reopened file: slots by RangeId, else by lap number
  and class (06 §5.6), else dropped with a hint; a Main the close moved elsewhere back to its slot; the source's own
  filter; its Data-window position (before the source that followed it). `closeAllSources` clears the parking.
- `DataWindowModel`: a source row answers `StaleRole` and adds a stale line to its tool tip; `refreshStale(id)`
  announces the row (`dataChanged`).
- Test: `tests/m4_reload/tst_source_reload.cpp` (`qtmodel.tst_source_reload`).

## M4 followups of the widgets (D-211)

- The channel resolution of `RenderService::prepareInputs`, `ValuesModel` and `ReportModel` falls back to the
  quantity's default display unit (`plot::defaultDisplayUnit`: a fraction stored in `ratio` shows in %) where the
  component names none of the channel's quantity, and fills `ResolvedChannel::displayUnitId`; `ReportModel` passes
  a report channel's `displayUnit`.
- `ValuesModel::setViewSource`: the view a values_list component with an unlinked cursor or datum reads its values
  at (the worksheet still gives the DistanceService).
