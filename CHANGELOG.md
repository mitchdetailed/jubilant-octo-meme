# Changelog

All notable changes are recorded here, newest first. The project follows semantic versioning from 0.1.

## Unreleased (M2)

- Fixes from the verification of D-190 (AC-M2-06): a save that fails part-way no longer moves the files of
  renamed or removed workbooks, unit maps and modules to `backup/` (until a save succeeds, `project.json` on disk
  still names them); an autosave whose journal file another instance deleted (a Discard on its recovery prompt)
  writes it again instead of skipping its unchanged edits (`Journal::ownFileExists`); clearing the journal deletes
  only the files it listed, never one another session writes meanwhile. Tests: the guarded-lines text-scan test,
  the maths module save test and the CLI `maths check` tests used fixed folders under the system temporary
  directory and failed when several processes ran them at once; each now creates a folder of its own. D-190 is
  listed in the Addendum A index of `DECISIONS.md`.

- Maths evaluation speed (D-191; AC-M2-10): the 20 maths channels of the RW-2 readiness set are ready in 28-29 ms
  instead of 113-127 ms on this machine (the two-frame budget, 33.3 ms, is met; D-183 is no longer needed), with
  every result unchanged bit for bit. The evaluator streams each plan through the extent in pieces of 16,384
  samples instead of whole-extent buffers, an expression channel's chunks are written and sealed as they are
  produced, and idle workers seal a channel's later chunks, so one channel alone is ready several times sooner
  (`derivative(smooth(...))` over 1.8 M samples: 64 ms before, about 3 ms now). Smooth, the two-point derivative,
  the RC filters, the elementwise loads and the chunk writes lost their per-sample overheads;
  `filter_lp(x, 5, -2)` over 21.6 M samples takes 225-234 ms instead of about 400 ms. `esses-maths-bench` adds
  `--digest` (a fingerprint of every result, to check that an evaluation change leaves results unchanged) and
  `--workers N`. CI: the `lint` job may run 45 minutes (its clang-tidy build was cancelled at 20).

- M2 review fixes, the last fourteen findings (D-190; AC-M2-06, AC-M2-11, AC-M2-13): a save that fails or is
  killed part-way no longer reports false conflicts at recovery (an entry whose file already holds it is not
  replayed, and a journal holding nothing else is deleted without a prompt), `project.json` is written only after
  every other document, clearing the whole journal is one folder rename and partial clears delete oldest first,
  journal files carry their session (`<seq>-<tag>.json`) so two sessions never replace each other's file, and the
  backup copy of a document is made before the document is replaced (a failed copy is reported, the save stands).
  Closing the Maths Editor saves the maths modules and their order only, leaving the project's other edits unsaved,
  undoable and journalled. Text files: a first line over 1 MiB is a Format error at once and rows are split only
  into the cells they can use (a hostile line no longer costs a cell per delimiter); sparse timestamped columns
  agree between the probe and the decode when rows are ragged (a missing cell is an invalid sample in both passes);
  both passes read a file through the page-fault guard in windows and a file changed on disk fails its next chunk
  instead of the process; a header-only CSV opens with no samples; a UTF-16 file of odd length no longer gains a
  row; a quoted field holding a line break is reported (`text.quoted_newline`); profiles and dictionaries refuse
  nested-quantifier patterns, dictionary patterns compile once and the sniff regex sees the first 16 lines. Opens
  are cancellable: closing the files or changing the project cancels the opens in flight and those waiting for
  the Mapping panel (`ImportOptions::cancel`, `SourceLoader::cancelOpens`). Sidecar edits are written by a
  Background task 500 ms after the first edit (coalesced, newest wins) and at once when the source closes.
  Autosave builds no journal entry when its set is unchanged, and the recovery prompt replays the journal once.

