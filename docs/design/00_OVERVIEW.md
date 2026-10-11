# 00 — Overview: Esses

**Scope.** This document is the entry point to the design set for *Esses* (working codename, D-003, confirmed by the user on 2026-10-10; no trademark search scheduled, the name may change later): a clean-slate, free and open-source reimplementation of the MoTeC i2 Pro analysis workflow as a Qt 6 Widgets application over a Qt-free C++20 core. It states the vision, what the incumbent is and why a clean-slate FOSS product is justified by the research, goals and non-goals, target users, product principles, the licence recommendation and the list of decisions the user must still confirm, the codename and trademark policy, a reading guide to documents 01–08, and a glossary of the domain terms the design uses. It does not restate the architecture (01) or any subsystem; it fixes vocabulary and intent so the other documents can be read in any order. Where this document and `DECISIONS.md` differ, `DECISIONS.md` wins.

**Related decisions.** D-001, D-002, D-003, D-004, D-005, D-006, D-007, D-010, D-013, D-014, D-015, D-016, D-017, D-018, D-019, D-020, D-027, D-028, D-030, D-033, D-034, D-035, D-036.

---

## 1. Vision

Esses is a native, cross-platform motorsport data-analysis workstation whose **circuit-analysis workflow is interchangeable with MoTeC i2 Pro's**: workbooks of worksheets of cursor-linked components; a Data window with Main / Overlay 0 / Overlays 1..n lap selection; time and corrected-distance domains with lap alignment, offsets and variance; a maths engine that evaluates existing i2 expressions unchanged; channel reports, time reports, histograms, scatter plots and track maps with user sections; one project folder per vehicle or category that a team can share. It reads MoTeC `.ld` logs with their `.ldx` companions and CSV first, grows to the broadest importer set in the field through a permissively licensed, fixture-tested format library, and runs on Windows, Linux and macOS from the same source.

The one-sentence product statement: *a race engineer can replace i2 Pro for a Saturday at release 0.1 (D-035), keep their existing i2 maths and, from v1.0, their existing i2 workbooks, and never hit a licence gate, a Windows-only wall or a corrupted project file.*

Order of work is fixed by the user's priorities and D-035: the **analysis application first**, format breadth later. Everything that is not the circuit analysis workflow (plugin ABI, scripting, video, live data, dozens of importers) is designed as a seam now and built after 0.1.

---

## 2. What i2 Pro is

All statements here come from public sources (MoTeC product pages, the shipped HTML help, shipped XML configuration files, SDK samples, forum posts and the FOSS projects surveyed in `background/research/landscape.md`); no MoTeC binary was inspected (D-004).

1. **Product.** MoTeC i2 is the Windows-only analysis application shipped with MoTeC loggers and ECUs. *i2 Standard* is included with hardware; *i2 Pro* is enabled by a device password or a per-PC feature licence, and is gratis for non-professional sim-racing use. Pro adds advanced maths, unlimited overlays, components, workbooks and worksheets, multi-channel suspension histograms, FFT, time/distance variance, GPS track mapping, synchronised video and CSV import. MoTeC has stated that no Linux version is planned; Wine and PlayOnMac attempts fail.
2. **Workflow.** A *project* (folder) holds workbooks, worksheets, channel overrides, aliases, mappings, options, global maths and track maps. The user opens one or more `.ld` logs into the *Data window*, selects a *Main* lap and any number of *overlay* laps, and works in *worksheets* of linked *components* (time/distance graph, outing graph, scatter, histogram, channel report, time report, track position, gauges, values list and others; nineteen component classes in all). The cursor, datum, zoom range and time/distance mode propagate between components on a worksheet. Maths channels are written in a small expression language, calculated on demand and reported in a Channel Status window. Edits to a log (beacons, details, local maths, trusted-lap flags) are stored in an `.ldx` companion file; the `.ld` is never modified.
3. **Ecosystem.** i2 workspaces (`.i2wsp`) and workbooks (`.i2wkb`) are the de-facto exchange format in sim racing (Kunos ACC ships one); at least ten FOSS projects exist only to *write* `.ld` so that i2 can read their data.
4. **What users praise.** Distance-aligned lap overlay; time/distance variance; maths channels; per-project worksheets shared as workspaces; cursor-linked components; the track map with user-defined sections; relative intuitiveness versus Pi Toolbox; training material.
5. **What users complain about.** Windows only; video stutter since 2023; project files corrupting after a crash; HiDPI edge cases; hotkey bugs; a steep learning curve; Pro licensing tied to hardware or a PC; CSV import and third-party formats gated behind paid feature licences; the download/unpack/convert friction of getting non-MoTeC data in.
6. **Size of the surface.** The Phase 1 feature inventory (`background/research/features_compact.md`) lists 271 features across basics, components, maths and miscellany, each classified *core* (MVP parity), *important* (v1.0), *nice* (later) or *skip* (vendor- or licence-specific). D-027 and D-035 derive the MVP and v1.0 scopes from that classification.

---

## 3. Why a FOSS clean-slate (research lessons)

`background/research/landscape.md` §8 identifies ten evidence-based reasons why every FOSS attempt at an i2-class tool has stalled. The design answers each one explicitly.

