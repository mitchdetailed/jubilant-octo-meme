# 07 — Roadmap: MVP 0.1, milestones M0–M7, v1.0 and v1.1

**Scope.** This document turns D-035 into an executable plan: the MVP 0.1 definition, milestones M0–M4 to the 0.1 tag, M5–M6 to v1.0 and M7 to v1.1. Each milestone lists deliverables by CMake target, numbered testable acceptance criteria (`AC-Mn-kk`), effort for one developer with AI assistance, dependencies (including user confirmations), a demo script and a "may slip" list. It also fixes the four M0 week-1 spikes, the per-milestone budget gates, the deferred-to-v1.0/v1.1 lists, the risk register and the user-confirm checklist. Target names, type names and JSON `type` strings are those of `ARCHITECTURE_OUTLINE.md` §1 and §4; behaviours come from `background/design-panel/parity-first.md` §5.4–§5.10 and §7.1–§7.6 (D-001). Where `DECISIONS.md` and the outline disagree, `DECISIONS.md` wins and §15 records the discrepancy.

**Related decisions.** D-002, D-003, D-004, D-005, D-006, D-007, D-011, D-012, D-013, D-014, D-015, D-016, D-017, D-018, D-019, D-020, D-021, D-022, D-023, D-024, D-025, D-026, D-027, D-028, D-029, D-030, D-031, D-032, D-033, D-034, D-035, D-036; review amendments D-037–D-047 (`DECISIONS.md` Addendum A).

---

## 1. Planning model

### 1.1 Effort units

- One "week" = one calendar week of ~25 focused hours by one developer using AI tooling (D-035). Hours below are 25 × weeks.
- The numbers are the review re-estimate (D-045). The original ×1.2 correction on parity-first's 25 weeks did not cover the infrastructure and test obligations that documents 01–08 specify, so every milestone was re-sized after the M0–M2 resequencing (D-044) and the move of eviction to M5 (D-043). A separate **15 % contingency** line (§1.3) follows M4; milestones draw on it only through the slip policy (§1.5), and unused contingency brings the 0.1 tag forward.
- Tests, CI and `docs/milestones/Mn.md` are inside each milestone's number at **30 %** of it (fixtures, goldens, Qt Test rows, benchmarks, fuzz), not separate line items. A milestone whose breakdown shows less than 25 % for tests is re-planned before it starts.
- No parallel workstreams are assumed. If a second contributor appears, the format/fixture work in M0 and M6 is the first thing to hand off (it is Qt-free, fixture-gated and documented).

### 1.2 Milestone exit rule

A milestone is closed when all four hold:

1. Every acceptance criterion `AC-Mn-kk` below is ticked in `docs/milestones/Mn.md` with the id of the test, benchmark or script that proves it.
2. All required CI checks (08_REPO_AND_BUILD.md §4.4) are green on `main` at the closing commit.
3. The milestone's demo script has been executed on the developer's machine and its output (terminal transcript, PNGs or screen recording) is linked from `docs/milestones/Mn.md`.
4. The closing commit is tagged `v0.1.0-mN` (pre-release tag, N = milestone number) so that benchmark baselines and golden images can be referenced by tag. (Convention D-115.)

### 1.3 Schedule

| Milestone | Name | Weeks | Hours | Cumulative weeks | Tag |
|---|---|---|---|---|---|
| M0 | Foundations | 4 | 100 | 4 | `v0.1.0-m0` |
| M1 | Walking skeleton | 8 | 200 | 12 | `v0.1.0-m1` |
| M2 | Channels, maths, persistence | 9 | 225 | 21 | `v0.1.0-m2` |
| M3 | The 80 % components | 11 | 275 | 32 | `v0.1.0-m3` |
| M4 | Hardening and 0.1 | 4 | 100 | 36 | **`v0.1.0`** (public alpha) |
| — | Contingency M0–M4 (15 % of 900 h) | 5.5 | 135 | **≈ 42** (expected 0.1 date) | — |
| M5 | v1.0 parity (incl. eviction, D-043) | 13 | 325 | ≈ 55 | — |
| — | Contingency M5 (15 %) | 2 | 49 | **≈ 57** | **`v1.0.0`** (public beta announcement) |
| M6 | Formats and SDK | 8 | 200 | ≈ 65 | `v1.0.x` point releases |
| M7 | v1.1 | 8 | 200 | ≈ 73 | — |
| — | Contingency M6–M7 (15 %) | 2.5 | 60 | **≈ 75** | **`v1.1.0`** |

The previous plan (30 weeks to 0.1, 750 h, ~20 % tests, no contingency) is superseded by D-045 (U-07, accepted by the user on 2026-10-10). If the user adopts the optional cut list of D-046 (U-15), M3 shrinks to ≈ 9 weeks / 225 h and 0.1 lands at ≈ 39–40 weeks; the cut items join M5 (≈ +2 weeks).

### 1.4 Dependencies between milestones and on the user

```
M0 ──► M1 ──► M2 ──► M3 ──► M4 ──► M5 ──► M6 ──► M7
 │      │      │      │      │      │
 │      │      │      │      │      └─ U-06 converter licence (needed by M6 start)
 │      │      │      │      └─ U-11 signing, U-16 eviction staging (M4 start)
 │      │      │      └─ U-15 optional 0.1 cut list, U-05 Values-window content (M3 start)
 │      │      └─ U-05 '-2^2' observation, U-10 maths precision (M2 start)
 │      └─ U-05 overlay colours/numbering, U-09 reference machine (M1 start)
 └─ nothing blocks the first commit: U-03 oracle policy and field
    of use, U-04 committable files, U-07 scope + re-baseline answered 2026-10-10;
    M0 week 1–2: U-05 shift-vs-mul file, U-13 real-file paths (sample files cleared), U-08 local Qt 6.12 (D-126)
```

User items are the rows of §14. U-01 (licence: MIT) and U-02 (codename: Esses; no trademark search needed for now) were answered on 2026-10-10. U-03 (yes to all three D-005 answers), U-04 (nothing real public for now) and U-07 (scope cut and re-baseline accepted) were answered on 2026-10-10 too, so no user item blocks the first push or the first commit of `CLEANROOM.md` and M0 can start. U-03 had gated `CLEANROOM.md` because the research phase already used the user's i2-written files (D-039). Its yes to (3) turns the U-05 i2 observations into scheduled tasks in M1, M2 and M3 (black-box only, never disassembly; the legal check is the user's own responsibility), and its ACC sub-question was answered the same day: ACC files are not MoTeC-licensed, so i2 is never run on them. U-13's sample-file part (local/private testing allowed, never committed) was answered that day as well. Every other milestone starts on its stated default when the user has not answered; the default is recorded in `DECISIONS.md` and the answer, when it arrives, is applied as a scripted change (for example a rename script or a licence file swap, had U-02 or U-01 been answered differently) rather than a redesign.

### 1.5 Slip policy

- Overruns draw first on the contingency line (§1.3); a milestone may draw at most its own 15 % before its "may slip" list is used.
- A milestone that is 30 % over its budget with open criteria cuts from its "may slip" list (each milestone has one) into the next milestone; it never cuts a criterion tied to a budget (§12), to clean-room (D-004) or to persistence round-trip (D-019), because those are the ones that cannot be retrofitted. The one exception for budgets: when the reference machine, not the code, is the cause (hardware change, OS update, runner outage), the budget row moves one milestone with the evidence in the milestone note (D-042).
- Items cut twice go to the v1.0 deferred list (§10) and the user is told in the milestone note.
- The 0.1 tag is never delayed for a v1.0 item; raster-only, Windows-installer-only 0.1 is an acceptable outcome (solo-dev judge, D-025).

---

## 2. MVP 0.1 definition (D-035)

### 2.1 The user story

"A circuit race engineer replaces i2 for one Saturday": open the day's `.ld` files (with their `.ldx`) and a CSV export from another logger; the laps appear with trust flags; worksheets of cursor-linked time/distance graphs, overlays aligned by distance with offsets and variance, datum measurements, histogram, scatter, channel and time reports, track position; maths in i2 syntax (global and local) with status; channel overrides, aliases and mappings; project saved and reopened with the previous selection; CSV export. They must rebuild worksheets (a default circuit workbook ships); migration of existing i2 workspaces arrives with v1.0.

### 2.2 In scope (outline §6, numbered for traceability)

1. Open several `.ld` (+ `.ldx` merged, D-015) and CSV (`motec.csv`, `generic.csv`) files; Import Log; inline `MappingPanel` for unknown units/names.
2. Project folder with journal autosave and `backup/`; reopen restores the previous `SelectionSnapshot`; `OptionsDialog`; two shipped schemes; default circuit workbook.
3. `DataWindow`: Main / Overlay 0 / Overlays 1..n, all filters incl. lap-number syntax, lap time, Δ to fastest, offset amount, editable trust; N/P, Q, F4, SHIFT+F4, CTRL+F, CTRL+ALT+F, J, W, DELETE.
4. Workbooks/worksheets with their hotkeys, free-placement components on the 1 % grid, `LayoutEditor` basics, `ZoomSummaryBar` with SHIFT+click extend.
5. `time_distance` with everything in parity-first §5.4; `outing`, `scatter`, `histogram`, `channel_report`, `time_report`, `track_position`, `values_list`, `gauge_numeric`, `gauge_bar`; animation (A) basic.
6. `ChannelsWindow` with i2 search; `ChannelEditor`; `ChannelStatusWindow` with `ResolutionTrace`s; overrides/aliases/mappings; `ValuesWindow`; `TrackMapWindow`.
7. Maths: full i2-compatible language (D-020), global + local maths, constants, enumerations, `filter`/`scale_offset`/`table2d` items, Pending status, multi-core evaluation (D-022; parallel across nodes in 0.1, chunk-parallel at v1.0, D-139), `MathsEditor` with live diagnostics; transient vs local maths from the channel label.
8. `esses.corrected_speed_distance` with its dialog; lap stretching with toolbar toggle and per-lap report; `LapEditor`; `DetailsEditor`; `TrackEditor` (both generation methods, default and user sections).
9. Full hotkey table as `data/keymaps/default.json` with one behaviour test per row; command palette.
10. `RasterBackend` meeting the MVP budgets (§12) on 20 traces; HiDPI correct at DPR 1 and 2.
11. `esses-cli info/channels/laps/eval/convert/export/validate/fixture/bench`.
12. Windows installer + portable zip (release-blocking); Linux AppImage and macOS DMG built best-effort, unsigned and labelled "builds green, not supported" (D-116, D-124).

### 2.3 MVP surface by milestone

| Kind | Item (JSON `type` / class) | Lands in |
|---|---|---|
| Component | `time_distance` (`TimeDistanceGraph`) core: groups and panels, like-units grouping, cursor, zoom, pan, F9, overlays, colours (overlapped mode in M3) | M1 |
| Component | `time_distance` completion: datum, measurements, variance, offset axis, bands, annotations, status strip, device events, SHIFT+F7, CTRL+SHIFT+C, Export CSV | M3 |
| Component | `values_list` (`ValuesList`) | M1 |
| Component | `outing`, `scatter`, `histogram`, `channel_report`, `time_report`, `track_position`, `gauge_numeric`, `gauge_bar` | M3 |
| Dock | `DataWindow`, `ChannelsWindow`, `ValuesWindow` (Data-window filters and Channels-window i2 search in M2), `MessagesWindow` (the Import Log and budget warnings exist from M1, 06 §11) | M1 |
| Dock | `ChannelStatusWindow` | M2 |
| Dock | `TrackMapWindow` | M3 |
| Dialog | `PropertiesDialog` (F5), all tabs for every MVP component; before M3, components are configured through the Channels window and the shipped workbook (D-044) | M3 |
| Dialog | `MathsEditor`, `ChannelEditor`, `DetailsEditor`, `LapEditor`, `OptionsDialog`, `MappingPanel` | M2 |
| Importer | `motec.csv` and `generic.csv` on `TextEngine`, `Dictionary`, `UnitMappingProfile` (moved from M0, D-044) | M2 |
| Dialog | `TrackEditor`, `CorrectedSpeedDialog`, `LayoutEditor`, `ZoomSummaryBar`, command palette | M3 |
| Shell | `MainWindow`, `DockHost`, `WorksheetCanvas`, hotkeys batch 1 | M1 |
| Shell | remaining hotkeys, animation (A), Print to Clipboard | M3 |
| Tool | `esses-cli info/channels/laps/validate/fixture` | M0 |
| Tool | `esses-cli convert/bench`, `esses-render` | M1 |
| Tool | `esses-cli eval`, `export` (project-aware), `maths check`, `fixture diff` | M2 |

