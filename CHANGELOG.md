# Changelog

All notable changes are recorded here, newest first. The project follows semantic versioning from 0.1.

## Unreleased (M1)

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