- M2 review fixes (D-185 to D-189; AC-M2-01, AC-M2-02, AC-M2-05, AC-M2-06): the maths engine evaluates a channel
  read only by a statistic at that channel's rate (statistics over the samples as logged); integer results are I32
  only when they provably fit, every other integer result F64 (no saturation), and an integer or Bool result with a
  declared non-base unit is stored as a real value (`choose(c, 50, 0)` declared `%` reads 50 %, not 0 %); infinite
  results are invalid samples; a literal factor (`/ 3.6`, `/ 9.81`) no longer suggests a wrong result unit; a
  table axis is checked against its unit; flipping the `-2^2` switch recomputes only the expressions it changes;
  Filter and Scale & Offset items carry no `W-UNIT-MISSING`, accept 0 and 1 points (a window of 2) and, for a
  temperature, scale in the offset's unit (80 °C × 1.02 = 81.6 °C; D-186); per-lap statistics step between laps;
  `previous_sample` of an item's own name is `E-CYCLE`; a marker edit that moves the math window's lap
  re-evaluates the statistics over it; variance against another source's lap reports `E-REF-SOURCE` instead of
  silent invalid values; timestamped channels get a whole-Hz rate. `esses-cli maths check` reports what
  `MathGraph` finds (cycles, synthesis and compile errors, the shadowing order; D-188); `esses-cli export` reports a
  failing maths input instead of "Calculating". The journal keeps one snapshot per session: an undone edit is not
  replayed, Save journals first so a failed save loses nothing, "Later" keeps autosaving this session's edits
  without deleting the deferred journal, Recover adopts the journal, and a workbook renamed only in letter case
  keeps its file (D-187). The workbook keys CTRL+1..CTRL+0 and CTRL+F11/F12 work; seven file and Units List keymap
  rows move to M3 (D-189). The case corpus grows to 256 files (every function and arity, the overflow cases, a
  symmetric zero-phase pulse long enough for the end effect to decay); the corpus runner prints its count; the
  60-lap marker rebuild (AC-M2-05) is a Release bench row (`analysis.marker_rebuild_60lap`, 5 ms). Fixed for CI:
  a clang-tidy finding in `ProjectFile::autosaveMinutes`, a clang-format violation, the `store.column` concurrency
  test and the Qt test scratch directories made deterministic under a parallel ctest, `maths_corpus_check.py` with
  a relative `--cli` on Windows, and the round-trip tests no longer skip on a stub.

- M2 integration (07 §5; AC-M2-01, AC-M2-05, AC-M2-09, AC-M2-10, AC-M2-11, AC-M2-13, AC-M2-14; D-180 to D-184): the shell
  wires the M2 editors (the Mapping panel above the worksheet area with the loader's hand-off on, so unknown units
  stop at it; the Channel Status dock behind CTRL+U and View > Channel Status; the Maths Editor CTRL+M, Channel Editor
  CTRL+E with its Aliases and Mappings editors, Details CTRL+D and the Lap Editor as modeless dialogs; SHIFT+F7 inserts
  a beacon at the cursor; the nine active-channel keys), and 19 keymap rows become implemented with their tests.
  `esses-cli export [<project>] <log>` writes logged and maths channels as CSV with the project's maths, mappings,
  unit maps and display units (`--channels`, `--range`, `--format csv|motec.csv`, `--rate`, `--units`);
  `esses-cli fixture diff` explains where a fixture differs (per channel the first differing sample and the largest
  ulp distance, JSON Pointers into the probe, markers and laps expectations). The maths check of the maintainer's own
  maths (private, counts only) reports 0 errors: a unitless channel read in `[ratio]` no longer warns, and the unit
  table gains `l/100km` (Fuel Consumption) and `psi (sg)`. New shell walkthroughs `ui.tst_lap_editor` and
  `ui.tst_mapping_panel`, ctests `cli.export.project` and `cli.fixture_diff.explains`, two more seeds of the undo
  property test, and CI steps for the maths case corpus (core) and the private corpus counts (lint). Fixed: a
  `WorksheetCanvas` destroyed while it had the focus could run a slot after its destructor.