### 2.4 Explicitly not in 0.1

The first v1.0 items are the i2 file importers, the indexed Open dialog, `GlBackend`, the export UIs, the eleven v1.0 components, Corrected GPS, Drag/Rally, print, scripting and plugins; the full lists with rationale are in §10.

### 2.5 Acceptance of 0.1

0.1 is accepted when M4 exits (§7.4). There is no separate 0.1 acceptance list.

---

## 3. M0 — Foundations (4 weeks, 100 h)

**Goal.** Everything below the Qt line that the walking skeleton needs, the repository and CI skeleton, the clean-room and governance files, and a fixture corpus with a `.ld` reader proven on the user's real files. No Qt code is written in M0.

**Status (2026-10-11).** M0 is complete locally: all twelve acceptance criteria are met on the developer machine (AC-M0-06, AC-M0-07 and AC-M0-10 locally only), and the checks that only the GitHub runners can make (Linux and macOS `core`, `reuse lint`, TSan, the Linux libFuzzer job) wait for the first push (`docs/milestones/M0.md` §1 and §4). As built, M0 departs from this section in four places, each recorded in `DECISIONS.md` A.11: dependencies are vendored under `third_party/` instead of coming from a vcpkg manifest (D-147), the `qt-app` job moves to M1 (D-150), `CLAUDE.md` and `AGENTS.md` move to M1 (D-151), and the step-4 `fixture add` command of §3.6 needs `--licence` (D-152). Tests use Catch2 3.7.1 amalgamated with the project's own test discovery (D-148), and `ESSES_WITH_<LIB>` switches let a configure leave out an unfinished library (D-149).

### 3.1 Deliverables by target

Class lists per target are those of outline §1.1; this table names only what M0 must contain and any M0-specific limitation.

| Target / area | Deliverable |
|---|---|
| Repository, build, CI | Layout, presets, vendored dependencies under `third_party/` with their provenance in `third_party/README.md` (D-147), `cmake/naming.cmake`, `cmake/qt-free-check.cmake` and the governance file set of 08_REPO_AND_BUILD.md §1, §3, §11 (D-004, D-034); CI jobs `core` (3 OSes, `ESSES_BUILD_QT=OFF`; macOS stays in M0 per D-034), `lint`, `sanitize` (TSan on `esses_base` tests until M1 adds maths/plotcore), `fuzz-smoke`. The `qt-app` skeleton (an empty `esses_app` built with Qt 6.12) moved to M1 (D-150), and so did `CLAUDE.md`/`AGENTS.md` of the governance set (D-151) |
| `esses_base` | Complete per outline; `Rate` integer `timeAt/indexAt` (D-008); `TaskPool` with three bands, key coalescing, `CancelToken` (D-031) |
| `esses_units` | Complete; `data/units.json`: own table from the SI Brochure / NIST SP 811 and standard automotive units, sized by need, with display families; i2 spellings defined from their physical meaning and flagged `compat` (02 §2.3, D-010) |
| `esses_store` | Complete except eviction: `ChunkCache` tracks the budget and raises a warning hook (D-011); factor-4 `MinMaxPyramid` and `SummaryPyramid` (D-012); `Resampler` with `Interp` |
| `esses_model` | Complete; `SidecarDocument` (`sidecar-1`, atomic write); `ChannelResolver` + `ResolutionTrace` (D-013–D-015) |
| `esses_formats` | `IImporter/IReader/IWriter/ImporterRegistry`, `ProbeResult` ↔ `probe_json` (`probe-1`); `motec.ld` reader written as new code from the port specification of the user's converter (`docs/formats/ld-converter-port.md`, largely the inverse of its writer), `docs/formats/ld.md` and the public notes (D-142 as amended by D-144) (mmap, `LdChunkProvider`, gap-block validity, device events, dialect flags incl. `ld.shiftOrder`, default `afterMul`, D-037) + tolerant `.ldx` parser preserving unknown elements (new code: the converter has no `.ldx` support); `.ld`+`.ldx` writer (fixtures only; the `.ld` part written from the same specification, D-144) (D-028). `TextEngine`, `motec.csv`, `generic.csv`, `Dictionary` and `UnitMappingProfile` move to M2 (D-044) |
| Schemas | `sidecar-1, probe-1, markers-1, fixture-1, units-1` (`importer-profile-1` and `dictionary-1` arrive with the CSV work in M2); validated from the CLI with the vendored json-schema-validator (`SchemaRegistry` proper arrives with `esses_project` in M1) |
| `esses_cli` | `info`, `channels`, `laps` (whole-outing only until M1; `.ldx` markers listed), `validate`, `fixture add \| verify` (`diff` in M2), all `--json` canonical |
| Fixtures, fuzz, docs | Synthetic generator (every `(dtype_a,dtype)` pair, multi-rate, shift/mul variants, aux-chained records (long names, enumeration references, gap blocks; the 212-byte case included), truncated names, `num_channs` mismatch, and sim-exporter dialect families parameterised from their documented differences, D-040); independent reference decoder `tools/ld_ref_decode.py`, written from the public layout notes, which produces the synthetic oracles (D-041); third-party sim exports in-tree only where their licence permits redistribution (sim-to-motec has none: private only, D-040); `fixtures/private` scaffold (D-005, D-033); fuzz targets `fuzz_ld_header`, `fuzz_ldx`; `docs/formats/ld.md`; `CLEANROOM.md` rows for `.ld`, `.ldx` and units (units worded as 02 §2.3: own table from SI Brochure / NIST SP 811 and standard automotive units; i2 `units.xml` consulted only for alias spellings; i2-only units flagged `compat`), the i2 field-of-use rule (D-038), the research-phase record (D-039) and sim-to-motec as "read only, unlicensed, no code or template copied"; `bench/workloads.md` |

### 3.2 Week plan

| Week | Work |
|---|---|
| 1 | Spikes S-1…S-4 (§11) time-boxed to 2.5 days total; repo skeleton, presets, vendored dependencies (D-147), governance files, `core` + `lint` CI green on a hello-world `esses_base` |
| 2 | `esses_base` (TaskPool, Rate, MappedFile), `esses_units` + `data/units.json`, `esses_store` chunks/pyramid/`SummaryPyramid`/resampler with property tests |
| 3 | `esses_model`, `esses_formats`: `.ld` reader on the spike's findings, `.ldx` parser, writer, `SidecarDocument`; `tools/ld_ref_decode.py` |
| 4 | `esses-cli`, fixture generator and corpus, `fixture verify` in CI, fuzz targets, `sanitize` job, `docs/formats/ld.md`, `CLEANROOM.md`, milestone note |

### 3.3 Acceptance criteria

| Id | Criterion | Proof |
|---|---|---|
| AC-M0-01 | `esses-cli info --json` output equals `expected.probe.json` for ≥ 20 `.ld`/`.ldx` fixtures covering: every synthetic `(dtype_a,dtype)` pair, multi-rate, shift≠0 ∧ mul≠1, aux-chained records (the 212-byte case included), truncated names, the synthetic sim-exporter dialect families (D-040) and the `synthetic-acc-dialect` family; a real ACC export generated by the developer is an ordinary third-party fixture and, like every real file for now, runs only from `fixtures/private` (Q-04, D-127). Third-party sim exports (sim-to-motec, MotecLogGenerator) count only when their licence permits redistribution; otherwise they are verified from `fixtures/private`. CSV fixtures are AC-M2-13 | `fixture verify` in `core` (+ private run) |
| AC-M0-02 | Per-channel decode equals the fixture oracle: bit-exact for integer dtypes, ≤ 1 ulp (F32) for floats, on every public fixture and on every private fixture when the submodule is present | `fixture verify` |
| AC-M0-03 | `.ld`+`.ldx` write → read round-trips bit-exact for samples, headers, markers and details on all synthetic families; unknown `.ldx` elements survive read → write byte-identical. The expected channel values of every synthetic family come from `tools/ld_ref_decode.py` or a pinned i3rs/motec-i2 decode, never from Esses' own reader, so an error made symmetrically in writer and reader fails (D-041) | Catch2 `formats/ld_roundtrip` + `fixture verify` |
| AC-M0-04 | `MinMaxPyramid` and exact M4 (hierarchical descent) equal a brute-force per-column reference on NaN-sprinkled random data at 50 random zooms × 10 seeds; `SummaryPyramid::minmax` equals brute force (F32; F64 and I32 bucket results enclose the reference within the directional `float` rounding of 02 §3.1, D-054) | Catch2 property tests in `store` |
| AC-M0-05 | Shift-vs-mul order and the dialect heuristics are recorded in `CLEANROOM.md` and `docs/formats/ld.md` with the fixture ids behind them. A dialect claim is marked settled only when a real-file or third-party-decoder oracle backs it; the shift-vs-mul order only by a real MoTeC-logger file with shift ≠ 0 ∧ mul ≠ 1, because the third-party decoders disagree on it (D-037); otherwise it is recorded "unsettled". Spike S-1 settled it on 2026-10-10: `afterMul`, by physical evidence on two logger channels, now the default (D-037). Every heuristic decision appears in `ImportLog` | review + `info --json` `warnings[]` |
| AC-M0-06 | Fuzz targets `fuzz_ld_header` and `fuzz_ldx` run 60 s each clean (no crash, no sanitizer report) seeded from the corpus | `fuzz-smoke` |
| AC-M0-07 | `ESSES_BUILD_QT=OFF` configures, builds and passes tests on ubuntu-24.04, windows-2022 and macos-14 runners with no Qt installed; `cmake/qt-free-check.cmake` fails a deliberately injected `#include <QString>` in `esses_base` (negative test in `lint`) | `core`, `lint` |
| AC-M0-08 | `Rate` property test: `indexAt(timeAt(i)) == i` for 10⁶ random `(num,den,i)` including 33.333 Hz and 7.5 Hz; 24 h at 1 kHz has zero drift | Catch2 `base/rate` |
| AC-M0-09 | `UnitRegistry` round-trips every unit in `data/units.json` (`x → base → x` within 1e-9 relative), rejects temperature-delta ↔ absolute conversion, resolves all i2 ASCII aliases case-insensitively | Catch2 `units/*` |
| AC-M0-10 | `reuse lint`, vendor-mark grep, schema validation of every JSON under `data/` and `fixtures/` pass; `LICENSE`, `NOTICES.md`, `GOVERNANCE.md`, `CLEANROOM.md`, `TRADEMARK.md`, `CONTRIBUTING.md` exist with the sections of 08_REPO_AND_BUILD.md §11 | `lint` |
| AC-M0-11 | `TaskPool` test: 10,000 tasks across three priorities complete; `Interactive` tasks submitted after 1,000 `Background` tasks start before the 20th `Background` task; equal-key coalescing returns one shared `Future`; `CancelToken` observed within one chunk | Catch2 `base/taskpool` under TSan |
| AC-M0-12 | The `.ld` spike (S-1) report is filed. Timing: `probe` of the 500 MB synthetic `.ld` takes ≤ 200 ms (median of 5 on the reference machine, D-042). Real files: every one of the user's supplied files probes and decodes, and every `ImportLog` warning code they emit is documented in `docs/formats/ld.md` (checked by `tools/check_importlog_codes.py`) | `docs/milestones/M0.md`, `lint` |

### 3.4 Effort breakdown (100 h)

Repo/CI/governance 14 · spikes 12 · `esses_base` 14 · `esses_units` + table 8 · `esses_store` 16 · `esses_model` 8 · `esses_formats` (`.ld`/`.ldx` reader and writer, `probe_json`) 16 · CLI + fixtures + reference decoder + fuzz 12. Tests are ≈ 30 h of the 100 (§1.1); the CSV engine left M0 (D-044). The `.ld` reader and writer are written as new code from the converter's port specification (D-144), which estimates about 3,700 production and 1,400 test lines for `.ld` alone, so the 16 h `esses_formats` line is likely to be exceeded; it is not re-baselined before M0 closes, and the writer's dialect options may slip to M1 (port specification §8 item 12).

### 3.5 Dependencies