| # | Lesson from the landscape | Design response |
|---|---|---|
| 1 | Single-maintainer projects with no second contributor (i3rs, TrackDataAnalysis, acctelemetry, Telemetry Viewer); bus factor 1 is the norm | Qt-free libraries with JSON schemas and golden fixtures make contributions small and testable without the GUI (D-007, D-033); ADRs and `CLAUDE.md`/`AGENTS.md` describe the architecture for AI-assisted contributors; governance kit from the first commit (D-004) |
| 2 | Undocumented binary formats are re-reverse-engineered by every project; each parser covers a different subset | One permissively licensed format library (`esses_formats`) with per-format provenance in `CLEANROOM.md`, hash-pinned fixtures and `probe_json` expectations that are simultaneously the fixture, the Open-dialog row and the catalogue row (D-028, D-033) |
| 3 | The "export to i2" gravity well: ≥ 10 projects stop at writing `.ld` | Their readers and fixtures are harvested, not their writers; our own `.ld`+`.ldx` writer exists from M0 only to generate synthetic fixtures (D-033, D-035) |
| 4 | UI and performance burden (docking, cursor-linked panels, 1 kHz × 200 channels) exceeds hobby capacity; projects stall at "graphs + overlay" | Chunked column store with colocated min/max pyramids and exact M4 decimation (D-011, D-012), a `DistanceIndex` per lap (D-016), layer-cached rendering with a `ChangeMask` so a cursor move costs ≤ 4 ms (D-018, D-025), measured budgets gating every milestone (outline §8) |
| 5 | Python packaging kills desktop adoption; web and Rust projects cite distribution | Native single-binary Qt 6 C++ application with installer, DMG and AppImage from M0 CI (D-006, D-034) |
| 6 | Company-backed FOSS drifts to cloud, closed or open-core | DCO (no CLA), `GOVERNANCE.md` with a published no-relicensing policy, official distribution contains only MIT code (D-002, D-004) |
| 7 | Licence hygiene failures block reuse | `REUSE.toml`, SPDX headers, `reuse lint` in CI, `NOTICES.md`, third-party manifest generated at build (D-004, D-034) |
| 8 | Scope drift to tuning-only or sim-only | Profiles are data (D-036); the circuit workflow ships first but the model carries runs and stages from M0; real-world loggers, ECUs and sims are all first-class inputs |
| 9 | No shared data model across projects | An explicit, documented model: `DataSource → DataLayer → Channel`, markers → rules → range groups → laps, `DistanceIndex`, stint/pit classification, all with JSON schemas (D-013, D-014, D-016) |
| 10 | AI-assisted solo velocity does not create users or contributors | Milestones end in checked acceptance criteria, not feature counts (outline §7); public alpha at 0.1, public beta announcement at v1.0 with workbook import so existing i2 users can migrate (D-035) |

Two further research facts shape the product: i2's "hidden" calculations (corrected speed/distance, GPS corrections, suspension maths) are internal plugins with opaque parameter strings, which the design replaces with transparent built-in modules whose settings are schema'd and visible (D-016, D-024); and i2's XML attribute bags and binary track maps are replaced by schema-validated JSON so projects are diffable and cannot be silently corrupted (D-019).

---

## 4. Goals and non-goals

### 4.1 Goals

1. **Workflow parity for circuit analysis** with i2 Pro as the written contract (`background/design-panel/parity-first.md` §5.3–§5.10, §7.1–§7.6, §3.3 adopted by D-001), including the full hotkey table shipped as `keymap.json` with one behaviour test per row (D-033).
2. **Read what users already have**: `.ld` with `.ldx` merged on every load (D-015), MoTeC CSV and generic CSV through declarative text profiles (D-029); from v1.0, import of `.i2wkb`, `.i2wsp`, `.mcs/.mfs` and i2 maths XML (D-027, D-035).
3. **Evaluate existing i2 maths unchanged**: a 100 % source-compatible grammar and function set, warnings instead of rejections, every ambiguous semantic documented in `docs/maths-compat.md` with a test case (D-020, D-021, D-022).
4. **Numbers users can trust**: explicit units on every persisted value (D-010), documented methods for corrected speed/distance and stretching with "results will not be bit-identical to i2" messaging, oracle tests against i2 exports of the user's MoTeC-device logs, permitted by the user on 2026-10-10 (D-005, D-033, D-038).
5. **Responsiveness as a gate**: cursor move ≤ 4 ms, zoom/pan ≤ 16 ms for 20 traces, first paint of a 100 MB `.ld` ≤ 500 ms in the MVP; stricter v1.0 budgets measured nightly (D-025, outline §8).
6. **Cross-platform from day one**: Windows first (MSVC, installer, portable zip), Linux and macOS building green in CI from M0 (D-006, D-034).
7. **A reusable core**: a Qt-free C++20 library set and CLI (`esses-cli`) that converter projects, plugins, scripts and bindings can use without the GUI (D-007, D-028, D-030).
8. **Durable, shareable projects**: a folder of schema-validated JSON with write-ahead autosave, atomic saves, backups, per-user layout kept out of shared files, and a workspace archive format at v1.0 (D-019).

### 4.2 Non-goals

1. Device download/unload, device-configuration viewers, feature licensing or activation of any kind, vendor telemetry transport (T2), COM/DDE automation (a JSON-RPC server replaces it after v1.1, D-030).
2. Excel-macro setup sheets (a CSV/JSON setup timeline arrives in v1.1), satellite map tiles in the MVP, localisation in the MVP.
3. Reading or writing i2's binary `.mt2` track maps; writing `.i2wkb/.i2wsp`; the `.ldx` writer exists only as an export (D-015).
4. QML/Qt Quick, a web UI, mobile, cloud services, Rust in the main tree (the C ABI leaves a door for Rust plugins, D-028).
5. Live telemetry, video and scripting before v1.0 (video and Lua are v1.1 items, D-030, outline M7); a GPU backend unless a measured budget fails (D-025).
6. Reproducing i2's Win32 chrome, icons, fonts, help text, templates, schemes or sample files: the look is OS-native and every shipped asset is our own (D-004).
7. Modifying a source log file, ever; inspecting MoTeC binaries, ever (D-004, D-015).

