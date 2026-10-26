# Changelog

All notable changes are recorded here, newest first. The project follows semantic versioning from 0.1.

## Unreleased (M4)

- The M4 contracts (D-206, `docs/milestones/M4.md` §2-3): the headers and `M4-STUB`s of the wave-1 packages
  (budget, reload, hidpi, followups) and the wave-2 placeholders (about, docs, soak, packaging), with a file
  ownership table so parallel work never edits the same file. The shell's M4 wiring goes through one hook:
  `MainWindow` creates a `ShellExtension` per package (`BudgetShell`, `ReloadShell`, `HiDpiShell`,
  `FollowupsShell`, `AboutShell`, `DocsShell`) and calls it at fixed points of its life; `SourceLoader` gains a
  marked section per package with private hooks in `published()`, `close()` and `cancelOpens()` and nested
  per-package state. New names: `formats::ResidentEstimate`/`estimateResident`/`estimateChannel` and
  `SourceFileState`/`checkSourceFile`; `qt::OpenBudgetCheck`, `PerformanceSnapshot`, `PerformanceMonitor`,
  `budgetFromSettings`, `budgetWarningText`, `performanceLine`/`performanceDetails`; the `SourceLoader` budget gate,
  stale-source and reload members with the signals `budgetExceeded`, `staleChanged`, `sourceReloaded` and
  `summariesReady`; `Message::altAction` and `MessagesModel::AltActionRole`; `DataWindowModel::StaleRole` and its
  stale query; `DprWatcher`, `ComponentWidget::recheckDevicePixelRatio`, `DockHost::windowOf`;
  `MessagesWindow::invokeAltAction`/`showPerformance`; `AboutDialog`, `buildInfo`, `versionReport` (what
  `esses --version` prints). The packages below replaced every stub (Help > About and the keymap's two M4 rows,
  `file.reload_source` and `help.contents`, among them); the top-level CMake adds `tests/soak/` and
  `tools/packaging/` once they exist, and
  `libs/formats/tests/m4/**` is one Catch2 executable (`formats_m4`). Contract tests: `formats_m4.m4 contract: ...`,
  `qtmodel.tst_m4_contract`, `widgets.tst_m4_contract`.