- U-01 licence, U-02 codename, U-03 (the three D-005 answers, the field-of-use rule and the research-phase record, D-038/D-039), U-04 committable files and U-07 scope are all answered (2026-10-10: MIT; Esses; yes to all three D-005 answers; nothing real public for now; scope cut and re-baseline accepted), so nothing blocks the first push or the first commit of `CLEANROOM.md`. `CLEANROOM.md` records the 2026-10-09 research use with the 2026-10-10 consent date.
- U-05 (scaling order): settled by spike S-1 on 2026-10-10 from two MoTeC-logger files with `shift ≠ 0 ∧ mul ≠ 1`; `ld.shiftOrder` defaults to `afterMul` (05_IMPORTERS_AND_PLUGINS.md §3.6, D-037), and an i2 CSV export of one of those files would add confirmation.
- The user's real `.ld` files and ACC exports on disk, path supplied to the spike runner; i2's own CSV export serves as an oracle (U-03 (1), yes on 2026-10-10) only for files from MoTeC devices (D-038), never for ACC exports, which the user confirmed on 2026-10-10 are not MoTeC-licensed. The sample files shipped with i2 may also be used locally (U-13, answered 2026-10-10) and are never committed.
- The port specification of the user's Log File Converter (`docs/formats/ld-converter-port.md`, written from its `src/parsers/LdParser.cpp` and `src/formats/LdWriter.cpp`) as the source of the `.ld` reader and writer, which are new code until the user confirms the converter's licence and lineage (D-142, D-144).
- Dependencies (D-147): M0 phase A (2026-10-10) vendored pugixml 1.13 and xxHash 0.8.3 from local copies under `third_party/`, with no download, and ran its tests on a local copy of Catch2 2.13.10 (single header). Later that day the user approved the downloads, and phase B vendored nlohmann/json 3.11.3, json-schema-validator 2.3.0, CLI11 2.4.2, fast_float 6.1.6 and Catch2 3.7.1 (amalgamated, D-148), which replaced Catch2 2.13.10. There is no vcpkg: `third_party/README.md` records each library's version, route, source and the SHA-256 of every vendored file. The criteria that read or write JSON (`probe_json`, `fixture.json`, schema validation) were met with the vendored libraries.

### 3.6 Demo script

```
# 1. Prove the Qt-free build
cmake --preset win-msvc-core ; cmake --build --preset win-msvc-core ; ctest --preset win-msvc-core
# 2. Probe a synthetic and a real file
esses-cli info fixtures/motec.ld/synthetic-multirate/input.ld --json
esses-cli info <user>/logs/bathurst_r1.ld --json | jq '.layers[0].channels | length'
esses-cli channels <user>/logs/bathurst_r1.ld            # table: name, unit, rate, dtype, kind, samples
esses-cli laps <user>/logs/bathurst_r1.ld                # markers from .ldx, whole-outing range
# 3. Verify the corpus and the sidecar
esses-cli fixture verify fixtures/
esses-cli validate <user>/logs/bathurst_r1.ld.sidecar.json
# 4. Round-trip
esses-cli fixture add build/tmp/synthetic.ld --format motec.ld --oracle ref-decode --out /tmp/fx --licence CC0-1.0 --provenance synthetic ; esses-cli fixture verify /tmp/fx   # oracle from tools/ld_ref_decode.py (D-041); a public fixture needs --licence; oracle.validity defaults to finite-only for ref-decode
```

### 3.7 May slip (to M1)

`SECURITY.md`, the `num_channs`-mismatch synthetic family, the `.ldx` marker listing in `esses-cli laps` (keep whole-outing ranges).

---

## 4. M1 — Walking skeleton (8 weeks, 200 h)

