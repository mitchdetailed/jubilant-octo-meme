# AGENTS.md: working on Esses

This guide is for AI coding agents and for the people who use them on this repository (D-004, D-128, D-151).
Claude Code reads `CLAUDE.md`, which imports this file. It summarises `docs/design/ARCHITECTURE_OUTLINE.md`,
`docs/design/01_ARCHITECTURE.md` and `docs/design/08_REPO_AND_BUILD.md` as built. Where it disagrees with them,
the design documents win (section 2), and the disagreement is a bug in this file.

Esses is a clean-room, MIT-licensed reimplementation of a motorsport data-analysis workflow: a Qt 6 Widgets
application over a Qt-free C++20 core. Status: M0 (foundations) is complete, M1 (walking skeleton) passed its
acceptance run, and M2 (channels, maths, persistence) is merged and tagged `v0.1.0-m2`
(`docs/milestones/M2.md`). M3 (the 80 % components) is built, its criteria are met and its pixel
goldens come from the ubuntu CI artefact; it is in review (`docs/milestones/M3.md`). See `docs/milestones/`
for what each milestone actually built and where it departs from the design.

## 1. Ground rules

These apply to every change, human or AI, and are checked in review and in CI.

1. **Clean room** (`CLEANROOM.md`, `CONTRIBUTING.md`). No disassembly or decompilation of the vendor's binaries
   or files, ever. Behaviour of the reference product comes only from its help pages as displayed by its help
   viewer, its shipped configuration files, SDK samples, public sources and black-box observation on the
   maintainer's licensed install. GPL projects are read for understanding only and never ported.
2. **AI-provenance rule** (verbatim from `CONTRIBUTING.md`): never paste i2 help text, i2 shipped files, vendor
   XML or GPL source into a prompt that generates committed code, documentation, keymap `doc` strings or
   tooltips. Describe the behaviour in your own words first.
3. **Provenance.** Code or behaviour derived from a public project or a vendor document gets a row in
   `CLEANROOM.md` (area, behaviour, category, source, date). A file ported from an MIT/BSD project keeps the
   upstream `SPDX-FileCopyrightText` line beside ours and gets a `ported-from` row.
4. **Never commit private or vendor material.** Nothing under `fixtures/private/` (the maintainer's real logs,
   vendor sample files, game exports, vendor-software exports and the private manifest) and nothing under
   `docs/design/background/` (clean-room research) is ever committed; both are git-ignored, and you must never
   `git add -f` them. The same holds for `research/`, `help_text/`, `conv_samples/` and vendor file types
   (`.chm`, `.i2wkb`, `.i2wsp`, `.mcs`, `.mfs`, `.mt2`); the `lint` job fails if one is tracked. Committed text
   may say that a private check ran and report aggregate results (counts, pass/fail); it never quotes a private
   file's content, names or values. Generated synthetic inputs (`fixtures/motec.ld/synthetic-*/input.ld`, `.ldx`)
   are not committed either; `fixture.json` pins their SHA-256.
5. **Research material.** `docs/design/background/` (local only) holds the research notes and design-panel
   proposals. Read the relevant summary (`research/features_compact.md`, `research/concepts.md`) before
   implementing a behaviour, but never copy its wording or any quoted help text into a committed file.
6. **Vendor marks** (00_OVERVIEW §8, D-003, D-125; `TRADEMARK.md` is withheld until a working model, D-192). The vendor's and product's names never appear in
   identifiers, file names, UI strings, string literals or JSON values, except for the five allow-listed
   nominative classes in `tools/vendor_marks_allowlist.txt` (importer ids `motec.ld`/`motec.csv`, dialog format
   labels, the CSV writer's format-name constant, the `x-i2`/`xI2` round-trip bag, and prose in `CLEANROOM.md`,
   `NOTICES.md`, `docs/` and fixture READMEs). Code that handles a vendor format uses neutral
   names (`Cfg*`, `Legacy*`). `python -I tools/check_vendor_marks.py` must report 0 findings.
7. **The design is the contract.** Use the names in `ARCHITECTURE_OUTLINE.md` §1 and §4 verbatim; a new public
   name is added to the outline first. A departure from the design is recorded (an ADR in
   `docs/design/DECISIONS.md` Addendum A and the milestone note), never made silently.