---

## 5. Target users

| User | Situation | What they need from Esses | When |
|---|---|---|---|
| The developer | Race engineer with MoTeC loggers, an existing i2 workflow and a ~24 k-line Qt C++ converter for MDF4/BLF/XRK/RPK/Haltech/Link/Emtron/Syvecs/MaxxECU/IBT/PDS | A tool that replaces i2 for a Saturday at the track; later, a plugin host for the converter's parsers (D-028) | 0.1 / M6 |
| Club and amateur circuit racers and their engineers | MoTeC or other loggers, i2 Standard limits, Windows laptops of mixed quality (Intel iGPU) | Unlimited overlays, components and maths without a licence; responsive on modest hardware | 0.1 |
| Sim racers (ACC, iRacing, AMS2 exports) | i2 Pro gratis but Windows-only; workspaces exchanged in the community | Open sim `.ld` exports, import the Kunos ACC workspace and community workbooks | 0.1 (files) / v1.0 (workbooks) |
| Teams | Shared projects per vehicle or category; several engineers | Diffable JSON projects, per-user layout kept out of shared files, workspace archives listing required channels and maths | 0.1 / v1.0 |
| Linux and macOS users | Locked out of i2 entirely | The same application on all three platforms | 0.1 (builds green) / release targets per open question Q-12 |
| Converter and tool authors, format contributors | Each project re-implements the same readers | A permissively licensed format library with fixtures, text profiles and dictionaries that make a new format a data PR (D-029, D-033) | v1.0 / M6 |
| AI-assisted contributors | Solo-developer bus factor | Qt-free libraries, schemas, ADRs and acceptance criteria that make small contributions verifiable | M0 onwards |

---

## 6. Product principles

Each principle is a rule the other documents must not contradict; the decision that establishes it is cited.

1. **No licence gates.** Every feature is available to every user: CSV import, overlays, FFT, workbooks, the C ABI, the CLI. There is no Standard/Pro split and no activation file (D-002, parity-first §7.7).
2. **Source logs are never modified.** All edits (details, markers, marker edits, range overrides, local maths, annotations, video links) live in a sidecar `<log>.<ext>.sidecar.json` next to the log, written atomically; the `.ldx` companion is read and merged on every load with precedence sidecar > companion > file, and is only ever *exported*, never written automatically (D-015).
3. **i2 compatibility is an import, not a file format.** Esses persists its own schema-validated JSON (project, workbook, maths, scheme, sidecar). i2 files (`.ldx`, `.i2wkb`, `.i2wsp`, `.mcs/.mfs`, maths XML) are imported through dedicated importers that keep unknown attributes in an `x-i2` bag and produce an import report; nothing is silently dropped (D-019, D-024, D-035).
4. **Cross-platform by construction.** Qt 6 Widgets with LGPLv3 modules only, dynamic linking, MSVC on Windows, GCC/Clang on Linux, Apple Clang on macOS; no QML; CI on all three operating systems from M0; packages on tags (D-006, D-034).
5. **Behaviour before infrastructure.** parity-first's product specification is the base; performance-first's ownership model, distance index and change mask are adopted as *interfaces* from M0 with the cheapest implementation that meets the MVP budgets; eviction, GL, plugins and scripting wait for a measured need (D-001, D-011, D-025).
6. **Everything the user authored is data.** Profiles, rules, units, dictionaries, text profiles, schemes, keymaps, workbooks and maths modules are JSON with `$schema` and `schemaVersion`, validated on load and by `esses-cli validate`, migrated by registered migrations, round-tripped byte-identically in tests (D-019, D-033).
7. **Numbers are explained.** Dimension mismatches warn but never block (D-021); every ambiguous i2 semantic has a chosen reading, a test case and, where permitted, an oracle (D-020); built-in calculations expose their method and parameters (D-016, D-024); the Channel Status window shows a resolution trace for every lookup (D-013).
8. **The GUI thread never blocks.** One task pool, one dispatcher, workers see only immutable snapshots; components draw a Pending state and repaint on completion (D-031).
9. **The CLI is the first client of every Qt-free feature.** `esses-cli info/channels/laps/eval/convert/export/validate/fixture/bench` run without Qt installed; a feature that cannot be exercised from the CLI or a Catch2 test is in the wrong layer (D-007, outline §1.2).
10. **Clean room, provably.** No MoTeC code, art, icons, help text, templates, schemes or trademarks; i2 behaviour only from help text, shipped XML, SDK samples and public sources; every i2-derived behaviour has a provenance line in `CLEANROOM.md`; CI greps for vendor marks (D-003, D-004).
11. **Permissive by default, governed against drift.** MIT for the whole tree (chosen by the user on 2026-10-10), DCO, no CLA, no relicensing without every contributor's consent (D-002, D-004).
12. **Parity is testable.** One Qt Test per hotkey row, golden images per component at DPR 1 and 2, golden JSON per CLI command, maths case files, fuzzing and benchmarks with CI gates (D-033).

---

## 7. Licence recommendation and the user-confirm list

### 7.1 Recommendation (D-002)

**Decided (2026-10-10, by the user): MIT for the whole repository**, SPDX `MIT`, DCO sign-off, no CLA, published no-relicensing policy in `GOVERNANCE.md`.