**Goal.** Open an ACC-dialect `.ld` (`fixtures/motec.ld/synthetic-acc-dialect` in public CI; the user's real ACC files where the private submodule is present), overlay the fastest lap against a chosen lap in distance mode with a cursor-linked Values window, inside the real shell with docks, within the MVP budgets, with golden images and hotkey tests. This is the earliest usable build and the proof that the renderer seam and the data path hold. M1 is deliberately narrow (D-044): one data path (ACC `.ld` → `BeaconPairRule` → `DistanceIndex` → built-in `fastestWheel` Corr Speed/Dist → `time_distance` + Values and Data windows in ADS docks). The maths language, CSV, Data-window filters, property dialogs and the other corrected-speed methods come later.

### 4.1 Deliverables by target

| Target | Deliverable (classes per outline §1.1 unless limited here) |
|---|---|
| `esses_analysis` | `RuleRunner` with `BeaconPairRule` (beacon-pair semantics, lap numbering, in/out/partial, stint/pit, trust) and `WholeOutingRule`; `DistanceIndex`; `CorrectedSpeed` (`fastestWheel` with clamps) and `CorrectedDistance` (D-014, D-016). Not yet: `LapStretch` (M2), `alternateList`/`gCorrection` (M3, with the dialog), track, variance, reports |
| `esses_maths` (minimal) | Only what the built-in module needs (D-016, D-044): basic `MathGraph` (define/require/version; shadowing rules in M2), `PlanProvider` into the `ChunkCache`, `EvalContext`, built-in `esses.corrected_speed_distance` (`fastestWheel` + clamps) with its settings schema. `Parser`, `TypeChecker`, the kernels and `esses-cli eval` arrive in M2 |
| `esses_project` | `ProjectFile`, `WorkbookFile`, `ComponentSpec`, `Scheme`, `Keymap`, `Profile` (Circuit) read/write paths (`SchemaRegistry` and `Migrations` in M2); shipped `circuit-default.workbook.json`, light/dark schemes, `keymaps/default.json` (full table, unimplemented rows marked `planned`) |
| `esses_plotcore` | `ViewState`, `ChangeMask`, `SelectionSnapshot`, `DomainMapper` (time affine; distance via `DistanceIndex` + offset + stretch), `TraceBuilder` (exact M4, gap splitting, display-unit `Converter`), `AxisLayout`, `KeyLayout` (labels, value@main), basic `HitTester`, `DrawList`, `IRenderBackend` (D-012, D-018, D-025) |
| `esses_qtrender` | `RasterBackend`, `LayerCache` (Static/Dynamic/Chrome by `ChangeMask`), `OffscreenRenderer`, `SchemeFonts`; `esses-render` |
| `esses_qtmodel` | `Dispatcher`, `DataSelectionModel` (all D-017 slots), `WorksheetViewModel` (`setDomain` with both F9 rules, zoom/pan/cursor), `ProjectSession` (no undo yet), `SourceLoader` (outline §2.1 minus `MappingPanel`), minimal `MathsService`, `CommandRegistry`, `KeymapBinder`, `SettingsStore`, `MessagesModel` (D-030, D-031) |
| `esses_widgets` | `DockHost` over ADS (the `QDockWidget` fallback is built only for an OS on which spike S-3 fails, D-026); `MainWindow` (menus, toolbar: open/save/time-distance/overlays/single colour); `WorksheetCanvas`; `ComponentWidget`; `TimeDistanceGraph` (groups and panels, like-units grouping, Manual vs Channel/Auto scale, cursor, zoom, pan, F9, overlays in both domains, D-017 colours; overlapped mode in M3); `ValuesList`; `DataWindow` (selection slots, lap time, Δ to fastest, trust; filters in M2); `ChannelsWindow` (plain filter; i2 search semantics in M2); `ValuesWindow`; `MessagesWindow` (Import Log, budget warnings; 06 §11); hotkeys batch 1 (views, data, zoom, pan, cursor). No `PropertiesDialog` before M3 (D-044) |
| CI | `qt-app` job created (moved from M0, D-150) with the empty `esses_app` and the first Qt targets; it runs Qt Test offscreen + goldens on three OSes; `bench` gates the MVP budget rows on the reference machine and runs the non-blocking hosted-runner alarm (D-042) |
| Repository | `CLAUDE.md` and `AGENTS.md` written from the outline and the M0 tree (moved from M0, D-151) |

### 4.2 Acceptance criteria

| Id | Criterion | Proof |
|---|---|---|
| AC-M1-01 | Walkthrough (Qt Test, offscreen): open `fixtures/motec.ld/synthetic-acc-dialect/input.ld` (public CI; the real ACC `.ld` too when the private submodule is present); F8; CTRL+F selects the fastest *trusted* lap as Main; ALT+ENTER on another lap adds Overlay 1; F9 switches to distance; the two traces align at the start line; cursor drag updates `ValuesWindow` rows for Main and Overlay 1 | `tests/ui/walkthrough_m1` |
| AC-M1-02 | Cursor move ≤ 4 ms (Dynamic layer only) and zoom/pan step ≤ 16 ms with 20 traces × 3,800 px at DPR 1, each the median of ≥ 5 runs on the reference machine (D-042); hosted runners record the same rows as a non-blocking ±30 % regression alarm | `bench/plot_bench` |
| AC-M1-03 | First paint of a 100 MB `.ld` (8 channels, full extent) ≤ 500 ms warm (median of ≥ 5 on the reference machine) | `bench/open_bench` |
| AC-M1-04 | Golden images for 8 worksheet cases (single trace, 20 traces, like-units group, two panels, distance mode with overlay, dark scheme, DPR 2 variants) at DPR 1 and 2 differ ≤ 0.5 % pixels from the committed PNGs on ubuntu (pixel goldens are authoritative there only); the same cases pass as `DrawList` JSON goldens on all three OSes, with render smoke tests on Windows and macOS (08 §6.4, D-127) | `tests/golden/images`, `core` |
| AC-M1-05 | `esses-render circuit-default.workbook.json fixtures/motec.ld/synthetic-acc-dialect/input.ld --worksheet 0 -o out.png` equals the on-screen `LayerCache` composite for the same `ViewState` (≤ 0.5 %) | `tests/ui/render_parity` |
| AC-M1-06 | One Qt Test per implemented row of `data/keymaps/default.json` (rows with `"status": "implemented"`) sends the key and asserts the state change; `tools/check_keymap_coverage.py` fails on an implemented row without a test | `qt-app`, `lint` |
| AC-M1-07 | `BeaconPairRule` golden JSON: `esses-cli laps --json` equals `expected.laps.json` for every fixture with beacons, including out/in/partial, RESET, IGRDBCN and `Start Lap` numbering; ≥ 15 s stationary inside a lap sets only the `pitStop` annotation (the lap stays trusted and keeps its class and number), and stints never split on ordinary beacon intervals (02 §4.4 rules 12–13); fastest-lap candidates follow the TT 2485 rule (trusted flying laps if any, else flying/partial; in/out/pit never; 06 §5.2) | `core` |
| AC-M1-08 | `DistanceIndex` property: `distAt(timeAt(d)) == d ± 0.25 m` on 1,000 random distances per lap; stopped-car plateau fixture returns the *first* time each distance is reached | Catch2 `analysis/distance_index` |
| AC-M1-09 | F9 preservation: with `Corr Dist` present the visible span converts through the Main lap's `DistanceIndex` and back within one sample; with `Corr Dist` absent the fraction rule gives `a' = R.start' + (a − R.start)/len(R) · len(R')` and the banner "no distance data" appears for that source | `tests/ui/f9_preservation` |
| AC-M1-10 | `Corr Speed`/`Corr Dist` are produced through `PlanProvider` as the built-in module (D-016) and prefetched at `Background` after open; distance mode is populated without user action; the module's settings validate against its schema (the dialog is M3) | `tests/ui/corr_dist_prefetch` |
| AC-M1-11 | `fastestWheel` `Corr Speed` equals a hand-computed reference on a synthetic four-wheel fixture (one wheel locking, one clamp-invalidated span); `Corr Dist` equals the trapezoid integral within 1e-9 relative and holds through invalid speed samples. (`esses-cli eval` and its budget moved to AC-M2-14, D-044) | Catch2 `analysis/corrected_speed` |
| AC-M1-12 | TSan clean on `esses_maths` and `esses_plotcore` tests; the GUI thread never blocks on a `Future` (assert in debug builds via `Dispatcher` thread check) | `sanitize` |
| AC-M1-13 | ADS `DockHost`: save/restore perspective, auto-hide, float/dock pass offscreen on three OSes; for any OS on which S-3 failed, the `QDockWidget` fallback passes the same test | `qt-app` |

### 4.3 Effort breakdown (200 h)

`esses_analysis` 16 · minimal `esses_maths` (graph, provider, built-in wrapper) 10 · `esses_project` documents + shipped data 12 · `esses_plotcore` 28 · `esses_qtrender` + `esses-render` 14 · `esses_qtmodel` 20 · widgets/shell/graph/docks 80 · cross-cutting goldens/bench/CI 20. Tests are ≈ 60 h of the 200 (§1.1).

### 4.4 Dependencies

M0 closed; U-05 overlay colours/renumbering observation, permitted since U-03 (3) was answered yes on 2026-10-10 (black-box only; the legal check is the user's own responsibility, D-038) and run in M1 with the other M1 checks of Q-08: out/in/partial and `Start Lap` numbering, one selection slot per range, the "fast laps" definition, and cursor and datum across worksheet switches (defaults until observed: compact renumbering, scheme-driven `overlayStyle`, the D-076, D-109, D-110, D-132 and D-133 rules, all recorded "chosen, not observed"); U-09 reference machine for the budget gates (default: the developer's machine as a self-hosted runner, D-042); spike S-4 result (raster budgets) and S-3 result (ADS; decides whether any `QDockWidget` fallback is built).

### 4.5 Demo script

```
esses -l <user>/logs/acc_spa.ld        # opens with the default circuit workbook (public demo: fixtures/motec.ld/synthetic-acc-dialect/input.ld)
F8 → CTRL+F                             # fastest trusted lap becomes Main (red dot)
↓ → ALT+ENTER                           # next lap becomes Overlay 1
F9                                      # distance mode; traces align at the start line
drag cursor; V                          # Values window follows Main and Overlay 1
CTRL+click two like-unit channels → grouped on one axis
N, P                                    # next/previous Main range, zoom preserved
esses-render data/workbooks/circuit-default.workbook.json <user>/logs/acc_spa.ld --worksheet 0 --dpr 2 -o m1.png
esses-cli bench --suite plot --json     # prints the budget rows
```

### 4.6 May slip (to M2)

The `esses-render` CLI wrapper (keep `OffscreenRenderer` for goldens; AC-M1-05 moves with it), the `ValuesList` component (keep the `ValuesWindow`), the CTRL like-units grouping gesture (keep automatic grouping by unit).

---

## 5. M2 — Channels, maths, persistence (9 weeks, 225 h)

**Goal.** The full maths language and engine (including the parser, type checker and `eval` moved from M1), the CSV importers with the Mapping panel (moved from M0), every channel-management surface, the Lap/Details editors with lap stretching, undo, and durable project persistence with journal autosave. After M2 the user's real i2 maths modules paste in and evaluate, and the day's CSV opens beside the `.ld` files (D-044).

### 5.1 Deliverables by target

| Target | Deliverable |
|---|---|
| `esses_maths` | `Parser` (full grammar, D-020), `TypeChecker` (warnings only, D-021), `compile`; all documented functions and arities across the D-022 kernel classes (Elementwise, Windowed, IIR incl. zero-phase whole-extent, Stateful, Range, Table 2D); `MathModule` (`maths-1`) with constants, enumerations, `condition`; item kinds `expression`, `filter`, `scale_offset`, `table2d`, `builtin`; `esses.gps_corrections` registered with its zero-latitude/longitude validity step always on (correction steps and the Corrected GPS dialog at v1.0, 03 §9.2); `MathGraph` shadowing order, cycles, versions, `SelectionDependent` keys; `PlanProvider` into `ChunkCache` on the 0.1 whole-node path (D-139, 03 §6.0: each node evaluated over the whole layer extent by one task keyed `…/all`, parallel across nodes; the chunk-parallel evaluator is v1.0, §8.1); `MathsCompat.unaryMinusAbovePower`; `IMathFunction` + manifest schema (no implementations); `docs/maths-compat.md`; ≥ 150 case files (D-020–D-024) |
| `esses_formats` | `TextEngine` + `TextProfile` with `motec.csv` and `generic.csv`; `Dictionary`; `UnitMappingProfile`; schemas `importer-profile-1`, `dictionary-1`; CSV fixtures (D-028, D-029; moved from M0, D-044) |
| `esses_analysis` | `LapStretch` (moved from M1) feeding the Lap Editor stretch report and the toolbar toggle (D-016) |
| `esses_model` / `esses_project` | Sidecar `markerEdits`, `rangeOverrides` keyed `(groupPath, startMarker)`, `localMaths`, `annotations`; `overrides-1`, `aliases-1`, `mappings-1`, `unitmap-1`; workbook `x-i2` bag and unknown-component preservation; `Journal` autosave every 2 min; `backup/` on explicit save; `SchemaRegistry` and `Migrations` with one real migration (D-015, D-019) |
| `esses_qtmodel` | `QUndoStack` commands for every D-032 undoable edit; full `MathsService` (status model, `invalidate(module)`); `SourceLoader` with `MappingPanel` hand-off |
| `esses_widgets` | `MathsEditor` (live diagnostics, `[unit]` popup, Insert Range, auto-save on close), `ChannelStatusWindow`, `ChannelEditor`, `MappingPanel` (Scan / Resolve / Save), `DetailsEditor`, `LapEditor` (the 0.1 functions of 06 §13.3 incl. beacon offset by distance and the stretch report; auto-insert by distance/GPS gate is v1.0), lap-stretch toolbar toggle, `DataWindow` filters (all, incl. lap-number syntax) and `ChannelsWindow` i2 search semantics (both moved from M1), `OptionsDialog`, scheme selector, Edit-menu undo/redo, active-channel hotkeys. Editor sub-features marked *(v1.0)* in 06 §13 are not built or budgeted in 0.1: Lap Editor auto-insert by distance/GPS gate, scheme editor, Keys recording, Fonts editors, Maths Editor auto-complete and Insert Function, Details custom details and column reorder, Track Editor rotation/direction, Channel Editor multi-select |
| `esses_cli`, fuzz | `eval` (moved from M1), `export` (project-aware CSV with maths), `maths check --json`, `validate` for every schema, `eval --case`, `fixture diff`; `fuzz_maths_parser`, `fuzz_csv_sniff`, `fuzz_text_profile` |

### 5.2 Acceptance criteria

| Id | Criterion | Proof |
|---|---|---|
| AC-M2-01 | `esses-cli maths check --json` over the user's real i2 maths modules (pasted as plain-text expressions into `maths-1` modules, not XML import) reports: 0 parse errors on i2-valid syntax; ≥ 95 % of items `Ok` or warnings only; every remaining `E-*` item listed in `docs/maths-compat.md` as a known gap with an id. The script's summary counts (no item names or expressions) are committed as the milestone evidence | `tools/maths_corpus_check.py` over `tests/maths/user_modules` (private); counts in `docs/milestones/M2.md` |
| AC-M2-02 | ≥ 150 `*.case.json` pass: all documented functions and arities, operator word aliases, `[unit]` tags, integer-vs-decimal window semantics, odd/even window centring, `derivative` default, zero-phase filter symmetry, `range_is/range_change`, table clamp/invalid, `invalid()` propagation, `choose` unification, `-2^2 = -4` and the compat switch | `core` |
| AC-M2-03 | Shadowing: local (sidecar) maths beats global; modules in project order; first definition in a module wins; a `filter` item with output name = input shadows for consumers and reads the unshadowed input; cycles reported with the path | Catch2 `maths/graph` |
| AC-M2-04 | Editing one item recomputes only its cone: a 50-item module with one edit re-evaluates ≤ 3 nodes (asserted by version counters) | Catch2 `maths/invalidation` |
| AC-M2-05 | Beacon insert (SHIFT+F7) and `MarkerEdit.offset` rebuild laps < 5 ms on a 60-lap outing; trust flags and alignment offsets keyed `(groupPath, startMarker)` survive insertion before the lap; "restore logged" deletes the op | Catch2 `model/marker_edit` + `tests/ui/lap_editor` |
| AC-M2-06 | Project reopens with the previous `SelectionSnapshot`, single-colour state, Data-window filter and worksheet; a kill at the 2-minute mark leaves `project.json` intact and the journal replays on open with a recovery prompt | `tests/ui/project_recovery` |
| AC-M2-07 | Canonical round-trip byte-identical for every file kind (`project-1`, `workbook-1`, `overrides-1`, `aliases-1`, `mappings-1`, `unitmap-1`, `maths-1`, `scheme-1`, `keymap-1`, `sidecar-1`, `journal-1`); an unknown component `type` and an `x-i2` bag survive load → save unchanged | Catch2 `project/roundtrip` |
| AC-M2-08 | `esses-cli validate` passes on every shipped file under `data/` and every fixture expectation; a deliberately broken file fails with the JSON pointer of the error | `lint` |
| AC-M2-09 | Undo/redo: 30 randomised command sequences (property test) leave the documents equal to a replay; selection/cursor/zoom are never on the stack | Qt Test `undo_property` |
| AC-M2-10 | The 20 maths channels defined in `bench/workloads.md` (a fixed mix of Elementwise, Windowed, IIR, Stateful and Range kernels) on RW-2 become ready ≤ 2 frames after request on the reference machine, with its core count stated in the result (D-042); multi-core evaluation is met by parallelism across nodes on the 0.1 whole-node path (D-139); `filter_lp` order −2 on 21.6 M samples is recorded only (its ≤ 150 ms gate is v1.0, §12) | `bench/maths_bench` |
| AC-M2-11 | `MappingPanel`: a CSV with two unknown units blocks publish until resolved or saved to `unitmaps/generic.csv.json`; a complete profile applies silently | `tests/ui/mapping_panel` |
| AC-M2-12 | Every `ResolutionTrace` kind (`mapping`, `alias`, `caseFold`, `loggedName`, `missing`) is produced by a test and rendered by `ChannelStatusWindow`; missing workbook references display "(missing)" and are never dropped | Catch2 + Qt Test |
| AC-M2-13 | CSV importers (moved from AC-M0-01): `esses-cli info --json` equals `expected.probe.json` for ≥ 4 `motec.csv`/`generic.csv` fixtures including a decimal-comma file and a sparse file; `fuzz_csv_sniff` runs 60 s clean; CSV 300 MB → first paint ≤ 10 s on the reference machine (§12) | `fixture verify`, `fuzz-smoke`, `bench/formats_bench` |
| AC-M2-14 | `esses-cli eval "'Ground Speed' * 2" fixtures/motec.ld/synthetic-acc-dialect/input.ld --json` returns rate, unit and values; elementwise 2.16 M samples ≤ 10 ms one core (moved from AC-M1-11, D-044) | `bench/maths_bench` |

### 5.3 Effort breakdown (225 h)

Parser + type checker + `eval` 16 · kernels + functions 40 · graph/cache/scheduler 16 · case corpus + `maths-compat.md` 18 · text engine + CSV profiles + dictionaries + unit maps 26 · persistence/journal/undo/schemas 28 · editors and docks (incl. Data-window filters, i2 search, lap stretch) 55 · cross-cutting tests/fuzz/CI 26. Tests are ≈ 68 h of the 225 (§1.1).

### 5.4 Dependencies

M1 closed; U-05 `-2^2` observation, permitted since U-03 (3) was answered yes on 2026-10-10 and run in M2 with the E-3 and E-4 checks of `OPEN_QUESTIONS.md` (default until observed `-4`, switchable, recorded "chosen, not observed", D-038); U-10 default maths precision (default F32 with F64 accumulators); U-03 oracle permission (given 2026-10-10) to add `provenance.kind = "oracle"` cases from i2 CSV exports of logs from MoTeC devices (private submodule, D-038), including the `motec.csv` lap-table check (D-106).

### 5.5 Demo script

```
esses -l <user>/logs/acc_spa.ld -l <user>/logs/other_logger.csv   # CSV opens beside the .ld; unknown units → Mapping panel
CTRL+M → new module "demo" → paste a block of the user's i2 maths
   (every item shows OK / warning / explained error live; hover help shows the '^' rule)