8. **Commits** carry no DCO sign-off while it is suspended (D-160), and an agent never adds `Signed-off-by:` in the
   maintainer's name; a pull request says which acceptance criterion
   (`AC-Mx-nn`) or decision (`D-nnn`) it serves and adds a `CHANGELOG.md` entry
   (`.github/PULL_REQUEST_TEMPLATE.md`).

## 2. Where things are

| Path | What |
|---|---|
| `docs/design/` | The design set. Start with `README.md` (reading order) and `00_OVERVIEW.md` (glossary). Authority: `DECISIONS.md` (ADRs and Addendum A) wins, then `ARCHITECTURE_OUTLINE.md`, then the numbered documents; within those, the document that owns a type (01 §2.1) wins for it |
| `docs/design/07_ROADMAP.md` | Milestones, deliverables, acceptance criteria (`AC-Mn-nn`), demo scripts, budgets (§12) |
| `docs/milestones/Mn.md` | As-built milestone notes: evidence per criterion, deviations, open issues |
| `docs/formats/`, `docs/spikes/` | The `.ld`/`.ldx` format as implemented, the converter port specification, spike reports |
| `libs/<lib>/` | One directory per `esses_<lib>` library: `include/esses/<lib>/` (public), `src/`, `tests/`, and a `README.md` stating its responsibility, what it must never depend on, and its deviations. Read it before using the library |
| `apps/cli`, `apps/render`, `apps/app` | `esses-cli` (Qt-free), `esses-render` (Qt Gui, offscreen), `esses` (the GUI) |
| `tests/` | Cross-library tests: `ui/` (Qt Test walkthroughs, keymap rows), `golden/` (pixel and `DrawList` goldens, fonts), `fuzz/`, and the build-level checks |
| `fixtures/` | The fixture corpus (`fixtures/README.md`); `fixtures/private/` is the git-ignored private checkout |
| `data/`, `schemas/` | Shipped data (all our own) and the JSON schemas with their examples |
| `tools/` | Lint scripts, the reference decoder, fixture and timing tools, `dev/` build wrappers |
| `bench/` | `workloads.md` (reference workloads RW-1..RW-5 and the reference machine); benchmarks from M1 |
| `third_party/` | Every library dependency, vendored unchanged with its licence (`third_party/README.md`, D-147), including the Qt Advanced Docking System (`qtads/`, LGPL, a shared library built by its own CMake project; D-153). Adding one needs the maintainer's approval for the download |
| `cmake/` | `esses_targets.cmake` (`esses_add_library`, `esses_add_test`, `esses_add_qt_test_dir`, the layer check), `qt-free-check.cmake`, `third_party.cmake`, `qtads.cmake` (ADS, `ESSES_DOCK_BACKEND`), warnings, sanitizers |

The design documents are large: search them (`grep -n`) and read the section you need by line range.

## 3. Architecture in one page

Layers (01 §2, outline §1). An arrow means "may depend on"; no layer depends upward.

```
5  esses (app) -> esses_widgets -> { esses_qtmodel, esses_qtrender }            Qt Widgets + ADS (shared)
4  esses-render -> { esses_qtmodel, esses_qtrender }                            Qt Gui, offscreen
   esses_qtmodel -> every Qt-free library                Qt Core (+ Qt Gui's QUndoStack/QUndoCommand, D-161)
   esses_qtrender -> esses_plotcore                                           Qt Gui only
------------------------------------ Qt-free line --------------------------------------------------------
3  esses-cli -> every Qt-free library;  esses_plotcore -> base, store, model, analysis, project
2  esses_formats, esses_analysis -> base, units, store, model
   esses_maths -> ... + analysis;  esses_project -> ... + analysis, maths  (same-layer edges)
1  esses_store -> base;  esses_model -> base, units, store
0  esses_base;  esses_units -> base
```

- **Forbidden edges:** `formats` never depends on `maths`, `analysis` or `project`; `analysis` never on `maths`;
  `maths` never on `project` or `formats`; `plotcore` never links `maths`; `qtrender` never depends on
  `qtmodel`. Each library declares every dependency in `esses_add_library(<name> LAYER n [QT_FREE] DEPS ...)`;
  configure fails on an upward edge and `tools/check_layering.py` fails on an include outside the declared
  `DEPS`.