Facts that make MIT viable: every Qt module the project uses (Core, Gui, Widgets, OpenGL, OpenGLWidgets, Svg, PrintSupport, Test, Concurrent, later Multimedia) is LGPLv3 and dynamically linked; Qt Advanced Docking System is LGPL-2.1 and used as a shared library; the GPL-only libraries that would otherwise tempt a Qt project (Qt Graphs, Qt Charts, KDDockWidgets, QCustomPlot, FFTW, vector_blf) are excluded by the stack regardless of licence; every reader the research marked safe to port is MIT/BSD (motec-i2, i3rs, motorsport-telemetry-rs, libxrk, TrackDataAnalysis, mdflib, dbcppp, gpmf-parser, matio, pocketfft, Iir1) or MPL2 (Eigen). GPL sources (gotzl/ldparser, racing-data-converter, OpenLap, UltraLog) are read for understanding only.

Why MIT over the alternatives: the research identifies converter projects, vendors and the user's own converter adopting the format library as the only realistic path to contributors; MIT maximises that and keeps in-process C-ABI plugins free of the GPL combined-work question. The accepted cost is that closed forks are permitted; the evidence says the real risk is zero contributors, and the DCO/no-relicensing policy already blocks the open-core drift GPL would guard against.

| Option | Reuse by converters, wheels, vendor plugins | Protection against a closed fork | Costs |
|---|---|---|---|
| **MIT whole tree** (chosen 2026-10-10) | Maximum | None beyond governance | Closed forks permitted |
| Apache-2.0 whole tree | Maximum, plus explicit patent grant | None beyond governance | Heavier notice obligations; one-way GPLv3 compatibility; otherwise a drop-in substitute with no design change |
| GPLv3 application + MIT `core`/`formats` | Libraries only | Yes, for the UI tree | Two trees to police; the maths engine must be assigned to one tree before the first public tag; every in-process C-ABI plugin raises the FSF combined-work question (deters vendor-written readers); relicensing foreclosed |

Obligations under every option: ship unmodified Qt shared libraries, LGPL notices and a link to the exact Qt source tarball (`NOTICES.md`); ADS as a shared library; third-party licence manifest generated at build (D-034).

### 7.2 Decisions the user must confirm

The design proceeds on the stated default; the user owns the final call. The rows below are the ADRs marked "User must confirm: yes" in `DECISIONS.md`, Addendum A included; the **Q** column is the open-question id of the consolidated list at the end of `DECISIONS.md` (Q-01…Q-18, whose "Was" column maps the numbers 1–12 of earlier drafts) and of `OPEN_QUESTIONS.md`.

| ADR | Q | Question | Default the design proceeds on | What the user must decide or do |
|---|---|---|---|---|
| D-002 | Q-01 | Project licence | MIT whole tree, DCO, no CLA, no-relicensing policy | **Answered 2026-10-10: MIT.** No further action. |
| D-003 | Q-02 | Codename | "Esses"; namespace `esses`, targets `esses_*`, executables `esses`, `esses-cli`, `esses-render` | **Answered 2026-10-10: Esses is the working name.** The user also answered that no trademark search is needed for now; the name may change later. The name stays swappable by a scripted rename of `cmake/naming.cmake` and docs. |
| D-005, D-038, D-039 | Q-03 | Oracle and user-data policy under the i2 EULA field of use | (1) i2 CSV/report exports of the user's MoTeC-device logs as private oracles; (2) parsing the user's i2-written files, already done for research on 2026-10-09 and recorded with its date; (3) no behaviour observation until the user's own legal check; ACC treated as not MoTeC-licensed | **Answered 2026-10-10: yes to all three**, so the design now proceeds on (3) = yes: black-box observation on the user's install only, never disassembly, with the legal check the user's own responsibility. The observations are scheduled in M1 to M3 (Q-08). The ACC question was answered the same day: ACC files are not MoTeC-licensed, so i2 is never run on ACC data, not even as an oracle (D-038 (a)); ACC exports are ordinary third-party fixtures, private for now (Q-04) |
| D-033, D-040 | Q-04 | Fixture commit policy | Nothing real in public; synthetic families (including `synthetic-acc-dialect`) public; the user's real logs, ACC exports, workbooks, schemes and i2 exports in a private submodule referenced by hash | **Answered 2026-10-10: nothing real is public for now**; every real file stays in the private submodule and the synthetic families stay public, as defaulted. Revisit before the first public release |
| D-035, D-045 | Q-05 | MVP scope cut and plan re-baseline | i2 workbook/workspace/scheme/maths importers at v1.0; Lap Editor, Details editor and Time Report with sections stay in 0.1; 0.1 ≈ 42 calendar weeks (900 h plus a 15 % contingency line, tests 30 % of each milestone) | **Answered 2026-10-10: both accepted.** No further action. |
| D-017, D-020 | Q-08 | Overlay colours and numbering; operator precedence | Overlays 1..n numbered in selection order and renumbered compactly, colours per ground truth with a scheme-driven `overlayStyle`; `-2^2 = -4` with the compat switch `maths.compat.unaryMinusAbovePower` | Observation is permitted since Q-03 (3) was answered yes on 2026-10-10: overlay colours and numbering in M1, `-2^2` in M2; until observed the defaults stand, recorded "chosen, not observed" |
| D-046 | Q-10 | Optional 0.1 cut list | Not adopted; the list is M3's ordered may-slip list | Decide by M3 start; adopting it saves ≈ 50 h of M3 (0.1 ≈ 39–40 weeks) |
| D-043 | Q-11 | Eviction and the stress fixture move to M5 | Yes: 0.1 tracks the memory budget and warns at open; LRU eviction and the RW-1-class stress fixture land in M5 | Confirm by M4 start; reversal puts +18 h back into M4 |
| D-116 | Q-12 | Linux and macOS at 0.1 | AppImage and DMG built and launch-tested, labelled "builds green, not supported" | Decide by M4 start whether either becomes a supported release target |
| D-125 | — | Neutral key for the `x-i2` round-trip bag (optional) | Keep `x-i2` as an allow-listed class | Choose `x-import` + `format` instead, preferably before M2 writes the first `workbook-1` files |
| D-136 | — | Schema-generated property grids for 0.1 (optional reversal of D-027) | Not adopted; hand-laid-out pages bound through `FieldBinder` | Adopt only if the user prefers generic grids to i2-style tabs; decide before M3 |