- Data editors and dock work for M2 (`esses_widgets`, `src/editors_data/` and `src/docks/`; 07 §5.1; AC-M2-05,
  AC-M2-11; D-179): the `MappingPanel` (follows the loader's pending opens with a queue; rows with sample values,
  combo-box editors for quantity, unit and rule; Scan, Resolve as one undoable "Resolve mapping" step with the
  ticked rows saved to the project's unit map and analysis names as mappings, Save, Open Anyway, Cancel); the
  `LapEditor` (laps and ignored beacons, provenance, Stretch column and report, inserts by session time, by lap
  time and at the cursor, per-beacon offsets, the beacon offset by distance, trust, Restore Logged Laps; SHIFT+F7
  as `cursor.insert_marker` and the toolbar's lap-stretch toggle kept in step); the `DetailsEditor` (typed cells
  with units, the original kept, Restore Original, sort, Copy as TSV); Data-window filters in the context menu with
  the lap-number editor and filter icons; the Channels-window word-prefix search with loose matching from the
  project. Five new Qt Tests.

- Maths editors (`esses_widgets` `src/editors_maths/`; 07 §5.1; AC-M2-12 in the window; D-177): `MathsEditor` (CTRL+M)
  with the modules, items and item panes of 03 §11 (global modules with enable, condition, add, delete, move,
  import and export; local maths per open source; transient maths listed; Expression, Constant, Filter and Scale &
  Offset pages), the live diagnostics line 150 ms after a keystroke (Errors red, Warnings amber, Info grey, spans
  underlined, the full list), the `[unit]` popup with the quantity's units, Insert Range and Insert Channel, hover
  help with the precedence rule and the `unaryMinusAbovePower` setting, Apply and item switch as one undo step,
  close and ESC refused on an Error with Revert, multi-select cut/copy/paste keeping names, Recalculate, the status
  column from `MathsService`, and the auto-save on close through the project's save; `ChannelStatusWindow` (CTRL+U)
  with the filter box, "Errors only", Copy Table, sorting, the resolution trace in a detail pane and the
  double-click to the Maths Editor, Alias or Mapping editor; `ChannelEditor` (CTRL+E, one channel at a time) with
  unit, limits, DPS, colour and interpolation overrides as undoable commands, the unit rule across open files and
  the Corr Dist unit, and the `AliasesDialog` and `MappingsDialog` editors (cycles refused); `ActiveChannelCommands`
  for the nine M2 `channel.*` rows (next/previous active channel, hide, remove, edit maths, status, transient
  filter, scale and offset, zero at cursor in the display unit with the decimals kept). New Qt Tests
  `widgets.tst_maths_editor`, `widgets.tst_channel_status_window`, `widgets.tst_channel_editor` and
  `widgets.tst_active_channel`.

- Options, schemes and the shell's M2 wiring (`esses_widgets`; 07 §5.1; AC-M2-06 in the shell, AC-M1-06 rows for
  the new commands; D-178): `OptionsDialog` (Tools > Options) with the General, Analysis, Colours, Fonts and Keys tabs
  of 06 §13.7 (only the changed project options written, as one undoable "Change options"; user settings for the
  language, the toolbar scheme selector and following the OS; invalid fields refuse Apply and OK; read-only Fonts and
  Keys with keymap Import/Export/Reveal); `SchemeSelector` (shipped and user schemes, live switch, follow OS
  light/dark, Duplicate/Import/Export, the application palette from the scheme); the `RecoveryDialog` ("Recover
  unsaved changes?" with Recover, Discard and Later) before the session restore of the previous data and selection;
  the shell adopts the qtmodel wiring (`connectProjectSession`, `connectMathsSelection`, user text profiles,
  `LapStretchService` with the toolbar and Data-menu toggle, `SessionRestore`), Edit > Undo/Redo and toolbar buttons
  on the session's undo stack with component edits as undoable commands, CTRL+Z in text fields kept as text undo,
  save-or-ask when leaving a dirty project (exit or another project) and the view state written on exit; `esses -w`
  hands its `-l` logs to the session restore. Keymap rows `edit.undo`, `edit.redo`, `tools.options`,
  `data.lap_stretch_toggle`, `view.messages_window` and `view.reset_docks` implemented with their `KEYMAP_TEST`
  rows (menu-only rows through their action); `check_keymap_coverage.py` accepts implemented rows of the milestone
  in progress. New Qt Tests `widgets.tst_options_dialog`, `widgets.tst_scheme_selector`, `widgets.tst_shell_m2` and
  `ui.tst_project_recovery` (AC-M2-06).

- Application model for M2 (`esses_qtmodel`; 07 §5.1; AC-M2-09, AC-M2-06/11/12 at the model level; D-032, D-161,
  D-172 to D-175): undo commands for every D-032 edit of 0.1 (workbook list, worksheets, component paste and delete
  with a clipboard payload, merged resizes, channel documents, unit maps, maths modules, options, every sidecar edit,
  the Data window's trust through the selection, the Mapping panel's decision as one macro) with the AC-M2-09
  property test (30 randomised sequences equal to an independent replay; view state never on the stack);
  `ProjectSession` loads and saves every project document (unchanged files not rewritten, retired files backed up,
  unreadable files never overwritten), the journal autosave on a timer and the recovery flow (prompt hook or
  `recoveryAvailable()`, replay, discard, later), dirty state from the stack's clean index; the full `MathsService`
  (project modules with only the edited cone re-versioned, compat switches, mappings and aliases, local maths from
  the sidecar, transient modules, `esses.gps_corrections` where GPS is logged, the selection and distance indexes
  for `SelectionDependent` nodes, the 03 §7.3 status per item, Recalculate, Channel Status rows, the live check) with
  `LiveDiagnostics`; `ChannelStatusModel` (items, built-ins, constants, aliases, mappings, missing references, every
  trace kind); the Mapping-panel hand-off in `SourceLoader` (task A ends at the panel, task B with the decision,
  Cancel, Open anyway, user text profiles); Data-window filters persisted in `project.json` with the lap-number
  editor messages; the word-prefix channel search with loose matching and the Alias and Constants categories;
  `LapStretchService` and the stretch toggle; `SessionRestore` and `connectProjectSession`; `edit.undo`/`edit.redo`
  and `data.lap_stretch_toggle` command handlers. Six new Qt Tests.

- Maths graph (07 §5.1; AC-M2-03, AC-M2-04, AC-M2-10 bench; D-022, D-139, D-140, D-171): `MathGraph::define`,
  `remove`, `setEnabled`, `setModules` (global modules in project order, local and transient maths),
  `setResolverTables`, `setCompat`, `setSelection`, `setDistanceIndexes`, `invalidate`, `trace`, `nodes()` and
  `status(NodeId)`: one shadowing chain per name (built-in outputs, local, global, transient), the first definition
  per module winning (`W-DUP-IGNORED`), disabled elements skipped, self-maths reading the element before it, module
  conditions over details (`E-CONDITION`), the Scope order of 03 §3.3 with mappings, aliases (`W-ALIAS-FALLBACK`)
  and constants across modules (`W-CONST-AMBIGUOUS`, `W-CONST-SHADOWED`), cycles with the path on every node
  (`E-CYCLE`), item kinds expression, filter, scale_offset (offset as a delta), table2d and `builtin` (a per-source
  settings override), versions that hash exactly what a node reads (an edit re-evaluates only its cone), and
  `SelectionDependent` keys with a column per selection. Evaluation through `MathsScheduler` (one task per node,
  share key `.../all`), tasks that compile a fresh plan from a frozen scope, sealed into the `ChunkCache` by
  `PlanProvider` (now with a vectorised path for chunks without invalid samples). `esses.gps_corrections` v1 with its
  zero-fix step always on (`Disabled (no GPS channels)` without both channels) and the settings schema
  `builtin-esses.gps_corrections-1` with examples. `bench/maths_bench` gains the AC-M2-10 row over RW-2 (the 20
  expressions of `bench/workloads.md`, logical processors and workers recorded) and the recorded `filter_lp` order -2
  row over 21.6 M samples; `--nodes` prints the time of each output.

- Maths kernels and functions (07 §5.1; AC-M2-02; D-022, D-080 to D-082, D-169, D-170): every intrinsic of 03 §5
  registered with a `FunctionManifest` (arguments and arities from the language's signature table, unit rules,
  descriptions) and a kernel for every non-elementwise class: `smooth` and `derivative` (points or seconds, odd and
  even windows, truncated edges, the with-respect-to form), the RC and Chebyshev/Butterworth filters with every
  order including zero phase, designed in-tree without Iir1 (D-169), `integrate`, `integrate_dist` (Corr Dist as a
  plan input), `stat_*` per segment or over the math window, `running_min/max`, `flip_flop`, `edge_delay`,
  `time_valid`, `time_shift`, `pulse_width`, `previous_sample`, `range_is`, `range_change`, `variance_time/dist`
  and the `table2d` Table kernel. `Kernel::status` reports failures known only at evaluation (`E-ARG-VALUE` at or
  above half the rate, `E-RANGE-UNKNOWN` naming the paths; `RangeTables::contains`/`paths`). The case corpus: 238
  `tests/maths/cases/*.case.json` (schema `maths-case-1`) generated by the independent reference
  `tools/maths_ref.py` and run by the Catch2 case runner; schema `mathfn-manifest-1`; property tests of the windows
  and filters; `docs/maths-compat.md` gains a row with its case ids for every kernel reading.

- CSV import (07 §5.1; AC-M2-13, AC-M2-11 at the registry level; D-028, D-029, D-044, D-167, D-168): the
  `TextEngine` of 05 §7 (pass 1: encodings with BOMs, UTF-16 and cp1252, LF/CRLF/CR lines, delimiter, decimal comma,
  preamble rules and captures, header and units rows, units in names, time column and unit, rate from the details
  or inferred, uniform or timestamped rows with the last-wins time rule, sparse `hold` and `timestamped` columns,
  text columns as enumerations, lap-counter and beacon-row lap sources; pass 2: one sweep per slab for every column
  of a source, siblings sealed with `ChunkCache::insertGroup` through the new `store::WeakColumn`), the shipped
  profiles `motec.csv` and `generic.csv` with their dictionaries (registered by `ImporterRegistry::withBuiltins`),
  `TextProfile`, `Dictionary` and `UnitMappingProfile` read and written canonically (schemas `importer-profile-1`,
  `dictionary-1`, `unitmap-1` with examples), the open pipeline's step 4 (column hints and dictionary, unit maps,
  load-time mappings), timestamped channels in the pipeline, `addProfile`, `load(…, MappingHook)` and `resume` (the
  Mapping-panel hand-off), and the `motec.csv` and `csv` writers. Four self-authored CSV fixtures
  (`tools/gen_text_fixtures.py`, oracle kind `generator` in fixture-1), `fixture verify` for timestamped channels,
  the fuzz targets `fuzz_csv_sniff` and `fuzz_text_profile` (seeds and dictionary in CI), `bench/formats_bench`
  (300 MB, 100 channels: probe and first data), `docs/formats/text.md` with every `text.*` Import Log code
  (`tools/check_importlog_codes.py` reads it). The vendor-mark allow-list gains the CSV format-name literal (class c).