- **Qt-free line:** layers 0-3 contain no `#include <Q...>`, no Qt type and link no `Qt6::` target
  (`cmake/qt-free-check.cmake` fails the configure). With `ESSES_BUILD_QT=OFF` they build and test with no Qt
  installed, on all three OSes. Strings there are UTF-8 `std::string`; `QString` appears only in layers 4-5.
  Qt modules per layer: `qtmodel` Core, plus Qt Gui for `QUndoStack`/`QUndoCommand` only (D-161, enforced by
  `tools/check_layering.py`); `qtrender` and `esses-render` Gui; `widgets` and `esses` Widgets (plus Svg,
  PrintSupport, OpenGL). Never Qt Sql, Qt Concurrent, QML or a GPL-only module (D-006, D-031).
- **Namespaces as built:** `esses` (base), `esses::units`, `esses::store`, `esses::model`, `esses::formats`,
  `esses::analysis`, `esses::maths`, `esses::project`, `esses::plot` (plotcore), `esses::qt` (qtmodel,
  qtrender), `esses::widgets`. Qt targets compile with `QT_NO_KEYWORDS`: write `Q_SIGNALS`, `Q_SLOTS`, `Q_EMIT`.
- **Threading** (01 §5, D-031, D-123): one priority `TaskPool` (`esses_base`); no other worker threads (no
  ad-hoc `std::thread` or `QThread`, no `QThreadPool`). Rule W: a pool thread never waits for a task that is
  not running. Workers see only immutable
  snapshots (`std::shared_ptr<const T>`, pinned `ChunkHandle`s, copied view and selection state), never a
  reference to GUI-owned state. One `Dispatcher` (`esses_qtmodel`) posts results to the GUI thread. The GUI
  thread never blocks: no `Future::get`, `take` or `Column::require` there (debug builds assert through
  `ThreadRole::isNoBlock()`); it draws Pending state and repaints on completion. `AppRuntime` owns pool,
  cache, dispatcher and sinks and shuts them down in a fixed order.
- **Errors** (01 §6): public functions in layers 0-3 that can fail return `esses::Result<T>`; no exception
  crosses a library boundary. Conditions the user must see are diagnostics (`ImportLogEntry`, maths
  `Diagnostic`, Messages rows), not errors. `ESSES_ASSERT` is active in debug and in the `ci-*` presets.
- **Data:** time is `Micros` (int64 microseconds), rates are exact `Rate{num, den}`; persisted documents are
  JSON with `$schema` (`urn:esses:<name>-<version>`) and `schemaVersion`, written through the canonical
  serialisation of 02 §6.5 and validated by `esses-cli validate`.

## 4. Building

The maintainer's machine is Windows 11 with MSVC 2022, Ninja and CMake 4.4; Qt 6.7.2 is at
`C:/Qt/6.7.2/msvc2019_64`; CI uses Qt 6.12.0. The repository path contains spaces: quote it. All binary
directories live under `build/` (git-ignored). When several agents or builds run at once, give each its own
binary directory (`-B build/<task>-<flavour>`) and never delete one you did not create.

```
tools\dev\build-core.cmd [preset] [binary-dir] [build-type]     Qt-free core: configure, build, ctest
tools\dev\build-qt.cmd   [preset] [binary-dir] [build-type]     with Qt (default win-msvc-qt, Qt 6.7.2)
tools\dev\build-core.cmd ci-win-msvc-core build\my-core         what CI's core job builds (/W4 /WX)
tools\dev\build-qt.cmd   ci-win-msvc-qt   build\my-qt           what CI's qt-app job builds (/WX, offscreen tests)
```

The wrappers set up `vcvars64.bat`, keep the CMake that was on `PATH`, put the Qt kit on `PATH` and set
`QT_QPA_PLATFORM=offscreen`. Environment: `ESSES_CMAKE_ARGS` (extra `-D` arguments), `ESSES_CTEST_ARGS`
(for example `-R maths -L unit`), `ESSES_SKIP_TESTS=1`, `ESSES_QT_DIR` (another kit). Details:
`tools/dev/README.md`.

On Linux and macOS, with a Qt kit for the Qt presets:

```
cmake --preset linux-gcc-core && cmake --build --preset linux-gcc-core && ctest --preset linux-gcc-core
cmake --preset linux-gcc-qt -DCMAKE_PREFIX_PATH=<qt kit> && cmake --build --preset linux-gcc-qt && ctest --preset linux-gcc-qt
```