Further open questions carry defaults and have no design dependency (the full list is `OPEN_QUESTIONS.md`): the reference machine and minimum hardware/OS for the budget gates (Q-07, D-042); default maths precision (Q-09, D-022); local Qt version, now recommended as 6.12 from M0 week 1 (Q-13, D-126); the code-signing budget (Q-14, D-034); the converter's licence and provenance (Q-15, D-028, D-107; until it is answered the M0 `.ld` reader and writer are new code written from a port specification of the converter, D-142, D-144); and whether a Python wheel is wanted before v1.1 (Q-16, D-030). Q-06 was answered by spike S-1 on 2026-10-10: the user's real `.ld` files settled `ld.shiftOrder = afterMul` (D-037), and the vendor sample files, the T2 test models included, are cleared for local testing only and never committed (D-117, D-146).

---

## 8. Codename and trademark policy

From D-003 and D-004; enforced by CI from M0.

1. The product, its identifiers, file extensions, schema ids and UI strings never contain "MoTeC" or "i2", except through the allow-list in rule 2. Working codename **Esses**: C++ namespace `esses`, CMake targets `esses_<lib>`, C ABI prefix `esses_`, GUI executable `esses`, CLI `esses-cli`, offscreen render tool `esses-render`, JSON `$schema` ids under a project-owned URL once a domain exists, sidecar `<log>.<ext>.sidecar.json`, workbooks `*.workbook.json`, workspace archives `*.esses-workspace.zip`. Classes, files and directories that handle i2 formats use neutral names (`Cfg*`/`Legacy*` classes, `libs/project/src/cfgimport/`), never `I2*`, `Mtc*` or `IMtc*`.
2. **Nominative use is allowed** where a file format is named. The CI allow-list (`tools/vendor_marks_allowlist.txt`, 08_REPO_AND_BUILD.md §5) has exactly these classes: (a) importer ids (`motec.ld`, `motec.csv`, and the v1.0 configuration-importer ids when assigned) with their dictionary and profile file names; (b) import/export dialog format labels ("MoTeC log file (.ld)"); (c) the format-name literal that the MoTeC CSV writer emits on line 1 and the `motec.csv` profile's sniff pattern matches ([WRITER_UNRESOLVED #70](background/design-panel/WRITER_UNRESOLVED.md)); (d) the D-019 round-trip bag: the JSON key `x-i2` in `workbook-1`/`maths-1` documents and schemas, and its C++ member `xI2` (outline §4); (e) prose in `CLEANROOM.md`, `NOTICES.md`, `TRADEMARK.md`, `docs/`, `fixtures/**/README.md` and `fixture.json` `source` fields. Nothing else is allowed. A rename of the D-019 bag to a neutral key (`x-import` with a `format` field) is offered to the user in DECISIONS.md Addendum A (D-125, open question Q-17); until accepted, class (d) stands.
3. A CI job (`lint`) fails, outside the allow-list, any UI string, asset path, file name or file extension containing `motec` or `i2` as a word (case-insensitive), and any identifier under `libs/`, `apps/`, `plugins/` and `sdk/` that contains `motec` anywhere or has a token `i2`, `i2pro`, `i2wkb`, `i2wsp` or `mtc` after splitting at underscores, hyphens, dots and lower→upper case changes (so `I2MathsImporter`, `xI2` and `IMtcMathPlugin` are caught; `kPi2` is not).
4. No MoTeC templates, schemes, icons, help text, sample files or workbooks are shipped; the two default schemes and the default circuit workbook are our own (D-004).
5. `TRADEMARK.md` states the project's own mark policy and the nominative-use rule; `CLEANROOM.md` carries one entry per format and per i2-derived behaviour naming the public source (help page, shipped XML, SDK sample, public project) it came from.
6. The codename lives in one place, the prefix table `cmake/naming.cmake`, plus documentation, so a scripted rename stays possible if the name changes later (no trademark search is scheduled; the user answered on 2026-10-10 that none is needed for now).

---

## 9. Reading guide

The design set is numbered 00–08. Each document opens with a Scope paragraph and a Related-decisions list, uses the target, type and schema names of `ARCHITECTURE_OUTLINE.md` verbatim, and cites `D-nnn` for every decision it relies on. Read 00 and 01 first; the rest in any order. Where a type is declared in more than one document, the owning document named in 01_ARCHITECTURE.md §2.1 is authoritative (for example 05 for the formats contracts, 04 for the renderer seam, 03 for maths diagnostics and `maths-case-1`).