- Maths language (07 §5.1; AC-M2-14, the language parts of AC-M2-01 and AC-M2-02; D-164, D-165, D-166): the
  `Lexer` and the Pratt `Parser` of 03 §2 (quoted names with `[unit]`, word operators, integer vs decimal
  literals, comments, `-2^2 = -4` with `MathsCompat::unaryMinusAbovePower` and `I-COMPAT-POW`, error recovery,
  a nesting limit), `print` (canonical form) and `Ast::sourceHash`; the `TypeChecker` of 03 §3 (kinds, units,
  dimension vectors and scales, constant folding, every diagnostic code of 03 §2.5 and §3.2, warnings never
  blocking); `compile` into plans (fused elementwise `EwKernel` stages, intrinsic stages through
  `makeIntrinsicKernel`, the result unit to base conversion, inputs, dtype, `defHash`) and the 0.1 whole-node
  `evaluateWhole` with `MathInputGatherer::fill`; `MathModuleIo` (maths-1 read and written canonically).
  `esses-cli eval` (`--json` with rate, unit and values; `--csv`; `--range`; `--unit`; `--compat`),
  `maths check --json` (per-item status and counts) and `maths functions`; `tools/maths_corpus_check.py`
  (AC-M2-01 counts); `fuzz_maths_parser` with its dictionary and seeds in CI; `docs/maths-compat.md` with the
  language readings and the known gaps.