| Presets | Use |
|---|---|
| `win-msvc-core`, `linux-gcc-core`, `mac-clang-core` (+ `ci-*`) | Qt-free core, Debug; `ci-*` adds warnings as errors and assertions |
| `win-msvc-qt` (`-rel`), `linux-gcc-qt`, `mac-clang-qt` (+ `ci-*`) | everything with Qt, Debug; the Linux and macOS ones take the kit from `CMAKE_PREFIX_PATH` |
| `linux-clang-asan`, `linux-clang-tsan`, `linux-clang-fuzz`, `win-msvc-fuzz` | sanitizers and fuzzing |
| `win-msvc-bench`, `linux-gcc-bench` | Release benchmarks (reference machine and hosted alarm, D-042) |

Machine-specific presets go in the git-ignored `CMakeUserPresets.json`. `ESSES_WITH_<LIB>=OFF` leaves an
unfinished library or application out of a configure (D-149).

**Before handing in a change:** it builds warning-free with the matching `ci-*` preset (MSVC `/W4 /WX`;
GCC/Clang `-Wall -Wextra -Wpedantic ... -Werror`, `cmake/warnings.cmake`), every test passes, the C/C++ files
you touched are clang-format clean
(`C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-format.exe -i <files>`;
CI runs this same release, LLVM 19.1.5, as clang-format on every file and as clang-tidy on the Qt-free
targets), and the lint scripts below are clean. Clang (macOS) and GCC (Linux) warn where MSVC does not: a local
that shadows a field, even in a static member function (`-Wshadow`), every implicit `float` to `double`
widening (`-Wdouble-promotion`; write the cast) and a reference bound to the result of a call that took a
temporary (GCC 13 `-Wdangling-reference`); floating-point `std::from_chars` does not exist in Apple Clang 15's
libc++ (use `fast_float`). Every source file starts with `// SPDX-FileCopyrightText: 2026 the Esses contributors` and
<!-- REUSE-IgnoreStart -->
`// SPDX-License-Identifier: MIT`.
<!-- REUSE-IgnoreEnd -->

## 5. Tests

New behaviour comes with tests; a bug fix comes with a regression test.

- **Qt-free libraries:** Catch2 v3 (`third_party/catch2v3`, amalgamated) in `libs/<lib>/tests/`, registered
  with `esses_add_test()`. Each `TEST_CASE` is one ctest entry `<lib>.<test name>`; names must be unique per
  executable ignoring case and must not start or end with `*`. Tags become ctest labels: `[unit]`,
  `[property]` (seed from `ESSES_TEST_SEED`), `[slow]`, `[oracle]` (needs private fixtures; `SKIP()` when
  absent). Include the modular headers (`<catch2/catch_test_macros.hpp>`, ...).
- **Qt layers:** Qt Test. Every `tst_<name>.cpp` under `libs/<qt lib>/tests/`, `tests/ui/` or
  `tests/golden/images/` is one executable and one ctest entry `<prefix>.tst_<name>` (label `qt`), run
  offscreen; other `.cpp` files there are shared helpers.
- **Hotkeys** (06 §14, 08 §6.5, AC-M1-06): every `implemented` row of `data/keymaps/default.json` has exactly
  one `KEYMAP_TEST("<command.id>")` under `tests/ui/keymap/`, which sends the keys and asserts the state change
  through the models (`DataSelectionModel`, `WorksheetViewModel`), never through pixels;
  `tools/check_keymap_coverage.py` enforces the pairing.
- **Goldens.** `DrawList` JSON goldens (`tests/golden/drawlist/<type>/<case>@<dpr>x.json`) are platform
  independent and checked on all three OSes; `ESSES_UPDATE_GOLDENS=1` rewrites them, and a mismatch writes
  `*.actual.json` into the build tree. Geometry-only reference images (`libs/qtrender/tests/reference/`) are
  compared on every OS; `ESSES_UPDATE_REFERENCES=1` rewrites them. **Pixel goldens with text**
  (`tests/golden/`) are authoritative only on ubuntu with Qt 6.12 (D-127): a locally rendered PNG may be
  committed only as provisional (`"generatedOn": "windows"` in `goldens.json`, compared in report mode; D-203
  (ae)), and is replaced from the `golden-diffs-ubuntu-24.04` artefact of the `qt-app` CI run after review.
  Fonts for every golden come only from `tests/golden/fonts/` (Noto Sans, OFL-1.1). An update of any golden
  is a reviewed change whose pull request says why.