| Doc | File | One line |
|---|---|---|
| 00 | `00_OVERVIEW.md` (this document) | Vision, why clean-slate, goals, users, principles, licence and confirm list, naming policy, glossary |
| 01 | `01_ARCHITECTURE.md` | Layered `esses_*` module map, dependency graph, file-to-pixels data flow, threading and ownership, `Result`/error conventions, memory budgets, logging and Messages, directories, extension points |
| 02 | `02_DATA_MODEL.md` — Data model: `esses_base`, `esses_units`, `esses_store`, `esses_model`, `esses_project` | `Micros`/`Rate`/`Uuid`/`Hash128`, the unit registry, chunk store and series kinds, sources/layers/channels/details/markers/ranges and the resolver, the sidecar document, and every project JSON document with its schema, migration, round-trip and undo rules |
| 03 | `03_MATHS_ENGINE.md` — Maths engine: `esses_maths` and the built-in analysis modules | i2-source-compatible expression language (lexer, Pratt parser, precedence), type system with unit/dimension tracking, intrinsic catalogue by kernel class, `Plan`/`Kernel` block evaluation, `PlanProvider`/`MathInputGatherer`/`MathGraph`/`MathsScheduler`, version-keyed caching and status, `maths-1` modules, the three built-in modules (corrected speed/distance, GPS corrections, damper velocity), `IMathFunction` with the C ABI and Lua, Maths Editor contract, i2 maths XML import mapping (v1.0), `maths-case-1` tests and the `docs/maths-compat.md` policy. Range rules are in 02 §4.5; report, variance and summary-component use of `esses_analysis` is in 04 |
| 04 | `04_RENDERING_AND_COMPONENTS.md` — Rendering and components: `esses_plotcore`, `esses_qtrender`, component behaviour | `ViewState`/`ChangeMask`/`SelectionSnapshot`, `DomainMapper`, exact M4 `TraceBuilder`, layout builders and `HitTester`, `DrawList` and `IRenderBackend`, `RasterBackend`/`LayerCache`/`OffscreenRenderer`, the GL escalation rule, HiDPI and goldens, then the behaviour contract of every MVP component |
| 05 | `05_IMPORTERS_AND_PLUGINS.md` — Importers, text engine and plugins: `esses_formats`, `sdk/esses_abi.h` | `IImporter`/`IReader`/`IWriter`, `ImporterRegistry` and the open pipeline, the independent `.ld`/`.ldx` reader from public documentation and the export-only writer, MoTeC CSV, `TextEngine` with JSON profiles, dictionaries and unit maps with the Mapping panel, fixtures and fuzzing, the C ABI designed now and frozen at v1.0, the converter harvest, format and writer roadmaps |
| 06 | `06_UI_SHELL.md` — UI shell: `esses_qtmodel`, `esses_widgets`, `esses_app` | `MainWindow`, `DockHost`, Workspace › Workbook › Worksheet › Component navigation and `WorksheetCanvas`, `DataSelectionModel` and `WorksheetViewModel`, the six MVP docks, properties dialogs and all editors, `CommandRegistry` + `keymap.json` with the complete hotkey table, command palette, scheme JSON, HiDPI/multi-monitor, session restore |
| 07 | `07_ROADMAP.md` — Roadmap: MVP 0.1, milestones M0–M7, v1.0 and v1.1 | D-035 as an executable plan: MVP definition, milestones with deliverables by target, numbered acceptance criteria (`AC-Mn-kk`), effort, dependencies and user confirmations, demo scripts, M0 week-1 spikes, budget gates, deferred lists, risk register, user-confirm checklist |
| 08 | `08_REPO_AND_BUILD.md` — Repository, build, CI, testing and packaging | Repository tree, CMake design (presets, `ESSES_BUILD_QT`, target helper and layer check, dependencies, Qt-free check), GitHub Actions matrix and required checks, per-layer testing (golden JSON and images, maths cases, fixtures, fuzz, bench), packaging per OS, versioning and release, LGPL compliance, REUSE/SPDX, governance files and the first-week bootstrap |

Supporting inputs remain authoritative for detail the set does not repeat: `DECISIONS.md` (ADRs D-001..D-036 and Addendum A D-037..D-146, wins on conflict), `ARCHITECTURE_OUTLINE.md` (names, interfaces, milestones), `background/design-panel/parity-first.md` (UX contract), `background/design-panel/performance-first.md` §1.2 (reference workloads RW-1..RW-5), `background/design-panel/ecosystem-first.md` §6.1 (C ABI rules), `background/research/*.md` (ground truth about i2). Open user decisions are collected in `OPEN_QUESTIONS.md`; `README.md` gives the reading order.

---

## 10. Glossary

Terms are listed in the sense the design uses them; the owning type or document is named. Hotkeys are i2's defaults, shipped unchanged in `keymap.json`.

**Data source** — One open log file (later: one live stream), listed in the Data window; `DataSource{id, provenance, details, layers, sidecar, version}` in `esses_model`. Published to the GUI as an immutable `std::shared_ptr<const DataSource>`; a new version is published whenever its sidecar changes. Identified by its `SourceId`, a 128-bit quick content hash (size, first and last MiB, format header; mtime excluded), so a moved or copied file is recognised; `SourceId` does not hash the importer id; channel ids (`ChannelId`) and column cache keys (`ColumnKey`) do (D-130; 02 §4.1, 05 §2).

**Data layer** — A processing-stage view of a source's channels, markers and ranges; `DataLayer{name, channels, byNameCI, markers, ranges}`. The canonical layer name is `"Normal"`; `""` is accepted as an alias on import and emitted by the `.ldx` writer. Future layers (fast, burst, telemetry) reuse the same type.

**Channel** — A named, unit-bearing sampled series from a source: `Channel{desc, series, status}` with `status ∈ {Ok, Pending, Error}`. `ChannelDescriptor` carries identity (`ChannelId`, canonical `name`, `shortName`, `loggedName`), unit and quantity, `Dtype {F32, F64, I32}` and `SampleKind {Real, Integer, Enum, Bool, Bitfield, Text}`, `Interp`, decimal places, colour index, optional enum table, `ChannelSource {Logged, Maths, Detail, Alias, Mapped, Constant, Setup}` and status text. The **name** is the lookup key everywhere (workbooks, maths, overrides), NFKC case-folded; the id is stable across sessions without a registry (D-013).