- Persistence (07 §5.1; AC-M2-05 core, AC-M2-07, AC-M2-08): `overrides-1`, `aliases-1` and `mappings-1` read and
  written canonically (`ChannelOverrides`, `Aliases` with the resolver's alias-cycle rule, `Mappings`);
  `SchemaRegistry` over every schema (JSON pointer of every error, three `$schema` forms); `Migrations` with the
  first real step, `project-1` 1 -> 2 (Data-window filter keys are `default` or `xxh3:<hex>`, D-162); the
  write-ahead `Journal` (`journal-1` entry files written atomically, replay of the newest entry per path across
  sessions with `baseHash` conflicts, damaged files skipped, the recovery hook `Journal::recover`); every project
  document loads through one pipeline (header, migrations, typed reader, `SchemaRegistry` over the written form;
  D-163) and keeps a newer minor's `schemaVersion` or refuses it as `Unsupported` (R-38). New schemas
  `overrides-1`, `aliases-1`, `mappings-1`, `journal-1`, `maths-1` (03 §4.1) with canonical examples;
  `project-1` minor 2. `LapStretch::apply` sets the index's stretch (`DistanceIndex::setStretch`), with
  `LapStretchSettings` and the per-lap report. `esses-cli validate` migrates older minors first and prints the
  steps, refuses a newer major at `/$schema`, and gains `--all`/`--root`/`--exclude`; the `lint` job runs
  `validate --all`. `esses_model` and `esses_analysis` spell `JsonDoc` where M1 wrote `model::OpaqueJson`.

- M2 contracts (07 §5.1; `docs/milestones/M2.md` §2): public headers and types of every M2 class with stubs marked
  `// M2-STUB` (maths language, kernels and graph; text importers, dictionaries, unit maps and the Mapping-panel
  hand-off; overrides/aliases/mappings, `SchemaRegistry`, `Migrations`, `Journal`; `LapStretch`; the undo commands,
  the full `MathsService`, `ChannelStatusModel`; the Maths, Channel Status, Channel, Mapping, Details, Lap and
  Options editors and the scheme selector), each library's `README.md` listing the stubs to replace. Final parts
  with tests: the intrinsic names and kernel classes, `FunctionRegistry`, the data-less `Scope`, `ArenaView`, the
  sidecar annotation/local-maths/restore-logged edits, `ChannelOverrides` editing, `Mappings::toTable`,
  `LapStretch::factor`, the undo command bases and the named document/sidecar commands.
- `esses_qtmodel` links Qt Gui for `QUndoStack`/`QUndoCommand` only (D-161, proposed): `tools/check_layering.py`
  rule 6 refuses every other Qt Gui header there (the configure step records Qt Gui's header directory in
  `esses_targets.json`); the configure negative test now refuses Qt Widgets in `esses_qtmodel`
  (`lint.configure_check.qt_module_qtmodel_widgets`) and accepts Qt Gui (`qt_module_qtmodel_gui`).
- Build: `esses_formats` and `esses_model` glob their sources (and `esses_model` its tests); the text importers get
  their own test executable (`formats_text.*`); `esses_widgets` gains the glob roots `editors_maths`, `editors_data`
  and `editors_options`. `MathItem::module` is spelled `moduleId` (03 §4.1) and `DisplayDefaults`, `MathRangeGeom`
  and `MathSelection` moved to `math_item.hpp` and `math_selection.hpp`.
- Private material: `tests/maths/user_modules/`, `tests/maths/oracle/` and `*.private.json` are git-ignored and
  refused by the `lint` job's forbidden-paths step.

## Unreleased (M1)

- Test fix: the AC-M0-11 priority-order test uses one worker, so a worker pre-empted between claiming an Interactive
  task and recording its start cannot make Background tasks look earlier (flaky on a loaded windows-2022 runner); it
  now also checks that every Interactive task starts before any queued Background task.
- DCO sign-off suspended while the maintainer is the only contributor (D-160): the `dco` CI job runs only when the
  repository variable `ESSES_REQUIRE_DCO` is `true`; contributor docs and the pull request template say so.
- Pull request #1, first `qt-app` ubuntu run (Qt 6.12, offscreen): shipped data is found at `<exe dir>/data` on every OS
  before the installed location (D-159), which fixes `esses-render`, the bench smokes and render parity on Linux; the
  ADS backend sets `Qt::AA_DontCreateNativeWidgetSiblings` on Linux when the platform is not xcb, because ADS's
  floating container calls `winId()` there, which made the main window's widgets native and crashed Qt's offscreen
  backing store on destruction (vendored ADS unchanged); static strips paint into the shared buffer with a clip
  instead of a translated strip image, so text keeps one-pass coordinates under FreeType's subpixel positioning
  (strips equal one pass exactly; new `subpixelTextEqualsOnePass` test).
- CI: the DCO sign-off check moved from `lint` into its own `dco` job (pull requests only, still required), so a
  missing sign-off no longer skips the remaining lint steps (clang-tidy, layering, REUSE, schema validation).
- Pull request #1 CI fixes (run 38202115135; `docs/milestones/M1.md` §2.7): the `lint` DCO step checks the pull
  request's own commits (`--no-merges`, from the PR head, not from GitHub's synthetic merge commit) and still
  requires a `Signed-off-by:` line on each; `MappedFile::guarded()` on POSIX keeps the descriptor and fails with
  `Io` when the file is now shorter than the view, since POSIX leaves such reads unspecified and macOS serves
  resident pages without `SIGBUS` (regression test for a truncation that touches no removed page); the
  Messages and Channels filters use `beginFilterChange()`/`endFilterChange(Direction::Rows)` from Qt 6.10
  (`invalidateRowsFilter()` is deprecated for 6.13 and warned about by 6.12), and the main-window drag test uses
  Qt 6.12's `QPointF` constructor of `QDragEnterEvent`, both under `QT_VERSION_CHECK` so Qt 6.7 still builds;
  `qt-app` installs Qt 6.12.0 with aqtinstall pinned to a commit of its master branch (`AQT_SOURCE`, py7zr
  1.1.3), because aqtinstall 3.3.0 cannot read the per-toolchain Windows repository layout of Qt 6.11 and later;
  on Windows `esses` attaches to the console of the terminal that started it before printing `--version`,
  `--help`, `--dump-commands` or a parser error, and detaches before the GUI runs (new ctest
  `app.version_console` runs it inside a pseudo console); `qtmodel.tst_source_loader` accepts the phase-2
  replacement of the published snapshot that a fast build completes before its first check.