- **CLI goldens:** `esses-cli laps --json` against each fixture's `expected.laps.json` (`fixtures.laps_golden`).
- **Lint tests** (ctest label `lint`): configure-time negative tests and the self-tests of the lint scripts.
  The scripts use the standard library only and run with `python -I`: `check_vendor_marks.py`,
  `check_layering.py --build-dir build/<dir>`, `check_schemas.py`, `check_keymap_coverage.py`,
  `check_importlog_codes.py`; each has `--self-test`.
- **Sanitizers:** ASan + UBSan run every Qt-free test; TSan runs the `base`, `store`, `maths` and `plotcore`
  tests (Linux CI only).
- **Benchmarks** (D-042, `.github/workflows/bench.yml`): budgets are absolute limits, median of at least 5
  runs on the reference machine; hosted runners give a non-blocking +-30 % alarm. The contract for the first
  benchmark (header of `bench.yml`): each benchmark is a ctest entry labelled `bench` that repeats
  `ESSES_BENCH_REPETITIONS` times and writes Google Benchmark JSON into `ESSES_BENCH_OUT` (both set by the
  bench presets).

## 6. Fixtures

Layout and `fixture-1` are defined in 05 §9.1; `fixtures/README.md` lists the families and commands.

- Synthetic families (`fixtures/motec.ld/synthetic-*`, CC0-1.0) are written by our own generator
  (`esses-gen-fixtures`, `tools/gen_ld_fixtures.cpp`). Only `fixture.json`, `README.md` and the expectations
  are committed; ctest regenerates the inputs into the build tree (`fixtures.generate_inputs`), so concurrent
  builds never write into `fixtures/`.
- **Expectations never come from the code under test** (D-041): channel values come from the independent
  reference decoder `tools/ld_ref_decode.py`, written from the public layout notes, and lap tables from the
  hand-checked tables in the generator. Never change the reference decoder to make it agree with the reader.
- A public fixture needs a licence (`esses-cli fixture add ... --licence <spdx>`, D-152) and documented
  provenance; an input over 2 MB is referenced by `remote` and hash instead of committed.
- Real files (the maintainer's logs, vendor samples, game exports such as ACC, vendor-software exports used as
  oracles) live only in `fixtures/private/` and are never committed (rule 4). Tests that need them are tagged
  `[oracle]` or use `fixture verify --if-present` and skip cleanly when the checkout is absent.

## 7. CI

`.github/workflows/ci.yml`:

| Job | Runs | Notes |
|---|---|---|
| `core` | ubuntu-24.04, windows-2022, macos-14, no Qt | all Qt-free tests, `DrawList` goldens, fixture verification |
| `qt-app` | windows-2022 and ubuntu-24.04 on every PR and push; macos-14 nightly and on manual dispatch | Qt 6.12.0 via install-qt-action; every test offscreen; pixel goldens (authoritative on ubuntu); layering with the Qt layers; golden diffs uploaded on failure |
| `lint` | ubuntu-24.04 | clang-format, clang-tidy, vendor marks, schemas, keymap coverage, forbidden paths, layering, `reuse lint`, schema validation, Import Log codes |
| `sanitize` | ubuntu-24.04 | ASan + UBSan; TSan on base, store, maths, plotcore |
| `fuzz-smoke`, `fuzz-smoke-windows` | ubuntu-24.04, windows-2022 | 60 s per fuzz target |

`.github/workflows/bench.yml` holds the nightly hosted benchmark alarm and the reference-machine gate (a
self-hosted runner, enabled by the repository variable `ESSES_REFERENCE_RUNNER`). Required checks for `main`:
`core` x3, `qt-app` on Windows and Linux, `lint`, `sanitize`, `fuzz-smoke` (08 §4.4).

## 8. Working alongside other agents

- Stay inside the files of your task. Libraries with several owners split their sources into separate glob
  roots (for example `libs/widgets/src/{shell,docks,components}`) so that parallel work never touches the
  same file; shared files (`CMakeLists.txt`, `CMakePresets.json`, `cmake/`) get small, targeted edits after
  re-reading the current content.
- Do not commit, push, reset or switch branches unless the maintainer asked you to; report what you changed.
- Do not reformat files you did not change, and do not "fix" a test by changing its expectation unless the
  expectation itself is shown to be wrong.