**Series** — The sample container behind a channel: `Series = std::variant<UniformSeries, TimestampedSeries>`. `UniformSeries{Rate rate, Micros t0, Column values}`; `TimestampedSeries{Column timesUs, Column values}` with an explicit I64 µs column. Never resampled at import; the `Resampler` is a view-layer service with per-channel `Interp {Linear, Closest, Previous, Next}` (D-008, D-009).

**Column / chunk / pyramid** — A `Column` is a lazily materialised sequence of 65,536-sample immutable `Chunk`s fed by an `IChunkProvider`; each chunk holds samples, an optional validity bitmap and a complete factor-4 `MinMaxPyramid` in one allocation; a `ChunkHandle` pins chunks for a worker; the `SummaryPyramid` (buckets ≥ 256) is always resident and serves full-extent views and autoscale (D-011, D-012).

**Marker** — A timestamped event with provenance: `Marker{id, cls, t, name, comment, prov {Logged, Companion, User, Derived}, flags, originalT, enabled}`; classes SOL/EOL/BCN/SPLTBCN/IGRDBCN/RESET/STGSTRT/STGEND. A **beacon** is a BCN marker. User edits of logged or companion markers are `MarkerEdit` ops (`offset`, `ignore`, `restore`) in the sidecar; user-inserted beacons (SHIFT+F7) are markers with `prov = User` (D-014).

**Range** — A selectable half-open span `[start, end)` of a layer produced by a rule: `Range{id, kind {Outing, Lap, Run, Stage, Section, Split, Data}, lapClass {Flying, Out, In, Partial, Pit}, startMarker, endMarker, start, end, name, abbrev, number, trusted, enabled, offsetUs, offsetM, stretch}`. Ranges live in a **range group** tree addressed by colon paths (`Outings`, `Outings:Laps`, `Outings:Laps:Track Sections:Default`). Persistent identity for overrides is `(group path, start marker id)`. `RangeRef{src, layer, groupPath, range}` is what selection and view state hold (D-014).

**Lap** — A circuit `Range` of kind `Lap` derived by the `beacon-pair` rule: SOL→BCN is the out lap, BCN→BCN flying laps numbered from 1, BCN→EOL the in lap, a RESET yields a partial untrusted lap, IGRDBCN markers are ignored. In/out laps are untrusted by default. Laps are always derived from markers, never stored; a marker edit rebuilds them synchronously (phase 1: markers and details), and stint/pit annotations and track sections follow on a worker (phase 2) (D-014, 02 §4.4 rule 13). A **trusted lap** is eligible as fastest (candidate rule: trusted flying laps if any, else flying or partial laps; in/out/pit never, 06 §5.2), included in reports and autoscale (project options). A **stint** starts on a lap-counter reset, a `RESET` or a gap of more than 10 s in the logged data; stints never split on ordinary beacon intervals. ≥ 15 s stationary inside a lap sets only the lap's `pitStop` annotation: the lap stays trusted and keeps its class and number (02 §4.4 rules 12–13).

**Outing** — A whole logged session (one file, one layer): the `whole-outing` rule yields one `Range` of kind `Outing`; the `outing` component draws the full extent from the `SummaryPyramid` and shows the zoom span (D-014, D-027).

**Main / Overlay 0 / Overlays** — The selection model, one `SelectionSnapshot{main, overlay0, overlays, overlaysHidden, singleColourMain}` per workspace in `DataSelectionModel`. **Main** (ENTER; red dot, column M): exactly one range, always drawn, the reference for cursor time, variance, deltas and offsets. **Overlay 0** (CTRL+ENTER; grey box, column O): a radio-style single overlay drawn in the scheme's Overlay 0 colour; Q swaps it with Main. **Overlays 1..n** (ALT+ENTER; column 1): a multi-set drawn in channel colours, numbered in selection order and renumbered compactly on removal (to verify, D-017). F4 hides overlays without deselecting; SHIFT+F4 clears them; J draws Main in a single scheme colour. The **reference overlay** used by variance, dual-source measurements and reports is Overlay 0 if set, else the lowest-numbered overlay.

**Component** — A display element on a worksheet named by its JSON `type` string (`time_distance`, `outing`, `scatter`, `histogram`, `channel_report`, `time_report`, `track_position`, `values_list`, `gauge_numeric`, `gauge_bar` in the MVP). Persisted as `ComponentSpec{type, version, rect[l,t,r,b], links{zoom,cursor,datum}, body, xI2}` with free fractional rectangles; unknown types are preserved verbatim and shown as a placeholder (D-019, D-027). Its **active channel** (square marker, PgUp/PgDn) is the target of per-channel hotkeys and datum measurements.

**Worksheet / workbook / project** — A **worksheet** is a page of components with its own `ViewState` (tabs, 0–9, F7, F11/F12). A **workbook** groups worksheets (CTRL+0–9, CTRL+F11/F12) and is one `workbooks/<name>.workbook.json` file. A **project** is a folder (name = project name) holding `project.json`, `channels/` overrides, aliases, mappings and unit maps, `maths/`, `workbooks/`, `trackmaps/`, `backup/`, a per-user `.local/layout.json` that is never shared, and a `.journal/` of write-ahead autosave entries (D-019). A **workspace** is the exportable set of workbooks and maths (`*.esses-workspace.zip`, v1.0). A **profile** (`Circuit` in the MVP; `Drag`, `Rally` at v1.0) is a project option that selects range kinds, rules, data views, default maths, default workbook and terminology (D-036).