- Qt Advanced Docking System (D-153, decided by the user on 2026-10-11; AC-M1-13, spike S-3): ADS 5.1.1 vendored
  unmodified under `third_party/qtads` (LGPL-2.1-or-later; archive SHA-256 and tree digest in
  `third_party/README.md`) and built by its own CMake project as the shared library `qtadvanceddocking-qt6`
  (`esses::qtads`, `cmake/qtads.cmake`) into `<build>/bin`, beside every executable and test. `AdsBackend` is the
  default `DockHost` backend: the worksheet area as ADS's central widget, tabbed registry areas, floating
  windows, auto-hide side bars with the fly-out of 06 §3.3 (collapse on focus loss), perspectives, Reset Dock
  Windows, and changes made with ADS's own controls. The `QDockWidget` backend stays: `ESSES_DOCK_BACKEND`
  selects it at run time (environment) or builds without ADS (CMake option). New `IDockBackend::setCentral` and
  `libraryVersion`, `DockHost::availableBackends`/`defaultBackendName`/`makeBackend`; `layout-1` writes
  `adsVersion` for ADS perspectives; `DockHost::focus` activates the dock's window when another (a floating
  dock) is active, on both backends. `widgets.tst_dock_host` runs every case on both backends. Only
  `esses_widgets` and `esses` may link ADS (`cmake/qt-free-check.cmake`, `tools/check_layering.py` rule 5,
  three new `lint.configure_check.ads_*` cases). CI: `qt-app` and the hosted bench install `libxcb1-dev`, and
  `qt-app` checks that the library lands beside the executables. `NOTICES.md` (LGPL notice, source, how to
  replace the library), `REUSE.toml`, `LICENSES/LGPL-2.1-or-later.txt`, `docs/spikes/S-3.md` (Windows passed;
  Linux and macOS from CI), `DECISIONS.md` D-153 decided, 08 §3.7.3 as built.