- The memory budget warns before a file is loaded (AC-M4-02, 07 §7.1, 01 §7.2; D-207). Opening a log whose
  resident estimate (`formats::estimateResident`: every logged channel in the chunk layout, raw samples, the
  factor-4 pyramid and a bitmap per chunk) exceeds the chunk cache's budget stops before anything of it is decoded:
  the Messages dock shows a Budget warning with the estimate, the budget and what is in use, with Continue Opening
  (the row's action) and Cancel Opening (its alternate action, SHIFT+ENTER, SHIFT+double-click or the row's context
  menu; `file.continue_open`, `file.cancel_open`, no keymap rows). Continue publishes the file as any open; Cancel
  drops it (`open.budget_cancelled`). The budget is Options > General "Memory budget" (settings.json
  `memory.chunkCacheBudgetBytes`, at least 1 GB; default a quarter of the RAM). The status bar gains a Performance
  field ("Resident 412 MB of 4.0 GB"; click for Messages) and the Messages dock a Stats line (resident and pinned
  bytes, chunks, hits, misses, the pool's queues), both from `qt::PerformanceMonitor`. `bench/open_bench` gains
  `open.resident_rw2_20maths`: RW-2 + the 20 maths channels of the readiness set peak at 823.6 to 831.9 MiB
  resident (median of 5, two sessions; chunk cache 527.3 MiB) against 1 GB. Tests: `formats_m4.budget: ...`,
  `qtmodel.tst_memory_budget`, `widgets.tst_budget_shell`, `widgets.tst_options_dialog`, `ui.tst_budget_warning`.
- Changed source files are detected and reloaded (AC-M4-10, 02 §8 AC-4a, E-5; D-208). When the application
  becomes active the shell re-stats every open log (`formats::checkSourceFile`); a file changed on disk or gone
  marks its source stale: an amber badge on its Data-window row, a Messages row (`open.file_changed`) whose action
  is the new File > Reload File (`file.reload_source`, keymap row implemented; also in the Data window's context
  menu), and no further chunk decoded from it (the providers' own check). Reload closes the source and opens it
  again through the open pipeline: the sidecar is re-applied and the selection comes back by RangeId, with its slot
  positions, the source's filter and its Data-window place (`SourceLoader::reload`, `sourceReloaded`,
  `DataSelectionModel::sourceReloaded`, `qt::connectSourceReload`). E-5 on NTFS: the sharing policy
  (`FILE_SHARE_READ | FILE_SHARE_DELETE`) lets POSIX-semantics, `ReplaceFileW` and rename-aside writers replace a
  mapped log while the view keeps the old bytes and refuses every in-place writer; a classic
  `MoveFileExW(MOVEFILE_REPLACE_EXISTING)` replace is refused (`ERROR_ACCESS_DENIED`) while the log is open, whatever
  the share mode, and changes nothing. `ld.io_fault` is logged once per source. Tests: `base.mapped file: AC-4a ...`
  (an SMB variant through `ESSES_TEST_SMB_DIR`), `formats_m4.reload: ...`, `qtmodel.tst_source_reload`,
  `ui.tst_reload_source`, the keymap row.
- HiDPI per monitor (AC-M4-07, 07 §7.1, 04 §6.6, 06 §17; D-209). A window or floating dock moved to a monitor
  of another scale, or a display whose scale changes, re-lays out and re-renders its components at the new DPR:
  `HiDpiShell` watches the main window and every dock window with a `DprWatcher` and re-checks the windows after a
  dock floats or docks back, a move Qt announces to no widget (`ComponentWidget::recheckDevicePixelRatio`,
  `WorksheetCanvas::refreshGeometry`). At a fractional DPR (125 %, 150 %) `RasterBackend` draws axis-aligned
  strokes wider than 1 px on whole device pixels and `LayerCache::paint` paints every layer from a whole device
  pixel, so grid lines, ticks, cursor and outlines stay crisp; DPR 1 and 2 render exactly as before and their
  goldens stay authoritative. The application sets the `PassThrough` rounding policy explicitly. New goldens: the
  `DrawList` JSON of the seven drawn MVP types at DPR 1.25 and 1.5 (`tests/golden/images/dpr_sweep/`, exact) and
  values_list at DPR 1.5 (provisional PNGs); the geometry-only reference `geometry_scene@1.5x.png` re-rendered.
  Tests: `ui.tst_dpr_switch` (the main window and a floating time_distance across monitors of DPR 1, 1.25, 1.5 and
  2, both dock backends), `widgets.tst_dpr_sweep` (the ten MVP types at the four DPRs), `widgets.tst_dpr_watcher`,
  `widgets.tst_dpr_values_list`, `golden.tst_dpr_goldens`, `qtrender.tst_crisp_strokes`. The two-monitor check
  is the maintainer's (`docs/milestones/M4.md` §6.3).
- The M3 follow-ups of `esses_qtmodel` and the Qt-free libraries (D-210; M3.md §6 items 18, 23, 25, 29-32).
  `TrackMapService` reads, writes and scans track-map files on the pool, never on the GUI thread: a read lands
  with `mapChanged`, writes follow the edits in order (merged behind one in flight, backed up as asked), a write
  that fails is a Messages row with the venue read back from its file and its undo step dropped, writes still
  queued are made when the service goes, and a generated map is handed over only once the venue's own file is
  known (new `asynchronous`, `isLoading`, `mapStamp`, `pendingIo`, `venuesChanged`; synchronous without a
  Dispatcher). `SourceLoader` announces the end of its summary pass (`summariesReady`), keys the Track Sections
  pass by the identity of the source's bound map and runs it again when a Venue detail edit binds another one.
  The channel report gains `options.ignoreInterpolated` (start, end and change from the range's own samples
  even with `interpolateBoundaries`; `channel_report-1` member written only when true, the Properties page binds
  it). `ReportModel` updates a table of the same shape in place (`dataChanged`, so the selection survives a
  refresh) and `ReportTable` re-measures its columns on it. A project's selection is restored only once the
  opens held at the memory budget warning are answered (`SessionRestore`; Continue restores it with the held
  file). New tests: `qtmodel.tst_track_map_io`, `qtmodel.tst_summaries_ready`, `qtmodel.tst_restore_budget`, the
  venue-edit and superseded-run cases of `qtmodel.tst_track_sections_pass`, the selection case of
  `qtmodel.tst_report_model`, a time report fed from `TrackSectionsRule` output (`analysis.report_time: ...`)
  and a variance case checked against `DomainMapper::xOf` (`plotcore.variance: ...`); `widgets.tst_track_editor`
  waits for the files the pool writes.
- The M3 follow-ups of `esses_widgets` (D-211; M3.md §6 items 18-19, 21-22, 24, 26-27). A channel without a
  named display unit shows in its quantity's default (`plot::defaultDisplayUnit`): a fraction stored in `ratio`
  (a logged throttle, brake or duty) reads 0-100 % everywhere, in the graphs, gauges, scatter, Values window,
  reports and exports, the Channel Editor and the Maths Editor; `scatter-1` gains `xDisplayUnit`,
  `traces[].displayUnit` and `colourChannel.displayUnit`, `channel_report-1` `report.channels[].displayUnit`
  (written only when set), and the shipped workbook names G and km/h where it reads them. The Properties pages
  pick display units from a combo of the units of the channel's quantity, following the channel field. The
  Track Editor gains a Speed class column (`track-1` `sections[].speedClass`: slow or fast) and shows and edits
  section bounds in the Corr Dist display unit (its override, else km for km/h and mi for mph, else m). Export
  Data of the graphs and gauges writes the legacy CSV dialect of `esses-cli export --format motec.csv`
  (metadata with the range and worksheet, display units). A time_distance group's key column is its heading:
  dragging it moves the group (one undo step, panel heights kept), a double-click opens Group Properties.
  `links.zoom`, `links.cursor` and `links.datum` are honoured: an unlinked component keeps its own zoom, cursor
  or datum, moved by its own mouse and, when focused, the cursor, zoom, pan and datum keys (the outing graph's
  zoom stays linked; a values list reads at its own cursor). The components rebuild when the loader's summary
  pass ends (`summariesReady` through `FollowupsShell`). Tests: `plotcore.followups D-211: ...` (6),
  `analysis.followups D-211: ...` (2), `widgets.tst_followups_units`, `widgets.tst_followups_view`; the export
  tests of the gauges, the outing and time/distance graphs and the shell read the legacy layout.
- The user guide and Help Contents (07 §7.1 "Docs", 06 §2.2, §14.4; D-215). `docs/user/` holds the 0.1 guide
  in plain Markdown: `index.md` (the start page), `install.md` (the Windows installer and portable zip, the AppImage
  and the DMG with their "builds green, not supported" labels, the Visual C++ runtime, the command line),
  `open-files.md` (`.ld`/`.ldx`, `motec.csv`, generic CSV, the Import Log, the Mapping panel, projects, changed files
  and Reload, the memory budget warning), `selection.md` (the Data window, Main and overlays, filters, trust and
  their keys), `graph.md` (workbooks, worksheets, the ten components, time and distance, zoom, cursor and datum,
  HiDPI), `maths.md` (the language in brief, the Maths Editor, local and transient maths, Channel Status, aliases
  and mappings), `troubleshooting.md` (the Messages window, where the files are, resetting settings, portable mode,
  reporting a problem without private data) and `relinking.md` (replacing the Qt libraries, the LGPL right). Help >
  Help Contents (`help.contents`, F1; keymap row implemented) opens the installed start page
  (`<data dir>/../docs/user/index.md`, else `<exe dir>/docs/user/index.md`), else, or when no program opens it, the
  start page in the public repository at the build's tag (`main` for an untagged build); when nothing opens, a
  Messages warning (`help.not_opened`) names both places (`DocsShell::openContents`, `installedStartPage`,
  `onlineUrl`, the injectable `setUrlOpener`). `CLEANROOM.md` gains 23 rows that complete the record for 0.1 (the key
  assignments, the Data window and selection, workbooks and docks, the time/distance core and values, rendering,
  corrected speed and distance, laps, channels, the command line, schemes, the project folder, the runtime, the D-211
  follow-ups, the M4 behaviours, the CLI, packaging, the dictionaries, the guide) and a coverage table from 07 §2.2's twelve items to
  the rows. Tests: `widgets.tst_docs_shell` (the lookup, the online URL per version, the Help menu and F1 row, the
  installed and online pages, the fallback, the warning, the default opener through `QDesktopServices` URL
  handlers), the `help.contents` rows of `ui.tst_keymap` (F1 from a component and from the Data window, and the
  palette).
- Packages, Help > About and the start of an instance (AC-M4-04 automated part, AC-M4-06, AC-M4-08, AC-M4-09; 07
  §7.1 "Packaging" and "Release", 06 §§18-19; D-212). `esses --version` prints the version, the commit, the Qt
  version run against and built against, the build date, the docking library and the exact source URL (the public
  repository's tag in a release build, configured with `-DESSES_RELEASE_TAG=v<version>`, which must match the stamped
  version; else the commit). Help > About shows the same with "This application uses Qt <version> under the GNU LGPL
  v3", the Qt source archive, the docking library's LGPL notice and tag, the source of this build, the installed
  NOTICES.md and the settings folder (marked portable), with Copy. A second launch hands its files to the running
  Esses of the same user and settings folder (a named pipe on Windows, a Unix socket in a private directory
  elsewhere; no Qt Network) and exits; `--new-instance` opens a window of its own; `--scheme <name>` and
  `--keymap <name|file>` choose the start's scheme and keymap. `tools/packaging/`: every install rule (components
  `app` and `cli`), `package.py` (windeployqt, linuxdeploy, macdeployqt, notices, SBOM, vendor-mark check of the
  tree, deterministic archives, checksums), the Inno Setup installer (`.ld` association task, `-w`/`-l`, the C++
  runtime check: an old runtime is updated or the install refused, a per-user install carries the runtime), the
  portable zip (`portable.marker`, the runtime, `.ld` association scripts), the AppImage (desktop entry, `.ld` MIME
  type, icon) and the DMG (Info.plist, universal, ad hoc signed), `esses-cli` in every package and standalone;
  `gen_third_party.py` writes each package's THIRD_PARTY_LICENSES.md and CycloneDX SBOM from what it contains;
  `smoke.py` tests each package; our own application icon. `NOTICES.md` covers every shipped component (Qt with the
  source offer and how to replace it, ADS, the vendored libraries, the MSVC runtime, the AppImage's system libraries);
  the Qt licence texts ship as `licenses/LGPL-3.0-only.txt` and `GPL-3.0-only.txt`. `check_vendor_marks.py --tree`
  scans a release tree (names, text, the strings of our programs) and the source scan covers the installer, desktop
  and plist files and `package.yml`. New workflow `.github/workflows/package.yml` (tags, manual dispatch, pull
  requests touching `tools/packaging` or `apps/app`): the packages on windows-2022, ubuntu-24.04 and macos-14 with Qt
  6.12, their smoke tests, artefacts with checksums, a draft release on tags. Tests: `widgets.tst_about_dialog`,
  `app.tst_launch_request`, `app.tst_instance_channel`, `app.tst_launch_options`, `app.tst_portable`,
  `packaging.*` (the tools' self-tests, the icons, the manifest's consistency).
- The Saturday soak (AC-M4-03, 07 §7.1 "Soak", 08 §6.10; D-213). `tests/soak/tst_saturday` (ctest
  `soak.tst_saturday`, labels `qt;soak`) drives the real shell offscreen with ten logs open, eight `.ld` + `.ldx`
  sessions on two venues and two CSV exports written at test time (once per build tree, never committed), through
  the day in a loop for `ESSES_SOAK_SECONDS` (under ctest 60 s and at least two cycles; `ctest -L soak`, not in the
  normal runs since the integration, D-216): a restart with the session
  restore; opens through the memory-budget gate answered by Continue and by Cancel; the selection from the Data
  window's keys and from the model; every worksheet with the graph's zoom, pan, cursor and datum keys, the reports
  and the Track Map dock; a maths module edited and recalculated; undo and redo, a journal entry by autosave and a
  save; a reload after a rename-over writer; closes and Close All. It fails on a crash; on a `Dispatcher` thread
  violation (the Dispatcher's GUI-thread check, the GUI thread's NoBlock checks, every other `ESSES_ASSERT` and Qt's
  thread-affinity warnings abort at once); on work that never drains; on growth of the heap, the resident set, the
  chunk cache, the shell's widgets and objects or the peaks of queued tasks and Dispatcher closures, judged once per
  cycle in the same state; and on a chunk cache that keeps a byte once every log is closed. The new job `soak` of
  `bench.yml` builds it with the `linux-clang-asan` preset and the Qt layers and runs it under ASan + UBSan with
  LeakSanitizer, 30 minutes nightly and for `soak_minutes` on manual dispatch (120 on the release candidate):
  `tests/soak/lsan.supp` suppresses only Qt, fontconfig and glib code that never calls back into the application, and
  `tests/soak/lsan_check.py` fails the run on a sanitizer error, a failed test or a leak with an esses frame (a leak
  without one is a warning), after proving the pipeline on a deliberate leak. Tests: `soak.tst_saturday`,
  `soak.guard` (the guard's rules, a recorded violation of each kind and the fatal path in a child process),
  `soak.lsan_check.self_test`.
- The Saturday walkthrough of 0.1 and the command line's golden tests (AC-M4-01, 07 §2.2; D-214).
  `ui.tst_walkthrough_saturday` drives the real shell offscreen through command ids and keys, one test function per
  0.1 item (`item01_...` to `item10_...`: opening `.ld` + `.ldx` and both CSV dialects with the Mapping panel; the
  project, journal, `backup/`, reopen, Options, the two schemes and the workbook; the Data window and its hotkeys;
  workbooks, worksheets, placement, Layout Editor and zoom summary; the ten components and animation; channels,
  overrides, aliases, mappings, Channel Status, Values and Track Map; maths; corrected speed, lap stretching and the
  Lap, Details and Track editors; the hotkey table and palette; 20 traces at DPR 1 and 2) plus the demo of 07 §7.5
  without the installer, over a day's logs the test writes, every expected value from their formulas. New
  `esses-cli convert <input> --format csv|motec.csv|motec.ld -o <out> [--channels] [--range] [--rate]` (05 §9.2;
  `--to` as an alias): a log's channels through the built-in writers; a whole `.ld` is written from its stored bytes
  (read, write, read bit-exact), `--range` cuts a lap out of a `.ld`, and the input, its `.ldx` and its sidecar are
  never overwritten. `ctest -L cli` gains `cli.golden.info`, `.channels`, `.laps`, `.eval`, `.convert`, `.export`,
  `.validate` and `.fixture` (expectations from the fixtures, `tools/ld_ref_decode.py` and the schema examples).
  The shipped circuit workbook's Overview gauges now sit on the 1 % grid; the schemes and the keymap were checked
  against 06 §14.4, §16 and §20 and are unchanged.
- The M4 integration (D-216). A build exactly at a tag now names its commit in `esses --version`, About and the
  package metadata, and a modified tree at a tag is `<version>.dirty` instead of a pre-release (`cmake/version.cmake`
  with `cmake/version_parse.cmake`; ctest `lint.version_stamp`); a release is built from a clean checkout of its tag.
  CI: qt-app runs the Saturday walkthrough as a step of its own (Windows and Linux on every pull request, macOS
  nightly; ctest TIMEOUT 1,500 s) and leaves the soak out (`-LE soak`; the soak is bench.yml's nightly job, the
  guard's tests stay in as `soak.guard`); core runs `ctest -L cli` as a step of its own; lint installs the Qt-free
  release tree (`tools/packaging/` now also configures without Qt) and scans it with `check_vendor_marks.py --tree`,
  and runs `reuse lint` 5.0.2; qt-app's timeout is 60 minutes. `esses_add_qt_test` gains `ARGS` and
  `esses_add_qt_test_entry`. Every M4 keymap row is implemented and the keymap checker's milestone is M4
  (`widgets.tst_m4_contract` `everyM4RowIsImplemented`); no contract stub is left. Help Contents and About share one
  repository URL. New test `app.tst_demo_command_line`: a real `esses --new-instance -w <project> -l <log>` opens the
  project and the log (the command line of the demo of 07 §7.5); the application's tests read a child's
  `settings.json` through `esses::readWholeFile`. The soak no longer fails at its 17th cycle, a restart right after
  Close All, where it looked for a Main that was not there.
- The M4 fix pass (wave 3, D-217): PR #5's first CI run and the audit's and reviews' findings. The main window no
  longer crashes at its teardown under Qt 6.12 (a dock added after the first tab bar: the window now deletes its
  dock contents and other children before its layout); three lines take a form every clang-format release accepts;
  the vendor-mark scan of programs ignores an allow-listed literal that an optimiser split into immediates; the
  Windows Qt install runs aqtinstall with one worker, so its 7-Zip processes no longer race for the kit's folder
  (`.github/aqt-settings.ini`), and still tries twice; every action of ci.yml and bench.yml is pinned and `package`
  runs on every pull request. **Reload** keeps the stale source open until its file is open again (a failed sidecar
  write or a missing file refuses it; the selection and edits move to the new source), decode faults and changes
  found while Esses stays active reach Messages and mark the source stale at once, and the activation check runs on
  the pool.
  **Single instance:** a forwarded request waits until no dialog or menu is open and applies after its answer
  reached the client; the channel's name carries a per-folder random salt (`instance.salt`) and the Windows pipe
  grants only the user; a POSIX client connects without blocking; a launch that can neither forward nor listen says
  so in Messages; a cancelled project switch opens none of the request's logs. **Versions:** a release build is
  stamped from its own tag whatever other tags the commit carries; a build without a reachable version tag is
  `<project version>+g<hash>` (`lint.version_git`). **esses-cli convert:** a `.ld` cut carries its own Log Time, Log
  Date and Start Lap; a stale `.ldx` beside the output is removed when none is written; only a `.ld` input's
  companion is protected; a conversion that writes no channel leaves no file and exits 1; a channel named twice is
  exit 2. **Bench:** new rows `plot.relayout_resize_20tr`, `values.refresh_250ch` and `app.startup_empty_warm` (07
  §12's last MVP rows); the hosted preset records results without the reference gates (`ESSES_BENCH_GATE`). The
  Values window computes only the rows on screen and draws its cells without the style's text layout (1.65 ms for
  250 channels, was 2.6 ms). **Packaging:** an upgrade removes stale runtime DLLs and Qt plugins and offers the
  association again; the uninstaller leaves another program's `.ld` association; the portable association script
  works from a folder with `&` in its name; the macOS bundle names its minimum system and exports the log type
  (`smoke.py dmg` checks both). Smaller: a time_distance group's vertical zoom follows it through a move, undo and
  redo and stays with a group edited in place (a channel added or removed, a rename); the Track Editor keeps the
  bound its field shows; Help Contents goes online when no program opens `.md`; the Data window's badges are
  drawn at each DPR; a channel report shows one channel in two units; File > Export Data survives the component
  closing during its prompts; one `ld.source_changed` per source in its DecodeLog. Deferred: the macOS strips at
  DPR 1.5 and 2, crisp 2 px lines at DPR 2 (the maintainer decides), same-size same-mtime replacements, Qt's bundled
  notices (`docs/milestones/M4.md` §10 items 51-54).
- PR #5's second CI run (D-218): fixes for the failures of fe6cf07's CI runs, the soak's first run among them.
  **Reference decoder:** `tools/ld_ref_decode.py` decodes every channel code on Python 3.8 and later. Before 3.12,
  `memoryview.cast` refuses half floats, and the qt-app and package jobs run Python 3.11. The decoder now falls back to
  `struct` there, and its float statistics no longer depend on the interpreter (`float_sum`, 3.12's compensated sum).
  New ctest `formats_ld.oracle_self_test`. The other Python tools no longer use `write_text(newline=)` (3.10) or a
  subscripted `tuple` at import (3.9). **The portable zip's association:** `unassociate-ld.cmd` no longer leaves `.ld`
  associated when another `find` (Git for Windows', MSYS2's) comes before Windows's on `PATH`. Both scripts call only
  `%SystemRoot%\System32\reg.exe`, by its full path. `.ld`'s default is removed only while it is exactly
  `Esses.LogFile`, in any language of Windows, and a failed removal exits 1. The developer scripts `build-core.cmd`
  and `build-qt.cmd` call `where.exe` by its full path. New ctest `packaging.association_scripts` runs both scripts
  against a stand-in `reg.exe` (`esses_reg_stub`) from awkward folder names, with decoy programs in their folder and
  on `PATH`; the registry is never touched. `smoke.py windows-portable` uses the same decoys and reports what it read
  when a check fails. `smoke.py --self-test` no longer runs `associate-ld.cmd`, whose `reg` calls it neutralised by
  their text (a call written another way reached the registry); it checks windows-portable's checks against
  stand-ins for the scripts and the registry. **Docking library:** two defects of ADS 5.1.1 that the soak's UBSan
  reported, and that upstream has not fixed, are fixed in the vendored copy (`third_party/qtads/UPSTREAM.md`). One
  was a call on a null focus controller at every pin to a side bar; the other read the dock manager as its own type
  after its destructor had run. `NOTICES.md`, Help > About, `relinking.md`, `GOVERNANCE.md` and `REUSE.toml` now say
  the library is built with those changes. `AdsBackend` no longer reads a dock that is being destroyed as a dock. New
  `widgets.tst_dock_host` functions `autoHideWithoutAdsFocusController` and `teardownWithTheFocusInADock`. **macOS
  tests:** `app.tst_instance_channel` compares forwarded paths by file identity, with a new row run through a linked
  folder. `widgets.tst_dpr_values_list` and `widgets.tst_values_list_goldens` place the list in a host as a worksheet
  does, and `belowTheTablesMinimumSize` is new. Deferred: ADS's restore of a damaged side-bar area in `layout.json`
  (`docs/milestones/M4.md` §10 item 58).
- PR #5's third CI run (D-219): fixes for the failures of 2d9a01c's CI runs. **Close All** no longer reads freed
  memory: the loader closed each source through a reference to that source's own id, and the selection, its last
  owner, freed it before the id's last use (the soak's AddressSanitizer report, in its second cycle).
  `SourceLoader::close()` and `closeAll()`, `DataSelectionModel::closeSource()` and `MathsService::removeSource()` copy
  the id first, every `sourceClosed` receiver gets the copy, and `mappingRequired` carries a copy of the pending entry,
  since a receiver may answer it at once. **Exit:** the loader's destructor flushed each pending sidecar edit through a
  key that the flush erased; where that memory changed (MSVC Debug), an older queued write ran last and the newest edit
  made within the coalescing interval before exit was lost. **Lap stretching:** after a Corrected Speed settings edit
  is undone, the laps asked for while Corr Dist was evaluated again are built without another request (Corr Dist back
  at an earlier version is announced by `statusChanged` alone), so the stretch report, the Lap Editor's Stretch column
  and the lap prefetch follow. `qtmodel.tst_lap_stretch` and `ui.tst_walkthrough_saturday` item 8 read the stretch
  report as `reportChanged` announces it, once every lap's length is in (a test race since M2), and
  `widgets.tst_channel_report` waits for the pool to settle. **Installer:** the uninstaller removes `.ld`'s default
  (still only while it is `Esses.LogFile`) at the start of the uninstall instead of after `unins000.exe` has returned,
  so `.ld` never names the removed ProgId and the shell is told of the change. `smoke.py windows-installer` checks the
  uninstaller's exit code, waits for the uninstall's end in its log, checks a per-user install's own association and
  prints the registry values and the uninstall log's `.ld` lines when an association check fails; `smoke.py
  --self-test` checks `esses.iss`'s `.ld` rule on a model of Inno Setup. **Soak:** `lsan_check.py` prints the first
  sanitizer report in the job log, whole up to 200 lines. New tests: `qtmodel.tst_source_loader`
  `sourceClosedHandsACopyOfTheId` and `destroyedWithAnOlderWriteQueuedKeepsTheNewestEdit`, `qtmodel.tst_dataselection`
  `closeSourceOfAReferenceIntoTheClosedSource`, `qtmodel.tst_maths_service`
  `removeSourceOfAReferenceIntoTheRemovedSource`, `qtmodel.tst_mapping_handoff` `aReceiverMayAnswerAtOnce` and
  `qtmodel.tst_maths_distance` `lapsWaitingForAnEarlierCorrDistVersionAreBuilt`.
- Two launches at the same moment no longer both become the running instance on Linux and macOS (PR #5 review, Codex
  P2; D-219 (d)). A launch that found the socket file taken and nobody accepting on it unlinked the file as stale,
  even when another launch had bound it and was about to listen; the bind, that check and the listen now happen
  under an advisory lock on `<socket file>.lock`, which the kernel drops if its instance dies. New test
  `app.tst_instance_channel` `aLaunchBetweenBindAndListenKeepsItsSocket`.

## Unreleased (M3)

- Distinct venues never share a track-map file (PR #4 review, Codex P2; D-205). `TrackMapFile::fileNameFor`
  kept only a lower-cased slug of the venue, so "Spa / Francorchamps" and "Spa-Francorchamps" (and every venue
  without a letter or digit, `track.track.json`) shared one file: `TrackMapService::setMap` saved the second
  venue over the first while the cache still showed both, and after a restart both loaded the map written last,
  because `map()` never looked at the file's `venue` member. A venue's file is now `<slug>-<hash>.track.json`:
  the slug stays readable (at most 48 bytes, Unicode spaces, format characters and invalid UTF-8 as separators,
  always valid UTF-8 and never a Windows device name) and the hash is the first 16 hex digits of xxh3-128 of
  the exact venue. The new `TrackMapFile::loadFor` reads a file only for the venue its `venue` member names, and
  `TrackMapFile::claimFor`, which `setMap` writes and removes through, refuses (Io) a name that holds another
  venue's map instead of overwriting it; `setMap` binds a map to the venue it is stored under, and `venues()`
  follows `loadFor`. Maps written by earlier builds under the slug-only name (`legacyFileNameFor`) are still read
  for their own venue, never for another venue of the same slug, and move to the new name on that venue's next
  write or removal. track-1 is unchanged (its schema text describes the rule). Regression tests: the
  `project.track map file*` cases (file names pinned by the published xxHash vectors, distinct venues, a map
  never loaded for another venue, the earlier builds' files) and `qtmodel.tst_track_map_service`
  `venuesOfOneSlugKeepTheirOwnMaps`, which leaves 2 files for 4 venues on the previous code.
- The Track Sections follow the lap stretch (PR #4 review, Codex P1; D-204). `SourceLoader`'s phase-2 key
  recorded only how many lap indexes a Track Sections run read, and nothing started phase 2 when
  `LapStretchService` published its factors, so once every index existed a new stretch (the same count of
  stretched index copies) left the sections of a lap measured from its end beacon (an out lap) or not ending
  at one (an in lap) at their unstretched times, and with them the section bands and time reports: on the
  synthetic oval with a 2050 m track length the out lap's Turn 1 stayed at 6.06 s instead of 6.70 s. The key
  now ends `/i<count>-<digest>` over every indexed lap's D-140 key (Corr Dist version, markers, bounds,
  stretch) and beacon ends, `setRangeStretch` starts phase 2 for its layer, and the dedupe is against the
  layer's latest run (running or done) instead of every run ever started, so inputs that come back to an
  earlier run's (the stretch cleared) run again; a run that is no longer its layer's latest publishes nothing,
  and `close` forgets the source's runs (the old set only grew). Identical inputs are still deduplicated.
  Regression test: `qtmodel.tst_track_sections_pass` `sectionsFollowTheLapStretch` (fails without the key
  digest, without the restart from `setRangeStretch`, and with the old dedupe); `sectionsComeBackAfterAReopen`
  guards the close.
- The M3 pixel goldens are the ubuntu renders: the 86 provisional Windows PNGs (plots, reports, track,
  values_list, zoom summary and the thirteen M3 `time_distance` cases) and the stale M1 `dark_scheme` were
  replaced from the `golden-diffs-ubuntu-24.04` artefact of CI run 39104943947 (commit 811f442), and every
  `goldens.json` says `"generatedOn": "linux"`, so the ubuntu `qt-app` job enforces the 0.5 % bound (D-127).

- CI's `golden.tst_golden_plots` determinism failure (ubuntu: `gauge_numeric/numeric_reference_text_band@2x`
  rendered twice in one process differed in 1,213 pixels) was a Qt FreeType state leak, now worked around in
  `RasterBackend` text drawing. Qt fills a glyph larger than its glyph cache (64 device px: the numeric gauge's
  dynamic-font value at DPR 2) from `QFontEngineFT::addGlyphsToPath`, which loads the outline under whatever
  transform the thread's face of that font file holds, including the subpixel offset that `loadGlyph` set for
  the last glyph the thread rasterised; with subpixel positioning (fontconfig's hintslight on Linux) the value
  moved by a fraction of a pixel depending on the text drawn before it (here the @1x row). `drawText` now
  resets each face of a run Qt may draw as outlines (a new engine of the face, rasterising one glyph at
  subpixel 0) before drawing it. Reproduced on Windows (the exact case and pixel count with the fonts switched
  to vertical-only hinting, and in a new unit test). Regression tests: `qtrender.tst_text_outline_state`
  (a large run, in the Static and Dynamic paths at DPR 1, 1.25, 1.5 and 2, renders identically after text
  drawn at subpixel positions) and `repeatableInAnyOrder` in `golden.tst_golden_plots` (every plot case at
  DPR 1, 1.25, 1.5 and 2 rendered again in reverse and interleaved orders, between other text, equals its
  first render exactly); the same check in `golden.tst_golden_images` covers the time/distance cases. Small
  text is unchanged.
- CI's `widgets.tst_outing_graph` failure (`dragTheZoomRectangleEndsAndBottomEdge`: 3 Static builds delivered
  of 4 submitted) was the test's: a build replaced while still queued is dropped by design (01 section 5.4
  rule 2: the replace key keeps one running and one pending build, and a newer submit resolves the pending one
  Cancelled, which RenderService never delivers), so delivered may stay below submitted for good. Reproduced
  under load and traced to such a replaced build before the drags. The test now checks that the drags
  submitted and delivered nothing and that the image shown is the latest submission's (`staticBuildShown()`,
  new on LayeredComponent and TimeDistanceGraph); `aBuildReplacedInTheQueueIsNeverDelivered` reproduces the
  drop deterministically with the pool's workers held. The M3 Qt tests no longer settle on a quiet period
  alone: `waitQuiet` (plots, time/distance, tests/ui) waits until the component's pool is idle with every
  posted result delivered (a lap's distance index arriving after 150 ms of quiet could rebuild a component
  behind a "nothing rebuilt" check), such checks are taken synchronously after the action and again once its
  queued request is delivered (`staticUntouched`) instead of after a fixed wait, the track position, channel
  report and shell animation checks count from a settled pool, and the demo's 500 ms wait before "no map was
  generated" is a wait for a quiet runtime. No product behaviour changes.
- The lint job's clang-tidy 19.1.5 (libstdc++) found 10 reads of a `std::optional` whose test it could not
  see (`bugprone-unchecked-optional-access`): the scatter and histogram geometry read `*rc->series` after
  `geom::readable(rc)`, a function of another translation unit, and `TimeReportEngine::eclectic` and the report
  bench tested and read the optional through separate `operator[]` and iterator calls. Each read now follows a
  test of the same object (`readable(rc) && rc->series`, or a reference bound once). clang-tidy 22.1.3 found
  three more in M3 code, now tested next to their use too: the scatter's interval steps and trend curve (drawn
  by two helpers of their own) and `valueAtX` of the time/distance builder, whose callers already require a
  descriptor. No behaviour changes (`readable` implies a series); the existing analysis, plotcore and report
  bench tests cover the code.
- The re-verification of the M3 fix pass (D-203 (ai)): `gCorrection` estimates the longitudinal accelerometer's
  offset while it follows the wheel (from the 1 s slip reference's error, critically damped) and ends a
  prediction after 3 s, taking its drift from the wheel as offset, so an offset can no longer latch the
  prediction: a constant 30 m/s wheel under a 2.1 or 2.5 m/s^2 offset now gives the wheel sample for sample (121.7
  and 147.8 m/s off after 60 s before), under 5 m/s^2 one excursion of at most 3.5 s (at most 16.7 m/s off) and
  then the wheel; a lock under a 2.5 m/s^2 offset is still bridged (within 0.07 m/s of the car). The
  `CorrectedSpeed::gCorrection` header describes the algorithm as built. The keymap harness's palette rows
  (AC-M3-12) send the user's key events: CTRL+SHIFT+P, the command id typed, DOWN, ENTER.
- The M3 fix pass (D-203): the 89 findings of the M3 reviews checked one by one, 80 fixed with a regression
  test, 2 rejected, 7 deferred. Engine: `gCorrection` catches a lock or spin that builds over several samples (a
  slip reference with a 1 s time constant); the channel report's Overlay 0 delta pairs rows below the compared
  ranges (different laps now have delta cells), untrusted laps are cut out of outing and detail rows too, `start`
  and `end` need the channel to cover the bound, `absMax` and `delta` are right under offset and negative-scale
  units, each column has one display unit across sources, detail-group headings read their detail, impossible
  dates are refused and an unambiguous mm/dd/yyyy is read as such; the time report skips disabled laps and keeps
  two sections of one name; `StatCache` fits its 64 bytes per cell; a sample at the histogram's upper limit is
  counted; track sections map proportionally onto a lap within 5 % of the map and stop at the end of a lap that
  does not end at a beacon; Speed+LatG closes figure-eight circuits; GPS dropouts across the start line are
  bridged; timestamped scatter x channels, the through-zero trend basis, scatter overlay units and the variance
  banner for multi-lap ranges are fixed. UI: hidden worksheets idle (no Dynamic builds, sources released, reports
  wait for their show) and keep their view models through worksheet renames and moves; edits from a dialog of a
  hidden worksheet are stored and undoable; Cancel keeps outside edits and unapplied rows survive them; channel
  pickers list the maths outputs and follow new logs; heading pickers offer the profile's report headings;
  Component > Add and right-click menus (channel label, component, canvas); Export Data runs on the pool;
  ALT+UP/DOWN zoom about the active channel's cursor value; the animation rates reach x10 in the menu; L, G, Y,
  E, SHIFT+E and K are offered only where they change something; component keys follow the user's keymap; the
  Track Editor gets the cursor span and a missing venue map is generated on open; Help > Keyboard Shortcuts
  (`help.keyboard_shortcuts`, a menu command without a keymap row, like Component > Add). Tests: the keymap harness runs every implemented
  command through the palette as well (AC-M3-12), the properties round trip checks every leaf, the demo checks
  the datum and delta numbers, negative checks drain the pool first. Docs: D-203 (amends D-127 (a): 21
  time_distance cases, locally rendered PNGs committed as provisional), CLEANROOM rows for D-200, D-202 and D-203,
  the track fixture family's layout recorded; the CI lint job's timeout is 75 min.
- The M3 acceptance pass (07 §6.2, §6.5; AC-M3-01 to AC-M3-12; D-202): the shipped circuit workbook gains the
  Overview, Braking, Corners, Reports and Track worksheets, so every MVP component type appears in it (M1's
  Overlay and Driver worksheets unchanged); the range key colour of 06 §5.3 item 3 (`plot::rangeKeyColour`: Main
  red, Overlay 0 its role, Overlay k `overlayPalette[k - 1]`) now colours the variance traces, the offset
  sub-axes, the range band's overlay cells, the Track Position dots, scatter and histogram overlays and a gauge's
  reference value, where the slot's trace colour made every overlay the same channel colour (eleven `DrawList`
  goldens changed in colour only, their provisional PNGs re-rendered); O turning the offset axis off zeroes every
  selected range's offsets as one undo step ("Clear alignment offsets"); the channel and time reports' `project`
  time format follows `options.lapTimeFormat`; `esses-plot-bench` gains
  `plot.variance_f3_no_overlay` (AC-M3-05: F3 without an overlay costs nothing and builds no `VarianceTable`);
  `tools/check_keymap_coverage.py` takes M3 as the current milestone and `ui.tst_keymap` gains the generic row of
  06 §14.5 for the planned and reserved rows. New tests: `widgets.tst_values_list_goldens` (three values_list
  cases at DPR 1 and 2: exact table goldens, provisional Windows PNGs), `ui.tst_offset_axis` (AC-M3-09 in the
  shell: steps per source, the Data window amounts, O zeroing as one step, the sidecar surviving a reopen) and
  `ui.tst_demo_m3` (the demo script of 07 §6.5 offscreen, over a log written on the synthetic circuit).
- The properties dialogs and the shell's M3 wiring (07 §6.1; AC-M3-01, AC-M3-08, AC-M3-10, AC-M3-11, AC-M3-12;
  D-200): `PropertiesDialog` with schema-driven tabs for the ten MVP types (F5, CTRL+N on the Channels tab;
  modeless, Apply/OK one undo step, Cancel restores, refreshed by undo/redo, geometry per type), `FieldBinder`
  (untouched members left as they were, inline validation against the body schema) and its editors
  (`field_editors.hpp`); the zoom summary bar above the worksheet tabs (click, drag, SHIFT+click, double-click);
  the Corrected Speed and Distance dialog; the Layout Editor (CTRL+F7: move, copy, move to, rename, delete,
  duplicate, new, import/export, Lock Layout); the command palette (CTRL+SHIFT+P; `>` worksheets, `@` channels,
  `#` ranges, `?` disabled commands, recent first); animation (A, the tool bar, Data > Animation Rate; the cursor
  advances with Dynamic-only repaints); Print to Clipboard and Export Data; component cut/copy/paste/delete and
  multi-select, move, resize, splitters, ALT+arrow nudges, z order and maximise on the canvas (edited in place,
  merged undo steps, the layout lock); the Track Map dock and one `TrackMapService` for the window; full screen;
  Select Worksheet (F7), Units List, Open Log with a Details pane, New/Open/Close Project, Close All, Import
  Data; the offset axis rows and Add Annotation; `ComponentWidget::handleCommand`, through which the shell
  offers the component rows (vertical zoom and pan, CTRL+SHIFT+C, the track view, Copy Equation, the mouse rows)
  to the focused component, and the generic L, K, G, S, ALT+n, Y, E, SHIFT+E, F3, M body edits. Every M3 keymap
  row is implemented with its keymap test (the mouse rows by pointer events). `esses-plot-bench` gains
  `plot.animation_frame_10x` (AC-M3-10). Goldens: the zoom summary bar's DrawList JSON (exact) and provisional
  Windows PNGs (`tests/golden/{drawlist,images}/zoom_summary/`).
- The time_distance completion (07 §6.1, parity-first §5.4, 04 §8; AC-M3-02, AC-M3-05, AC-M3-09; D-201):
  `TimeDistanceBuilder` draws the range, section and marker bands (neighbouring laps, track sections by category,
  beacons, device events, sidecar annotations with their dotted lines), overlapped mode with Lo/Hi handles, the
  zero / min / max scale lines (the channel override limits, now on `ResolvedChannel`), the status strip, the
  variance panel (one `VarianceTable` per visible overlay, none without one), the offset axis with each slot's
  offset, the horizontal and the active group's vertical scrollbar, the key's value@datum and Δ and the
  measurements row (min / max / avg read on the worker), and the crosshair and "none" cursor styles; every
  member of `time_distance-1` is typed. `TimeDistanceGraph` answers F3, M, E, SHIFT+E, Y, G, S (undoable body
  edits), CTRL+SHIFT+C, the vertical zoom and pan keys and the programmatic forms of the mouse rows; band
  double-clicks zoom to a range or section, the scrollbars pan, page and reset, Lo/Hi handles and panel splitters
  edit the groups, sub-axis drags offset a slot (one commit at the release), the measurement hover line and
  double-click, tooltips, annotation Edit / Remove, Export Data. `WorksheetViewModel` steps each range by its own
  source's sample period and holds the commit during a drag; `DataSelectionModel::commitSidecar`. `KeyLayout`
  keeps up to 30 % of the key for the labels. Goldens: thirteen M3 time_distance cases as `DrawList` JSON (exact)
  and provisional Windows PNGs (`goldens.json` gains per-case provenance; `dark_scheme` changed with the datum's
  key columns and is marked stale until replaced from the ubuntu artefact).
- The report and track components (07 §6.1; AC-M3-02, AC-M3-03, AC-M3-04; D-199): `esses_qtmodel` `ReportModel`
  (inputs on the GUI thread, `ReportEngine`/`TimeReportEngine` on the pool, the workspace `StatCache`, scopes,
  zoom link, both layouts, overlay row labels, pending/missing/error cells, time formats, styling roles, TSV/CSV
  quoting), the new `TrackMapService` (track maps by venue in `trackmaps/`, written at once with a backup,
  `generate` for GPS and Speed+LatG on a worker) and `TrackMapCommand` (every track-map edit one undo step),
  `RenderService::pool`/`dispatcher`/`statCache`/`setTrackMaps` (the Main source's venue map in the build
  inputs) and `SourceLoader::setTrackMaps` (phase 2 builds the Track Sections groups, again on new lap indexes and
  map changes). `esses_widgets` `ChannelReport` and `TimeReport` (frozen first column, min/max highlight, delta
  vs Overlay 0, transposed layout, % bands with ALT+n, italic untrusted rows, Export Data as CSV, Copy as TSV),
  `TrackPosition` (the venue map with the zoom span and cursor dots; PLUS/MINUS, wheel, drag and double-click as
  one spec edit per gesture; background mode), `TrackMapWindow` (the Track Map dock's content and actions) and
  `TrackEditor` (lap and method, Generate with preview, Accept with default sections, user sections, rename,
  delete, auto-generate option; its constructor no longer takes a `SourceLoader`). Goldens: three cases each at
  DPR 1 and 2 (provisional pixel goldens in `tests/golden/images/{channel_report,time_report,track_position}/`,
  exact table goldens in `tests/golden/tables/`, `DrawList` JSON goldens
  `tests/golden/drawlist/track_position/widget_*`).
- The plots components (07 §6.1; AC-M3-02; D-198): `ScatterPlot` (points, trend and equation with Copy Equation,
  overlapped/tiled/average, interval average, colour channel and legend, gating; a click moves the cursor to the
  nearest Main point, LEFT/RIGHT step Main points; box zoom, wheel and axis restore on its own axes; G, S, Y, K, L),
  `Histogram` (bar/line/text, overlapped/tiled, gating, colour sub-bars, overlays outlined; the cursor bin; a click
  on a tile makes its trace active; G, S, K, L), `OutingGraph` (moved to `src/components/plots/`: the draggable
  zoom rectangle, the lap double-click, the wheel and rubber band on the worksheet zoom, S and L, like-units
  grouping), `NumericGauge` and `BarGauge` (ALT+n colour bands), all on the new `LayeredComponent` base and each
  with Export Data as CSV. `esses_plotcore` `GaugeNumericBody`, `GaugeBarBody` and `GaugeGeometry` (the numeric
  gauge's value and reference with a dynamic font, colour bands on the text or the background; the bar gauge's
  scale, band lines or strip, bar from its origin, reference marker), `ScatterGeometry::exportSets`/`axes`,
  `HistogramGeometry::exportTable`, and the binners now read each channel's own interpolation. The `DrawList` IR
  gains `TextRun::scale` (drawn by `RasterBackend`; in the JSON form only when not 1). Goldens: `DrawList` JSON in
  `tests/golden/drawlist/gauge_numeric`, `gauge_bar`; pixel goldens of three cases per component in
  `tests/golden/images/{scatter,histogram,outing,gauge_numeric,gauge_bar}/` (provisional Windows renders,
  `golden.tst_golden_plots`).
- `esses_analysis` reports (07 §6.1; AC-M3-03; D-197): `ReportSpec` and `TimeReportSpec` read their body JSON
  (`ReportSpec::toJson` writes every member); `StatCache` is an LRU of 128-bit key digests within its 64 MB budget;
  `ReportEngine::statistic` computes avg, min, max, absMax, start, end, change, stdDev and delta over the valid
  samples with the 5 s probe and interpolated boundaries (one pass per range and channel; new trailing
  `store::Interp` parameter); `ReportEngine::cells` divides the ranges in scope by the headers in any order
  (detail headings label rows), excludes untrusted ranges unless included, orders sources by Log Date / Log Time,
  pairs Main with Overlay 0 for the delta columns, converts to display units, marks column extremes, reuses and
  fills the StatCache and prefetches chunks ahead of its scans; `TimeReportEngine` builds section-time tables with
  the eclectic row, the rolling minimum over contiguous laps, totals and % bands. `plot::componentChannelUses` now
  lists a `channel_report`'s channels. New `bench/report_bench` (`esses-report-bench`; 50 laps x 30 channels x
  8 statistics cold within 300 ms; ctest `bench.report_bench`, `bench.report_bench.smoke`).
- `esses_plotcore` M3 geometry (07 §6.1; AC-M3-02; D-196): `BandLayout` (range, section and marker bands with
  hits), the `KeyLayout` datum and measurement columns (`cellRect`, `emitDatumValues`, `emitMeasurements`;
  the default columns keep the M1 layout), the `HitTester` point sets (64 x 64 buckets, per-point tags), the
  `ScaleSpec`/`ColourBands` JSON helpers, and the bodies and builders of `outing` (whole outing from the summary
  path, lap band, zoom rectangle), `track_position` (fit, origin rule, sections, zoom span, cursor dots, banners),
  `scatter` (tiles, colour bands and legend, trend and equation, interval average, density, cursor ring) and
  `histogram` (bin slots, bars with colour sub-bars, lines, text table, cursor bin). `DrawList` JSON goldens at
  DPR 1 and 2 in `tests/golden/drawlist/{band_layout,key_layout,outing,track_position,scatter,histogram}/`.
- Track maps and sections (07 §6.1; AC-M3-04; D-195): `esses_analysis` `TrackMap` reads and writes `track-1`
  (`{v, unit}` lengths, unknown top-level members kept) with `validate` (10 m minimum, bounds, ids) and
  `pointAt`; `TrackGenerator::fromGps` (WGS-84 tangent-plane projection, Hermite bridging of dropouts, an
  adaptive local-linear smoother, optional inner and outer lines) and `fromSpeedLatG` (dead reckoning with loop
  closure; the raw closure error reported; `TrackGeneratorOptions::invertLatAccel`); `SectionDetector::defaults`
  (Turns and Straights from curvature, 10 m minimum, S-bends, the new `minTurnDeg`, user sections kept);
  `TrackSectionsRule` (rule-1 parser, the `Outings:Laps:Track Sections:<set>[:<category>]` groups with the
  backward origin of laps that do not start at a beacon) and a `RuleRunner::runPhase2` overload that builds them
  from a track map, with D-140 versions; `RuleRunner::fromRuleIds` now loads `circuit.track-sections`.
  `esses_project` `TrackMapFile` (load pipeline with the map's checks, canonical atomic save into `trackmaps/`,
  `fileNameFor`). New CC0 fixture `fixtures/track/synthetic-circuit` written by `tools/gen_track_fixtures.py`
  (true centreline, GPS with noise and a dropout, IMU), ctest `fixtures.track_generator_current`; on it the GPS
  map lies 0.80 m RMS from the truth and the Speed+LatG map 4.1 m RMS from the GPS map after a rigid alignment.
- `esses_analysis` binning, variance and corrected speed (07 §6.1; AC-M3-05, AC-M3-06, AC-M3-07; D-194):
  `BandAxis` JSON and `generateBands` (width, count, centred, arbitrary, open bands, palettes, unit
  conversion); `HistogramBinner` (counts, percent and time with gating, untrusted exclusion and the colour
  split); `ScatterBinner` (union grid with per-channel `Interp`, y delay, gating, colour; interval average;
  density grid); `TrendFit` (linear and polynomial 2-6, through zero, Givens QR, equation text) checked against
  `tools/trend_ref.py`, an exact-arithmetic least-squares reference with its freshness test;
  `VarianceTable` (dt(d) and dd(t) per column with offsets and lap stretch) against a brute-force reference;
  `CorrectedSpeed::alternateList` and `gCorrection` (lock and spin prediction from the longitudinal G with a
  linear blend back), used by `CorrectedSpeed::compute`, which now evaluates the `gCorrection` method. New
  defaulted members `ScatterChannel::interp`, `ScatterInput::gatingInterp`, `HistogramInput::colourInterp`.
- M3 contracts (07 §6.1; `docs/milestones/M3.md` §2; D-193): public headers of every M3 class with stubs marked
  `// M3-STUB`, each library's `README.md` listing the stubs and their owner roots. `esses_analysis`:
  `CorrectedSpeed::alternateList`/`gCorrection`, `VarianceTable`, `TrackMap` (`track-1`), `TrackGenerator`,
  `SectionDetector`, `TrackSectionsRule` (dispatched by `makeRangeRule`, still skipped until its parser exists),
  `ReportSpec`, `StatCache`, `ReportEngine`, `TimeReportEngine`, `BandAxis`, `HistogramBinner`, `ScatterBinner`,
  `TrendFit`. `esses_plotcore`: `BandLayout`, the `KeyLayout` datum and measurement columns, the full `HitTester`
  surface (new kinds and handles, `hitAll`, point sets), the component bodies and `componentChannelUses`,
  `ScatterGeometry`, `HistogramGeometry`, `TrackGeometry`, `OutingGeometry`, `GaugeGeometry`;
  `ComponentBuildInputs::trackMap`. `esses_project`: `TrackMapFile`. `esses_qtmodel`: `ReportModel`;
  `componentChannels` covers every MVP type. `esses_qtrender`: `OffscreenRenderer` wires the drawn M3 types to
  their builders. `esses_widgets`: the eight new component classes registered by `registerBuiltinComponents`
  (`OutingGraph`, `ScatterPlot`, `Histogram`, `ChannelReport`, `TimeReport`, `TrackPosition`, `NumericGauge`,
  `BarGauge`), `TrackMapWindow`, `TrackEditor`, `PropertiesDialog` (F5 opens it through
  `ComponentWidget::openProperties`), `FieldBinder`, `ZoomSummaryBar`, `CorrectedSpeedDialog`, `LayoutEditor`,
  `CommandPalette`, `AnimationController`, `ComponentClipboard`; new glob roots so parallel work never edits a
  shared `CMakeLists.txt`. Schemas: the component body schemas `schemas/components/<type>-1.schema.json` of the
  ten MVP types (AC-M3-08) and `track-1`, with valid and invalid examples; `tools/check_schemas.py` and the
  schema example tests read `schemas/components/`, and every component body of the shipped workbook validates
  against its schema. Final parts with tests: the report statistic and scope names, the value-type defaults,
  `TrackMap::sectionSets`, `ReportTable::pending`, `HitTester::hitAll`, `componentChannelUses`, the component
  registrations with their overlay policies and key contexts, `FieldBinder`'s binding table and
  `AnimationController`'s state.

## Unreleased (M2)

- The task-pool tests declare their `Gate`s and the other locals their tasks reference before the `TaskPool`,
  so the pool joins its workers before those locals go out of scope, and `Gate::open()` notifies under its lock
  (ASan stack-use-after-scope in `base.taskpool: tryRunInline runs a queued task on the caller` on the Linux
  ASan job; test-only).

- Task-pool, test-output and source-loader fixes (pull request 2). A share-key `TaskPool::submit` no longer joins a
  task whose `Future` is already ready: the worker drops the key a moment after it publishes the result, and a
  submit in that moment got the old result without a run (MathGraph's Recalculate re-evaluation, and timed runs
  of the maths bench that skipped evaluations); it now starts a task of its own (01 §5.4 rule 1). A requester
  that registers just after a share task latched its cancellation gets a task of its own too, instead of a
  Cancelled result. Qt tests run with `QT_FORCE_STDERR_LOGGING=1` (`ESSES_QT_TEST_ENVIRONMENT`), so their output
  reaches ctest on Windows, where Qt Test otherwise writes to `OutputDebugString` when the process has no
  console. `SourceLoader` emits `sourcePublished()` before the distance prefetch, so `channelReady()` never
  precedes the source it belongs to (a trust edit made in reply was dropped); regression test
  `qtmodel.tst_source_loader` `noChannelIsAnnouncedBeforeItsSource`, and `tst_source_loader` waits on conditions
  instead of fixed times. Test: `base.taskpool: a share key submitted once the task's Future is ready starts a
  new task`.

- Reads are no longer refused while `AtomicFile` replaces the file (Windows). `readWholeFile` read through a
  `std::ifstream`, which does not share delete access, and `AtomicFile` renamed its temporary into place
  through its writing handle (write access, no sharing), closed a moment after the rename: a read in that moment
  failed with a sharing violation (about one in ten reads while another thread replaced the file in a loop),
  and an open stream made the rename itself fail. `readWholeFile` and the new `readFileHead(path, maxBytes,
  what)` now open with `CreateFileW` sharing read, write and delete access and read with `ReadFile` (the POSIX
  path and every error are unchanged). `AtomicFile` writes through a handle that shares read and delete access
  and renames through a second handle with delete access only, so `MappedFile` (which never shares write
  access) is not refused either; a rename refused with `ERROR_ACCESS_DENIED` or `ERROR_SHARING_VIOLATION`
  (a few in a thousand while other threads open the target) is retried for up to 500 ms before the `MoveFileExW`
  fallback. While the rename is under way the target's name can be missing (gaps of about 10 ms measured, and
  renames of up to 200 ms on a loaded machine), so `readWholeFile`, `readFileHead` and `MappedFile::open` look
  again for up to 500 ms when a file is not found or is being deleted; a sharing violation is still reported at
  once. `ImporterRegistry::sniff(path)` reads its 64 KiB head through `readFileHead`, and `tst_source_loader`
  reads the sidecar through `readWholeFile`. Regression tests, each holding the handle states of the moment
  after the rename still and then replacing the file again and again, with no failed read or open allowed:
  `base.read file: a document that AtomicFile replaces reads whole at every instant` (also an open reader that
  does not keep a replacement out; 500 reads straight after a write, then at least 2000 reads against a writer
  thread), `base.atomic file: a mapped reader opens the file at every instant of a replacement` (at least 2000
  opens against a writer thread) and `formats.registry: sniff reads a log through the handles of an atomic
  replacement` (200 sniffs straight after a write); the first fails against the previous reader and against the
  previous writer, the second against the previous writer, the third against the previous sniff.

- One whole-file reader for documents (pull request 2, the macOS `core` job). The four directory regression tests
  of the previous entry failed on macOS: libc++ opens a directory as a stream and reads it as an empty file, with
  no error and no exception (libstdc++ throws `std::ios_base::failure` with EISDIR, MSVC refuses the open), so
  the readers returned empty text and the failure surfaced later as Format, or, in `saveWithBackup`, an empty
  "previous version" was copied to `backup/` before the replacement failed. `esses_base` gains
  `readWholeFile(path, what)` and `requireRegularFile(path, what)` (`read_file.hpp`): the file type is checked
  with `std::filesystem::status` before anything is opened (a directory, device, FIFO or socket is Io "... is
  not a regular file", a missing file Io "cannot open ..."), the file is read in binary mode through
  `istream::read`, which turns a failed read into badbit instead of letting it escape, and no exception leaves
  the call. The seven copies of the old read (`jsonio::readFile`, the settings schemas of the two built-in maths
  modules, the sidecar, the unit table, the project session's document reads, `esses-cli`'s `readFile`) now
  call it with their own names in the messages, and `saveWithBackup` refuses a target that exists but is not a
  regular file before it creates `backup/`. The two other reads of document input that met the same platform
  difference check the type too: `esses-cli`'s `sha256File` (fixture verification hashed a directory as an
  empty file on macOS) and `ImporterRegistry::sniff(path)` (a directory read as an empty head on POSIX and was
  offered to the importers by its extension; Io on Windows). Tests: `base.read file: ...` (regular, empty,
  missing, directory, and a file that cannot be opened: held without sharing on Windows, without read
  permission on POSIX) and a directory case in `formats.registry: sniff confidence bands and companions`; the
  four regression tests are unchanged.

- Fixes for the first CI run of the public repository (pull request 2). `bench.maths_bench.smoke` aborted on
  Linux with "ChunkCache destroyed while Columns still use it": the readiness block of `esses-maths-bench` (and
  its `--digest` mode) declared the `TaskPool` before the `ChunkCache`, so the cache was destroyed first, while a
  worker could still hold the shared state of the task it had just run, and through it the result's `Column`
  (a worker publishes a result, wakes the requester, then runs the continuations and completes the task). The
  cache is now declared before the pool, which joins its workers before the cache goes; the other benchmarks,
  the tests' runtimes and `AppRuntime` already had that order. Linux's scheduler tends to run the woken
  requester at once and preempt the worker inside that window; Windows usually wakes it on another processor,
  but the abort reproduces there with the benchmark pinned to one logical processor. The same pinned runs
  showed a second race: a timed run's fresh graph could join the warm-up run's still-completing task of the
  same key and skip an evaluation ("expected one evaluation per output"); each run now starts once the pool
  has completed every task. Regression test `maths.math graph teardown: ...` holds a worker inside the window
  and checks that the cache keeps the result's chunks until the pool has joined. A save over a directory
  standing where a document belongs (`qtmodel.tst_project_recovery`) threw `std::ios_base::failure` on Linux:
  libstdc++'s file buffer throws a failed read (POSIX opens a directory, the read fails with EISDIR) whatever
  the stream's exception mask, and `istreambuf_iterator` lets it through. `jsonio::readFile` (the save, backup
  and journal path) returns Io instead, as do the other whole-file readers (sidecar, unit table, the settings
  schemas of the built-in maths modules, the project session's document reads, `esses-cli`); regression tests
  in `base`, `project`, `model` and `units`. `SchemeSelector::applySystemScheme(Qt::ColorScheme)` applies the
  scheme the OS reports, and the shell passes the one `QStyleHints::colorSchemeChanged` carries;
  `widgets.tst_scheme_selector` drives light, dark and Unknown through it on every Qt version, and checks a
  Qt 6.8 `QStyleHints::setColorScheme()` request against what the platform theme then reports (the old check
  expected the request to take effect, which the offscreen platform of Qt 6.12 does not do; on Qt 6.7 it was
  compiled out). clang-tidy 19 (the `lint` job): the 25 findings are fixed without NOLINT (unchecked
  `std::optional` access in the maths graph, compiler and kernels and in the text importer, a `std::string`
  parameter taken by value, two optional-to-value-to-optional conversions), and the run over every Qt-free
  translation unit is clean.

- Review fixes in `esses-cli` (AC-M2-01, AC-M2-08, AC-M2-14; D-008). `eval --rate` and `export --rate` are exact:
  each of `<Hz>`, `<num>` and `<den>` is an unsigned decimal with an optional fraction and exponent, converted
  exactly and reduced (`20.5/1` is 41/2 Hz, which was truncated to 20 Hz; `33.3333` is 333333/10000, which was
  rounded to thousandths); a sign, a zero, a non-number or a num or den beyond 32 bits once reduced is exit 2 with
  the reason, and `export` checks its `--rate` before opening the log. `maths check` reports a module's condition
  as a row of its own whatever the module holds (with the graph's `E-CONDITION` when it refuses the condition) and
  counts it: its codes and parse errors in the summary, a condition in error as a module error and exit 1 (a
  module with only a built-in item and a broken condition checked as 0 errors, exit 0); a built-in module that
  fails to register is a module error too, and the text summary ends with the module errors. `validate` reads a
  `$schema` major without `std::stoi`: a major beyond the integer range made the whole batch stop with "internal
  error: stoi" and is now that document's newer-major error; the newer-major search lists the schema folder once
  instead of probing every number below the major (`project-2147483647` took billions of file checks); a
  `schemaVersion` beyond the integer range is newer than any instead of wrapping round. The other `std::stoi`
  calls on document input are gone: `SchemaRegistry::schemaOf` and the text importer's header rows
  (`TextProfile::toJson` and the analysis return Invalid for a row that is not a number instead of throwing).

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