**Cursor / datum** — The **cursor** is the shared x-position across linked components, stored as `cursorT` in Main-lap time and converted per domain; arrows move it (CTRL fast), HOME/END, H centres. The **datum** is a second fixed cursor for differential measurement (D toggles, SPACEBAR places, X swaps, Z zooms between them); readings for the active channel show value@main, value@datum and Δ in the key column. Both live in `ViewState{domain, zoom, cursorT, datumT, offsetAxis, version}` and reach components through a `ChangeMask` so a cursor move redraws only the Dynamic layer (D-018).

**Time mode / distance mode** — The x-axis basis of time/distance components, toggled by F9. Overlays align at lap start in time mode and by corrected distance in distance mode; the zoom span is preserved across F9 through the Main lap's `DistanceIndex` when present, else by fraction of the Main range (D-016).

**Corrected speed / corrected distance** — `Corr Speed` is derived vehicle speed from the built-in module `esses.corrected_speed_distance` (`method: fastestWheel | alternateList | gCorrection`, channel lists, clamping); `Corr Dist` is its integral as an F64 series, registered for every source on open and prefetched at `Background` priority. It is required for distance mode; when no usable speed channel exists the graph shows a "no distance data" banner for that source. Units follow speed units and are overridable in the Channel Editor (D-016).

**DistanceIndex** — Per `(source, lap)`: a table `tAtD[]` of seconds from lap start at each 0.25 m step (first time each distance is reached, so plateaus are handled), `lengthM` and a `stretch` scalar; `timeAt(d)`, `distAt(t)`, `indexAt(d, rate)` are O(1). It drives distance-domain decimation, variance and F9 preservation (D-016).

**Lap stretching** — Scaling each lap's computed distance to the track map length when within a tolerance (default 5 %, per project; toolbar toggle; per-lap report in the Lap Editor). Implemented as the `stretch` scalar on the `DistanceIndex` (D-016).

**Alignment offset** — A per-range constant (`offsetUs`, `offsetM`) added before the domain lookup to align overlays; shown and dragged on the offset axis (O), adjusted with CTRL+SHIFT / ALT+SHIFT arrows, zeroed when O is pressed again; persisted in the sidecar's `rangeOverrides` (D-015, D-016).

**Variance** — The difference between an overlay and Main as a function of the domain: `Δt(d) = tAtD_k(d) − tAtD_main(d)` in distance mode, `Δd(t)` in time mode, drawn per visible overlay in the graph's variance panel (F3) from the two laps' `DistanceIndex` tables in O(pixels). The maths functions `variance_time()`/`variance_dist()` use the same tables for Main versus the reference overlay, are selection-dependent and invalid outside the Main selection (D-023).

**Alias / mapping / override** — Three project-level channel resolvers applied in the order **mappings → aliases → case-insensitive canonical name → logged name**, each step recorded in a `ResolutionTrace` shown in Channel Status. A **mapping** renames a logged name to an analysis name and/or stamps a unit (`channels/mappings.json`). An **alias** is a channel that resolves to the first available candidate in an ordered list and carries its own descriptor and colour (`channels/aliases.json`). An **override** replaces per-channel display properties (unit, min, max, decimal places, colour, interpolation) for every component using Channel/Auto scale (`channels/overrides.json`); a unit override that cannot apply to every open file is dropped, as in i2, on both paths: refused at edit time with the conflicting files listed, and removed undoably with a Messages row when a newly opened file conflicts with it (06 §13.1 item 6; D-010, D-013). A **unit map** (`channels/unitmaps/<importer>.json`) maps a foreign importer's `(name, unit)` to a quantity and unit by `nameOnly | unitOnly | nameAndUnit`; a per-importer **dictionary** maps vendor names/ids to canonical names before resolution (D-028, D-029).

**Sidecar / companion** — The **sidecar** is Esses's own per-log edit file `<log>.<ext>.sidecar.json` (schema `sidecar-1`). The **companion** is i2's `.ldx` next to an `.ld`, parsed tolerantly and merged below the sidecar; unknown elements are kept as opaque strings for round-trip export (D-015).

**Maths module / item / local maths** — A **module** (= an i2 maths file) is `maths/<module>.maths.json` with `{id, enabled, condition?, constants[], enumerations[], items[]}`; **items** are `expression | filter | scale_offset | table2d | table3d | builtin | plugin | regression`. **Local maths** belong to one log and live in its sidecar; they are evaluated before global modules. Scale/offset/zero applied from a channel label become local maths; a filter applied from the label is transient and never persisted. Shadowing follows i2's order of calculation (D-022, D-024).

**Track map / section** — `trackmaps/<venue>.track.json` (`track-1`): an ENU centreline with cumulative distance, optional inner/outer lines, length, direction, origin, and `sections[]` (default Turns/Straights ≥ 10 m plus user sections with categories) used by the time report, the section band and `Outings:Laps:Track Sections:<set>` range groups. Generated from Speed + Lateral G or GPS by the Track Editor when no same-named file exists (D-027).

**Scheme / keymap** — A **scheme** (`scheme-1`, per user) names every colour role, separate channel palettes for light and dark backgrounds, the Main and Overlay 0 colours, `overlayStyle` and fonts. A **keymap** (`keymap-1`) binds key chords to `CommandRegistry` ids with i2's defaults shipped unchanged; every row has a behaviour test (D-017, D-030, D-033).

**Import Log / Messages** — The **Import Log** is the ordered list of warnings and heuristic decisions an importer recorded for one source (every `.ld` dialect choice, unknown unit, sentinel, skipped row); it is part of `probe_json` and of the source's provenance. The **Messages** dock shows Import Logs, maths diagnostics, budget warnings and project notices (D-011, D-028; see 01 §8).

**Pending** — The state of a maths channel or trace whose evaluation has been requested but not completed; components draw hatching or "…" and repaint on the dispatcher's completion notice. Nothing in the GUI waits (D-022, D-031).