- First CI run fixes (`main` at 7c5b615, run 38163002936; `docs/milestones/M1.md` §2.5): the Clang and GCC
  warnings MSVC does not raise, in M0 and M1 code and tests (locals shadowing a field renamed, every implicit
  `float` to `double` widening written as a cast, GCC 13 `-Wdangling-reference` and
  `-Wmissing-field-initializers`, a test helper no longer named `signal`); `BeaconPairRule` parses numeric
  details with fast_float, since Apple Clang 15's libc++ has no floating-point `std::from_chars`; the `lint` job
  installs clang-format and clang-tidy 19.1.5, the maintainer's version, so that local and CI formatting agree,
  and the clang-tidy findings that only a libstdc++ build shows are fixed (unchecked `std::optional` access, a
  read of a `Future` result before it is set now fails loudly, feature macros without values); the `.ldx` writer
  splices verbatim elements into its output in one pass, so an element that itself contains the reserved
  placeholder instruction can no longer capture another element's placeholder and make the output malformed
  (the `fuzz_ldx` abort), with a regression test, a fuzz seed and dictionary entries.
- M1 acceptance review fixes (`docs/milestones/M1.md` §3), each with a regression test:
  worksheet switches keep the hidden worksheet's components and their Static images (D-058; F11/F12 and
  1-9 no longer rebuild every component, and the shown worksheet selected again does nothing); ESC in the graph
  ends a rubber band before `view.return_focus` (`ComponentWidget::cancelTransient`); the rubber band follows
  button-less moves (mouse tracking while it is open); fractional wheel deltas add up to notches; the key
  router no longer takes a reused event address for a propagation; phase-2 stint/pitStop annotations survive a
  sidecar commit made while they run (merged into the newest snapshot, task key with the Corr Speed version,
  older runs cancelled); lap-index builds are raised to the requester's band and keyed by the whole
  `DistanceIndexKey`; maths chunk keys carry the node version and a re-versioned node cancels its superseded
  evaluation; maths versions resolve inputs by logged name too and include the channel status;
  `DistanceIndex::timeAt` is the exact first reach between the 0.25 m steps (D-157); `fastestWheel` starts at
  the earliest wheel; regime-A M4 columns break at excluded (unindexed-lap) samples; marker order puts `EOL`
  before `SOL` at equal times, the beacon-pair rule closes an outing still open at a new `SOL` and ends a
  missing final `EOL` at the data end (D-158); one Budget row per chunk-cache warning, window-owned Messages
  sinks (`AppRuntime::removeSink`) and budget hook; `AppRuntime` restores the previous `ThreadRole`;
  `ValuesModel` no longer accumulates connections; `selectionKeys` caches project-relative paths; the
  `SidecarCommit` hook guards the loader. `TaskPool::queuedTasks(prefix)` (diagnostics and tests).
- Tests: the GUI thread is marked NoBlock in every shell, dock and `tests/ui` runtime with an assertion recorder
  checked in `cleanup()` (AC-M1-12 at shell level); the walkthrough also runs on a synthetic log with unequal
  laps (exact CTRL+F lap, distance versus time alignment); the keymap suite checks N/P's kept zoom and cursor
  and no wrap, and runs every global row from the Data window too; the coalescing and prefetch tests wait for
  an idle throttle / a quiet first log instead of fixed times (no flakes in 25 runs under a parallel-16 load).
- `bench.yml`: the reference gate's `bench_compare.py` step skips with a notice until the script exists (the
  ctest `--gate` step is the gate).