CTRL+U                                   # Channel Status: each maths item Pending → OK, traces for aliases
drag 'Corr Dist' into a graph; edit a constant → only dependants flash Pending
Lap Editor → insert beacon at cursor → laps renumber; trust/offset of later laps unchanged
CTRL+Z twice                             # undo the insert and the constant edit
kill -9 esses (after > 2 min) → relaunch → "Recover unsaved changes?" → Yes
esses-cli export <project>/ acc.ld --channels "Corr Dist,Lap Delta" --range "Outings:Laps/3" -o lap3.csv
```

### 5.6 May slip (to M3)

`OptionsDialog` Fonts/Keys tabs, `table2d` editor UI (keep JSON items), `MappingPanel` Save-to-unitmap, `Migrations` test, `eval --case` CLI runner, `fixture diff` (keep `verify`), `fuzz_text_profile`.

---

## 6. M3 — The 80 % components (11 weeks, 275 h)

**Goal.** Every MVP component, dock, dialog and hotkey; the full `time_distance` behaviour of parity-first §5.4; track generation; reports. After M3 the Saturday story is complete except hardening and packaging.

### 6.1 Deliverables by target

| Target | Deliverable |
|---|---|
| `esses_analysis` | `CorrectedSpeed` `alternateList` and `gCorrection` methods (moved from M1, shipped with `CorrectedSpeedDialog`); `VarianceTable` (D-023); `TrackMap` (`track-1`), `TrackGenerator` (Speed+LatG with loop closure; GPS in ENU with optional inner/outer laps), `SectionDetector::defaults` (≥ 10 m), `TrackSectionsRule`; `ReportEngine`, `ReportSpec`, `StatCache` (5 s probe, untrusted exclusion, Δ vs Overlay 0), `TimeReportEngine` (eclectic, rolling minimum, % bands); `HistogramBinner`, `ScatterBinner`, `TrendFit` (linear, polynomial 2–6, through zero, equation text) |
| `esses_plotcore` | `BandLayout`, `KeyLayout` completion (value@datum, Δ, measurements row), full `HitTester`, `ScatterGeometry`, `HistogramGeometry`, `TrackGeometry`, `OutingGeometry` |
| `esses_widgets` | `time_distance` completion to the whole of parity-first §5.4 (overlapped mode, datum, measurements, variance panel, offset axis, scale lines, Lo/Hi handles, annotations, status strip, device events, SHIFT+F7, CTRL+SHIFT+C, Export CSV); `OutingGraph`; `ScatterPlot`, `Histogram`, `ChannelReport`, `TimeReport`, `TrackPosition` + `TrackMapWindow` + `TrackEditor`, `NumericGauge`, `BarGauge` with every behaviour D-027 lists for them; `PropertiesDialog` (first appearance, D-044) with tabs for every MVP component, Apply and undo; `ZoomSummaryBar`; `CorrectedSpeedDialog`; `LayoutEditor` basics; animation (A); Print to Clipboard; clipboard/maximise/multi-select; command palette; L/K/G/S cycles; remaining hotkey rows |
| Data | Default circuit workbook completed (overview, braking, corners, reports, track worksheets); scheme `overlayStyle` and palettes |

### 6.2 Acceptance criteria

| Id | Criterion | Proof |
|---|---|---|
| AC-M3-01 | Every row of `data/keymaps/default.json` whose `milestone` is ≤ M3 has `"status": "implemented"` and a passing Qt Test; `planned` v1.0 rows and `reserved` rows are covered by the generic row of 06 §14.5; `check_keymap_coverage.py` green | `qt-app`, `lint` |
| AC-M3-02 | Golden images for all ten MVP components at DPR 1 and 2 (≥ 3 cases each; `time_distance` ≥ 12 cases including variance, datum, offset axis, bands, annotations, status strip) ≤ 0.5 % | `tests/golden/images` |
| AC-M3-03 | `ChannelReport` cells equal hand-computed fixtures for avg/min/max/absMax/start/end/change/stdDev/delta including the 5 s probe and untrusted exclusion; `TimeReport` eclectic and rolling-minimum rows equal the hand-computed fixture; 50 laps × 30 channels × 8 stats cold ≤ 300 ms | Catch2 `analysis/report_*`, `bench/report_bench` |
| AC-M3-04 | Track generation is measured for shape, not self-consistency. (a) GPS method: on a synthetic circuit with a known centreline plus simulated GPS noise (σ = 1.5 m) and a 2 s dropout, the generated track lies within 2 m RMS of the true centreline. (b) Speed+LatG method: on a fixture lap carrying both GPS and IMU channels, the map after best-fit rigid alignment (rotation + translation) lies within 15 m RMS of the GPS-derived map, and every default-section apex lies within 20 m of lap distance of its GPS counterpart (thresholds provisional until the first real fixture, then frozen in `docs/milestones/M3.md`); the closure error before loop-closure correction is reported, not gated. Default sections obey the 10 m minimum; user sections persist in `trackmaps/<venue>.track.json` and reload | Catch2 `analysis/track_*` |
| AC-M3-05 | Variance panel equals a brute-force Δt(d) reference ± 1 ms at every pixel for 3 overlay pairs; F3 costs 0 ms when no overlay is selected (no `VarianceTable` built) | Catch2 `analysis/variance`, `bench/plot_bench` |
| AC-M3-06 | Scatter trend equation matches a least-squares reference (coefficients within 1e-6 relative) for linear and polynomial 2–6, with and without through-zero | Catch2 `analysis/trend_fit` |
| AC-M3-07 | Histogram counts/percent/time equal brute force for width- and count-defined `BandAxis` bins with gating and untrusted exclusion | Catch2 `analysis/histogram` |
| AC-M3-08 | Schema-driven `PropertiesDialog` round-trip (06 §12 item 7): for each component type, every property of its body schema (`schemas/components/<type>-<version>.schema.json`) has a `FieldBinder` binding (an unbound property fails), each is mutated through its bound editor, then Apply, save, reopen → identical body | Qt Test `properties_roundtrip` |
| AC-M3-09 | Offset axis: CTRL+SHIFT+→ shifts Main by one offset step, one sample period of the active channel in that slot's source (time) or 0.25 m (distance), and CTRL+SHIFT+↑/↓ by ten steps (asserted) (04_RENDERING_AND_COMPONENTS.md §3.3, 06 §24 item 5, D-138); a drag commits one undo command; O zeroes; amounts appear in `DataWindow`; stored `offsetUs/offsetM` survive reopen | Qt Test `offset_axis` |
| AC-M3-10 | Animation at 10× advances `cursorT` with Dynamic-only repaints; CPU per frame ≤ 4 ms | `bench/plot_bench` |
| AC-M3-11 | Cut/copy/paste a component across worksheets of different domain keeps its own domain setting; pasted rectangles snap to 1 % | Qt Test `component_clipboard` |
| AC-M3-12 | Command palette lists every `CommandRegistry` id with its keymap binding; executing from the palette equals pressing the key (shared test harness) | Qt Test `palette` |

### 6.3 Effort breakdown (275 h)

`time_distance` completion 56 · reports (engine + two components) 48 · track (generator, editor, position, dock) 42 · scatter + histogram 32 · outing + gauges 12 · properties dialogs (all components, first appearance) 30 · summary bar / layout editor / palette / animation / clipboard 22 · cross-cutting goldens and tests 33. Re-estimated from 200 h after the review found the first four items under-sized by 1.5–2× (D-045); tests are ≈ 83 h of the 275 (§1.1). With the U-15 cut list (D-046), M3 is ≈ 225 h / 9 weeks. Keymap behaviour tests (≈ 100 MVP rows, one Qt Test each) are budgeted at 30–40 h spread over M1–M3 with the hotkey batches (≈ 10 h in M1, ≈ 10 h in M2, ≈ 15 h in M3) inside each milestone's 30 % test share, not inside M3's cross-cutting goldens line.

### 6.4 Dependencies

M2 closed; U-05 Values-window content rule (default: the focused component's channels with an "All channels" header toggle, parity-first §5.10 and 06_UI_SHELL.md §8, D-047) and default stretch tolerance (default 5 %), both observed on i2 in M3 (permitted since U-03 (3) was answered yes on 2026-10-10) and recorded "chosen, not observed" until then; U-15 decides before M3 starts whether the D-046 cut list leaves M3.

### 6.5 Demo script

```
Open the "braking" worksheet of the default workbook; select Main + Overlay 0 + two overlays
F3 (variance panel appears per overlay in its key colour); D, SPACE (datum), key column shows value@datum and Δ
M → hover measurements; O → offset axis; ALT+SHIFT+→ ×4 nudges Overlay 1
Track Editor → Generate from GPS → default sections → add "T1 entry" section
Channel Report: scope = Laps, headings Venue › Driver › Lap, stats avg/min/max, Δ vs Overlay 0 → Export TSV
Time Report: section times with eclectic and rolling minimum; ALT+3 colours the 3 % band
Scatter: Throttle vs Brake, trend polynomial 3, equation shown; click a point → cursor jumps
A → animation at 10×; CTRL+SHIFT+P → "Toggle single colour"
```

### 6.6 May slip (to M4)

If U-15 is "no", the D-046 list is the ordered may-slip list: `LayoutEditor` (keep drag/resize on the canvas); scatter interval average, tiling and colour channel; histogram text and line styles and tiling; `TimeReport` % bands, eclectic column and rolling minimum (keep plain section times); Channel Report transposed layout; animation; annotations, status strip and device events; Speed+LatG track generation (keep GPS). Also `Print to Clipboard` and the animation rate menu. Items cut twice go to v1.0 (§1.5) and the user is told.

---

## 7. M4 — Hardening and 0.1 (4 weeks, 100 h)

**Goal.** Budget warnings, HiDPI sweep, soak, packaging, notices and documentation; tag `v0.1.0`. LRU eviction and the stress fixture moved to M5 (D-043).

### 7.1 Deliverables

| Area | Deliverable |
|---|---|
| `esses_store` / `esses_qtmodel` | Budget tracking as in M0–M3, no eviction in 0.1 (D-043); `SourceLoader` warns in `MessagesWindow` before loading when the probe estimate (raw + pyramids) exceeds the budget (25 % RAM, min 1 GB, configurable) and lets the user continue or cancel; `Stats` surfaced in `MessagesWindow` and a Performance line in the status bar (D-011) |
| Changed source files | `MappedFile` sharing policy (`FILE_SHARE_READ \| FILE_SHARE_DELETE`, never `FILE_SHARE_WRITE`) verified on NTFS and an SMB share; `changedOnDisk()` checked on application focus and before each decode batch; a changed file marks its source stale (Data-window badge, Messages row with Reload) and `file.reload_source` reopens it through the open pipeline with the sidecar re-applied and the selection restored by `RangeId` (02 §1 behaviour 5, §8 AC-4a; 06 §6, §14.4; E-5) |
| HiDPI | Sweep at DPR 1, 1.25, 1.5, 2 and mixed-DPI floating docks on Windows; per-monitor DPR change relayout; goldens at 1 and 2 stay authoritative (D-025) |
| Soak | `tests/soak/saturday` Qt Test with 10 files open under ASan/UBSan: 30 min nightly, 2 h before each release tag; committed `tests/soak/lsan.supp` suppresses Qt, fontconfig and glib leak stacks |
| Packaging | windeployqt + Inno Setup installer + portable zip (`.ld` association task, `-w <project> -l <log>`); macdeployqt DMG and linuxdeploy AppImage unsigned; `NOTICES.md` + Qt source offer + generated third-party manifest; CLI inside every package and standalone (D-034) |
| Docs | `docs/user/` skeleton (install, open files, selection, graph, maths, troubleshooting); `CLEANROOM.md` complete for every 0.1 behaviour; `CHANGELOG.md`; `docs/milestones/M4.md` |
| Data | Default circuit workbook final; default schemes final; `keymap.json` final |
| Release | `v0.1.0` tag; GitHub release with checksums; public alpha note (not the beta announcement, D-035) |

### 7.2 Acceptance criteria

| Id | Criterion | Proof |
|---|---|---|
| AC-M4-01 | Every §2.2 item maps to a named test id in `docs/milestones/M4.md`: items 1–10 in the Qt Test walkthrough `tests/ui/walkthrough_saturday` (offscreen on Windows and Linux as required checks, macOS nightly; one test function per item so a failure names the item), item 11 as ctest golden-JSON tests of each `esses-cli` subcommand, item 12 on the manual VM checklist of AC-M4-04 | `tests/ui/walkthrough_saturday`, `ctest -L cli`, VM checklist |
| AC-M4-02 | Budget behaviour without eviction (D-043): opening a synthetic `.ld` whose probe estimate exceeds the configured budget shows the Messages warning with the estimate before any chunk is decoded; RW-2 + 20 maths channels stays ≤ 1 GB resident (§12). The eviction and stress criteria are AC-M5-09 | `tests/ui/budget_warning`, `bench/open_bench` |
| AC-M4-03 | Soak with 10 files open under ASan/UBSan, 30 min nightly and 2 h on the release candidate: no crash, no `Dispatcher` thread violation, and no leak whose stack contains an `esses_*` frame (Qt, fontconfig and glib stacks are suppressed by `tests/soak/lsan.supp`) | `soak` (nightly 30 min; 2 h pre-tag) |
| AC-M4-04 | Installer and portable zip launch on clean Windows 10 22H2 and Windows 11 VMs, plus a Windows VM with an old VC++ redistributable (< 14.40) on which the installer's runtime check installs or refuses as designed (08 §7.1); AppImage launches on a clean Ubuntu 24.04 VM; the DMG passes a launch smoke test on the `macos-14` runner (a macOS VM is permitted only on Apple hardware); AppImage and DMG are "builds green, not supported" (D-116) | manual VM checklist in `docs/milestones/M4.md`, `package` smoke tests |
| AC-M4-05 | All MVP budget rows of §12 green on the reference machine (median of ≥ 5 runs, D-042); hosted-runner alarms reviewed | `bench` |
| AC-M4-06 | `NOTICES.md` lists every shipped third-party component with licence text or pointer; the About dialog shows the Qt LGPL notice and the exact source URL; `reuse lint` and the generated SBOM pass | `lint`, `package` |
| AC-M4-07 | Mixed-DPI: dragging a floating `time_distance` between a DPR 1 and a DPR 2 monitor re-renders at the new DPR with no blur and no layout drift (manual on Windows + automated DPR-switch test offscreen) | Qt Test `dpr_switch` |
| AC-M4-08 | Vendor-mark grep passes on the release tree including installer strings, icons, file names and the `.desktop`/`Info.plist` metadata | `lint` |
| AC-M4-09 | `esses --version` prints version, commit, Qt version and build date; the installer's version equals the tag | `package` |
| AC-M4-10 | 02 §8 AC-4a passes on NTFS and on an SMB share: a rename-over writer succeeds while the old bytes stay mapped, an in-place truncating writer gets a sharing error, a fault inside `guarded()` returns `Io` with the chunk sealed `AllInvalid`, and `changedOnDisk()` flips after a replace; the source then shows the stale badge and `file.reload_source` reopens it with its sidecar and selection restored (until this passes, E-5's default applies: a changed file is detected and marked stale) | Catch2 `MappedFile` tests on `windows-2022`, SMB case on the manual VM checklist, Qt Test `tests/ui/reload_source` |

### 7.3 Effort breakdown (100 h)

Budget warning + Performance line 4 · HiDPI 14 · soak + suppressions + fixes 16 · packaging + notices 26 · docs 16 · release 10 · Saturday test map + VM checklist 14. (Eviction + stress, 18 h, moved to M5, D-043.)

### 7.4 Dependencies

M3 closed; U-08 (Qt 6.12 installed locally since M0 week 1, so the release build is reproduced on the developer's machine; D-126); U-11 signing budget (default: unsigned 0.1); U-09 Linux/macOS release status for 0.1 (default: artefacts published, labelled "builds green, not supported"); U-16 eviction staging (default: eviction in M5, D-043).

### 7.5 Demo script

```
Install esses-0.1.0-win64.exe on a clean VM → double-click a .ld (association) → app opens with the file
esses -w <project> -l <log>              # command-line project + log
Open a file larger than the budget → Messages warns with the estimate before loading; continue → the Performance line shows resident bytes against the budget
Help → About → Qt notice and source link; NOTICES.md in the install directory
Unzip the portable build on a USB stick → settings live beside the exe
```

### 7.6 May slip (to v1.0.x)

macOS DMG (if no hardware), mixed-DPI floating docks (document the limitation), user docs beyond install/open/graph.

---

## 8. v1.0 — M5 parity (13 weeks, 325 h) and M6 formats and SDK (8 weeks, 200 h)

### 8.1 M5 deliverables

| Area | Deliverable |
|---|---|
| i2 file importers (D-005, D-019, D-024) | `.i2wkb` v101/v200 → `workbook-1` with `x-i2` bag and import report; `.i2wsp`; `.mcs/.mfs` → `scheme-1`; maths XML → `maths-1` (1:1 item mapping, known `Params` → module settings, rest flagged); from public config-model notes and the user's own files only (D-005, D-039); the Kunos ACC workspace and vendor templates are private fixtures with counts-only public goldens (D-040) |
| Store (D-011, D-043) | `ChunkCache` LRU eviction of unpinned chunks against the byte budget (moved from M4), with eviction groups (all or none) weighted by measured recompute cost for whole-node maths outputs and text slabs (02 §3.2 staged refinements, 03 §6.2); RW-1-class stress fixture (synthetic 2.85 GB `.ld`, 6 h × 250 channels); `OutOfBudget` when pinned bytes alone exceed the budget (D-053); the 1 GB cold first-paint gate |
| Components | `spectrum` (pocketfft, Bartlett averaging), `suspension_histogram`, `mixture_map`, `track_report`, `rainbow_track`, `gps_track`, `gauge_dial`, `gauge_light`, `gauge_wheel`, `notes`, `picture` |
| Dialogs, export | Corrected GPS dialog and the `esses.gps_corrections` correction steps (its zero-fix validity step ships in M2, 03 §9.2); Run and Stage editors; four-pane indexed Open dialog on the sqlite3 amalgamation keyed `(path, mtime, hash)` storing `probe_json` rows; print; `.ld`+`.ldx` export UI; `.mat`; KML; workspace `Archive` |
| Maths, profiles | Chunk-parallel maths evaluator replacing the 0.1 whole-node path (D-139, 03 §6.0: register bytecode, cumulative halos and stage buffers, `Ordered` checkpoints, per-chunk tasks, the chunk-order determinism test) with the range provider call `IChunkProvider::materialise(ChunkRange, ChunkSink&, CancelToken)` (02 §3.2 staged refinements); 3D tables, `regression` item, `esses_mathfn_v1` host, `esses.damper_velocity`; Drag (`runs`, splits, split stretching) and Rally (`stages`, whole-file tracks) as profile data + rules (D-036) |
| Renderer (D-025) | `GlBackend` **only if** the v1.0 budgets fail on the user's hardware or RW-4; granularity decided by benchmark |
| ABI (D-028) | `sdk/esses_abi.h` frozen; `motec.csv` moved out of tree to `plugins/formats/csv/`; Clang and MinGW example plugins load in the MSVC host; `abi-compat` job |
| Release | `v1.0.0`; public beta announcement |

### 8.2 M5 acceptance criteria

| Id | Criterion |
|---|---|
| AC-M5-01 | The user's real workbooks import with ≥ 90 % attribute coverage (mapped attributes ÷ attributes present, computed by the import report) and every unmapped attribute listed with its `x-i2` path; for each, a hand-built reference `workbook-1` of the same layout yields `DrawList` goldens that the imported workbook matches for ≥ 90 % of components |
| AC-M5-02 | The Kunos ACC workspace (`base_ACC.i2wsp-archive`, private fixture, D-040) imports with a diagnostics list and no crash |
| AC-M5-03 | The list of feature-inventory row ids tagged "core" and "important" (excluding video, telemetry and setup sheets) is frozen in `docs/milestones/M5.md` at the M4 tag; every frozen row is closed with a test id or an explicit, user-approved deferral |
| AC-M5-04 | v1.0 budgets (§12) green nightly on the reference machine (median of ≥ 5, D-042) |
| AC-M5-05 | `abi-compat` green: the Clang and MinGW example plugins load in the MSVC host on windows-2022; a Rust plugin if cheap |
| AC-M5-06 | The indexed Open dialog filters 5,000 catalogued files in ≤ 100 ms; a moved file is detected by hash |
| AC-M5-07 | `spectrum` equals a pocketfft reference on a synthetic two-tone signal; `mixture_map` and `suspension_histogram` equal brute force |
| AC-M5-08 | `.ld`+`.ldx` export of a sidecar-edited source re-imports into Esses with the edited markers and details, and pinned versions of i3rs and motec-i2 (both MIT) decode it to the same channel values (ints exact, floats ≤ 1 ulp). i2 is never run on Esses-written files (EULA field of use, D-038) |
| AC-M5-09 | Eviction (moved from AC-M4-02, D-043): the RW-1-class stress fixture opens and a 10-minute scripted browse keeps resident memory within the budget (25 % of RAM) throughout; eviction never frees a pinned chunk (assert); a 1 GB `.ld` reaches first paint ≤ 3 s cold (fresh process, OS file cache flushed, median of ≥ 5 on the reference machine) |

### 8.3 M6 deliverables and acceptance

Converter harvest wrap-then-port (D-028): each of the user's converter parsers is first wrapped as a C-ABI plugin under `plugins/formats/<vendor>/` (QtCore privately linked), then XRK, MDF4+DBC, IBT and PDS are ported into `esses_formats` gated by a hash-pinned fixture and an oracle diff. The converter is its Qt C++ version: on 2026-10-11 the user declared the Python version deprecated, and the Qt C++ tree with its per-format JSON mapping files (Haltech, MaxxECU, Emtron, Emublog, Cosworth, Syvecs, iRacing IBT, WinDarab) is the reference for the harvest and for the text profiles below; verbatim reuse still waits for U-06 (Q-15, note on D-144). Text profiles (profile + fixture + dictionary each) for Pi, EFI, RacePak ASCII, VBO, TrackAddict, RaceChrono, RaceBox, MSL, RaceCapture, Haltech/Link/MaxxECU/Syvecs. `OWNERS` per format once a second contributor exists; SDK packaging; `DcfProvider` only on a measured CSV-reopen complaint; fixtures corpus moved to its own CC0 repository.

| Id | Criterion |
|---|---|
| AC-M6-01 | ≥ 15 formats with ≥ 2 fixtures each and oracle diffs (bit-exact ints, ≤ 1 ulp floats) |
| AC-M6-02 | A plugin built out of tree with `find_package(EssesSDK)` loads on three OSes |
| AC-M6-03 | CSV 300 MB → first paint ≤ 6 s; reopen ≤ 600 ms if `DcfProvider` was built |
| AC-M6-04 | Every harvested parser has a `CLEANROOM.md` entry naming its provenance and the user's confirmation (U-06) |

### 8.4 v1.0 demo script (M5)

```
File → Import → i2 workbook: <user>/workbook.i2wkb → import report lists unmapped attributes → worksheets render
File → Open (indexed dialog): filter Venue = Bathurst, Driver = …, 2024 → Details pane shows probe_json
Add spectrum of 'Damper Pos FL' over the zoom; mixture map RPM × MAP coloured by lambda
Profile → Drag: runs appear with splits; Run Editor edits a split
File → Export Data → .ld + .ldx → re-open the result in Esses; i3rs/motec-i2 decode it to the same values (i2 is not run on Esses output, D-038)
esses --render-backend gl (only if GlBackend was built) → same goldens ≤ 2 %
```

---

## 9. v1.1 — M7 (8 weeks, 200 h)

Deliverables (D-030, outline M7): `esses_script` (Lua 5.4 + sol2 sandbox: restricted `_ENV`, no `io/os/debug`, no binary chunks, instruction-count hook, host reader object, off the UI thread) exposing the object model for user maths functions (`IMathFunction`), lap rules (`IRangeRule`), scripted importers (`IImporter`) and report templates; `esses_video` (Qt Multimedia FFmpeg backend behind `IVideoPlayer`, PTS/log-time offset in `videoLinks`, sync points, camera views); alarms (basic); setup-sheet CSV/JSON timeline; JSON-RPC automation server; `esses_capi` (+ Python wheel only on demand, U-12); `RhiBackend` evaluation behind the `IRenderBackend` seam.

| Id | Criterion |
|---|---|
| AC-M7-01 | Scripted tutorial test: `tests/script/tutorial_importer` builds the tutorial's Lua importer step by step and opens its CSV fixture to the expected `probe_json`. A trial by someone other than the author (< 1 h) is a non-blocking goal recorded in the milestone note |
| AC-M7-02 | Sandbox tests: `io`, `os`, `debug`, `load` of binary chunks, infinite loops and file access outside the host reader are all rejected |
| AC-M7-03 | Video stays within one frame of the cursor at 1× playback on a GoPro H.264 clip; frame-step emulation documented |
| AC-M7-04 | RPC drives a headless export end to end; `esses_capi` symbols are stable (ABI check) |

---

## 10. Deferred lists

### 10.1 Deferred from 0.1 to v1.0 (M5/M6)

1. `.i2wkb` v101/v200, `.i2wsp`, `.mcs/.mfs`, maths XML importers and the import report (D-035, U-07).
2. Indexed four-pane Open dialog and sqlite3 catalogue (D-019).
3. `GlBackend` (conditional on budgets, D-025); `RhiBackend` later.
4. `.ld/.ldx` and `.mat` export UI; KML export; workspace `Archive`.
5. Components: `spectrum`, `suspension_histogram`, `mixture_map`, `track_report`, `rainbow_track`, `gps_track`, `gauge_dial`, `gauge_light`, `gauge_wheel`, `notes`, `picture`.
6. Corrected GPS dialog and the correction steps of `esses.gps_corrections` (duplicate removal, time shift, shadow or prefixed outputs; its zero-latitude/longitude validity step is always on from M2, 03 §9.2); `esses.damper_velocity`.
7. 3D tables, `regression` item, C-ABI maths functions; the chunk-parallel maths evaluator and the range provider call that replace 0.1's whole-node evaluation (D-139, 03 §6.0, 02 §3.2).
8. Drag and Rally rules, Run/Stage editors (model carries `RangeKind::Run/Stage` and `STGSTRT/STGEND` from M0, D-036).
9. Print (CTRL+P).
10. C ABI freeze, `motec.csv` out of tree, `abi-compat` job, SDK packaging, example plugins.
11. Converter harvest and the text-profile wave; `OWNERS`; fixtures repository.
12. `DcfProvider` (on-disk column cache) and SIMD decode kernels, both only on a measured complaint (D-011, D-029).
13. Schema-generated property grids (for plugin components only).
14. Code signing (U-11) if budget arrives.
15. `ChunkCache` LRU eviction with eviction groups weighted by recompute cost (02 §3.2), the RW-1 stress fixture and the 1 GB cold first-paint gate (D-043, U-16).
16. If U-15 = yes: the D-046 cut list (Speed+LatG track generation, Time Report eclectic/rolling minimum/% bands, annotations/status strip/device events, animation, `LayoutEditor`, scatter interval average/tiling/colour channel, histogram text/line styles/tiling, Channel Report transposed layout).

### 10.2 Deferred to v1.1 (M7)

1. Lua scripting (`esses_script`) for functions, rules, importers, report templates.
2. `esses_video`, camera views, sync points.
3. Alarms (basic); setup-sheet timeline.
4. JSON-RPC automation; `esses_capi`; Python wheel on demand.
5. `RhiBackend` evaluation; `QRhiWidget` path.
6. Multi-channel maths plugins (`IMtcMathPlugin` analogue as N-channel modules).
7. Live sources (`LiveProvider`, RW-5), `colour_strip`.

### 10.3 Later or never

Telemetry transport, WebView, Race Control, device download/config viewers, licence activation, COM automation (replaced by RPC), Excel-macro setup sheets, satellite tiles (optional QGeoView pane later), Engine profile (reduced component set, later), Python host plugin.

---

## 11. Early spikes (M0 week 1, 2.5 days total)

Each spike is a throwaway branch `spike/<id>`; the report goes into `docs/spikes/<id>.md` and `docs/milestones/M0.md`. Spikes do not add production code; they settle a decision that would otherwise be made by guesswork.

| Id | Question | Method | Pass criteria | Decision on fail | Time box |
|---|---|---|---|---|---|
| S-1 `.ld` reader on the user's real files | Do the public layout notes decode every file the user owns, and which shift/mul order is right? | Prototype reader (mmap, header walk, per-channel decode) on every `.ld` the user supplies plus the public-project fixtures; emit `probe_json` and per-channel CSV; compare with motec-i2/i3rs output and, for a file from a MoTeC device, i2's own CSV export (U-03 (1) answered yes on 2026-10-10, D-038); find a channel with `shift ≠ 0 ∧ mul ≠ 1` | 100 % of channels decode; ints bit-exact, floats ≤ 1 ulp against one oracle; scaling order settled by a real MoTeC-logger file or recorded "unsettled, default `ld.shiftOrder = beforeMul`" (D-037) | Per-file dialect flags, `--dialect` override, tolerant reader with warnings | 1 day |
| S-2 M4-decimation benchmark | Does exact M4 with hierarchical descent over the factor-4 pyramid meet D-012's cost model? | Catch2 bench: 20 columns × 3.6 M F32 with 1 % NaNs, chunked and pyramided; M4 for 3,800 px at 6 zooms, one thread; equality with brute force | ≤ 5 ms total for 20 traces at every zoom on one core; pyramid build ≥ 1 G samples/s per core; memory 0.67× raw | Keep factor 4; parallelise per trace earlier; raise the `spp ≥ 32` threshold; record toward D-025's GL trigger | 0.5 day |
| S-3 ADS on three OSes | Does ADS build as a shared library and behave behind `DockHost` on Windows, Linux, macOS? | FetchContent ADS against 6.7.2 locally and 6.12 in CI; six-dock test app: save/restore perspective, auto-hide, float, re-dock, close; offscreen in CI, interactive on Windows (macOS interactive only with hardware, U-09) | Build + Qt Test pass on all three runners; no visual breakage on Windows | `QDockWidget` fallback for the failing OS (D-026); macOS "builds green" only | 0.5 day |
| S-4 raster budget test | Can QPainter into `QImage` layers meet cursor ≤ 4 ms and zoom ≤ 16 ms for 20 traces × 3,800 px at DPR 1 and 2? | Throwaway widget: Static = 20 antialiased polylines × 15,200 points + axes + grid into `Format_ARGB32_Premultiplied` at device pixels; Dynamic = cursor line + 20 `QStaticText` labels; 200 frames at 1920×1080 DPR 1 and 2560×1440 DPR 2 | Static ≤ 12 ms, Dynamic ≤ 2 ms at both sizes (headroom for layout and data work) | Cosmetic pens, no AA when `spp ≥ 32`, preallocated `QPolygonF`, per-group Static images; if still over, D-025's GL trigger fires at M3 and M1 adds a `GlBackend` skeleton | 0.5 day |

**S-1 result (2026-10-10).** Report: `docs/spikes/S-1.md`. The prototype was Python (`tools/ld_ref_decode.py` and `tools/spikes/s1/`), not a C++ `ld_probe.exe` on a spike branch, because M0 had no reader yet and git history was not to change in that phase. 86 of 87 `.ld` files decode (the 87th is header-only); unscaled integers and floats are bit-exact against the converter where it decodes (scaled integers within 2 F32 ulp, because the converter computes in single precision), and 53,431 cells of vendor-software CSV exports within print precision; the shift-vs-mul order was settled as `afterMul` by physical evidence (D-037); the layout facts amended 05 §3 (u16 channel count, u32 detail pointers, pointer-chained gap and aux blocks, M1 terminator, `dtype_a` 0x06 integers).

Also in week 1, not a spike: the vendor-mark grep and `qt-free-check` must exist before the first library is written, so clean-room and layering are enforced from commit one (D-003, D-007).

---

## 12. Budget gates by milestone

Budgets are the outline §8 rows; the milestone column says where each first becomes a gate (D-042). Every row is an **absolute limit**, checked as the median of ≥ 5 repetitions on one **reference machine**: the developer's machine registered as a self-hosted runner, or a pinned dedicated runner, with CPU, core count, RAM and OS recorded in `bench/workloads.md`. The self-hosted job runs on pushes to `main`, nightly, on tags and on manual dispatch, never on pull requests from forks. GitHub-hosted runners run the same benchmarks only as a non-blocking ±30 % regression alarm against their own baseline. "Cold" means a fresh process with the OS file cache flushed on the reference machine. A row that fails because of the reference machine, not the code, moves one milestone under §1.5.

| Budget | MVP gate | From | v1.0 gate (nightly) | From |
|---|---|---|---|---|
| Cursor move, 20 traces × 3,800 px | ≤ 4 ms (Dynamic only) | M1 | RW-4 ≤ 2 ms CPU | M5 |
| Zoom/pan step, 20 traces | ≤ 16 ms | M1 | RW-4 rebuild ≤ 33 ms hard, ≤ 8 ms typical | M5 |
| Full relayout / resize | ≤ 50 ms, coalesced to 30 Hz | M1 | same | M5 |
| Open 100 MB `.ld` → first paint (8 ch) | ≤ 500 ms | M1 | ≤ 300 ms | M5 |
| Open 1 GB `.ld` cold → first paint | — | — | ≤ 3 s | M5 |
| `SummaryPyramid` for all channels of 1 GB (background) | — | — | ≤ 6 s | M5 |
| Values window refresh, 250 channels | ≤ 2 ms | M1 | ≤ 1 ms | M5 |
| Elementwise maths, 2.16 M samples, one core | ≤ 10 ms | M2 | ≤ 3 ms | M5 |
| 20 maths channels of `bench/workloads.md` on RW-2 ready after request | ≤ 2 frames | M2 | ≤ 2 frames | M5 |
| `filter_lp` order −2, 21.6 M samples, one core | — (M2 records) | M2 | ≤ 150 ms | M5 |
| Channel Report 50 laps × 30 ch × 8 stats, cold | ≤ 300 ms | M3 | ≤ 200 ms | M5 |
| CSV 300 MB (100 columns) → first paint | ≤ 10 s | M2 | ≤ 6 s | M6 |
| Resident memory, RW-2 + 20 maths channels | ≤ 1 GB | M2 | ≤ 1 GB | M5 |
| Resident memory, RW-1 | — (0.1 warns at open, D-043) | M4 | budget (25 % RAM) honoured | M5 (stress, D-043) |
| Startup to empty workspace (warm) | ≤ 1 s | M1 | ≤ 1 s | M5 |
| Probe of a 500 MB synthetic `.ld` | ≤ 200 ms | M0 | ≤ 200 ms | — |

---

## 13. Risk register

L/I = likelihood / impact (H/M/L). "Signal" is the observable that says the risk is materialising.

| Id | Risk | L/I | Signal | Mitigation (in the design) | Spike / owner |
|---|---|---|---|---|---|
| R-01 | Effort optimism (judges: parity-first ~1.5× optimistic; the review re-estimate, D-045, raised 0.1 from 750 h to 900 h + 15 % contingency) | H/H | M0 exceeds 130 h, or any later milestone exceeds its hours by > 20 % | Re-plan M1–M4 with M0's actual/plan ratio before M1 starts; contingency line (§1.3); U-15 cut list (D-046); §1.5 slip policy; raster-only, Windows-first 0.1 acceptable; never cut what cannot be retrofitted | every milestone note |
| R-02 | Numbers differ from i2 (window edges, `derivative`, filters, stretch, corrected speed) | H/H | AC-M2-01 items "OK" but values differ from the user's traces | `docs/maths-compat.md`, ≥ 150 case files, compat switches, oracle cases (U-03 answered yes on 2026-10-10), "method" selectors with "not bit-identical" messaging | M2; U-03 |
| R-03 | `.ld` dialect zoo | H/M | S-1 misdecodes a file | Fixture family per dialect, per-file dialect flags, `ImportLog` surfaces every heuristic | S-1, M0 |
| R-04 | Raster ceiling before v1.0 | M/H | S-4 fails or `bench` trends to the limit | Renderer seam + `LayerCache` from M1; budgets gate every milestone; `GlBackend` additive (D-025) | S-4, M1 |
| R-05 | Scope creep toward the 271-feature inventory | H/H | Items outside §2.2 in a milestone branch | Inventory priorities and D-035 drive milestones; "nice" → §10 or contributors; PR template asks which AC a change serves | every PR |
| R-06 | Clean-room slip (vendor string, icon, help text, sample file) | L/H | Vendor-mark grep fails; fixture without provenance | `CLEANROOM.md`, CI grep, no vendor templates/schemes/icons/samples (the i2 install's `Samples/` and the 2009 `conv_samples/` files are never committed), PR provenance checkbox | M0 week 1 |
| R-07 | Threading bugs | M/H | TSan report; UI stall > 100 ms in soak | One pool, one `Dispatcher`, immutable snapshots, `ChunkHandle` pinning from M0, TSan on maths/plotcore | M0, M4 |
| R-08 | Bus factor 1 | H/H | No second contributor by M6 | Qt-free libs with fixtures and schemas; ADRs; `CLAUDE.md`/`AGENTS.md`; MIT so converter projects adopt the format library; CC0 fixtures repo | M6 |
| R-09 | Qt version churn | H/L | 6.12 breaks while 6.7.2 passes | Minimum 6.7, CI on 6.12 from M0, yearly bump task, Qt confined to layers 4–5 | M0 CI |
| R-10 | ADS unusable on macOS | M/M | S-3 fails on macos-14 | `DockHost` with `QDockWidget` fallback (D-026); macOS "builds green" at 0.1 | S-3 |
| R-11 | Memory before eviction exists (M0–M4, release 0.1) | M/M | Budget warning during M2–M4 testing; 0.1 users report files above the budget | Fixture set sized to fit; budget tracked from M0; warning at open from the probe estimate (M4); LRU with the stress fixture in M5 (D-043) | M5 |
| R-12 | Workbook import fidelity at v1.0 | M/M | AC-M5-01 < 90 % | `x-i2` bag + import report; measured weekly on the user's real workbooks | M5 |
| R-13 | ABI frozen too early | M/M | A harvested parser needs a missing field | `struct_size`, append-only, `_v1`/`_v2` coexistence; unstable until the M5 freeze; CSV-out-of-tree and example plugins as consumers | M5 |
| R-14 | Fixture hosting and donated-file licensing | M/L | Repo growth; fixture without `licence` | Small in-tree, large via `remote` + sha256; CC0 synthetic; permission recorded; private submodule; unlicensed third-party exports and vendor/ACC maths private only, counts-only public goldens (D-040) | M0 |
| R-15 | Trademark or EULA exposure (the i2 EULA licenses use only with data from MoTeC devices or MoTeC-licensed software and forbids reverse engineering) | M/H | A test, demo or criterion runs i2 on non-MoTeC or Esses-written data; a provenance line cites user files without the U-03 consent date | No vendor marks (D-003); field-of-use rule in `CLEANROOM.md` (D-038): i2 runs only on MoTeC-device files, never on Esses output; behaviour observation black-box only, never disassembly, with the legal check the user's own responsibility (U-03 (3), yes on 2026-10-10); i2 XML only from user-owned files with the research-phase use recorded (D-005, D-039); `TRADEMARK.md`; no trademark search scheduled (U-02, answered 2026-10-10), the name stays swappable by scripted rename | M0, before `CLEANROOM.md` |
| R-16 | Late user confirmations | M/M | A §14 row past its "needed by" | Every row has a default; answers applied by script, never by redesign | continuous |
| R-17 | Golden images flaky across platforms | M/L | Goldens differ by OS | Pixel goldens authoritative on ubuntu only and regenerated only from CI artefacts; Windows and macOS run the Qt-free `DrawList` JSON goldens and render smoke tests; pinned Noto Sans test font in `tests/golden/fonts/`, offscreen platform, ≤ 0.5 % tolerance; `time_distance` capped at 12 named cases (08 §6.4, D-127) | M1 |
| R-18 | Unsigned binaries → SmartScreen/Gatekeeper friction | H/L | User reports | Documented in the release note; signing gated on U-11 | M4 |
| R-19 | Self-hosted bench runner exposed to untrusted code | L/H | A fork pull request reaches the self-hosted job | The self-hosted job never runs on `pull_request`; only pushes to `main`, schedule, tags and manual dispatch (D-042) | M1 |

---

## 14. User-confirm checklist

Each row: the question, the decision it feeds, the default the plan proceeds on, when the answer is needed, and how a different answer is applied (always by script or data edit, never by redesign).

| Id | Question | Decision | Default if unanswered | Needed by | Applied by |
|---|---|---|---|---|---|
| U-01 | Licence: MIT, Apache-2.0, or GPLv3-app/MIT-libs? | D-002 | MIT whole tree, DCO, no CLA | **Answered 2026-10-10: MIT** | `LICENSE`/SPDX swap; GPL split additionally assigns the maths engine and documents plugin separateness in `sdk/` |
| U-02 | Codename "Esses"; trademark search before release | D-003 | Esses | **Answered 2026-10-10: Esses.** No trademark search is needed for now; the name may change later | `tools/rename.py` over `cmake/naming.cmake`, namespaces, docs |
| U-03 | The three D-005 answers: may i2 exports of the user's logs be black-box oracles (only for logs from MoTeC devices, D-038); may the user's i2-written files be parsed (the research already did so on 2026-10-09, D-039); may behaviour be observed on the user's install (only after the user's legal check that black-box observation is not "reverse engineering" under the EULA)? | D-005, D-038, D-039 | No to all three: provenance lines citing user files are re-sourced or marked "unsourced, chosen default"; D-017/D-020/D-047 defaults recorded "chosen, not observed" | **Answered 2026-10-10: yes to all three** (observation black-box only, never disassembly; the legal check is the user's own responsibility). The ACC sub-question was answered the same day: ACC files are not MoTeC-licensed, so i2 is never run on ACC data (D-038 (a)) | `provenance.kind = "oracle"` cases in `fixtures/private`; `CLEANROOM.md` rows; user-file-derived sections of `background/research/config_model.md` kept out of the public repository |
| U-04 | Which real files may be committed, public vs private submodule? | D-033, D-040 | Nothing real; synthetic families only; third-party exports only where their licence permits redistribution; the user's ACC files are ordinary third-party fixtures, private for now (D-127) | **Answered 2026-10-10: nothing real public for now**; revisit before the first public release | `fixtures/private` population; in-tree fixtures with `licence` |
| U-05 | i2 observations, permitted since U-03 (3) was answered yes on 2026-10-10: overlay 1..n colours/renumbering; `-2^2`; Values-window content; stretch tolerance. Real-file evidence (no i2 run needed): shift-vs-mul from a MoTeC-logger file with shift ≠ 0 ∧ mul ≠ 1 | D-016, D-017, D-020, D-037, D-047 | Compact renumbering + scheme `overlayStyle`; `-2^2 = -4` switchable; Values = the focused component's channels with an "All channels" toggle; 5 %; `ld.shiftOrder = afterMul` (settled by spike S-1, D-037); the i2 items recorded "chosen, not observed" | M0 (shift/mul: answered by S-1, 2026-10-10), M1 (colours), M2 (`-2^2`), M3 (rest) | Scheme edit, compat flag default, option default, dialect flag |
| U-06 | Converter: MIT-compatible; which parsers are the user's own; buildable as a plugin DLL? | D-028, D-144 | Harvest waits for a yes; until then the `.ld` reader and writer are new code written from the port specification and nothing is ported verbatim (D-144) | M6 start (any time for verbatim `.ld` reuse) | CSV-type parsers become text profiles regardless; binary formats from public sources |
| U-07 | Scope cut (workbook/workspace import at v1.0) and the re-baselined plan (0.1 ≈ 42 weeks including 15 % contingency, tests 30 %) | D-035, D-045 | Cut stands; re-baseline accepted | **Answered 2026-10-10: both accepted** | Reversal inserts "M2b importers" (+4–6 weeks); 0.1 at ≈ 47 weeks |
| U-08 | Qt 6.7.2 locally until M4, or 6.12 now? | D-006, D-126 | Install Qt 6.12 locally in M0 week 1 (08 §3.7.4, §12) | M0 week 1 | One-hour aqtinstall; removes CI-only failures |
| U-09 | Reference machine and minimum hardware/OS for budgets; Linux/macOS release targets for 0.1; macOS hardware? | D-025, D-034, D-042 | Developer's machine as the self-hosted reference runner, Windows 10+; Linux/macOS "builds green, not supported" | M1 (budgets), M4 (labels) | VM checks added to AC-M4-04; baseline lowered for weaker hardware |
| U-10 | Default maths precision F32 or F64 | D-022 | F32 buffers, F64 accumulators | M2 | `maths-1` module default |
| U-11 | Code-signing budget | D-034 | Unsigned | M4 | Secrets added to `package` |
| U-12 | Python wheel before v1.1? | D-030 | Formats first | M7 | Wheel work moves into M6 |
| U-13 | Paths of the user's real files for S-1; may the 2009 i2 sample files be local oracles only? | D-004, D-005, D-117 | User's own files only; vendor samples unused | Paths: M0 week 1. **Answered 2026-10-10 for the samples**: allowed for local/private testing; the paths were supplied for S-1 the same day, and the T2 test models count as samples (D-146) | Never committed or redistributed in any case (D-004) |
| U-14 | Stretch tolerance default; alignment offsets in the sidecar (D-015 says yes) | D-015, D-016 | 5 %; sidecar | M3 | Option default |
| U-15 | Adopt the optional 0.1 cut list up front (Speed+LatG track generation, Time Report eclectic/rolling minimum/% bands, annotations/status strip/device events, animation, `LayoutEditor`, scatter interval average/tiling/colour channel, histogram text/line styles/tiling, Channel Report transposed layout)? | D-046 (amends D-035/D-027 if yes) | No: D-035 scope stands and the list is M3's ordered may-slip list | M3 start | Items move to §10.1; M3 ≈ 9 weeks; 0.1 ≈ 39–40 weeks |
| U-16 | Eviction and the stress fixture at M5 instead of M4 (0.1 warns on files above the memory budget) | D-043 (amends D-011) | Yes, M5 | M4 start | Reversal restores AC-M5-09 as an M4 criterion (+18 h) |

---

## 15. Discrepancies and proposed decisions (carried to `DECISIONS.md` open questions)

1. **Bench gate machine.** D-025 said "CI gate from M1 on the developer's laptop class"; D-033 said "on a fixed runner class (±15 %)". Resolved by D-042: absolute limits as the median of ≥ 5 runs on one reference machine; hosted runners are a non-blocking ±30 % alarm. Recorded in §12; 08_REPO_AND_BUILD.md §4.1 and §6.9 follow.
2. **Pre-release tags per milestone** (`v0.1.0-mN`, §1.2) are a proposed convention not in D-034.
3. **M0 week-1 spikes** (§11) are not in D-035/outline §7; they are placed inside M0's four weeks and cost 2.5 days.
4. **`esses-cli` subcommand staging.** Outline M0 lists `info/channels/laps/validate/fixture`; D-007 lists the full set without milestones. Proposed: `convert/bench` in M1; `eval`, `export`, `maths check` and `fixture diff` in M2 (D-044, D-090).
5. **`fuzz_maths_parser`** cannot exist in M0 (no parser); D-033's parser fuzz target starts in M2.
6. **Linux/macOS at 0.1.** Outline M4 acceptance says packages "launch on clean Windows 10/11, Ubuntu 24.04 and macOS 14 VMs" while open question Q-12 (formerly 9) leaves Linux/macOS release status open. Proposed: artefacts are built and launch-tested, labelled "builds green, not supported" unless U-09 says otherwise.
7. **Vendor sample files.** The importer samples shipped with i2 Pro (the install's `Samples/` folder, including four 2009 Conversion Wizard samples; never copied) are vendor-shipped; D-004 forbids shipping them. Proposed: never committed; local oracle use only if U-13 permits (U-03 was answered yes on 2026-10-10); `CLEANROOM.md` records the use. U-13 was answered on 2026-10-10: the samples may be used for local/private testing and are still never committed or redistributed (D-117).
8. **`keymap.json` rows marked `planned`.** D-033 requires one test per row; rows for commands not yet implemented carry `"status": "planned"` with a milestone and are exempt until that milestone (AC-M1-06, AC-M3-01).
9. **`DrawList` JSON goldens** in the Qt-free `core` job (R-17) are a proposed addition to D-033's golden set.
10. **Values-window content rule default** is the focused component's channels with an "All channels" toggle (parity-first §5.10, 06_UI_SHELL.md §8, D-047); an earlier draft of this document said "all channels of the Main source".
11. **Shift-vs-mul default and vocabulary.** An earlier draft here defaulted to the i3rs reading while 05 §3.6 defaults to `beforeMul` (gotzl); resolved by D-037: `ld.shiftOrder ∈ {beforeMul, afterMul}`, default `beforeMul`, the same key in fixture `importOptions` and Import Log details (08 §6.7 and 01 §8.2 follow). Spike S-1 settled the order on real logger data on 2026-10-10, and D-037 now defaults to `afterMul`.
12. **i2 field of use.** AC-M5-08 and the §8.4 demo ran the user's i2 on Esses-written files, outside the EULA's licensed use; replaced by re-import and third-party decoding (D-038).
13. **Plan re-baseline.** M0–M2 resequencing (D-044), eviction to M5 (D-043), 30 % test share and 15 % contingency (D-045) and the optional cut list (D-046) change outline §7 and D-035's timing; both are updated in the same revision.
14. **Offset nudge step.** D-093 chose one device pixel of domain units; 04 §3.3 and 06 §24 item 5 were then revised to one sample period of the active channel (time) / 0.25 m (distance) with ten-step UP/DOWN, which AC-M3-09 now uses. Proposed as D-138 (amends D-093).

**Cross-references.** 08_REPO_AND_BUILD.md (CI jobs, test layout, packaging, bootstrap); the sibling design documents for the data model/store (D-008..D-012), formats (D-028, D-029), maths (D-020..D-024), plot engine (D-025), UI shell and components (D-017, D-018, D-027) and persistence (D-015, D-019); `docs/maths-compat.md`; `bench/workloads.md`; `docs/milestones/Mn.md`; `CLEANROOM.md`.