- Design records: `DECISIONS.md` A.12 (D-153 to D-158, proposed; D-153 needs the user's choice on ADS), 03 §9.1
  and 07 §4.1 for the `Speed` alternate (D-154), 07 §4.5's demo names the benchmark executables (D-155), 02 §4.3
  behaviour 8 (D-158), `docs/spikes/S-3.md` (not run), `docs/milestones/M1.md`.
- M1 walking skeleton (07 §4): `esses_analysis` (`RuleRunner`, `BeaconPairRule` with stint/pit annotations,
  `WholeOutingRule`, `DistanceIndex`, `CorrectedSpeed`/`CorrectedDistance`, the fastest-lap rule),
  `esses_maths` (minimal `MathGraph`, `PlanProvider`, the built-in `esses.corrected_speed_distance` module and its
  settings schema), `esses_project` (project, workbook, component, scheme, keymap and profile documents with
  schemas `project-1`, `workbook-1`, `scheme-1`, `keymap-1`, `profile-1`, `rule-1`), `esses_plotcore` (view
  state, change masks, `DomainMapper`/`DistanceChain`, exact-M4 `TraceBuilder`, layout, hit testing,
  `DrawList`, `TimeDistanceBuilder`), `esses_qtrender` (`RasterBackend`, `LayerCache`, `OffscreenRenderer`,
  scheme fonts; `esses-render`), `esses_qtmodel` (dispatcher, runtime, selection, view models, services,
  commands, keymap, settings, messages), `esses_widgets` (dock host with the `QDockWidget` backend, main
  window, worksheet canvas, `time_distance` and `values_list` components, Data, Channels, Values and Messages
  windows, the key router), `esses` (the GUI), shipped circuit profile, rules, light/dark schemes, default
  workbook and keymap, golden fonts, `DrawList` and pixel goldens, `tests/ui` walkthroughs, benchmarks
  (`esses-plot-bench`, `esses-open-bench`) and the `qt-app` and `bench` CI jobs, `CLAUDE.md` and `AGENTS.md`.

## Unreleased (M0)

- M0 acceptance review fixes (`docs/milestones/M0.md` §3): GCC 13 `-Werror` portability (comment, missing
  initialisers, dangling-reference heuristics) and `fast_float` for floating-point text (Apple Clang 15);
  clang-tidy clean `esses_formats`; `.ld` reader: C-series record rules keyed on the logger layout, tighter [mi2]
  fingerprint, work budgets for shared gap/aux blocks and the enumeration scan, linear duplicate renaming,
  reversed gap pairs and the `ld.gapConvention` option, cut UTF-8 tails, F64 for scaled integers beyond F32 and
  for u32, the all-zero terminator rule, device-event array bounds, new Info codes `ld.gap_block_shared`,
  `ld.gap_convention_shifted`, `ld.enum_index_duplicate`, `ld.enum_ref_ignored`; Esses' own device-event labels;
  `Column` exception safety and in-order builds for sequential providers; share-key tasks whose cancellation
  fired are not joined; JSON nesting limit (256); `fixture add` requires `--licence` for public fixtures and
  defaults `oracle.validity` to `finite-only` for the reference decoder; `bench/workloads.md`;
  `docs/milestones/M0.md`.
- Build (phase B): vendored-library targets `esses::nlohmann_json`, `esses::json_schema_validator`,
  `esses::cli11`, `esses::fast_float`, `esses::catch2` (Catch2 v3.7.1 amalgamated with its default main) and
  `esses::catch2_nomain`; all tests moved from Catch2 v2 to v3 and Catch2 v2 removed; test discovery through
  Catch2's JSON reporter, so test names with `;` and other special characters register correctly (names a test
  spec cannot select exactly are refused); per-component switches `ESSES_WITH_UNITS/MODEL/FORMATS/CLI`;
  provenance of every vendored library in `third_party/README.md`, `NOTICES.md` and `REUSE.toml` updated.
- Repository skeleton: CMake presets, prefix table, target helpers with the layer check, configure-time
  Qt-free check with negative tests, vendored pugixml/xxHash/Catch2 targets, CI workflow stubs, governance
  and clean-room files, vendor-mark and layering lint scripts.
- `esses_base`: time base, exact 128-bit helpers and `Rate`, `Result`/`Error`, `Uuid`, `Hash128`, UTF-8 and
  case folding, `CancelToken`, `Future`, `TaskPool`, `Executor`, `ThreadRole`, `MappedFile`, `ByteReader`,
  `AtomicFile`, `Paths`, developer log, version stamping.
- `esses_store`: `Chunk`/`ChunkBuilder` (F32/F64/I32 and I64 time chunks, validity modes, NaN downgrade),
  factor-4 `MinMaxPyramid` with directional float rounding, `SummaryPyramid`, the exact-M4 range query
  (hierarchical boundary descent), `IChunkProvider`, `ChunkCache` with budget tracking and a warning hook (no
  eviction before M5), `ChunkHandle` pins, `Column` (Rule W `require`, `tryPin`/`prefetch`, `buildSummary`),
  `UniformSeries`/`TimestampedSeries`, `Resampler`/`ResampleView` with the four interpolation policies,
  `InMemoryProvider`.
- `esses_units`: `Dimension` (with `pow`/`unknown`), affine `Converter`, `Quantity`/`Unit` and `UnitRegistry`
  (exact, case-folded and normalised lookups, quantity-scoped lookups, family-checked conversions, delta
  quantities, `byDimension`), loaded from the new `data/units.json` (29 quantities, 111 units from the SI
  Brochure and NIST SP 811) and an optional `units.user.json`; `schemas/units-1.schema.json` gains `toBase`,
  `caseSensitive`, `note` and quantity `aliases`.
