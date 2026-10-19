# DECISIONS.md — Architecture Decision Records for the synthesis design

Working codename: **Esses** (see D-003). Namespace `esses::`, CMake targets `esses_*`, GUI executable `esses`, headless CLI `esses-cli`.

## How this document was produced

Inputs: four proposals (`background/design-panel/core-first.md`, `background/design-panel/parity-first.md`, `background/design-panel/performance-first.md`, `background/design-panel/ecosystem-first.md`), three judge scorecards (`background/design-panel/solo-dev-realist.md`, `background/design-panel/i2-power-user.md`, `background/design-panel/systems-architect.md`), the aggregate weighted tally `{parity-first 22.825, core-first 22.80, performance-first 22.60, ecosystem-first 21.85}` and the Phase 1 research (`background/research/phase1_summary.md`, `features_compact.md`, `concepts.md`, plus `tech_stack.md`, `config_model.md`, `landscape.md`, `formats.md` where cited).

Method: **parity-first is the base** (it won the tally and both the power-user and solo-developer lenses; its §5.4–§5.10 and §7.2–§7.5 are adopted verbatim as the UX/behaviour contract). The other three proposals supply grafts the judges named explicitly: core-first's licence/governance, CLI-first testing discipline, rational sample rate, explicit-unit persistence, `ChannelResolver` diagnostics, `CommandRegistry` and converter-harvest plan; performance-first's chunk/pin ownership model, `DistanceIndex`, `SummaryPyramid`, `ChangeMask`, kernel classification, journal autosave, reference workloads and budgets; ecosystem-first's C-ABI conventions, `probe_json` = fixture = catalogue row, fixture workflow, dictionaries, text-profile schema, governance/hygiene kit, marker-edit ops and unknown-component preservation. Every contradiction the judges listed (solo-dev 22, power-user 20, architect 28, largely overlapping) is resolved below; the ADR that resolves it names the contradiction.

Each ADR has: Title · Context · Options considered (with the research facts that matter) · Decision · Consequences · User must confirm (yes/no). "User must confirm = yes" means the design proceeds on the stated default but the user owns the final call.

## Index

| ID | Topic | Decision (one line) | Confirm |
|---|---|---|---|
| D-001 | Synthesis base | parity-first product spec on a layered Qt-free core with grafts from the other three | no |
| D-002 | Project licence | **MIT whole tree** (chosen 2026-10-10) + DCO, no CLA, no-relicensing policy; alternatives Apache-2.0 or GPLv3-app/MIT-libs | **yes** (answered 2026-10-10) |
| D-003 | Codename | **Esses**; never "MoTeC"/"i2" in product, identifiers, extensions or UI strings | **yes** (answered 2026-10-10; no trademark search scheduled) |
| D-004 | Governance and clean-room | DCO, `GOVERNANCE.md`, `CLEANROOM.md` per-format provenance, `TRADEMARK.md`, REUSE/SPDX lint, CI grep for vendor marks | no |
| D-005 | Oracle and user-data policy | i2 *exports* as black-box oracles and import of user-owned i2 XML are allowed only if the user says so; i2 runs only on MoTeC-device data, never on Esses output (D-038); research-phase use recorded (D-039) | **yes** (answered 2026-10-10) |
| D-006 | Qt policy | Qt 6 Widgets, LGPLv3 modules only, dynamic linking, minimum 6.7 (local kit), releases on 6.12 LTS, MSVC on Windows | no |
| D-007 | Qt-free boundary | Data, formats, maths, analysis, project documents and plot *geometry* are Qt-free; the application model may use Qt Core; rendering uses Qt Gui | no |
| D-008 | Time base and rate | `int64` microseconds; rational `Rate{num,den}`; timestamped series carry explicit I64 µs columns | no |
| D-009 | Series kinds, sample types, validity | `UniformSeries`/`TimestampedSeries`; F32 default, F64/I32 where needed; per-chunk validity mode + NaN-aware kernels | no |
| D-010 | Units | SI base units in memory; every persisted number carries an explicit unit object; i2 symbol table as aliases; AFR = lambda × constant | no |
| D-011 | Column store and ownership | 65,536-sample immutable chunks with colocated factor-4 pyramid, `ChunkHandle` pinning, `IChunkProvider`; eviction at M5 (D-043), DCF deferred until measured | no |
| D-012 | Decimation | Exact M4 from the pyramid with hierarchical boundary descent; `SummaryPyramid` always resident | no |
| D-013 | Channel identity and resolution | Name is the i2 identity; deterministic UUID per (source, layer, logged name); `ChannelResolver` records every step | no |
| D-014 | Markers, ranges, laps, rules | Markers with provenance, `MarkerEdit` ops, ranges bound to marker ids, laps always derived; declarative JSON rules behind `IRangeRule` | no |
| D-015 | Sidecar and `.ldx` | `<file>.<ext>.sidecar.json`; `.ldx` merged on every load (sidecar > companion > file), unknown elements kept; `.ldx` writer = export only (v1.0) | no |
| D-016 | Distance model | Corr Speed/Dist as a transparent built-in module computed on open; `DistanceIndex` per (source, lap); stretch as a scalar; F9 fraction rule when distance is missing | no |
| D-017 | Selection model and overlay look | Main / Overlay 0 / Overlays 1..n exactly as i2; colours per ground truth, scheme-driven `overlayStyle`; compact renumbering (documented choice) | **yes** (verify on i2) |
| D-018 | View state and linking | One `ViewState` per worksheet; cursor stored in Main-lap time; `ChangeMask` pass invalidation; per-component `links{zoom,cursor,datum}` | no |
| D-019 | Project and workbook persistence | Project = folder of schema-validated JSON; free fractional rectangles; `x-i2` bag; unknown component types preserved; per-user `layout.json`; journal autosave + atomic writes; no index DB in MVP | no |
| D-020 | Maths language | 100 % i2-source-compatible grammar; `^` binds tighter than unary minus (`-2^2 = -4`) with a compat switch; separate `&`, `\|`, `&&`, `\|\|` levels | **yes** (verify on i2) |
| D-021 | Maths types and dimension checking | Typed AST with dimension vectors; mismatches are warnings, never rejections; `LapScalar` lowers to a Hold series | no |
| D-022 | Maths evaluation and caching | 4,096-sample blocks, kernel classes with halo/whole-extent, F32 buffers with F64 accumulators, version-keyed chunk cache, `SelectionDependent` key, i2 shadowing order | no |
| D-023 | Variance | A graph feature computed from `DistanceIndex` tables per overlay vs Main; `variance_time/dist()` use the same tables through `EvalContext` | no |
| D-024 | Maths extensibility | Built-in modules with schema'd settings replace opaque `Params`; C-ABI functions at v1.0; Lua at v1.1; one `IMathFunction` interface | no |
| D-025 | Plot engine | QPainter `RasterBackend` first behind a Qt-free `DrawList` IR and `IRenderBackend`; Static/Dynamic/Chrome layer cache; `GlBackend` only when a measured budget fails; GL granularity decided by measurement; budgets measured on one reference machine (D-042) | no |
| D-026 | Docking | Qt Advanced Docking System (LGPL-2.1) as a shared library behind a `DockHost` facade; `QDockWidget` fallback | no |
| D-027 | Components and properties UI | MVP component set fixed (ten components + editors); modeless tabbed property dialogs with Apply and undo; i2 tab ergonomics hand-built | no |
| D-028 | Importer interface, C ABI, converter harvest | In-tree C++ `IImporter` mirroring a designed-now C ABI with ecosystem-first conventions; ABI frozen at v1.0; converter harvested wrap-then-port after v1.0 | no |
| D-029 | Text engine, profiles, dictionaries | Own light two-pass text engine on `fast_float`; JSON profiles with `lapSource`; per-importer dictionaries; inline Mapping panel | no |
| D-030 | Scripting | None before v1.0; sandboxed Lua 5.4 + sol2 at v1.1; Python wheel over `esses_capi` only on demand; JSON-RPC later | no |
| D-031 | Threading | One Qt-free priority `TaskPool`; GUI thread never blocks; single `Dispatcher` to the GUI thread; immutable chunk snapshots for workers | no |
| D-032 | Undo/redo | `QUndoStack` in the Qt application model, one per project session; selection/cursor/zoom are not undoable | no |
| D-033 | Testing and fixtures | Catch2 + Qt Test (one test per hotkey row) + golden images via the raster backend + golden JSON via CLI + fixture corpus with provenance + maths case files + fuzz + benchmarks; fixture licensing D-040, writer-independent oracles D-041 | **yes** (which files may be committed; answered 2026-10-10) |
| D-034 | Packaging and CI | CMake presets, vcpkg, GitHub Actions matrix, windeployqt + Inno Setup, DMG, AppImage, LGPL notices + Qt source offer; signing budget open | no |
| D-035 | MVP scope and milestones | parity-first's MVP minus i2 file importers, index DB and GL (all v1.0); Lap/Details editors, Time Report and sections stay in MVP; ~30 weeks to 0.1, re-estimated ≈ 42 with contingency (D-045) | **yes** (scope cut; answered 2026-10-10) |
| D-036 | Profiles | Circuit profile only in MVP; range kinds for Run/Stage exist in the model from M0; Drag/Rally rules and editors v1.0 | no |

Addendum A (after the open questions) adds D-037–D-199: decisions proposed by the section writers of documents 00–08, by the 2026-10-10 review (A.1–A.8) and by the final cross-document revision (A.9), plus implementation decisions taken once M0 started (A.10: the user's instructions, and the reconciliation with spike S-1 and the converter port specification) the departures of M0 as built from the design (A.11), those of M1 as built (A.12, proposed), those of M2 (A.13, proposed) and those of M3 (A.14, proposed), with their own index. Where an addendum entry amends an ADR above, the ADR carries a one-line "Amended by" note.

---

## D-001 Synthesis base: parity-first's product specification on a layered core

**Context.** The weighted tally is nearly flat (22.825 / 22.80 / 22.60 / 21.85). All four proposals adopt the research stack (Qt 6 Widgets, LGPL modules, ADS, own M4/pyramid plot core, own i2-compatible maths engine, JSON project files, a C ABI for format plugins). They differ in ordering, where the Qt-free line sits, and how much infrastructure must exist before the first graph. The power-user judge found parity-first "a full tier above the field" on fidelity and completeness and noted that its deficits are "the easy-to-graft kind", while "retrofitting i2 semantics onto a finished engine is where every FOSS predecessor stalled" (landscape lesson 4). The architect judge preferred performance-first's store/DAG/renderer foundations because they are the hardest to retrofit. The solo-developer judge preferred parity-first because its MVP needs none of a GPU backend, a plugin ABI, a chunk cache with eviction, a custom scheduler or a Lua sandbox.

**Options.** (a) Parity-first as-is. (b) Performance-first's foundations with parity-first's UX bolted on (architect's recommendation). (c) Parity-first's spec and sequencing on a cleaner layering, adopting performance-first's *interfaces* (chunk ownership, `DistanceIndex`, `ChangeMask`) without its pre-MVP infrastructure (eviction, DCF, SIMD dispatch, custom scheduler), and ecosystem-first's conventions for the surfaces that are frozen later (ABI, fixtures).

**Decision.** (c). The written product contract is parity-first §5.3–§5.10 (components), §7.1–§7.6 (shell, Data window, Channels window, editors, hotkey table, schemes) and §3.3 (rules). The layering follows core-first's discipline (D-007). The data path adopts performance-first's ownership model and distance index as interfaces from M0 with the cheapest implementation that satisfies the MVP (D-011, D-016). The extension surfaces adopt ecosystem-first's conventions when they are built (D-028, D-033).

**Consequences.** Section writers expand parity-first's text, not the other proposals', when a behaviour is in question. Where this document and parity-first disagree (licence, Qt-free line, renderer IR, validity, pyramid, `.ldx` merge, range identity, importer timing), this document wins.

**User must confirm:** no.

---

## D-002 Project licence

**Context.** Research facts: Qt 6 open-source modules the project needs (Core, Gui, Widgets, OpenGL, OpenGLWidgets, Svg, PrintSupport, Test, Concurrent, Multimedia later) are LGPLv3; a fixed list is GPLv3-only (Qt Graphs, Qt Charts, Qt Quick 3D, …) and none is used. Qt Advanced Docking System is LGPL-2.1; KDDockWidgets, QCustomPlot, FFTW and vector_blf are GPL and are excluded by the stack regardless. Every reusable reader the research marked safe to port is MIT/BSD (motec-i2, i3rs, motorsport-telemetry-rs, libxrk, TrackDataAnalysis, mdflib, dbcppp, gpmf-parser, matio, pocketfft, Iir1; Eigen is MPL2). GPL references (gotzl/ldparser, racing-data-converter, OpenLap, UltraLog AGPL) are read for understanding only under any licence. Landscape lessons 6 and 7: company-backed FOSS drifts to open-core; licence hygiene failures make code unreusable; "publish the format library under a clear permissive licence with golden fixtures so converter projects can contribute". Dynamic linking satisfies LGPLv3 identically under every option.

**Options.**
1. **MIT whole tree** (core-first, performance-first). Maximum reuse by converter projects, Python wheels, Rust plugins and vendors; one licence, no directory policing; in-process C-ABI plugins carry no combined-work question. Cost: closed forks are permitted.
2. **Apache-2.0 whole tree** (ecosystem-first). Same reuse profile plus an explicit patent grant; Apache-2.0 is one-way compatible with GPLv3; slightly heavier notice obligations.
3. **GPLv3 application + MIT `core`/`formats`** (parity-first). Insurance against a closed fork of the UI; keeps KDDockWidgets/QCustomPlot/FFTW as legal fallbacks. Costs noted by all three judges: policing two trees, the maths-engine licence left open, the FSF combined-work question for every in-process C-ABI plugin (deters vendor-written readers), and relicensing foreclosed without every contributor's consent.

**Decision.** Default **MIT for the whole repository**, SPDX `MIT`, DCO sign-off, no CLA, a published no-relicensing policy (D-004). Trade-off in two sentences: MIT maximises the chance that converter projects, vendors and the user's own converter adopt and contribute to the format library, which the research identifies as the only realistic path to contributors, and it keeps in-process plugins free of GPL questions. The price is that anyone may ship a closed fork; the evidence says the project's real risk is zero contributors, not a closed fork, and the DCO/no-relicensing policy already blocks the open-core drift that GPL would guard against.

If the user prefers the patent grant, Apache-2.0 is a drop-in substitute with no design change. If the user wants copyleft on the application, option 3 is viable but then the maths engine must be assigned to one tree before the first public tag and the C ABI must document that plugins are separate works communicating through a published interface.

**Consequences.** `LICENSE` (MIT), `NOTICES.md` (Qt LGPL notices, source offer, third-party list), `REUSE.toml` + SPDX headers from the first commit. GPL sources never pasted; the `.ld` reader is written from the public layout notes (i3rs `docs/ld-file-format.md`, motec-i2, motorsport-telemetry-rs). Harvested converter parsers must be the user's own work or MIT-compatible (open question).

**Amended by (A.12):** D-160 (DCO sign-off suspended while the maintainer is the only contributor).

**User must confirm:** **confirmed by the user on 2026-10-10: MIT for the whole tree.** DCO, no CLA and the no-relicensing policy apply as decided above.

---

## D-003 Codename and naming

**Context.** The product must not be named with "MoTeC" or "i2" (EULA/trademark); describing it as "reads MoTeC log files" is nominative use. The proposals used Chicane (×2), Datum and Stint. A GitHub search (WebSearch, 2026-10) found "chicane" crowded: the CRAN package CHiCANE, `zoontek/chicane` (React router, ~460 stars) and `pepeien/chicane` (a C++ graphics engine, a direct C++ namespace collision); "scrutineer" has several active projects including a security scanner; "datum" and "stint" are generic English words with many repositories. "esses" (the motorsport term for a sequence of S-bends) returned no software project under that name.

**Options.** Chicane (continuity with two proposals, but collides with a C++ engine); Esses (clean, short, motorsport term, usable as prefix); Stint/Datum (generic).

**Decision.** Working codename **Esses**. C++ namespace `esses`, CMake targets `esses_<lib>`, C ABI prefix `esses_`, GUI executable `esses`, CLI `esses-cli`, offscreen render tool `esses-render`, JSON `$schema` ids under a project-owned URL once the domain exists. File names are vendor-neutral by construction (`<log>.ld.sidecar.json`, `*.workbook.json`). A CI grep fails any UI string, identifier, asset or file extension containing `motec` or `i2` (case-insensitive, with an allow-list for the importer ids `motec.ld`, `motec.csv` and the import dialog labels that name the file formats).

**Consequences.** No trademark search is scheduled: on 2026-10-10 the user answered that none is needed for now and that the name may change later. The name can be swapped by a scripted rename because it appears only in the prefix table (`cmake/naming.cmake`) and documentation.

**Amended by (A.9, proposed):** D-125 (allow-list classes, identifier-token lint, optional `x-i2` rename).

**User must confirm:** **confirmed by the user on 2026-10-10 as the working name.** The user also answered on 2026-10-10 that no trademark search is needed for now (Q-02); the name may change later, through the scripted rename (`tools/rename.py` over `cmake/naming.cmake`).

---

## D-004 Governance, provenance and clean-room enforcement

**Context.** Landscape lessons 1, 6, 7 and 10 (bus factor 1, open-core drift, licence hygiene, velocity does not create contributors). Clean-room rules: no disassembly of MoTeC binaries; i2 behaviour comes only from help text, shipped XML/config files, SDK samples and public sources; no MoTeC code, art, icons, help text or trademarks.

**Options.** core-first (DCO + `GOVERNANCE.md` + `NOTICES.md`), ecosystem-first (adds `REUSE.toml`, SPDX, `reuse lint`, `CLEANROOM.md` per-format provenance, `TRADEMARK.md`, `SECURITY.md`, `OWNERS` per format), parity-first (`docs/CLEANROOM.md` provenance log + CI grep).

**Decision.** Adopt the union, all cheap: DCO (no CLA); `GOVERNANCE.md` stating the official distribution contains only MIT code and that relicensing requires every contributor's consent; `CLEANROOM.md` with one entry per format and per i2-derived behaviour naming the public source (help page, shipped XML file, public project) it came from; `TRADEMARK.md`; `REUSE.toml` + SPDX headers + `reuse lint` in CI; `CONTRIBUTING.md` with a provenance checkbox in the PR template; CI grep for vendor marks (D-003); no MoTeC templates, schemes, icons or sample files shipped; own two default schemes and own default circuit workbook; `OWNERS` per format directory once a second contributor exists. `CLEANROOM.md` also states the i2 field-of-use rule (D-038), records the research-phase use of user-written i2 files (D-039), and lists unlicensed public projects that were only read (e.g. GeekyDeaks/sim-to-motec: "read only, unlicensed, no code or template copied", D-040).

**Consequences.** A few hours in M0; thereafter each format PR carries a provenance note. `CLEANROOM.md` is also the written answer if the clean-room posture is ever challenged.

**Amended by (A.9, proposed):** D-128 (AI-provenance rule, forbidden paths, text-overlap check, ported-code attribution, CLEANROOM category column).

**Amended by (A.10):** D-143 (the `.ld` pro-logging field is never computed or written; working around a vendor licence check is out of scope) and D-145 (`CLEANROOM.md` categories user-contributed own work and user-owned device logs).

**Amended by (A.12):** D-160 (DCO sign-off suspended while the maintainer is the only contributor).

**User must confirm:** no.

---

## D-005 Oracle policy and use of user-owned i2 files

**Context.** Three load-bearing test strategies depend on the user: (1) i2's own CSV/Channel-Report exports of the user's logs as black-box oracles for `.ld` decoding, lap rules and maths semantics; (2) importing the user's `.i2wkb/.i2wsp/.mcs/.mfs` and maths XML files (user-owned data written by their own installation; the EULA's reverse-engineering clause covers the Software); (3) observing i2's behaviour on the user's install to settle undocumented semantics (overlay colours/numbering, `^` precedence, Values-window content). None involves inspecting binaries. Parity-first and ecosystem-first both flag this as an EULA call for the user. Review (2026-10-10) of the install's EULA added two facts: its LICENSE clause permits use of i2 only with data generated by MoTeC logging devices or MoTeC-licensed third-party software and excludes data generated directly or indirectly by non-MoTeC products; its RESTRICTIONS clause forbids reverse engineering, decompiling and disassembly except where law prohibits that restriction. The research phase (2026-10-09) already parsed project files that the user's i2 wrote (category 2) before any answer was given. The shift-vs-mul order is settled from a real logger file read by Esses, not by observing i2 (D-037).

**Decision.** Default posture: (1) oracles from exports are used only for the user's own files, stored in a private fixture submodule, never redistributed; (2) i2 XML import is implemented from the public config-model notes and the user's own files; (3) behaviour observation is recorded in `CLEANROOM.md` as "observed on a licensed install, no internals inspected". All three proceed only on the user's explicit yes. Amended by D-038 and D-039: (4) **field of use** — i2 is run only on files from MoTeC logging devices (sim exports only if the user confirms the producer is MoTeC-licensed; the user stated on 2026-10-10 that ACC is not) and never on files Esses wrote; export checks use re-import by Esses plus decoding by i3rs and motec-i2 (MIT); (5) (3) additionally needs the user's legal comfort that black-box observation is not "reverse engineer" under the EULA; until then the D-017, D-020 and D-047 defaults are recorded as chosen, not observed (the user answered yes to (3) on 2026-10-10; that legal check is the user's own responsibility and is not recorded as performed); (6) the three answers are blocking before the first commit of `CLEANROOM.md` (all three were given on 2026-10-10, so it no longer blocks), which records the research-phase use of category (2); if (2) is no, each provenance line that cites the user's files is re-sourced from help text or shipped XML or marked "unsourced, chosen default", and the user-file-derived sections of `background/research/config_model.md` never enter the public repository.

**Consequences.** If the user says no to (1), maths and lap compatibility rely on help-text semantics plus hand-derived cases (`tests/maths/*.case.json`), and the risk table in the outline rises for "numbers differ from i2". If no to (3), D-017 and D-020 defaults stand undocumented-but-chosen.

**User must confirm:** **confirmed by the user on 2026-10-10 (Q-03): yes to all three.** (1) i2 CSV/report exports of the user's MoTeC-device logs may be private test oracles, kept in the private submodule. (2) The research-phase use of the user's i2-written files on 2026-10-09 stands; `CLEANROOM.md` records it with both dates. (3) i2's behaviour may be observed on the user's install: black-box observation only, never disassembly or decompilation; the legal check that this is not "reverse engineering" under the EULA is the user's own responsibility and is not recorded as performed. The ACC sub-question was answered the same day: ACC files are not MoTeC-licensed, so i2 is never run on ACC data, not even as an oracle (D-038 (a)); ACC exports are ordinary third-party fixtures, private for now (Q-04).

---

## D-006 Qt version, modules and linking policy

**Context.** Research: Qt 6.12 LTS released 2026-09-30 (support to 2031, open-source users get only the first patch releases); 6.8 open-source patches ended at 6.8.3; the user's local 6.7.2 kit is out of support but works with VS 2022 (MSVC ABI compatibility); GitHub's MinGW is ABI-incompatible with Qt's MinGW builds, so MSVC on Windows; `QRhiWidget` exists since 6.7 with no BC guarantees and needs `Qt::GuiPrivate`; `QQuickWidget` disables the threaded render loop; static linking under LGPLv3 makes the app a potential derivative. Contradiction (solo-dev 22, architect 28): code to 6.5 API (parity) vs `QT_MIN_VERSION 6.7` (performance).

**Decision.** Qt 6 **Widgets** shell. Modules allowed: `Core, Gui, Widgets, OpenGL, OpenGLWidgets, Svg, PrintSupport, Test, Concurrent` (Concurrent only in tests), `Multimedia` when the video component arrives; `Sql` is **not** used (D-019); no GPL-only module ever. **Minimum Qt 6.7** (the kit the developer actually builds with; nothing older is tested), CI and releases on **6.12 LTS** via `aqtinstall`/`install-qt-action`, yearly minor bump as a scheduled task. Dynamic linking only; the installer ships unmodified Qt DLLs, LGPL notices and a link to the exact Qt source tarball. MSVC 2022 on Windows, GCC/Clang on Linux, Apple Clang on macOS. No QML/Qt Quick.

**Consequences.** The developer can keep 6.7.2 locally until M4; CI catches 6.12 drift. `QRhiWidget` remains a post-v1.0 option behind the renderer seam (D-025).

**Amended by (A.9, proposed):** D-126 (Qt 6.12 installed locally in M0 week 1).

**User must confirm:** no (local Qt version: Q-13; default changed by D-126 to installing 6.12 in M0 week 1).

---

## D-007 Where the Qt-free line sits

**Context.** Contradiction in all three scorecards (solo-dev 4/5, power-user 4, architect 4/5): everything below widgets Qt-free with own signals/executor/mmap/undo and a Qt-free software rasteriser (core-first, performance-first, ecosystem-first) vs only core/formats/maths Qt-free, model on Qt Core (`QUndoStack`), plot on Qt Gui (`QPainterPath`, `QString`, `QImage`) (parity-first). The solo-dev judge: "the bridge tax is the hidden cost of the stricter line … headless render needs Qt-free text rasterisation (unbudgeted)". The architect: parity's layering is the least clean and the plot core is not toolkit-agnostic; core-first's `CHICANE_BUILD_QT=OFF` is "the single most valuable maintainability property any proposal offers".

**Options.** (a) Strict line (plot core incl. rendering Qt-free + CPU rasteriser). (b) Parity's line. (c) A split line: *geometry* Qt-free, *rendering and the application model* on Qt.

**Decision.** (c).
- **Qt-free** (CMake `ESSES_BUILD_QT=OFF` builds them plus `esses-cli` and all their tests with no Qt installed; `cmake/qt-free-check.cmake` fails configure on any `#include <Q` or `Qt6::` link in these targets): `esses_base`, `esses_units`, `esses_store`, `esses_model`, `esses_formats`, `esses_maths`, `esses_analysis`, `esses_project` (documents and schemas only), `esses_plotcore` (view state, decimation, geometry builders, axis/band layout, hit testing, `DrawList` IR with text *runs*, not glyphs), `esses_cli`, later `esses_capi` and `esses_script`.
- **Qt Core allowed**: `esses_qtmodel` (observable application model: selection, view-model wrappers, project session, `QUndoStack` commands, async facades, `Dispatcher`, settings).
- **Qt Gui**: `esses_qtrender` (`RasterBackend` on QPainter, `LayerCache`, offscreen rendering for goldens/print/clipboard; later `GlBackend`). Text is rasterised only here.
- **Qt Widgets + ADS**: `esses_widgets`, `esses_app`.

No custom software rasteriser: golden images come from `RasterBackend` run offscreen (`QT_QPA_PLATFORM=offscreen`), which is deterministic across GPUs (solo-dev judge). Strings are UTF-8 `std::string` below the Qt line; `QString` appears only in `esses_qtmodel` and above.

**Consequences.** `esses-cli info/channels/laps/eval/convert/validate/fixture/bench` run without Qt; `esses-render worksheet.json x.ld -o out.png` is a small Qt Gui tool. The plot geometry is unit-testable against brute-force references without a window. The "bridge" is one `Dispatcher::post` (D-031), not an adapter per model object.

**Amended by (A.9, proposed):** D-122 (`esses_project` links `esses_maths` as a same-layer edge and hosts every i2 configuration importer under `libs/project/src/cfgimport/`).
**Amended by (A.13, proposed):** D-161 (`esses_qtmodel` also links Qt Gui, for `QUndoStack`/`QUndoCommand` only).

**User must confirm:** no.

---

## D-008 Time base and sample rate

**Context.** i2's API uses double microseconds; all proposals use `int64` µs. Rate: rational `Rate{num,den}` (core-first) vs `double rateHz` (others) — contradiction (power-user 20, architect 25). 33.333 Hz and 7.5 Hz loggers exist in the format matrix (Pi, EFI, VBO at 10/20 Hz, AiM at 10/20/50 Hz; sim exporters at 60 Hz).

**Decision.** `using Micros = std::int64_t` (microseconds since the data layer's origin; optional UTC anchor per layer). `struct Rate { std::uint32_t num, den; }` in Hz; `timeAt(i) = t0 + llround_div(i * 1'000'000 * den, num)` in integer arithmetic; `indexAt(t) = floor((t - t0) * num / (1'000'000 * den))`. `Rate::hz()` returns a `double` for display and for maths rate inference. `TimestampedSeries` stores an explicit I64 µs column (no F64 approximation).

**Consequences.** No drift on 24 h logs or odd rates; importers map `.ld` integer Hz to `{hz,1}` and CSV sample periods to exact rationals; the maths "Auto = highest input rate" compares rationals exactly.

**Amended by (A.9, proposed):** D-129 (exact `indexAt`, 128-bit floor arithmetic, value ordering of `Rate`).

**User must confirm:** no.

---

## D-009 Series kinds, sample types and validity

**Context.** Research: never resample at import; `UniformSeries{rate,t0,values}` or `TimestampedSeries{t[],values[]}`; uniform array + validity mask is i2's `DataArray`. Contradiction (solo-dev 7, power-user 7, architect 7): mandatory parallel validity bitmask (core, parity, eco) vs NaN-as-invalid plus a per-chunk validity mode with bitmap only for I32 (perf). GPS lat/lon need F64 (6 decimals at ±180°).

**Decision.** Two series kinds, `Series = std::variant<UniformSeries, TimestampedSeries>`, never resampled at import; the `Resampler` is a view-layer service with per-channel `Interp {Linear, Closest, Previous, Next}` (i2 names), defaulting to `Previous` for int/enum/bool/gear. Sample types `Dtype {F32, F64, I32}` plus descriptor-level `SampleKind {Real, Integer, Enum, Bool, Bitfield, Text}`; F32 default, F64 for GPS lat/lon, timestamps, `Corr Dist`, integrators and any expression with an F64 input; I32 for raw bitfields/enums/counters. **Validity** is a per-chunk mode `Validity {AllValid, Bitmap, AllInvalid}` with an 8 KiB bitmap only when needed, *and* every kernel is NaN-aware for float columns (NaN is invalid). The public API is `validAt(i)` / `Validity` spans; importers set bitmap bits for sentinels (`-###`, `none`, zero lat/lon without fix); maths `invalid()`, gating and lookups clear bits; rendering splits polylines at gaps; statistics skip invalid samples.

**Consequences.** Fully valid columns (the common case) cost no extra stream; I32 columns and partially valid floats pay 1 bit/sample; the SIMD-free MVP loops are simple because a chunk's mode is one branch.

**User must confirm:** no.

---

## D-010 Units: storage unit and persisted values

**Context.** Research (config-model implications): store samples and every persisted limit in the quantity's SI base unit exactly as i2 does (Overrides are in rad/s, Pa, K, ratio); keep i2's affine model (`value_in_unit = base*scalar + offset`), the Delta-vs-Absolute quantity split, all 444 symbols + 44 aliases as case-insensitive compatibility keys; model AFR as lambda × a vehicle constant. Contradiction (solo-dev 6, power-user 6, architect 6): core-first keeps samples in the *logged* unit with unit-tagged persisted limits; the other three and the research store base units. All judges' verdict: base units in memory, explicit unit on persisted values.

**Decision.** Samples are stored in the **SI base unit** of their quantity (conversion applied once at decode; `.ld` round-trip exactness is handled by the writer re-quantising with the original scale/dec/mul). The `UnitRegistry` is our own table generated from physical definitions in `data/units.json` with dimension vectors `[L,M,T,I,Θ,N,J]` plus a *display family* so `Temperature` and `Temperature Delta` (and `Pressure` / `Pressure Delta`) stay distinct; i2's ASCII spellings (`kPa a`, `psi g`, `P A/F`, `dKPA`, `rpm`, …) are included as aliases because they are facts needed to parse user maths. AFR is lambda × stoichiometric constant with the i2 AFR units kept as import aliases. **Every persisted number carries an explicit unit object** `{"v": 8000, "unit": "rpm"}` (core-first graft) in overrides, workbook scales, sidecar details, maths constants and track maps; loaders convert to base on read. Display unit and DPS live in channel overrides (project) exactly as i2's.

**Consequences.** Overlaying two files that logged the same channel in different units needs no conversion path; autoscale across files and reports compare base values; project files remain human-readable. The unit override that "cannot apply to all open files is dropped" (i2 rule) is implemented in the override loader.

**Amended by (A.9, proposed):** D-141 (units table provenance).

**User must confirm:** no.

---

## D-011 Column store, chunk ownership and memory policy

**Context.** Research: SoA chunked column store, 64 k-sample chunks, float32, mmap. Architect: performance-first's `Chunk` + `ChunkCache` with pin counts + RAII `ChunkHandle` + `IChunkProvider` is "the only proposal with a correct answer to what happens when a column is evicted while someone is reading it" (core-first and parity-first return `const Series&` from an evictable cache: use-after-free). Solo-dev: eviction, DCF, live-append pyramids and a custom scheduler are premature before a worksheet exists; RW-2 (the common pro case, 384 MB with pyramids) is fully resident. Pyramid memory: factor 2 ≈ 2× raw (core-first's 50 % estimate is wrong), factor 4 ≈ 0.67×, 16× steps ≈ 13 %.

**Decision.** `esses_store` defines from M0: `Chunk` (65,536 samples; one allocation holding samples | optional validity bitmap | complete factor-4 `MinMaxPyramid` levels 4…65536; `version`; `builtLevels`), `Column` (lazy sequence of chunks fed by an `IChunkProvider`; `tryChunk(no, prio)` non-blocking for the GUI thread, `require(first,last,prio,cancel)` blocking for workers, returning a `ChunkHandle` that pins), `ChunkCache` (process-wide; byte budget; LRU of unpinned chunks; `Stats`), `SummaryPyramid` (levels with bucket ≥ 256, ≈ 1 % of raw, always resident, built in the background after open, gives full-extent traces and autoscale without touching the file). Providers: `LdChunkProvider` (mmap decode per chunk), `ImporterChunkProvider` (generic `IReader` reads), `PlanProvider` (maths, D-022), later `DcfProvider` and `LiveProvider`. **Implementation staging:** M0–M3 implement `ChunkCache` *without eviction* (budget tracked, exceeded-budget warning in Messages, fixture set sized to fit); LRU eviction lands in M5 with the RW-1 stress fixture (amended by D-043 from M4; 0.1 also warns at open when the probe estimate exceeds the budget); the on-disk column cache (DCF) and SIMD decode kernels are added only when a benchmark shows CSV reopen or decode is a complaint (post-v1.0). Default budget 25 % of physical RAM (minimum 1 GB), configurable. Chunks are immutable after sealing; a worker holding a `ChunkHandle` can never observe eviction.

**Consequences.** The ownership model that is hard to retrofit exists from day one at ~1,500 lines; the infrastructure the solo-dev judge warned about is deferred behind interfaces. Text importers produce chunks through the same provider so later caching is uniform.

**Amended by (A.9, proposed):** D-130 (`SourceId` is the quick content hash) and D-123 (Rule W: `Column::require` materialises inline; `Column::tryPin`/`prefetch` replace the raw `tryChunk` accessor; chunk memory is freed only under the cache lock at pin count zero).

**User must confirm:** no.

---

## D-012 Decimation: exact M4 over a factor-4 pyramid

**Context.** Contradiction (solo-dev 8, power-user 8, architect 8): factor 2 (core), 16× steps (parity), factor 4 colocated with ≤ 1/8-px horizontal error (perf), branching 64 with ~70 node visits per column (eco); strict M4 exactness vs a documented bound.

**Decision.** Factor-4 pyramid colocated per chunk (D-011). For a column spanning samples `[a,b)`: whole buckets at the largest level `L` with `bucket(L) ≤ (b−a)/8` are read directly; the two partial buckets at the edges are refined by **hierarchical descent** (level L−1 buckets fully inside, then L−2 for the remaining straddle, … down to raw samples, ≤ 4 reads per level), so `min`/`max` are exact; `first`/`last` are the raw samples at `a` and `b−1`. Cost per column ≈ 8–32 whole buckets + 2 × (levels × ≤ 4) ≈ 60–100 reads; the chunk is resident so the raw reads hit the same allocation. Thresholds: `spp < 2` draw raw samples joined (points when `spp < 0.25` or style Points); `2 ≤ spp < 32` raw-sample M4; `spp ≥ 32` pyramid path. Validity gaps break the polyline. Property test: M4 output equals a brute-force per-column reference on NaN-sprinkled random data at every zoom.

**Consequences.** Pixel-exact (no documented bound to explain), memory 0.67× raw per materialised chunk, O(pixels) per trace. The `SummaryPyramid` alone serves full-extent views.

**User must confirm:** no.

---

## D-013 Channel identity and resolution

**Context.** Research: channel NAME is the identity everywhere in i2 (workbooks, maths, overrides); lookups case-insensitive; Mappings (rename + unit stamp) and Aliases (ordered fallback) are load-time resolvers with diagnostics; per-importer dictionaries generalise `ldmap.xml`/`bitchans.xml`. Contradiction (power-user 20, architect 25): UUID (core, parity, eco) vs xxh3 content hash (perf).

**Decision.** `ChannelDescriptor { ChannelId id; std::string name /*canonical analysis name*/, shortName, loggedName; UnitId unit; QuantityId quantity; Dtype dtype; SampleKind kind; Interp interp; int dps; int colourIndex; std::optional<EnumTableId> enums; ChannelSource source {Logged, Maths, Detail, Alias, Mapped, Constant, Setup}; std::string statusText; }`. `ChannelId` is a deterministic 128-bit id (UUIDv5-style hash of `(source content hash, layer, logged name, importer id)`) so it is stable across sessions without a registry; the **name** remains the lookup key for workbooks, maths and overrides (NFKC case-fold). `ChannelResolver` applies project Mappings → Aliases → case-insensitive canonical name → logged name, and records a `ResolutionTrace` per lookup (e.g. `'Wheel Speed' → alias → 'Wheel Speed FL' (km/h)`) that the Channel Status window shows. Unresolved references in workbooks are kept and shown as "(missing)" labels, never dropped. Per-importer `dictionary.json` (vendor name/id → canonical name, unit, quantity, interp, status-bit expansion) is applied by the importer before resolution.

**Consequences.** `.i2wkb` import later is a pure name mapping; alias colours stay consistent across outings because the alias's own descriptor carries the colour index.

**Amended by (A.9, proposed):** D-130 (source identity) and D-132 (alias chains with cycle detection; Error/Missing candidates fall through).

**User must confirm:** no.

---

## D-014 Markers, ranges, laps and rules

**Context.** Research: adopt i2's Marker → Rule → RangeGroup (hierarchical, colon paths) → DataView → Mode pipeline as data; marker classes SOL/EOL/BCN/SPLTBCN/IGRDBCN/RESET/STGSTRT/STGEND; beacon-pair semantics (SOL→BCN out lap, BCN→BCN flying lap numbered from 1, BCN→EOL in lap, RESET ⇒ partial untrusted, IGRDBCN ignored, in/out untrusted by default); keep user beacon edits in the sidecar; classify laps like motorsport-telemetry-rs. Contradiction (architect 13): range overrides keyed by lap index (core, perf) vs name (parity, eco) vs start/end marker ids (eco's `Range`); index and name both break under beacon insertion. Rules: JSON state machines (core, parity, perf) vs JSON-or-Lua (eco).

**Decision.**
- `Marker { MarkerId id; MarkerClass cls; Micros t; std::string name, comment; Provenance prov {Logged, Companion, User, Derived}; std::uint32_t flags; std::optional<Micros> originalT; bool enabled; }`. `MarkerId` is deterministic for logged/companion markers (hash of class, logged time, group, ordinal) and a UUID for user markers.
- User edits of logged markers are **`MarkerEdit` ops** in the sidecar (`offset`, `ignore`, `restore`) referencing the marker id (ecosystem-first graft); user-inserted beacons are `Marker`s with `prov = User`. `SHIFT+F7` inserts a BCN at the cursor.
- `Range { RangeId id; RangeKind kind {Outing, Lap, Run, Stage, Section, Split, Data}; LapClass lapClass {Flying, Out, In, Partial, Pit}; MarkerId startMarker, endMarker; Micros start, end /*[start,end)*/; std::string name, abbrev; int number; bool trusted, enabled; Micros offsetUs; double offsetM; std::optional<double> stretch; }`. **Range identity for persistence is `(group path, startMarker id)`**; the sidecar's `rangeOverrides` (trusted flag, alignment offsets) are keyed that way, with `name` kept as a fallback key for `.i2wsp` import (`source::layer::group::name` form, layer `""` ↔ `Normal`).
- Laps are always **derived**, never stored; a marker edit rebuilds the affected groups synchronously (O(markers)).
- Rules are declarative JSON state machines in `data/rules/*.rule.json` behind `IRangeRule(const DataLayer&, const Profile&) -> RangeGroupTree`: `beacon-pair` (circuit laps with the semantics above plus stint classification: counter reset or > 10 s gap starts a stint, ≥ 15 s stationary inside a lap ⇒ pit), `whole-outing`, `track-sections` (laps × track map sections → `Outings:Laps:Track Sections:<set>`), later `runs` (drag) and `stages` (rally). A Lua rule host is the v1.1 extension (D-030).
- `RangeRef { SourceId src; std::string layer; std::string groupPath; RangeId range; }` is what selection and view state hold.

**Consequences.** Beacon insertion/offset never orphans a trust flag or an alignment offset; the Lap Editor's "restore logged" is "delete the `MarkerEdit`". Stint/pit classification is available to filters and reports from M1.

**Amended by (A.9, proposed):** D-132 (Start Lap numbering, stint/pit annotations, detail groups, per-category track-section groups, device-event markers, `offsetDistance`).

**User must confirm:** no.

---

## D-015 Sidecar document and `.ldx` handling

**Context.** i2 never modifies the `.ld`; all edits live in the `.ldx`. The `.ldx` grammar is only partly observed (`MarkerBlock/MarkerGroup/Marker{ClassName, Name, Flags, Time=cumulative µs}`, `Details/String`, further classes such as SOR). Contradictions: sidecar name `<file>.<ext>.sidecar.json` (core, parity, eco) vs `<base>.datum.json` (perf); `.ldx` imported once as initial state (parity) vs merged with precedence on every load (perf, core, eco); `.ldx` writer in MVP (core, perf, eco) vs never (parity). Research open question: are per-lap alignment offsets in the project or the `.ldx`? (parity: sidecar).

**Decision.** Sidecar file `<logfile>.<ext>.sidecar.json` next to the log (`bathurst_r1.ld.sidecar.json`, `bathurst_r1.csv.sidecar.json`), schema `sidecar-1`, written atomically (temp + rename), keyed by data layer (`"Normal"` is the canonical layer name; `""` is accepted as an alias on import and emitted by the `.ldx` writer). Contents per layer: `details` (user edits with provenance and original value), `markers` (user markers), `markerEdits` (ops on logged/companion markers), `rangeOverrides` (trusted, `offsetUs`, `offsetM`, keyed by `(groupPath, startMarker)`), `localMaths` (same schema as a maths module), `annotations` (absolute Main-source time, text, channel), `videoLinks`, `runs`/`stages` (profiles later). A `source` block carries path, size, xxh3-128 content hash and importer id/version so a moved file is detected. The `.ldx` companion is **merged on every load** with precedence *sidecar > companion > file*, companion markers get `prov = Companion`, unknown `.ldx` elements are kept as opaque strings for round-trip; a `.ldx` **writer exists for export only** (v1.0, File → Export Data to `.ld`+`.ldx`), never written automatically. Alignment offsets persist in the sidecar (answering the research open question) because they are per-lap facts about a file, not layout.

**Consequences.** Users who keep using i2 in parallel see their later `.ldx` edits (the one-time-import model would silently ignore them); our edits never touch vendor files. Transient (filter) maths are never persisted; scale/offset/zero from the channel label become `localMaths` items with DPS preserved (i2 rule).

**Amended by (A.9, proposed):** D-130 (sidecar `source.contentHash`).

**User must confirm:** no.

---

## D-016 Distance model: corrected speed/distance, `DistanceIndex`, stretching, F9

**Context.** i2: Corr Dist is integrated from Corr Speed (fastest wheel / alternate list / G correction / clamping) and is required for distance mode ("no data shown if missing"); lap distance stretching scales each lap to the track length within a tolerance; zoom must be preserved across F9 even when Corr Dist is missing; overlays must be compared in distance mode; overlays align at lap start in time mode. Contradiction (power-user 12, architect): compute corrected speed/distance on open so distance mode is never empty (perf) vs banner + fraction-based zoom preservation (parity) vs unspecified. Performance-first's `DistanceIndex` (tAtD table at 0.25 m) was named a graft by all three judges. Ordering inconsistency in performance-first (module in M2, engine in M3) must not recur.

**Decision.**
- `Corr Speed` and `Corr Dist` are produced by a **built-in maths module** (`esses.corrected_speed_distance`) with schema'd settings (`method: fastestWheel | alternateList | gCorrection`, channel lists, clamp min/max with invalidate flags, units follow speed units) shown in the Corrected Speed/Distance dialog; the algorithm lives in `esses_analysis`, the module wrapper in `esses_maths`. It is registered for every source on open and **prefetched at `Background` priority** so distance mode is populated without user action; when no usable speed channel exists the graph shows a "no distance data" banner for that source.
- `DistanceIndex` per `(source, lap)`: `stepM = 0.25`, `tAtD[]` seconds from lap start at each step (first time each distance is reached, so stopped-car plateaus are handled), `lengthM`, `stretch` scalar, built lazily from the `Corr Dist` F64 column and cached by `(source.version, lap id, module version)`. `timeAt(d)`, `distAt(t)`, `indexAt(d, rate)` are O(1).
- **Lap stretching** is the per-lap `stretch` scalar, applied when `|lengthM − track.length| / track.length ≤ tolerance` (default 5 %, per project, toolbar toggle, per-lap report in the Lap Editor; works with beacon offsets because the index is rebuilt from edited ranges).
- **Alignment offsets** (O axis, CTRL+SHIFT / ALT+SHIFT arrows, drag) are per-range constants added before the lookup; pressing O again zeroes them; selecting a lap with a stored offset auto-shows the axis; amounts appear in the Data window.
- **F9 zoom preservation**: the zoom span is converted through the Main lap's `DistanceIndex` when present; when absent, by *fraction of the Main range* (`a' = R.start' + (a − R.start)/|R| · |R'|`), so toggling is lossless either way. Pasted components keep their domain setting.
- Overlays in time mode align at lap start (i2 behaviour), with a hint to switch to distance.
- Variance uses the tables (D-023).

**Consequences.** Distance-domain decimation runs per lap with no resampled channel copies; variance is O(pixels); the module appears in Channel Status like any maths item (no hidden plugin). The maths engine therefore must exist by the milestone that ships distance mode (outline M1 ships a *minimal* engine: `MathGraph`, `PlanProvider` and the built-in module; the parser, expression evaluation and the full language follow in M2 — amended by D-044).

**Amended by (A.9, proposed):** D-134 (`DistanceChain`) and D-140 (`DistanceIndex` key).
**Amended by (A.12, proposed):** D-154 (the default `alternateList` ends with `Speed`) and D-157 (`timeAt` exact between the 0.25 m steps).
**Amended by (A.13, proposed):** D-171 (`esses.gps_corrections` in M2: zero-fix step only, shadowing outputs first in their chains, Disabled without both channels).
**Amended by (A.13, proposed):** D-173 (`esses.gps_corrections` registered for a layer that logs both GPS inputs) and D-174 (0.1 track length = the source's `Venue Length` until track maps; factors published as `Range::stretch`; the stretch toggle is view state).

**User must confirm:** no (tolerance default is an open question).

---

## D-017 Selection model, overlay colours and numbering

**Context.** Ground truth (features/concepts): Main (red dot, exactly one, ENTER), Overlay 0 (radio, grey box, CTRL+ENTER, distinct scheme colour), Overlays 1..n (ALT+ENTER, multi-set, channel colours); F4 hides without deselecting, SHIFT+F4 clears, Q swaps Main/Overlay 0, N/P preserve zoom, W zooms to the Main range and pulls the cursor inside, CTRL+F fastest *trusted* lap (in/out never count when a trusted lap exists; untrusted never fastest when no trusted laps exist), CTRL+ALT+F all fast laps, J single-colour Main; variance, dual-source measurements and reports use Main vs Overlay 0 or the lowest-numbered overlay. Research open question: how overlay numbers are assigned/re-assigned on deselection. Colour rules: Main channel colours (or scheme Main colour under J), Overlay 0 scheme colour, Overlays ≥ 1 channel colours, separate palettes for light/dark backgrounds. Power-user: core-first's per-overlay dash/tint is a design choice presented as parity; parity's scheme-driven `overlayStyle` is safer.

**Decision.** `SelectionSnapshot { RangeRef main; std::optional<RangeRef> overlay0; std::vector<RangeRef> overlays; bool overlaysHidden; bool singleColourMain; }` held by `DataSelectionModel` (one per workspace, observed by every component). "Reference overlay" = Overlay 0 if set, else the lowest-numbered overlay. Overlays are numbered in selection order and **renumbered compactly on removal** (documented choice; verify on i2). Colours exactly as the ground truth by default; the scheme carries `overlayStyle` (per-overlay colour source, dash, lightness) so an observed i2 difference is a scheme edit, not code. Data window filters (All / Selected / Fastest / Fastest 3 / Within 1 % / Lap number with `3`, `3-7`, `3,5,9` syntax) persist in `project.json`; "apply filter to all sources" is a toolbar action. Selection and single-colour state are restored with the project ("load previous data with project").

**Consequences.** Every component reads the same snapshot; reports and variance never disagree about the reference overlay.

**Amended by (A.9, proposed):** D-135 (fastest-lap candidate rule per TT 2485).

**User must confirm:** **yes** — verify overlay 1..n colours and renumbering on the user's i2 install (D-005 §3). Permitted since the user's yes to Q-03 (3) on 2026-10-10 (black-box only; the legal check of D-038 (c) is the user's own responsibility) and scheduled for M1; until observed the default is recorded "chosen, not observed".

---

## D-018 View state and component linking

**Context.** i2: components on a worksheet are linked (cursor, datum, zoom range, time/distance mode propagate); workbook XML carries per-component `LinkZoom`/`LinkCursor`/`LinkDatum`, so per-component opt-out is parity, not deviation (parity-first; power-user graft). Performance-first: cursor stored in Main-lap time and converted per domain; `ChangeMask` so a cursor move invalidates only the overlay pass (all judges listed as a graft). Zoom summary bar above the worksheets with SHIFT+click extend; Outing graph reads the same state.

**Decision.** `ViewState { Domain domain; XSpan zoom /*[a,b) in µs or metres relative to the Main range start*/; Micros cursorT; std::optional<Micros> datumT /*both in Main-lap time*/; bool offsetAxis; std::uint64_t version; }` is a Qt-free struct in `esses_plotcore`, one per worksheet, wrapped by `WorksheetViewModel` (QObject) in `esses_qtmodel` which emits `changed(ChangeMask)`. `ChangeMask` bits: `Cursor, Datum, Zoom, Domain, Selection, Offsets, Data, Scale, Size, Scheme`. Components map bits to passes: `Cursor|Datum` → Dynamic layer only; `Zoom|Offsets|Scale|Size` → Static + Dynamic; `Domain|Selection|Data|Scheme` → everything. Each component declares `links { zoom, cursor, datum }` (default all true) mirroring i2's attributes; a component with `links.cursor = false` keeps a private cursor. Summary components (histogram, scatter, FFT, mixture, suspension) recompute over `zoom` asynchronously. The zoom summary bar and the Outing graph render from the same `ViewState`. Animation (A) is a timer advancing `cursorT` (Dynamic-only, so cheap by construction), rate from the Data menu.

**Consequences.** A cursor drag touches only cached layers + labels (budget D-025); the F9 rule in D-016 is implemented once in `WorksheetViewModel::setDomain`.

**Amended by (A.9, proposed):** D-133 (one workspace-level zoom/cursor/datum; domain per worksheet).

**User must confirm:** no.

---

## D-019 Project, workbook and layout persistence

**Context.** i2: project = folder (name = folder), autosave every 2 min and on exit with prompt, `.backup/`, maths saves on edit, schemes per user; complaints include project-file corruption. Research: schema-validated JSON everywhere, typed values, versioned records, keep Workspace → Workbook → Worksheet → Component with fractional rectangles and the 19-component vocabulary, persist definitions not derived data. Contradictions: layout model free rectangles (core, parity, perf) vs splitter tree (eco; lossy for overlapping/pinwheel layouts and i2 import); properties UI (D-027); ADS perspective in `layout.json` (core) / inside the workbook (perf) / derived (eco); autosave `*.autosave.json` (core) / `backup/` + 2 min (parity) / write-ahead journal (perf); index DB SQLite via QtSql from M1 (parity) vs sqlite3 amalgamation (core, eco) vs absent (perf); solo-dev verdict: absent from MVP.

**Decision.**
- A project is a **folder** containing `project.json` (schema `project-1`: profile id, options incl. autoscale flags/time format/stretch tolerance, MRU data, last selection snapshot, Data-window filter, maths order), `channels/overrides.json`, `channels/aliases.json`, `channels/mappings.json`, `channels/unitmaps/<importer>.json`, `maths/<module>.maths.json`, `workbooks/<name>.workbook.json`, `trackmaps/<venue>.track.json`, `backup/` (previous copy on every explicit save, i2 behaviour), `.local/layout.json` (per-user dock perspectives keyed by workbook id, **not shared**), `.journal/` (write-ahead autosave).
- Every file carries `$schema` and `schemaVersion`; `schemas/*.schema.json` ship in the repo; the loader validates and runs registered migrations; `esses-cli validate <file>` checks any file; byte-identical canonical round-trip tests per file kind.
- Workbook JSON: worksheets with `name, linkTo, domain, grid`, components as **free fractional rectangles** `[l,t,r,b]` (0..1; overlap allowed, "send to background" is z-order; i2 v101 `RectGridPts` and v200 `PosPC` convert losslessly), each record `type` + `version` + `links` + type-specific body; channel references by **name**; shared sub-schemas `BandAxis`, `ReportSpec`, `ColourBands`; **unknown component types are preserved verbatim** and shown as a placeholder; attributes imported from i2 but not implemented go to an `x-i2` bag with an import report.
- Autosave: every 2 min (default, i2) the session appends to the **write-ahead journal** and never rewrites project files; explicit Save writes atomically (temp + rename) and clears the journal; on open, a non-empty journal offers recovery. Maths modules save on editor close; schemes per user (`~/.config/esses/schemes/`), keymap per user, dock geometry per user.
- **No index database in the MVP** (the Open dialog is `QFileDialog` + a Details side pane that probes selected files). When the four-pane indexed dialog arrives (v1.0) it uses the sqlite3 amalgamation in Qt-free code, keyed by `(path, mtime, content hash)`, storing `probe_json` rows (D-028), never `QtSql`.
- Workspace archive export (`*.esses-workspace.zip` with `manifest.json`: name, author, licence, profile, `requiresChannels[]`, `requiresMaths[]`, min app version) is v1.0; on import the app lists missing channels.

**Consequences.** Projects are git-diffable; a crash at the 2-minute mark cannot corrupt a project; a workbook shared with a teammate carries no personal dock state; i2 workbook import (v1.0) is a mapping onto the same vocabulary.

**Amended by (A.13, proposed):** D-162 (`project-1` minor 2: Data-window filter keys and the 1 -> 2 migration) and D-163 (load pipeline: typed reader before the schema; newer minors kept on save or refused, R-38).
**Amended by (A.13, proposed):** D-178 (the recovery prompt before the session restore; leaving a project saves, asks or writes the view state; the scheme choice and the toolbar selector are user settings).
**Amended by (A.13, proposed):** D-177 (the Maths Editor's auto-save on close is the project's save, after a module edit only).

**User must confirm:** no.

---

## D-020 Maths language and operator precedence

**Context.** Research: keep the expression language 100 % source-compatible (`'Name'[unit]`, same ~60 functions, operator word aliases, `"Range:Path"` literals, comments, int-vs-decimal window semantics, `invalid()`). Open questions: precedence/associativity of `^` and unary minus; channel vs function case sensitivity (help: not case sensitive; release notes: functions case sensitive). Contradiction (all three scorecards): `^` tighter than unary minus so `-2^2 = -4` (core, eco) vs unary minus tighter so `-2^2 = 4` (parity, perf); core-first lumps `& | && ||` at one level.

**Decision.** Pratt parser; precedence highest → lowest: postfix call and `[unit]` → `^` (right-associative) → unary `- + ! ~ NOT` → `* /` → `+ -` → `< <= > >= LT LTE GT GTE` → `== != EQ NEQ` → `&` → `|` → `&& AND` → `|| OR`. Hence `-2^2 = -4` (mathematical convention; Python/Excel-formula differ and i2 is undocumented). A compat switch `maths.compat.unaryMinusAbovePower` (per project, default off) flips the two rows; the Maths editor hover help states the rule. Tokens: `'Channel Name'[unit]` (`[]`/omitted = as-is; NFKC case-fold), `"text"`, `"Outings:Laps"` range literals, integer vs decimal literals as distinct AST nodes, scientific notation, `//` and `/* */` comments, arity overloading. Channel and function names are case-insensitive; a mismatched-case function name produces a style warning. Bitwise `& | ~` require integer operands; `bit_xor()` for xor. Every ambiguous semantic (window edge rules, `derivative` default, `pulse_width`, `bit_decode`, `integrate_dist` arities, running min/max names, table out-of-range) is listed in `docs/maths-compat.md` with the chosen reading, a `*.case.json` test and, where allowed (D-005), an oracle from the user's i2 exports.

**Consequences.** Users paste i2 expressions unchanged; the only divergence risk is `^`/unary minus and it is switchable.

**Amended by (A.13, proposed):** D-165 (diagnostic codes beyond 03 §2.5, the nesting limit 256).

**User must confirm:** **yes** — verify `-2^2` on the user's i2 install if possible (D-005 (3), D-038 (c)); permitted since the user's yes to Q-03 (3) on 2026-10-10 and scheduled for M2. Until observed the default stands, recorded "chosen, not observed".

---

## D-021 Maths type system and dimension checking

**Context.** Contradiction (solo-dev 15, power-user 10, architect 11): warnings only (core, parity, perf) vs ecosystem-first's "`+ -` require equal dimensions … violations are diagnostics, never silent" that may reject i2-accepted expressions. Ecosystem-first's `LapScalar` lowering to a Hold series answers ungated `stat_*`.

**Decision.** `Type { Kind {Scalar, Series, LapScalar, Bool, Text, RangeRef, Invalid}; UnitId unit; Dimension dim; std::optional<Rate> rate; bool integer; }`. The checker infers bottom-up (`m/s × s → m`; relational/boolean yield `Bool` 0/1 series; `choose` unifies branches; text only in `==`/`!=`; bitwise require integer). **Dimension or unit mismatches are warnings** shown live under the editor and in Channel Status; they never block evaluation (i2 only syntax-checks, and users' existing maths rely on "numbers are numbers once annotated"). The declared result unit is stamped on the output; display unit is separate. `LapScalar` values (ungated `stat_*`, numeric details per range) lower to a `Previous`-interpolated series over the ranges. Plugin functions declare unit rules in their manifest so they participate in inference.

**Consequences.** No rejected i2 expressions; a strict mode (errors) is a later option behind a project flag.

**Amended by (A.13, proposed):** D-164 (bare literals take the dimension they meet; an undeclared result unit is not checked).

**User must confirm:** no.

---

## D-022 Maths evaluation, caching and invalidation

**Context.** Research: block-wise vectorised evaluator over SoA buffers, stateful intrinsics, DAG with version-based invalidation, lazy cached evaluation, thread pool. Contradictions: block size 4,096 (core, perf) vs 64 k (parity, eco; 512 KB `double` operands spill L2); `double` blocks (parity) vs F32 with F64 accumulators; zero-phase IIR cannot run in a one-pass halo scheme (architect on core-first). Parity-first's `SelectionDependent` cache key for ungated `stat_*`/`variance_*`; performance-first's kernel classes; i2 order-of-calculation rules.

**Decision.** The typed AST compiles to a `Plan` of `Kernel`s over **4,096-sample blocks** (`BlockCtx { first, n, t0, rate, ranges, mainDist, refDist, temps }`). Kernel classes: Elementwise (fused subtree, `chunkIndependent`, halo 0), Windowed (`smooth`, `derivative`, Filter item; halo = n/2; odd windows centred, even biased one earlier, seconds → odd count at the result rate), IIR (`filter_lp/hp`, `filter_cheby_*`; forward orders chunk-independent with state carry, negative orders = forward+backward which **forces whole-extent evaluation**), Stateful (`integrate`, `integrate_dist`, `stat_*`, running min/max, `flip_flop`, `edge_delay`, `time_valid`, `time_shift`, `pulse_width`, `previous_sample`; sequential, whole extent, one worker), Range (`range_is`, `range_change`), Variance (D-023), Table (2D/3D, Linear/Next/Previous, out-of-range clamp with an "invalid" option). Precision: F32 buffers by default, F64 for integrators, distance and any F64 input; per-module F64 option (i2's "maths precision"). Inputs are `ResampleView`s at the result rate (explicit Hz or Auto = highest input rate) using each channel's `Interp`. `PlanProvider : IChunkProvider` writes results as chunks into the shared `ChunkCache` (D-011) so maths outputs are plotted, pyramided and evicted like logged data. `MathGraph` (one per `(DataSource, layer)`): node version = hash(definition hash, enabled, upstream versions, source version); cache key `(ChannelKey, version[, selectionKey])` where `selectionKey` is added for `SelectionDependent` nodes (ungated `stat_*`, `variance_*`); editing an item invalidates only its cone. Resolution follows i2: local (sidecar) maths before global, modules in project order, first definition in a module wins, self-maths (Filter/Scale&Offset with output name = input) shadows the channel for consumers but reads the unshadowed input; cycles → status `Error: cycle A → B → A`. Every maths channel starts **Pending** and is evaluated on demand (component, report, Values row, export); Values-window scrolling queues at `Background` priority; the GUI thread never evaluates. Per-module detail-based enable condition (`"condition": "'Vehicle Id' == \"GT3-07\""`) is honoured, including for constants.

**Consequences.** Chunk-granular reuse, L1-resident blocks, correct zero-phase filters, and the Channel Status window has a precise state for every item.

**Amended by (A.9, proposed):** D-139 (0.1 whole-node staging) and D-140 (node versions hash only what a node reads).

**Amended by (A.13, proposed):** D-170 (`Kernel::status` for failures known only at evaluation; `integrate_dist` reads Corr Dist as a plan input) and D-171 (built-in outputs first and transient maths last in a chain, disabled elements skipped, conditions as constant expressions over details, alias fall-through on definition errors, the descriptor unit as stored).

**User must confirm:** no.

---

## D-023 Variance implementation

**Context.** Contradiction (power-user 11): graph feature from per-lap distance tables (perf), plot + analysis (core), graph panel (parity) vs maths-module channel (eco; fights multi-overlay, selection-dependent caching and F3/VariancePos/AutoScale semantics). Ground truth: F3 toggles a cumulative variance trace per overlay vs Main, drawn only when an overlay is selected; `variance_time()/variance_dist()` are maths functions meaningful only within the main selection.

**Decision.** Variance is a **graph panel feature** (`variance { show, heightFrac, autoScale, pos }` on `time_distance`): for each visible overlay *k* vs Main, in distance mode `Δt(d) = tAtD_k(d) − tAtD_main(d)`, in time mode `Δd(t)` from the inverse tables, computed per pixel column from the two laps' `DistanceIndex` tables by `esses_analysis::VarianceTable` — O(pixels), no channel resampling — and drawn in the overlay's key colour. `variance_time(x)`/`variance_dist(x)` maths functions use the same tables via `EvalContext` (Main vs reference overlay per D-017), are `SelectionDependent` (D-022) and invalid outside the Main selection.

**Consequences.** F3 costs nothing when no overlay is selected; multi-overlay variance is natural; reports needing Δ vs Overlay 0 reuse `VarianceTable`.
*Refined by D-194 (M3, proposed): each lap's stretch scales its distances in the panel; columns outside either lap are blank.*

**User must confirm:** no.

---

## D-024 Maths extensibility: modules, C-ABI functions, Lua

**Context.** i2's internal maths (CircuitMaths, GPSMaths) persist as opaque `Params` strings; external functions are COM objects registered in the Windows registry; research recommends transparent built-in modules with documented parameters and a versioned C ABI mirroring `IMtcMathFunction`. Contradiction (solo-dev 18, architect 24): Lua in v1.1 (core) / after v1.0 (parity) / M5 (perf, eco); built-in Lua + Python wheel (eco).

**Decision.** Maths **modules** (= i2 maths files) are JSON (`{id, enabled, condition?, constants[], enumerations[], items[]}`, item kinds `expression | filter | scale_offset | table2d | table3d | builtin | plugin | regression`). Built-in items (`esses.corrected_speed_distance`, `esses.gps_corrections`, `esses.damper_velocity`) have schema'd settings pages (no `Params` strings). External functions implement one interface `IMathFunction { manifest(); evaluate(EvalContext&, args) }`; the **C ABI** `esses_mathfn_v1` (mirrors `IMtcMathFunction`: args, `create_result` pre-sized to the result rate and extent, `mathStartUs/mathEndUs`, `BLOCK_CAPABLE` flag) arrives at v1.0 with the importer ABI (D-028); **Lua** user functions arrive at v1.1 behind the same interface (D-030). Dotted names (`mylib.rolling_rms`) keep i2's `ProgId.name` spelling so imported maths resolve. Multi-channel plugins (`IMtcMathPlugin`) are modelled as modules producing N channels (post-v1.0). The i2 maths XML importer (v1.0) maps `MathExpression/MathFilter/MathTable2D/MathConstant/MathEnumeration` 1:1 and translates known `Params` into module settings, flagging the rest.

**Consequences.** Nothing scripted or compiled is needed for the MVP; the manifest schema is written in M2 so editors can autocomplete plugin functions later.

**Amended by (A.13, proposed):** D-171 (a `builtin` item overrides the registered module's settings for its source; `enabled` may gate optional steps only, as for `esses.gps_corrections`).

**User must confirm:** no.

---

## D-025 Plot engine: backend, IR, layers, budgets, HiDPI

**Context.** Research recommends a custom GPU renderer on `QOpenGLWidget` from day one (M4 + pyramids + FBO cache + QPainter overlay) and leaves "per-widget vs sub-viewport" as an open benchmark. Tech-stack facts: QPainter re-tessellates polylines on the CPU each frame, but M4 bounds a trace to ≈ 4 × 3,840 vertices, so raster is viable for ~20 traces, not for 1,200 polylines at 60 fps. Contradictions (all scorecards 2/3/5): GL first (core M2, perf M1, eco M3) vs QPainter raster first with GL at M4 (parity); one `QOpenGLWidget` per component (core, eco) vs one canvas per worksheet (perf); software rasteriser oracle (core, perf) vs QPainter oracle (parity) vs offscreen GL (eco). Judges: raster first behind a fixed seam (solo-dev, power-user "raster only as fallback" vs solo-dev "raster first") — the solo-dev lens decides on feasibility; the architect wants a Qt-free `DrawList` IR.

**Decision.**
- `esses_plotcore` emits a Qt-free **`DrawList`** (`Cmd {Polyline, Points, Quads, Lines, FilledRect, TextRun, Scissor, Clear}` over interleaved device-pixel vertices, colours as `#rrggbbaa`, text as UTF-8 runs with a style id and anchor) per layer `LayerKind {Static, Dynamic, Chrome}`.
- `IRenderBackend { beginLayer(kind, sizePx, dpr); draw(const DrawList&); endLayer() → LayerHandle; compose(handles…) }`. **`RasterBackend`** (QPainter into `QImage` layers at device pixels) ships first (M1) and is also the print/clipboard/golden path. Static = axes, grid, bands, traces (redrawn on zoom/pan/data/scale/size); Dynamic = cursor, datum, measurement hover line, key values, drag feedback (redrawn on cursor/datum); Chrome = caption, focus border.
- **`GlBackend`** is built only when a measured budget fails on the user's hardware or the v1.0 workloads (RW-4: 10 laps × 12 channels × 10 components) demand it; granularity (one GL canvas per worksheet with viewports vs per-component `QOpenGLWidget`) is **decided by a benchmark in that milestone**, not by design; the `DrawList` IR supports both. `RhiBackend` is a later option behind the same seam.
- Decimation per D-012; `TraceBuilder` runs on the `TaskPool` per component (parallel per trace) and hands the `DrawList` to the GUI thread for painting.
- **MVP budgets** (parity-first, gated from M1 as absolute limits on one reference machine, D-042): cursor move ≤ 4 ms (Dynamic only), zoom/pan ≤ 16 ms for 20 traces × 3,800 px, full relayout ≤ 50 ms, resize coalesced to 30 Hz, first paint of a 100 MB `.ld` ≤ 500 ms. **v1.0 budgets** (performance-first §1.3 subset, nightly): RW-4 cursor ≤ 2 ms CPU, zoom rebuild ≤ 33 ms hard limit, Values refresh 250 channels ≤ 1 ms, 1 GB `.ld` cold → first paint ≤ 3 s, RW-2 resident ≤ 1 GB.
- HiDPI: layout in logical px, rendering at device pixels, line widths × DPR, fonts via `QFontMetricsF`, per-monitor DPR change relayouts; goldens at DPR 1 and 2.

**Consequences.** The first months go to behaviour; the renderer seam and layer cache from day one mean GL is an additive backend, not a rewrite. Golden images are deterministic across machines (fonts pinned by shipping the scheme's default font files for tests).

**Amended by (A.9, proposed):** D-137 (only the Static layer is a cached image).

**User must confirm:** no.

---

## D-026 Docking library

**Context.** Research: Qt Advanced Docking System 4.x/5.0 (LGPL-2.1; auto-hide, perspectives, floating; Windows primary, macOS compiles but untested by maintainers); KDDockWidgets is GPL (excluded under MIT); plain `QDockWidget` lacks perspectives and auto-hide. i2 dock windows: Docked / Pop-up / Fly-out with pin, ESC returns focus, Reset Dock Windows.

**Decision.** ADS as a **shared library** (LGPL compliance) via vcpkg or FetchContent, wrapped by a `DockHost` facade (`addDock(id, widget, area)`, `savePerspective()/restorePerspective()`, `setAutoHide(id)`, `resetToDefault()`). Fallback `QDockWidget` implementation of the same facade for macOS if ADS proves unusable. Fly-out = ADS auto-hide; pop-up = floating.

**Consequences.** Dock state persists per user (D-019); the facade keeps the ADS API out of components.

**Amended by (A.12):** D-153 (decided 2026-10-11: ADS 5.1.1 is vendored and the default backend, as a shared library linked by `esses_widgets` only; the `QDockWidget` backend is kept as a run-time and build-time fallback).

**User must confirm:** no.

---

## D-027 Component set, MVP, and the properties UI

**Context.** Feature inventory priorities (core/important/nice/skip); power-user wants Lap Editor, Details editor, Time Report and i2 workbook import inside the MVP; solo-dev wants i2 file importers, Time Report/sections and the indexed Open dialog moved to v1.0. Properties UI contradiction (solo-dev 12, power-user 14, architect 16): dock (core) vs modeless tabbed dialog with Apply (parity, perf) vs schema-generated grid (eco).

**Decision.** Components by phase (JSON `type` names):
- **MVP (0.1):** `time_distance` (full parity-first §5.4: caption, range/section/marker bands, groups/panels/overlapped with Lo/Hi handles, like-units grouping with CTRL, Manual vs Channel/Auto scale with autoscale options, scale lines, key column with value@main/value@datum/Δ, measurements M, cursor/datum/zoom/pan semantics, offset axis O, variance F3, status & errors strip E, device events SHIFT+E, annotations, SHIFT+F7, CTRL+SHIFT+C, Export CSV), `outing`, `scatter` (points / points+trend / trend; linear and polynomial 2–6 trend, through-zero, equation text; overlapped/tiled/average; interval average; gating; colour channel; click → nearest Main point), `histogram` (BandAxis bins width/count, count/percent/time, bar/line/text, overlapped/tiled, gating, colour channel, exclude untrusted), `channel_report` (`ReportSpec`: scope, ordered heading hierarchy, stats avg/min/max/absMax/start/end/change/stdDev/delta, 5 s probe for start/end, min/max highlight, Δ vs Overlay 0, transposed layout, CSV/TSV export), `time_report` (sections of a range group, eclectic column, rolling minimum row, % bands ALT+n), `track_position` (map, Main red dot + overlay dots, zoom span highlight, section labels, send-to-background), `values_list`, `gauge_numeric`, `gauge_bar` (colour bands ALT+0..9). Docks: Data (F8), Channels (C), Values (V), Track Map, Channel Status (CTRL+U), Messages. Editors/dialogs: Channel Editor (CTRL+E), Maths Editor (CTRL+M), Lap Editor, Details editor (CTRL+D), Track Editor (Speed+LatG and GPS generation, default Turns/Straights with 10 m minimum, user sections with categories), Corrected Speed/Distance, Options (General/Analysis/Colours/Fonts/Keys), Mapping panel, Layout Editor basics (CTRL+F7), zoom summary bar, animation (A, basic).
- **v1.0:** `spectrum` (FFT, pocketfft, Bartlett averaging), `suspension_histogram`, `mixture_map`, `track_report`, `rainbow_track`, `gps_track` (path, fit modes, heading arrows), `gauge_dial`, `gauge_light`, `gauge_wheel`, `notes`, `picture`; Corrected GPS dialog; `.ld/.ldx` and `.mat` export; KML export; 3D tables; regression item; four-pane indexed Open dialog; i2 workbook/workspace/scheme/maths importers; Drag/Rally rules and editors; print.
- **Later:** `video`, `colour_strip`; telemetry/WebView/Race Control are skipped.

**Properties UI:** modeless tabbed `QDialog` per component (F5, CTRL+N for a channel) with Apply and full undo (D-032), tabs hand-built to i2's vocabulary (Channels / Display / Colour Channel / Data / Colour Bands / Report / Load Channel / Velocity Calculation as applicable). Schema-generated property grids are reserved for third-party plugin components (post-v1.0). All components share: focus border, TAB/SHIFT+TAB, context menu mirroring the Component menu, active channel square (PgUp/PgDn, CTRL+H, CTRL+DELETE, CTRL+SHIFT+M/S/F/O/Z), L/K legends, G layout cycle, S style cycle, ALT+0..9 colour bands, Export Data (CSV always), Print to Clipboard, cut/copy/paste/delete with domain preserved, maximise, CTRL+click multi-select, 1 % snap grid.

**Consequences.** Ten components, four docks and seven dialogs define the MVP surface; D-035 sizes it.

**Amended by (A.9, proposed):** D-136 (optional schema-generated property grids; default not adopted).
*Refined by D-197 (M3, proposed): the channel and time report statistics, headings, Overlay 0 delta and cache key as built.*
*Refined by D-198 (M3, proposed): the scatter, histogram, outing and gauge components as built (gauge layout, the numeric gauge's dynamic font, the scatter's own zoom, Export Data, ALT+n on BandAxis components).*
*Refined by D-199 (M3, proposed): the channel and time report, track position, Track Map dock and Track Editor as built (report styling roles, TrackMapService, the track view as body edits).*

**User must confirm:** no (the MVP cut is confirmed under D-035).

---

## D-028 Importer interface, plugin C ABI and the converter harvest

**Context.** Research: sniff → probe → lazy-read interface with companion discovery; in-process versioned C ABI (function-pointer table, no C++/Qt types, JSON manifest), Qt plugins only for UI. The user's ~24 k-line Qt C++ converter (MDF4/BLF/XRK/RPK/Haltech/Link/Emtron/Syvecs/MaxxECU/IBT/PDS) is harvested *after* the analysis app works. Contradictions (solo-dev 9, power-user 5, architect 17): built-ins through the vtable from M0 (eco) / C ABI with zero-copy `raw_block` from M0 (perf) / C ABI after v1.0 (core) / seam designed now, proven at v1.0 by moving CSV out of tree (parity); shape: sink callbacks (core) vs JSON probe + sink (parity) vs `raw_block` + `read_block` (perf) vs `struct_size` structs, host services, JSON metadata, host-owned blocks (eco); per-channel random access is awkward for message-stream formats (architect on eco). Solo-dev verdict: in-tree C++ `IImporter` now, C ABI when the harvest starts, JSON for metadata.

**Decision.**
- **Now (M0):** in-tree C++ `IImporter { id(); extensions(); sniff(path, head64k) → SniffResult{confidence, companions, reason}; probe(ByteSource&) → ProbeResult; open(ByteSource&, ImportOptions) → unique_ptr<IReader> }`, `IReader { readBlock(layer, channel, first, count, Block& out, CancelToken) ; readMany(layer, channels[], first, count, BlockSink&, Progress, CancelToken) /*sweep for message-stream formats*/; readTimestamps(...); markers(layer); enumTable(id); warnings() }`, `IWriter`, `ImporterRegistry`. `ProbeResult` serialises to **`probe_json`** (schema `probe-1`), which is also the fixture expectation and the future catalogue row (ecosystem-first graft). Built-ins (`motec.ld`, `motec.csv`, `text.profile:*`) implement `IImporter` directly (`motec.ld` in M0, the text built-ins in M2 per D-044); the `.ld` reader uses mmap and decodes per chunk internally (no `raw_block` in the ABI).
- **Designed now, frozen at v1.0:** C ABI header `sdk/esses_abi.h` as a 1:1 mirror with ecosystem-first's rules: C11, `struct_size` first in every struct, append-only fields, `esses_plugin_entry_v1` coexisting with a future `_v2`, UTF-8 `{ptr,len}` strings, all plugin-produced strings via `host->alloc`, host-owned sample blocks, cancel/progress callbacks, host services (`log`, `resolve_unit`, `read_companion`), JSON only for metadata (`probe`, `markers`, `warnings`, `enum_table`), `plugin.json` manifest (id, version, licence, binaries per platform, formats, math functions, trust). A generated adapter wraps `IImporter` ↔ vtable in both directions. The ABI is **proven at v1.0** by moving the CSV importer out of tree and loading one example plugin built with Clang/MinGW (and Rust if cheap) into the MSVC host; the `abi-compat` CI job starts then, not at M0.
- **Converter harvest (after v1.0, outline M6): wrap then port.** Step 1 (days per format): each converter parser becomes a C-ABI plugin in `plugins/formats/<vendor>/` that privately links QtCore (allowed outside the Qt-free rule). Step 2 (as value dictates: XRK, MDF4+DBC, IBT, PDS first): port into `esses_formats` replacing Qt types; accepted only with a hash-pinned fixture and an oracle diff (bit-exact integers, ≤ 1 ulp floats) against the converter's output. CSV-type parsers (Haltech, Link, MaxxECU, Syvecs, …) become **text profiles** (D-029), not code.
- Unit/alias mapping: `UnitMappingProfile` per importer (`channels/unitmaps/<importer>.json`, rules `nameOnly | unitOnly | nameAndUnit`), applied silently when complete, else the inline Mapping panel (Scan / Resolve / Save) before the file opens; dictionaries per D-013.

**Consequences.** Nothing ABI-related blocks the MVP; the harvest cannot drag Qt into the core (plugins are outside the rule, ports are gated by the qt-free check); the user's converter stays buildable from its own tree during step 1.

**Amended by (A.9, proposed):** D-131 (one header, 1:1 status, bitmap validity, per-call context, `qt` modes, disable-never-unload).

**Amended by (A.10):** D-142 (the `.ld` reader and the `.ld` part of the fixture writer are ported from the user's converter in M0, ahead of the harvest; the `.ldx` code is new) and D-144 (until the user confirms the converter's licence and lineage, nothing is ported verbatim: the reader and writer are new code written from the converter's port specification and the public notes).

**Amended by (A.13, proposed):** D-179 (the Mapping panel's queue, Save and unit-map rows) and D-168 (unit-map rules before the registry, "unresolved" = an unknown unit or a quantity without a unit, the hand-off through `resume`, timestamped channels and the reader hooks of step 4).

**User must confirm:** no (converter licence/provenance is an open question, Q-15; since D-144 it also gates any verbatim reuse of the converter's `.ld` code in M0).

---

## D-029 Text engine, profiles and dictionaries

**Context.** Research: a generic CSV/TSV engine with declarative profiles (encoding, delimiter, decimal comma, preamble regexes, header/units rows, `Name (unit)`/`Name[unit]`, time column, sentinels, duplicate-name policy, sparse cells → timestamped) makes dozens of ECU/app dialects data. Contradiction (solo-dev 17, architect 26): `fast-cpp-csv-parser` (core, parity, eco) vs own column-oriented SIMD engine with DCF (perf). MoTeC CSV dialect: 11 quoted metadata lines, blank rows, names row, units row, single rate; Pi versioned ASCII has per-channel blocks.

**Decision.** Own **light** two-pass text engine (pass 1: line index + preamble/header/units detection against the profile; pass 2: per-column decode of 65,536-row slabs into chunks) using `fast_float` for number conversion, no SIMD, no DCF in the MVP (added only on a measured complaint, D-011). Header-only CSV libraries are not used because preambles, per-channel blocks (Pi), sparse cells and ragged rows are outside their model. Profiles are JSON (`schemas/importer-profile-1`): `sniff {preambleRegex, extensions, confidence}`, `encoding`, `delimiter`, `decimal`, `quote`, `preamble {lines, capture → details}`, `header {namesRow, unitsRow, unitInName}`, `time {column, unit}`, `sentinels`, `sparse: timestamped | hold`, `duplicateNames`, `columns {vendor → {name, quantity, dtype, interp}}`, `lapSource {column, kind: lap_counter_edge | beacon_event | lap_table}` (ecosystem-first graft). Shipped in MVP: `motec.csv` (details from the preamble, lap times to markers), `generic.csv`. v1.0: Pi, EFI, RacePak ASCII, VBO, TrackAddict, RaceChrono, RaceBox, MSL, RaceCapture, Haltech/Link/MaxxECU/Syvecs exports (each = profile + fixture).

**Consequences.** Adding a CSV dialect is a data PR with one fixture; the engine is ~1,500 lines; the chunk provider path means a later DCF cache needs no importer change.

**Amended by (A.13, proposed):** D-167 (the text engine's rules as built: `@auto` names row, decimal comma, checked rate candidates, column sampling, portable multi-line sniff, `capture` direction, the slab decoder with `WeakColumn`, `blocks` deferred) and D-168 (step 4 and the Mapping-panel hand-off as built; shipped profiles registered by `withBuiltins`).

**User must confirm:** no.

---

## D-030 Scripting and automation

**Context.** Research: Lua 5.4 + sol2 (MIT) in-process sandboxable scripting for rules, user functions, report templates, importers; optional pybind11 CPython host (no sandbox); JSON-RPC to replace COM `i2Application`. Judges: ecosystem-first's SDK surface is "a second product" for a solo developer; Lua at v1.1 (core, parity) is the consensus timing; the Python wheel only pays off with contributors.

**Decision.** **No scripting before v1.0.** v1.1: `esses_script` — Lua 5.4 + sol2 with ecosystem-first's sandbox design (restricted `_ENV`, no `io/os/debug`, no binary chunks, instruction-count hook, file access only through a host reader object, scripts off the UI thread) exposing one object model (`App → DataSources → DataSource → Layer → Channels / RangeGroups / MarkerGroups; Channel → Descriptor + DataArray(samples, validity, rate, t0); Details; Units`) for user maths functions (via `IMathFunction`), lap rules (via `IRangeRule`), scripted importers (via `IImporter`) and report templates. Python: `esses_capi` (stable C surface over the Qt-free libs) and a pybind11 wheel only when a contributor or a concrete user need appears (post-v1.1). JSON-RPC automation server after v1.1. From M1 a single `CommandRegistry` (command ids drive menus, `keymap.json`, the command palette CTRL+SHIFT+P and later scripting).

**Consequences.** The hooks exist (`IMathFunction`, `IRangeRule`, `IImporter`, `CommandRegistry`); the sandbox is built once, later.

**User must confirm:** no.

---

## D-031 Threading and ownership model

**Context.** Contradiction (solo-dev 16, architect 20): injected `Executor` + GUI-thread bridge per object (core) vs `std::jthread` pool inside maths with a Qt facade (parity) vs one global priority `TaskSystem`, no QtConcurrent (perf) vs work-stealing pool in host (eco). Solo-dev: the per-object bridge is "a classic source of threading bugs"; architect: trim performance-first's M0 to a std::thread pool.

**Decision.** One Qt-free **`TaskPool`** in `esses_base` (`std::jthread` workers = hardware threads − 1, minimum 2; three priority bands `Priority {Interactive, Visible, Background}`; `CancelToken`; request coalescing by key; `Future<T>` with shared results so a channel requested by ten components evaluates once). No `QtConcurrent`, no `QThreadPool`. The **GUI thread** owns widgets, `QImage` layers, every QObject in `esses_qtmodel`, the project session and the undo stack, and performs all model mutations. **Workers** decode chunks, build pyramids and `DistanceIndex`es, run `TraceBuilder`, evaluate maths plans, parse text, compute report cells and summary statistics; they operate only on immutable snapshots: `std::shared_ptr<const DataSource>` views, `ChunkHandle`-pinned chunks, copied `ViewState`/`SelectionSnapshot`. Results reach the GUI through one **`Dispatcher::post(std::function<void()>)`** (a single QObject with a queued signal), never through per-object adapters. The GUI never blocks on a future: components draw a Pending state and repaint on completion. Lap rebuilds after marker edits are synchronous on the GUI thread (O(markers)). Sanitiser (TSan) runs on `esses_maths` and `esses_plotcore` tests in CI.

**Consequences.** One pool, one bridge, one ownership rule ("workers see only immutable snapshots"); the CLI uses the same pool directly.

**Amended by (A.9, proposed):** D-123 (`std::thread` workers, `Dispatcher::post(context, fn)`, `AppRuntime`, Rule W, `ThreadRole`, two-phase lap rebuild).

**User must confirm:** no.

---

## D-032 Undo/redo

**Context.** i2 has no undo. Contradiction (architect 5): core `UndoStack` + `QUndoStack` facade (core) vs `QUndoStack` directly (parity) vs toolkit-free command model (perf, eco). Qt Core is allowed in `esses_qtmodel` (D-007).

**Decision.** One `QUndoStack` per `ProjectSession` (CTRL+Z / CTRL+Y, Edit menu). Undoable: component add/remove/move/resize/property edits, worksheet/workbook CRUD, channel overrides/aliases/mappings, maths items/modules, details edits, marker inserts and `MarkerEdit` ops, range overrides (trust, offsets), track map section edits, scheme edits. **Not undoable** (view state, as in i2): data selection, cursor, datum, zoom, domain, dock layout, file open/close. Commands wrap the Qt-free document types in `esses_project`/`esses_model`; no second undo abstraction.

**Consequences.** Modeless property dialogs (D-027) are safe to use freely; the CLI needs no undo.

**Amended by (A.13, proposed):** D-161 (`QUndoStack` is a Qt Gui class in Qt 6: `esses_qtmodel` links Qt Gui for the undo framework only).
**Amended by (A.13, proposed):** D-172 (whole-document commands; undone options edits keep the view state; the F9 domain stays view state over 02 §7; workbook-list and Mapping-panel edits; dirty state, save and autosave rules).
**Amended by (A.13, proposed):** D-178 (Edit > Undo/Redo are the registry commands `edit.undo`/`edit.redo`, not `QUndoStack::createUndoAction`; component edits from the canvas are `WorkbookCommand`s; text-editing chords in a text field are the field's own).

**User must confirm:** no.

---

## D-033 Testing strategy and fixtures

**Context.** Landscape: fixtures matter more than feature velocity; every project re-reverse-engineers the same formats. Contradictions (architect 19): golden oracle = software rasteriser (core, perf) vs QPainter raster (parity) vs offscreen GL (eco). Ecosystem-first's fixture workflow and parity-first's "one Qt Test per hotkey row" were named by all judges.

**Decision.**
- **Unit:** Catch2 v3 per Qt-free lib (80 % line coverage target on store/model/formats/maths/analysis/plotcore); property tests (units round-trip and delta/absolute separation; resampler boundaries and monotonicity; pyramid vs brute force on NaN-sprinkled data; M4 vs per-column reference; `DistanceIndex` round trips; parser arities/aliases/window rules; DAG shadowing/cycles/versions).
- **Golden JSON:** `esses-cli info/channels/laps --json` outputs vs committed expectations.
- **Fixtures** (ecosystem-first layout): `fixtures/<formatId>/<name>/{input.<ext> | fixture.json remote ref, fixture.json (sha256, bytes, source, licence, oracle, notes), expected.probe.json, expected.markers.json, expected.channels/<channel>.csv.gz, README.md}`; `esses-cli fixture add | verify | diff`; synthetic `.ld/.ldx` families generated by our own writer (every dtype pair, multi-rate, shift/mul variants, 212-byte M1 headers, truncated names, sim-exporter dialects) whose expected values come from an oracle independent of that writer (D-041); third-party sim exports in-tree only where their licence permits redistribution (sim-to-motec has none: private only), the sim dialects being covered publicly by the synthetic families (D-040); the user's real logger files, workbooks and i2 exports in a **private submodule** (D-005); large files by hash manifest; corpus later moved to its own CC0 repo.
- **Maths:** `tests/maths/*.case.json` (expression + inputs + expected) hand-derived from help semantics; the public parser corpus is the project's own default module, hand-written i2-syntax cases and fuzz-generated expressions; ACC workspace and vendor template maths live only in the private submodule and their public goldens record counts only, never names or expressions (D-040); oracle cases from i2 exports of MoTeC-device logs, permitted by the user on 2026-10-10 (Q-03 (1); D-005, D-038).
- **Golden images:** every component rendered offscreen through `RasterBackend` at DPR 1 and 2 with pinned fonts; perceptual diff ≤ 0.5 % pixels; when `GlBackend` exists it is compared to the raster goldens at ≤ 2 %.
- **Behaviour:** Qt Test (`QT_QPA_PLATFORM=offscreen`) with **one test per hotkey row** of the keymap (send key, assert state), plus selection model, F9 preservation, like-units grouping, N/P zoom preservation, CTRL+F rules, workbook round-trip byte-identical.
- **Fuzz:** libFuzzer on `.ld` header/channel list, `.ldx`, CSV sniffer/profile engine, expression parser; 60 s per PR, 30 min nightly.
- **Benchmarks:** Google Benchmark with JSON baselines; MVP budgets (D-025) gate from M1 and v1.0 budgets nightly, both as absolute limits (median of ≥ 5 runs) on one reference machine; hosted runners give a non-blocking ±30 % regression alarm (amended by D-042).

**Consequences.** Parity is testable, not aspirational; readers are contributable as data + fixture.

**Amended by (A.9, proposed):** D-127 (Linux-only pixel goldens, Noto Sans test font, private game-generated fixtures, `fixture-1` in 05 §9.1, `expected.laps.json`).

**Amended by (A.11):** D-148 (Catch2 3.7.1 amalgamated and the project's own test discovery, which fails the build on a test name that a Catch2 test spec cannot select exactly) and D-152 (`esses-cli fixture add` requires `--licence` for a public fixture and defaults `oracle.validity` to `finite-only` for the reference decoder).

**Amended by (A.13, proposed):** D-170 (the maths case corpus `tests/maths/cases/`, schema `maths-case-1`, expected values from `tools/maths_ref.py`).

**User must confirm:** **confirmed by the user on 2026-10-10:** no real file is public for now, every real file stays in the private submodule and the synthetic families stay public (Q-04, to be revisited before the first public release); i2 exports of the user's MoTeC-device logs may serve as private oracles (Q-03 (1), D-005).

---

## D-034 Packaging, build and CI

**Context.** Research: CMake presets + Ninja + vcpkg manifest, Qt via aqtinstall/install-qt-action, MSVC on Windows (MinGW ABI mismatch), Catch2/Qt Test/Google Benchmark, windeployqt / macdeployqt + DMG / linuxdeploy AppImage, signing budgeted; Ninja not on PATH today.

**Decision.** CMake ≥ 3.28 presets (`win-msvc-{debug,release}`, `linux-gcc`, `linux-clang-asan`, `mac-clang`, each with `ESSES_BUILD_QT=ON|OFF`), Ninja, C++20, `/W4 /permissive-` and `-Wall -Wextra -Werror` in CI, clang-format and clang-tidy on the Qt-free libs. vcpkg manifest: nlohmann-json, pugixml, zlib, fast_float, xxhash, catch2, benchmark, cli11, eigen3; vendored header-only: pocketfft (BSD-3), Iir1 (MIT), json-schema-validator (MIT); ADS as a shared lib. GitHub Actions: `core` (ubuntu/windows/macos, no Qt, unit + golden JSON, ~4 min), `qt-app` (windows-2022 MSVC + Qt 6.12, ubuntu-24.04 GCC, macos-14; Qt Test offscreen + goldens), `sanitize` (clang ASan/UBSan; TSan on maths/plotcore), `fuzz-smoke`, `bench` (nightly), `lint` (clang-format, clang-tidy, `reuse lint`, `qt-free-check`, vendor-mark grep, schema validation of every JSON in `data/` and `fixtures/`), `package` on tags (windeployqt + Inno Setup installer + portable zip with `.ld` association and `-w <project> -l <log>` args; macdeployqt + DMG; linuxdeploy AppImage; `NOTICES.md`, Qt source offer, third-party licence manifest generated at build). The Qt-free CLI ships inside each package and as a standalone download. Code signing (Authenticode/notarisation) is gated on secrets and budget.

**Consequences.** Linux/macOS "build green" from M0 so they never become "later"; releases on 6.12 even while the developer stays on 6.7.2.

**Amended by (A.9, proposed):** D-124 (compiled Iir1 and json-schema-validator, triplet, vcpkg files cache, required checks, best-effort DMG/AppImage).

**Amended by (A.11):** D-147 (every library dependency is vendored under `third_party/`; no vcpkg manifest, triplet or vcpkg cache) and D-150 (the `qt-app` job starts in M1 with the first Qt target).

**User must confirm:** no (signing budget is an open question).

---

## D-035 MVP scope, sequencing and timing

**Context.** User priorities: analysis application first, formats later; MVP must read `.ld/.ldx` and CSV. Judges: parity-first's MVP is the broadest and ~1.5× optimistic (25 weeks → 35–40); solo-dev moves i2 file importers, Time Report/sections and the indexed Open dialog to v1.0; power-user keeps Lap Editor, Details editor, Time Report and workbook import in the MVP; architect keeps the walking skeleton at M1. Public-beta timing: eco at M5 before the format wave vs after v1.0 (others).

**Decision.** MVP = release **0.1** = parity-first's MVP **minus**: `.i2wkb/.i2wsp/.mcs/.mfs`/maths-XML importers (→ first v1.0 item; the `x-i2` schema slot is reserved in M2), the index database and four-pane Open dialog (→ v1.0), the GL backend (→ v1.0, and only if budgets fail), `.ld/.ldx` writer as a user feature (the writer exists in M0 for fixtures; the export UI is v1.0). **Kept in the MVP** (inventory "core"): Lap Editor, Details editor, Time Report with default/user sections, Track Editor with both generation methods, status strip, device events, annotations, animation (basic). Milestones (outline §7): M0 Foundations 4 wks · M1 Walking skeleton 7 wks · M2 Channels, maths, persistence 7 wks · M3 The 80 % components 8 wks · M4 Hardening and 0.1 4 wks → **≈ 30 calendar weeks** at ~25 focused h/week with AI assistance (parity-first's 25 × 1.2 after the cuts). **Re-estimated by D-045 (review, 2026-10-10):** M0 4 · M1 8 · M2 9 · M3 11 · M4 4 weeks = 900 h with tests at 30 %, plus a 15 % contingency line → **≈ 42 calendar weeks** to 0.1 (≈ 39–40 with the optional D-046 cut list); M5 13 weeks (it gains eviction, D-043). Then M5 v1.0 parity (≈ 12 wks), M6 Formats + SDK (≈ 8 wks: ABI freeze, converter harvest, fixtures repo), M7 v1.1 (≈ 8 wks: Lua, video, RPC). 0.1 is a public alpha tag; the public beta announcement is v1.0.

**Consequences.** A race engineer can replace i2 for a Saturday at 0.1 but must rebuild worksheets (a default circuit workbook ships); migration of existing i2 workspaces arrives with v1.0.

**User must confirm:** **confirmed by the user on 2026-10-10 (Q-05): the scope cut stands.** The i2 workbook/workspace/scheme/maths importers are v1.0 items; the scope cut (workbook import to v1.0) was the one place the two user-facing judges disagreed.

---

## D-036 Profiles

**Context.** i2 profiles (Circuit, Drag, Rally, Engine) switch terminology, range type, default maths and components; the research recommends profiles as data. Parity-first and performance-first ship Circuit first.

**Decision.** `data/profiles/<id>.profile.json` (schema `profile-1`): range kinds, rules, data views (`Laps | Outings | Runs | Stages | Data`), default maths modules, default workbook, terminology strings. **Circuit** ships in the MVP; the data model carries `RangeKind::Run/Stage` and marker classes `STGSTRT/STGEND` from M0; Drag (runs, splits, split stretching, Run Editor) and Rally (stages, whole-file tracks regenerated per load, Stage Editor) rules and editors are v1.0; Engine is a reduced component set (later).

**Consequences.** Switching profile is a project option, not a new project.

**User must confirm:** no.

---

## Open questions carried to the user (consolidated)

Rewritten on 2026-10-10 after the review. The list is deduplicated across this document, Addendum A, [`WRITER_UNRESOLVED.md`](background/design-panel/WRITER_UNRESOLVED.md) and the discrepancy sections of documents 00–08, and ordered by when the answer is needed. **Blocking** rows have no default the work may proceed on; every other row proceeds on its recommended default until the user answers. Since 2026-10-10 no row is blocking: Q-01 to Q-05 are answered, so nothing blocks the first push or the first commit of `CLEANROOM.md`. "07" is the matching row of 07_ROADMAP.md §14; "Was" is the number in the previous version of this list, which earlier drafts of 00_OVERVIEW.md §7.2 cited (it now cites the Q ids). Q-17 and Q-18 come from Addendum A.9. `OPEN_QUESTIONS.md` restates every row with the consequence of choosing otherwise and adds the design follow-ups of the final revision.

| Q | Needed by | Question | ADRs | Recommended default | 07 | Was |
|---|---|---|---|---|---|---|
| Q-01 | **Answered 2026-10-10** | Licence: MIT, Apache-2.0 or GPLv3-app/MIT-libs? | D-002 | Chosen: MIT whole tree, DCO, no CLA, no-relicensing policy | U-01 | 1 |
| Q-02 | **Answered 2026-10-10**: Esses is the working name; no trademark search is needed for now (the name may change later) | Confirm the codename "Esses". | D-003 | Chosen: Esses; no trademark search scheduled | U-02 | 2 |
| Q-03 | **Answered 2026-10-10**: yes to (1), (2) and (3); the ACC sub-question was answered the same day: ACC is not MoTeC-licensed | The three D-005 answers under the EULA field-of-use rule: (1) i2 CSV/report exports of the user's MoTeC-device logs as private oracles; (2) parsing the user's i2-written files, which the research already did on 2026-10-09; (3) observing i2's behaviour, which also needs the user's own legal check that black-box observation is not "reverse engineering" under the EULA. Also: is any non-MoTeC source the user analyses in i2 (e.g. ACC) MoTeC-licensed? | D-005, D-038, D-039 | Chosen: (1) yes, MoTeC-device logs only, private submodule; (2) yes, the 2026-10-09 research use stands, recorded with its date, user-file content never public; (3) yes, black-box observation on the user's install only, never disassembly, the legal check being the user's own responsibility; ACC is not MoTeC-licensed (answered 2026-10-10), so i2 is never run on ACC data and ACC exports are ordinary third-party fixtures, private for now | U-03 | 3 |
| Q-04 | **Answered 2026-10-10**; revisit before the first public release | Which of the user's files (logs, ACC exports, workbooks, schemes, i2 exports) may be committed publicly, and which go to the private submodule? | D-033, D-040 | Chosen: nothing real in public for now; synthetic families public; every real file in the private submodule | U-04 | 4 |
| Q-05 | **Answered 2026-10-10** | Accept the MVP scope cut (i2 workbook/workspace/scheme/maths import at v1.0) and the re-baselined plan (0.1 ≈ 42 weeks including 15 % contingency, tests 30 % of each milestone)? | D-035, D-045 | Chosen: accept both | U-07 | 7 |
| Q-06 | **Answered 2026-10-10**: spike S-1 decoded 87 of the user's `.ld` files, two of them MoTeC-logger files with `shift ≠ 0 ∧ mul ≠ 1` (`docs/spikes/S-1.md`); the sample-file part was answered the same day | Paths of the user's real `.ld` files for spike S-1, including one from a MoTeC logger with `shift ≠ 0 ∧ mul ≠ 1`; may the i2 install's sample files be used locally as oracles (never committed)? | D-037, D-117, D-146 | Chosen: `ld.shiftOrder = afterMul`, settled by physical evidence on real logger data (D-037 as amended; a vendor-software CSV export of `20251009-1552000.ld` would add confirmation); chosen for the samples: local/private testing allowed, never committed or redistributed, the vendor T2 test models included (D-146) | U-05, U-13 | 5 (part) |
| Q-07 | M1 start | Reference machine for the budget gates, and the minimum hardware/OS. | D-025, D-042 | The developer's machine as a self-hosted runner; Windows 10 22H2 and later | U-09 | 9 (part) |
| Q-08 | M1–M3; permitted since Q-03 (3) was answered yes on 2026-10-10 (scheduled in `OPEN_QUESTIONS.md` section B) | i2 observations: overlay 1..n colours and renumbering; `-2^2`; Values-window content; default lap-stretch tolerance; out/in/partial lap numbering; whether `Start Lap` numbers the first flying lap or the out lap; one selection slot per range; the "fast laps" definition; whether cursor and datum persist across worksheet switches. | D-016, D-017, D-020, D-047, D-076, D-100, D-109, D-110, D-132, D-133 | The documented defaults, recorded "chosen, not observed" until each observation runs | U-05, U-14 | 5 |
| Q-09 | M2 start | Default maths precision. | D-022 | F32 buffers with F64 accumulators; F64 for integrators, distance and F64 inputs | U-10 | 10 |
| Q-10 | M3 start | Adopt the optional 0.1 cut list up front? | D-046 | No; the list becomes M3's ordered may-slip list | U-15 | new |
| Q-11 | M4 start | Eviction and the stress fixture at M5, with 0.1 only warning about files above the memory budget? | D-043 | Yes, M5 | U-16 | new |
| Q-12 | M4 start | Linux and macOS at 0.1: supported release targets or "builds green" artefacts? | D-034, D-116 | Artefacts published, labelled "builds green, not supported" | U-09 | 9 (part) |
| Q-13 | M0 week 1 | Install Qt 6.12 locally now, or stay on 6.7.2 until M4? | D-006, D-126 | Install 6.12 in M0 week 1 beside 6.7.2 (D-126) | U-08 | 8 |
| Q-14 | M4 start | Code-signing budget (Authenticode, Apple notarisation). | D-034 | Unsigned 0.1 with documented SmartScreen/Gatekeeper steps | U-11 | 11 |
| Q-15 | M6 start; any time for verbatim reuse of the `.ld` code (D-144) | Converter: is its licence MIT-compatible, which parsers are the user's own work (no GPL or vendor-SDK lineage), and may it be built as a plugin DLL from its own tree? | D-028, D-107, D-142, D-144 | Harvest waits for a yes; CSV-type parsers become text profiles regardless; until the user confirms that the converter's `.ld` code is their own MIT work with no GPL lineage, the M0 `.ld` reader and writer are new code written from the port specification and nothing is ported verbatim (D-144). *2026-10-11:* the user declared the converter's Python version deprecated in favour of the Qt C++ version, which holds the conversion mappings from specific formats to `.csv` or `.ld`; that tree and its per-format mapping files are the reference for the later input-format phase and the Python version is not used (note on D-144). Authorship and licence are still unconfirmed, so the question stays open | U-06 | 6 |
| Q-16 | M7 start | Python wheel before v1.1, or the effort to formats? | D-030 | Formats first | U-12 | 12 |
| Q-17 | M2 start (first `workbook-1` written) | Rename the D-019 round-trip bag `x-i2` to a neutral `x-import` + `format` key? | D-125 | No; `x-i2` stays an allow-listed class | — | new |
| Q-18 | M3 start | Generate 0.1 property pages from the body schemas instead of i2-style tabs (reverses D-027)? | D-136 | No; hand-laid-out pages through `FieldBinder` | — | new |

---

## Addendum A — decisions proposed during documentation

**How this addendum was produced.** Sources: the 97 items of [`WRITER_UNRESOLVED.md`](background/design-panel/WRITER_UNRESOLVED.md) (cited WU-n, the item number in that file); the closing discrepancy or open-item sections of 01 (§12), 03 (§16), 04 (§20), 05 (§15), 06 (§24), 07 (§15) and 08 (§13) (00 and 02 have none of their own; 02's items are WU-17…34); and the 2026-10-10 review of 07_ROADMAP.md, this document and the outline (cited "review"). Section A.9 (D-122–D-141) records the choices of the final cross-document revision (cited "revision"). Section A.10 (D-142 to D-146) records implementation decisions taken once M0 started: from the user's instructions (cited "user") and from the reconciliation of the design with spike S-1 and the converter port specification (cited "S-1" and "port specification"). Section A.11 (D-147 to D-152) records where M0 as built departs from the design, from the milestone note `docs/milestones/M0.md` (cited "M0"). Items that state the same choice are merged into one entry. These items get no id: length notes (WU-1, 15, 17, 52, 81, 97), restatements of open questions (WU-16, 51), confirmations of existing ADRs (WU-64 → D-012/D-025; WU-71 and 05 §15.3 → D-029/D-035; WU-82; WU-83 and 06 §24.1 → D-027) and the cross-reference task list. Where two sources disagree, the entry is marked **Conflict** and states the resolution. "User must confirm: yes" means the design proceeds on the stated default but the user owns the call. D-037–D-047 come from the review and amend the ADRs they name; those ADRs carry an "Amended by" note.

### Addendum index

| ID | Topic | Confirm | Sources |
|---|---|---|---|
| D-037 | `.ld` scaling order: default and vocabulary (default `afterMul` since spike S-1) | no | WU-72, 05 §15.4, review, S-1 |
| D-038 | i2 EULA field of use (amends D-005, D-004) | **yes** (answered 2026-10-10) | review |
| D-039 | Research-phase use of the user's i2-written files (amends D-005) | **yes** (answered 2026-10-10) | review |
| D-040 | Fixture licensing and the public/private split (amends D-033) | no | review, 08 §13.10 |
| D-041 | Writer-independent oracles for synthetic fixtures (amends D-033) | no | review |
| D-042 | Budget measurement on one reference machine (amends D-025, D-033) | no | review, 07 §15.1, 08 §13.9 |
| D-043 | Eviction staging moves to M5 (amends D-011) | **yes** | review |
| D-044 | M0–M2 resequencing and the M1 maths subset (amends D-016) | no | review, WU-5, 01 §12 D-4 |
| D-045 | 0.1 re-baseline: test share and contingency (amends D-035) | **yes** (answered 2026-10-10) | review |
| D-046 | Optional 0.1 cut list (proposed amendment to D-035/D-027) | **yes** | review |
| D-047 | Values-window default content | no | WU-63, WU-94, 06 §24.11, review |
| D-048 | Layer ordering vs link edges | no | WU-2, 01 §12 D-1 |
| D-049 | `Executor` is test-only | no | WU-3, 01 §12 D-2 |
| D-050 | `SelectionSnapshot` placement | no | WU-4, 01 §12 D-3 |
| D-051 | `ErrorCode` and the C-ABI status | no | WU-6, 01 P-1 |
| D-052 | `TaskKey` naming and coalescing | no | WU-7, 01 P-2 |
| D-053 | `OutOfBudget` when pinned bytes exceed the budget | no | WU-8, 01 P-3 |
| D-054 | Pyramid entry type | no | WU-9, 01 P-4 |
| D-055 | Per-user directories, portable mode, `settings-1` | no | WU-10, WU-91, 01 P-5, 06 §24.8 |
| D-056 | Logging | no | WU-11, 01 P-6 |
| D-057 | Messages model | no | WU-12, 01 P-7 |
| D-058 | Layer release for hidden worksheets | no | WU-13, WU-66, 01 P-8, 04 §20.13 |
| D-059 | Import Log records every heuristic | no | WU-14, 01 P-9 |
| D-060 | Channel status vs maths state | no | WU-18, WU-35, 03 §16.1 |
| D-061 | `track-1` ownership | no | WU-19 |
| D-062 | `Dtype::I64` for time columns | no | WU-20 |
| D-063 | `ChannelId` for non-logged sources | no | WU-21 |
| D-064 | Marker group, device events, unknown classes | no | WU-22, WU-74, WU-80, 05 §15.6 |
| D-065 | `MarkerEdit` restore and orphans | no | WU-23 |
| D-066 | `RangeId` for derived ranges | no | WU-24 |
| D-067 | Resampling boundary rules | no | WU-26 |
| D-068 | AFR modelling | no | WU-27 |
| D-069 | Sidecar write policy | no | WU-28 |
| D-070 | Journal entries and backup retention (amends D-019) | no | WU-29 |
| D-071 | Per-user layout file and workbook id | no | WU-30, WU-90, WU-93, 06 §24.7, §24.10 |
| D-072 | `$schema` placeholder form | no | WU-31, 08 §13.4 |
| D-073 | Names and schema ids added to the outline | no | WU-32, WU-45, WU-53, WU-80, 03 §16.8, 04 §20.1 |
| D-074 | Alias dps and override unit spelling | no | WU-33 |
| D-075 | Stint and pit thresholds | no | WU-34 |
| D-076 | Lap numbering for out, in and partial laps | no | WU-25 |
| D-077 | `ExtentMode::Ordered` | no | WU-36, 03 §16.3 |
| D-078 | Optional `Windowed` reclassification | no | WU-37, 03 §16.7 |
| D-079 | Math window when a source does not hold Main | no | WU-38, 03 §16.4 |
| D-080 | `stat_*` and running-function readings | no | WU-39, 03 §16.5 |
| D-081 | `variance_*` reading | no | WU-40 |
| D-082 | Remaining maths compatibility readings | no | WU-41 |
| D-083 | `esses.gps_corrections` registered disabled | no | WU-42, 03 §16.6 |
| D-084 | G-correction and Corr Dist integration | no | WU-43 |
| D-085 | `Type::scaleToBase` and unknown dimensions | no | WU-44, 03 §16.2 |
| D-086 | C-ABI maths validity and plugin descriptor | no | WU-47, 03 §16.9 |
| D-087 | Deterministic random functions | no | WU-48, 03 §16.10 |
| D-088 | Whole-extent spill and halo limit | no | WU-49 |
| D-089 | i2 maths importer placement | no | WU-50 |
| D-090 | CLI subcommand set and staging | no | WU-46, WU-80, 07 §15.4 |
| D-091 | Cursor and datum are Main-lap-relative | no | WU-54, 04 §20.2 |
| D-092 | F9 fallback extent | no | WU-55, 04 §20.3 |
| D-093 | Offset nudge steps and drag commits | no | WU-56, WU-88, 04 §20.4, 06 §24.5 |
| D-094 | M4 vertex order and summary-path bound (amends D-012) | no | WU-57, WU-58, 04 §20.5–6 |
| D-095 | Overlapped-mode Lo/Hi | no | WU-59, 04 §20.7 |
| D-096 | Mixed manual and automatic scaling in a group | no | WU-60, 04 §20.8 |
| D-097 | Time Report rolling minimum | no | WU-61, 04 §20.9 |
| D-098 | Overlay scope of gauges and `values_list` | no | WU-62, 04 §20.10 |
| D-099 | `DistanceIndex` time unit | no | WU-65, 04 §20.14 |
| D-100 | Overlay 1..n default style | no | WU-67, WU-95, 04 §20.15 |
| D-101 | Time-of-day axis | no | WU-68, 04 §20.11 |
| D-102 | Sectioned text profiles (amends D-029) | no | WU-69, 05 §15.1 |
| D-103 | Vendor-mark allow-list | no | WU-70, 05 §15.2, 08 §13.12 |
| D-104 | Lap counter as a lap source for `.ld` | no | WU-73, 05 §15.5 |
| D-105 | Session-less ABI `probe` | no | WU-75, 05 §15.7 |
| D-106 | MoTeC CSV lap table | no | WU-77 |
| D-107 | Converter harvest gating details | no | WU-78, WU-79, 05 §15.9–10 |
| D-108 | W lives in `WorksheetViewModel` | no | WU-84, 06 §24.13 |
| D-109 | One selection slot per range | no | WU-85, 06 §24.2 |
| D-110 | N/P, CTRL+F and CTRL+ALT+F rules | no | WU-86, 06 §24.3 |
| D-111 | F4 hides the reference overlay everywhere | no | WU-87, 06 §24.4 |
| D-112 | Key 0 for worksheets and workbooks | no | WU-89, 06 §24.6 |
| D-113 | Single-instance forwarding | no | WU-92, 06 §24.9 |
| D-114 | Reserved, undocumented i2 views | no | WU-96, 06 §24.12 |
| D-115 | Roadmap conventions | no | 07 §15.2, §15.3, §15.5, §15.8 |
| D-116 | Linux and macOS at 0.1 | **yes** | 07 §15.6 |
| D-117 | Vendor sample files (extended by D-146) | no | 07 §15.7, WU-76 |
| D-118 | Golden conventions | no | 07 §15.9, 08 §13.5, §13.7 |
| D-119 | ADS acquisition | no | 08 §13.1 |
| D-120 | Build and repository conventions (amended by D-147, D-148, D-149) | no | 08 §13.2, §13.3, §13.8, §13.11 |
| D-121 | `.ld` file association | no | 08 §13.6 |
| D-122 | i2 configuration importers in `esses_project` (amends D-007, D-048, D-089, D-103) | no | revision, 01 §12 D-1 |
| D-123 | Threading and runtime refinements (amends D-031, D-052) | no | revision, 01 §12 D-5, P-10…P-14 |
| D-124 | Build and CI refinements (amends D-034; amended by D-147, D-150) | no | revision, 01 §12 D-6, 08 §13 |
| D-125 | Vendor-mark allow-list classes and identifier linting (amends D-003, D-103) | **yes** (optional rename) | revision, 08 §13.12 |
| D-126 | Qt 6.12 installed locally in M0 week 1 (amends D-006) | no | revision, 08 §13.18 |
| D-127 | Goldens and fixtures (amends D-033, D-040, D-118) | no | revision, 08 §13.7, §13.16, §13.19 |
| D-128 | Clean-room controls for AI-assisted work (amends D-004; amended by D-145, D-151) | no | revision, 08 §13.20–21 |
| D-129 | Exact `indexAt` and `Rate` ordering (amends D-008) | no | revision, 02 §9.1 |
| D-130 | Source identity (amends D-011, D-013, D-015) | no | revision, 02 §9.6, 05 §15.12 |
| D-131 | C ABI refinements (amends D-028, D-051, D-086) | no | revision, 05 §15.10–11 |
| D-132 | Model rules: laps, groups, device events, aliases (amends D-013, D-014, D-064, D-075) | no | revision, 02 §9.2–5, 05 §15.6 |
| D-133 | One workspace-level view (amends D-018) | no | revision, 04 §20.16, 06 §24.14 |
| D-134 | `DistanceChain` (extends D-016) | no | revision, 04 §20.17 |
| D-135 | Fastest-lap candidate rule (corrects D-017) | no | revision, 06 §24.3 |
| D-136 | Schema-generated property grids for 0.1 (optional reversal of D-027) | **yes** (optional) | revision, 06 §24.18 |
| D-137 | Static-only layer image (amends D-025, D-058) | no | revision, 04 §20.13 |
| D-138 | Offset nudge step (amends D-093) | no | revision, 04 §20.4, 06 §24.5 |
| D-139 | 0.1 maths staging (amends D-022) | no | revision, 03 §16.11 |
| D-140 | Node versions and the `DistanceIndex` key (amends D-016, D-022) | no | revision, 03 §16.12 |
| D-141 | Units table provenance (refines D-010) | no | revision, 02 §9.8 |
| D-142 | `.ld` reader and fixture writer ported from the user's converter (amends D-028; keeps D-041; amended by D-144) | no | user, 2026-10-10 |
| D-143 | The `.ld` pro-logging field is never computed or written (amends D-004) | no | S-1, port specification §1.13 |
| D-144 | `.ld` reader and writer written fresh while the converter's licence and lineage are unconfirmed (amends D-142, D-028; note of 2026-10-11: the Qt C++ converter is the reference, the Python version is deprecated) | no | port specification §0, §8 item 1; user, 2026-10-11 |
| D-145 | `CLEANROOM.md` categories: user-contributed own work, user-owned device logs (amends D-128) | no | S-1, port specification §7, §8 item 2 |
| D-146 | Vendor T2 test models count as vendor sample files (extends D-117) | no | S-1 |
| D-147 | Dependencies vendored under `third_party/`; no vcpkg manifest (amends D-034, D-120, D-124) | no | M0 §5 deviation 2, `third_party/README.md` |
| D-148 | Catch2 3.7.1 amalgamated with the project's own test discovery (refines D-033; amends D-120) | no | M0, `third_party/README.md`, `tools/dev/README.md` |
| D-149 | Per-component `ESSES_WITH_<LIB>` build switches (extends D-120) | no | M0, `CMakeLists.txt` |
| D-150 | The `qt-app` CI job starts in M1 (amends D-034, D-124) | no | M0 §5 deviation 1 |
| D-151 | `CLAUDE.md` and `AGENTS.md` are written in M1 (amends D-128) | no | M0 §5 deviation 3 |
| D-152 | `esses-cli fixture add` defaults (applies D-040, D-041) | no | M0 §3, §5 deviation 4 |
| D-153 | Dock backend: ADS by default, the `QDockWidget` backend kept as the fallback (amends D-026, D-044, D-119, D-147) | no (decided by the user, 2026-10-11) | M1 §5 deviation 1 |
| D-154 | The default `alternateList` ends with `Speed`; M1 uses the `alternateList` fallback (amends D-016, D-044) | no | M1 §5 deviation 2 |
| D-155 | Budget rows from the benchmark executables; no `esses-cli bench` (amends 07 §4.5) | no | M1 §5 deviation 3 |
| D-156 | The shell (`MainWindow`, `LayoutFile`) lives in `esses_widgets` (amends 01 §2.1) | no | M1 §5 deviation 4 |
| D-157 | `DistanceIndex::timeAt` is exact between the 0.25 m steps (refines D-016) | no | M1 §5 deviation 5 |
| D-158 | Marker order: at equal times `EOL` before `SOL` (amends 02 §4.3 behaviour 8) | no | M1 §3 review fix |
| D-159 | Shipped data: `<exe dir>/data` first on every OS (amends 01 §9) | no | PR #1 CI (qt-app ubuntu) |
| D-160 | DCO sign-off suspended while the maintainer is the only contributor (amends D-002, D-004) | no (user decision 2026-10-12) | PR #1 |
| D-161 | `esses_qtmodel` links Qt Gui for the undo framework only (amends D-007, D-032; 01 §2.1–2.2, 06 §1) | no | M1 §6 item 9, M2 contracts |
| D-162 | `project-1` minor 2: Data-window filter keys are the hash text; the first real migration (amends D-019; 02 §6.2) | no | M2 persistence, 07 §5.1 |
| D-163 | Load pipeline of the project documents: typed reader before the schema, newer minors kept or refused (amends D-019; 02 §6.1, §6.6; closes R-38) | no | M2 persistence, M1 §6 item 7 |
| D-164 | Maths dimension warnings: bare literals and undeclared result units (refines D-021; 03 §3.2 rules 4 and 10) | no | M2 language |
| D-165 | Maths diagnostics beyond 03 §2.5, and the nesting limit (refines D-020; 03 §2.5, §14.3) | no | M2 language |
| D-166 | `esses-cli eval` evaluates on the calling thread, over logged channels and details (amends 03 §13) | no | M2 language, AC-M2-14 |
| D-167 | Text engine as built: rules that differ from 05 §7 (amends D-029; 05 §7.1–7.3) | no | M2 CSV import, AC-M2-13 |
| D-168 | Open-pipeline step 4, the Mapping-panel hand-off and the shipped text profiles as built (amends D-028, D-029; 05 §2, §8) | no | M2 CSV import, AC-M2-11 |
| D-169 | IIR filters designed in-tree, not through Iir1; the Chebyshev reading (amends D-124, 03 §1, §5.3; refines D-082) | no | M2 kernels |
| D-170 | Maths kernels as built: failures known at evaluation, `integrate_dist`'s Corr Dist input, the case corpus (amends D-022, D-033; 03 §6.1, §14.1) | no | M2 kernels, AC-M2-02 |
| D-171 | `MathGraph` as built: the ends of a shadowing chain, disabled elements, conditions, the descriptor unit and the M2 graph surface (refines D-022, D-024; amends 03 §4.2–4.3, §7.1, §8, §9.2) | no | M2 graph, AC-M2-03, AC-M2-04 |
| D-172 | Undo, persistence and the journal in `esses_qtmodel` as built: whole-document commands, view state kept out of undone edits, workbook and Mapping-panel edits, dirty state, save and autosave rules (refines D-019, D-032, D-070; amends 02 §6.8, §7) | no | M2 qtmodel, AC-M2-06, AC-M2-09 |
| D-173 | `MathsService` and Channel Status as built: GPS corrections where GPS is logged, local maths from the sidecar, the live check's scope, the rows and kinds of Channel Status (refines D-013, D-016, D-022; amends 03 §9.2, §11, 06 §10) | no | M2 qtmodel, AC-M2-12 |
| D-174 | Lap stretching in 0.1: the track length is the source's `Venue Length` until track maps; factors published as `Range::stretch`; the toolbar toggle is view state (refines D-016, D-140) | no | M2 qtmodel |
| D-175 | Channels-window search: the query is split like the names; loose is a substring per word, still in order (refines 06 §7 item 3) | no | M2 qtmodel |
| D-176 | (number not used: reserved during the M2 work and never written) | — | — |
| D-177 | The maths editors as built: auto-save on close through the project save, the apply and close rules, the item pages of 0.1, Channel Status and Channel Editor routing, the active-channel commands (refines D-013, D-019, D-021, D-027; amends 03 §11, 06 §10, §13.1, §13.2, §14.4) | no | M2 maths editors, AC-M2-12 |
| D-178 | The shell's M2 wiring as built: Options, schemes, Edit-menu undo, text-field chords, the recovery prompt and leaving a project; keymap rows of the milestone in progress (refines D-019, D-032, D-161; amends 06 §2.1, §13.7, §14.2, §16, §18) | no | M2 options and shell, AC-M2-06 |
| D-179 | The data editors and the Data/Channels dock work as built: the Mapping panel's queue, Save and unit-map rows, the Lap Editor's rows and beacon edits, typed details, the filter UI and the proof of AC-M2-05/-11 (refines D-028, D-014, D-015; 05 §8.1, 06 §5.4, §6, §7, §13.3, §13.4, §13.8) | no | M2 data editors, AC-M2-05, AC-M2-11 |
| D-180 | The shell's wiring of the M2 editors as built: the Mapping panel's place, the Channel Status dock, modeless editors on first use, and the proofs of AC-M2-05/-11 in `tests/ui` (refines D-177, D-178, D-179; 06 §2.2, §3.2, §10, §13, §14.4) | no | M2 integration, AC-M2-05, AC-M2-11 |
| D-181 | `esses-cli export` and `fixture diff` as built (refines D-090; 05 §9.2, 07 §5.1, §5.5) | no | M2 integration |
| D-182 | Maths units found in the user's maths: `[ratio]` on a unitless channel, `l/100km` and `psi (sg)` (refines D-164, D-010; 03 §2.4 rule 5) | no | M2 integration, AC-M2-01 |
| D-183 | AC-M2-10 on the 0.1 whole-node path: measured, not met; the two-frame gate moves to M5 (withdrawn 2026-10-13: superseded by D-191, AC-M2-10 met) | no (withdrawn) | M2 integration, AC-M2-10 |
| D-184 | AC-M2-14 reads the fixture's own channel name (amends 07 §5.2 AC-M2-14) | **yes** (confirmed 2026-10-14) | M2 integration, AC-M2-14 |
| D-185 | Maths engine corrections of the M2 review: result rate through statistics, integer and Bool dtypes, non-finite results, literal factors, table axis units, the power switch's hash, synthesised self references, filter windows, stepping constants, recursive `previous_sample`, the math window in the selection key, variance across sources, timestamped rates (refines D-021, D-022, D-164, D-170, D-171; amends 03 §3.2, §4.4, §5.2, §5.4, §5.6, §6.4–6.6, §7.2, §7.6, §8) | no | M2 review, AC-M2-02 |
| D-186 | `scale_offset` is computed in the unit of its offset (amends 03 §4.4) | **yes** (confirmed 2026-10-14) | M2 review |
| D-187 | The journal as a per-session snapshot: write-ahead save, autosave beside a deferred recovery, Recover adopts, case-only renames (amends D-172 (h)–(i); 02 §6.8) | no | M2 review, AC-M2-06 |
| D-188 | `esses-cli maths check` reports MathGraph's nodes (refines D-166; 03 §13, AC-M2-01) | no | M2 review, AC-M2-01 |
| D-189 | Seven keymap rows of milestone M2 move to M3; the workbook rows are built (amends 06 §14.1's milestone column, D-178 (e)) | no | M2 review |
| D-190 | The rest of the M2 review: recoverable saves, bounded and guarded text reads, coalesced sidecar writes, cancellable opens, the Maths Editor's partial save (amends D-167, D-172, D-177 (a), D-187; 02 §4 sidecar rule 4, §6.8; 05 §7.1) | no | M2 review, AC-M2-06, AC-M2-11, AC-M2-13 |
| D-191 | The 0.1 evaluator streams a plan in pieces and seals a node's chunks on idle workers (refines D-139; 03 §6.0; AC-M2-10; supersedes the proposal of D-183) | no | AC-M2-10 |
| D-192 | Public README is a work-in-progress notice; TRADEMARK.md withheld until a working model (amends D-004; AC-M0-10) | no (user request 2026-10-13) | Maintainer |
| D-193 | The M3 contracts: component body schemas under `schemas/components/`, shared definitions repeated per schema instead of `common-1`, the names added, the dispatch points wired once (refines D-027, D-061, D-073; amends 02 §6.4, 04 §7.3, §16-17, outline §1.1 and §5) | no | M3 contracts, AC-M3-08 |
| D-194 | Binning, variance and corrected-speed readings fixed in M3: per-channel `Interp` members, band generation, histogram percent and time, the scatter grid, `TrendFit` by Givens QR with an exact-arithmetic reference, variance with lap stretch, `gCorrection`'s edges and its compute path (refines D-023, D-084, D-193 (h); 03 §9.1 items 2-3, 04 §7.3, §8.8, §10.1, §11.1) | no | AC-M3-05, AC-M3-06, AC-M3-07 |
| D-195 | Track maps as built: GPS and Speed+LatG generation, default sections with a minimum turn, the track-sections rule and its phase-2 pass, the track-map file (refines D-027, D-061, D-140; 02 §4.5, §6.9, 06 §13.5) | no | AC-M3-04 |
| D-196 | The plotcore M3 geometry as built: point-set tags, scenes apart from emission, summary scales for scatter, the outing's own layout, the track origin rule in plotcore (refines D-193; amends 04 §8.4, §9, §10.1, §11.1, §14, outline §4) | no | M3 geometry, AC-M3-02 |
| D-197 | Reports as built: the statistic definitions, the probe and interpolated boundaries, rows divided by headers in any order, the Overlay 0 delta by paired statistics, the StatCache key, the rolling minimum over contiguous laps, the cold report bench on the pool (refines D-027; amends 01 §4.4, 04 §12 items 1-6, §13 items 1-2, outline §2) | no | AC-M3-03 |
| D-198 | The plots and gauges as built: the gauge bodies and builders, `TextRun::scale` for the numeric gauge's dynamic font, `LayeredComponent`, the scatter's own axis zoom, the export tables, ALT+n on the BandAxis components, the outing graph with the plots work (refines D-027, D-193, D-196; amends 04 §7.2 items 4 and 8, §9, §10.1 items 4, 7-8, 10, §11.1 item 10, §16-17, outline §4) | no | M3 plots and gauges, AC-M3-02 |
| D-199 | The report and track components as built: ReportModel's styling roles, the workspace StatCache and track maps reached through RenderService, TrackMapService and TrackMapCommand, generation through esses_qtmodel, the Track Sections pass in SourceLoader, the track view as body edits, the Track Editor without the SourceLoader (refines D-027, D-193, D-195, D-197; amends 04 §12 item 1, §13 item 6, §14 items 1 and 4, 06 §9, §13.5, outline §4) | no | M3 reports and track, AC-M3-02, AC-M3-03, AC-M3-04 |
| D-200 | The properties dialogs and the shell's M3 wiring as built: the component command hook, binders that keep untouched members and validate against the body schema, Apply as one spec edit followed through the canvas, the canvas edited in place, the clipboard payload, the palette's providers and order, animation state without a worksheet, workbook names as file names, the M3 keymap rows and their tests (refines D-027, D-032, D-133, D-193; amends 06 §2.2, §4.1-4.5, §12, §13.6, §14.4, §15, 04 §7.2, outline §4) | no | M3 properties and shell, AC-M3-01, AC-M3-08, AC-M3-10, AC-M3-11, AC-M3-12 |
| D-201 | The time_distance completion as built: widget-local state and a build report beside the build inputs, override limits on the resolved channel, the datum's key columns in the Static layer, the M3 bands and panels, per-source offset steps and held drags, annotation edits through the selection, the key's label floor, per-case golden provenance (refines D-023, D-027, D-193, D-196; amends 04 §2.2, §8.1-§8.11, §8.13, 06 §14.4, outline §4) | no | M3 time_distance, AC-M3-02, AC-M3-05, AC-M3-09 |
| D-202 | The M3 acceptance as built: the completed circuit workbook, the range key colour of 06 §5.3 item 3 restored in the M3 builders, the F3 bench row, M3 as the keymap checker's milestone with the generic row, O zeroing as one undo step, the values_list goldens, the offscreen demo and the reports' project time format (refines D-198, D-199, D-200, D-201; amends 07 §6.2 AC-M3-05 proof, §6.5) | no | M3 acceptance, AC-M3-01, AC-M3-02, AC-M3-05, AC-M3-09 |
| D-203 | The M3 fix pass: the review findings fixed, rejected or deferred (gCorrection's slip reference, the report engine's pairing, trust and units, the histogram's closed last band, the track-sections origin rule for stretched and unfinished laps, loop closure and the GPS seam, idle hidden worksheets, the shell's add, context menus and asynchronous export, heading pickers, rename and move without a rebuild, D-127 (a) amended, the deferrals) (refines D-194, D-195, D-197, D-198, D-199, D-200, D-201, D-202; amends D-127 (a), 04 §7.2 item 8, §8.2 item 4, §12 item 6, 06 §13.5 item 4, §14.4) | no | M3 fix pass, AC-M3-01 to AC-M3-12 |
| D-204 | The Track Sections pass keyed by the identity of its lap indexes: a stretch published at the same index count runs it again, the dedupe is against the layer's latest run, only the latest run publishes, a closed source's runs are forgotten (refines D-195, D-199 (e); reads 02 §4.4 rule 13) | no | PR #4 review, AC-M3-04 |
| D-205 | Track-map file names that keep the venue's identity: a readable slug and a hash of the exact venue, a file read only for the venue it holds, a write refused over another venue's file, the names of earlier builds read and moved (refines D-195 (e), D-199 (c); reads 02 §6.9 `trackmaps/<venue>.track.json`, amends outline §4) | no | PR #4 review, AC-M3-04 |
| D-206 | The M4 contracts: one shell hook with an extension per package, frozen shared headers with per-package nested state and hook calls, the open-time budget check at publication with Continue and Cancel as a Messages row's two actions, a polled PerformanceMonitor, the re-stat check of every source, the Data window's stale query, DprWatcher and DockHost::windowOf, no new keymap rows, the wave-2 placeholders (refines D-011, D-043, D-193 (f); reads 07 §7.1, 01 §7.2, 02 §1 behaviour 5, 06 §6 and §17; amends 06 §2.2, §11, outline §4) | no | M4 contracts, AC-M4-02/-07/-09/-10 |
| D-207 | The memory budget as built: the estimate as the upper bound of every chunk, Continue as the held open's own publication, Cancel releasing only an unshared decode log, the warning row's alternate action on SHIFT and in a row menu, Continue and Cancel enabled while an open waits, the setting floored and given back, the Options field in GB, the Performance field and the Stats line, the resident row as the peak resident set of a fresh process (refines D-011, D-043, D-206 (c)-(e); reads 07 §7.1, §7.2 AC-M4-02, §12, 01 §7.1-§7.2, §8.3, 06 §2.1, §11; amends 06 §11, outline §4) | no | M4 budget, AC-M4-02 |
| D-208 | Changed source files as built: E-5's answer on NTFS (the POSIX-semantics, ReplaceFileW and rename-aside writers pass, a classic MoveFileExW replace is refused whatever the share mode, so the sharing policy stays), the stale mark sticky until reload or close, the Messages row added by connectSourceReload, Reload as a close then an open with the selection parked by the close, the command enabled while a source is stale, a fault-injection seam in the .ld provider and ld.io_fault once per source (refines D-206 (f); reads 02 §1 behaviour 5, §8 AC-4a, 06 §5.6, §6, §14.4, E-5; amends 06 §6 "Changed files", outline §4) | no | M4 reload, AC-M4-10 |
| D-209 | HiDPI as built: per-monitor DPR through a watcher of every top-level window with a re-check after dock moves, crisp axis-aligned strokes on whole device pixels at a fractional DPR drawn by the backend, every layer painted from a whole device pixel, the definitions of crisp and no drift, the offscreen DPR sweep and its goldens (refines D-025, D-206 (g); reads 04 §6.6, 06 §1, §17, 07 §7.1 row 3, §7.2 AC-M4-07; amends 04 §6.6 items 1, 3 and 4, outline §4) | no | M4 hidpi, AC-M4-07 |
| D-210 | The M3 follow-ups of esses_qtmodel and the Qt-free libraries as built: track-map files on the pool, summariesReady, the channel report's "ignore interpolated data", ReportModel's in-place refresh, the Track Sections pass keyed by its bound map, the missing report and variance tests (refines D-195, D-197, D-199 (c) and (e), D-203 (n), D-204; reads 01 §5.3 rules 3-4, 02 §3.2 rule 10, 04 §12 items 2-3 and 8, §13 item 5; amends 04 §12 items 2 and 8, outline §4) | no | M4 followups, M3.md §6 items 18, 23, 25, 29-32 |
| D-211 | The M3 follow-ups of esses_widgets as built: a channel's default display unit (a fraction in %), display units of the scatter and channel report channels, unit pickers in the Properties pages, the Track Editor's speed class and Corr Dist unit, Export Data in the legacy dialect, the time_distance group headings, component links honoured, summariesReady reaching the components (refines D-010, D-177 (e), D-195, D-198 (e), D-200, D-201, D-203 (af) and (m), D-210 (b); reads 03 §9.1 item 6, 04 §2.4, §7.2 item 8, §8.2 item 4, 05 §6, 06 §12, §13.5 item 4; amends 04 §7.2 item 8's reading in D-198 (e), outline §4) | no | M4 followups (widgets), M3.md §6 items 18-19, 21-22, 24, 26-27 |
| D-212 | Packaging and 0.1 identification as built: the About dialog and esses --version with the tag or commit source URL, release builds bound to their tag, single-instance forwarding over the operating system's local IPC with a channel per settings folder, --scheme and --keymap, the notices and the user guide beside the data in every package, a per-package third-party manifest and CycloneDX SBOM, the Qt licence texts outside LICENSES/, the installer's runtime check with an app-local runtime for per-user installs, the portable zip's association scripts, the release-tree vendor-mark scan, package.yml (refines D-006, D-034, D-055, D-113, D-116, D-206 (i); reads 06 §§2.2, 18-19, 22, 07 §7.1-§7.2, §7.5, 08 §§3.10-3.11, 4.1, 5, 7, 9, 10; amends 06 §18 step 1, §19 item 5, §24 item 9, 08 §3.11, §4.1, §7.1-§7.4, §10, outline §4) | no | M4 packaging, AC-M4-04/-06/-08/-09 |
| D-213 | The Saturday soak as built: tests/soak/tst_saturday with its own driver over ten logs written at test time, a Dispatcher thread violation as a failed ESSES_ASSERT or a Qt thread-affinity warning made fatal, pending work and growth judged per cycle, LeakSanitizer suppressions limited to code that never calls back with lsan_check.py counting the leaks that have an esses frame, the soak job on linux-clang-asan with the Qt layers (refines D-031, D-123 (e), D-206 (i); reads 07 §7.1, §7.2 AC-M4-03, 08 §4.1, §6.10, 01 §5.3 rules 1, 3-4 and 11, 06 §18; amends 08 §4.1 soak row, §6.10) | no | M4 soak, AC-M4-03 |
| D-214 | The Saturday walkthrough, the CLI golden tests, esses-cli convert and the Data finals as built: one test function per 0.1 item in the real shell over data written by the test, item 10's functional half beside the bench rows, golden-JSON tests per subcommand with expectations from the corpus and the reference decoder, convert with --format (alias --to), a CLI-side cut of a .ld range and the input never overwritten, "final" as the design's checks asserted by the walkthrough (refines D-155, D-181 (e), D-202 (a), D-206 (i), D-209; reads 05 §5, §9.2, 06 §4.3, §14.4, §16, §20, §22, 07 §2.2, §7.1 "Data", §7.2 AC-M4-01, §7.5; amends 05 §9.2's convert row, 07 §7.2 AC-M4-01's test path) | no | M4 walkthrough, AC-M4-01 (items 1-11), 07 §7.1 "Data" |
| D-215 | The user guide and Help Contents as built: Markdown pages under docs/user/ with index.md as the start page, the installed page beside the data directory then the executable, the online page at the build's tag, the online page when the installed one does not open, a Messages warning when nothing opens, CLEANROOM.md completed for 0.1 (refines D-004, D-206 (h)-(i); reads 06 §2.2, §14.4, 07 §7.1, 08 §7, §9 item 4; amends 06 §14.4's "local HTML or URL") | no | M4 docs, 07 §7.1 "Docs", AC-M4-01 (help.contents row) |
| D-216 | The M4 integration as built: the commit stamped at a tag, the soak out of the normal runs with its guard's tests in them, the walkthrough and the CLI golden tests as CI steps of their own, the install rules with and without Qt and the release tree scanned in lint, every M4 keymap row implemented with M4 as the keymap checker's milestone, the demo's command line in a real process, one repository URL, the soak's restart check after a Close All (refines D-206, D-212 (b), D-213 (f), D-214, D-215; reads 07 §7.2, §7.5, 08 §3.10, §4.1, §4.4, §5; amends D-213 (f), 08 §4.1's qt-app and lint rows) | no | M4 integration, AC-M4-01/-03/-08/-09 |
| D-217 | The M4 fix pass: PR #5's first CI failures fixed at the root (the main window's teardown, the formatter, the vendor-mark scan of optimised binaries, the Qt install with one aqtinstall worker), Reload that keeps the source until its file is open again, decode-time diagnostics in Messages, the activation check on the pool, forwarded requests at the top of the event loop and only after their answer, a salted per-user channel, the release stamped from its own tag and development builds marked, convert's companion, cut details, empty output and duplicate channels, the hosted bench without the reference gates, the three missing MVP bench rows, installer upgrades and uninstalls that keep what is not theirs; the deferrals (macOS strips, crisp 2 px lines at DPR 2, same-size same-mtime replacements, Qt's bundled notices) (refines D-208, D-209, D-212, D-214, D-215, D-216; reads 02 §1 behaviour 5, 05 §5, §9.2, 06 §6, §17, §18-19, 07 §1.5, §7.2, §12, 08 §3.10, §4, §7; amends D-208's "Reload as a close then an open", D-212 (b)'s channel name, D-216 (a)'s version of a build without a version tag) | **yes**: (n) only | M4 fix pass, AC-M4-01/-04/-05/-07/-08/-09/-10 |

### A.1 Review amendments

#### D-037 `.ld` scaling order: default and vocabulary
**Context.** Public layouts disagree: gotzl computes `(raw/scale × 10^−dec + shift) × mul`, i3rs `(raw/scale) × 10^−dec × mul + shift`. 05 §3.6 defaulted to gotzl's order, 07 to i3rs's, and 08 §6.7 and 01 §8.2 used two further spellings.
**Decision.** One option everywhere: `ImportOptions.options.ld.shiftOrder ∈ {"beforeMul", "afterMul"}`, default **`afterMul`** since the spike S-1 amendment below (originally `beforeMul`, gotzl's order, which the public writers follow). Fixture `dialect` blocks write `"shiftOrder"` with the same values, and Import Log code `ld.shift_mul_order` carries the value in `detail`. Only a real MoTeC-logger file with `shift ≠ 0 ∧ mul ≠ 1` settles the order; the third-party decoders each encode one reading, so they are not oracles on this point. The result is recorded in `CLEANROOM.md` and the default flips if needed. **Conflict** (05 vs 07, 08, 01) resolved in favour of 05, which owns the format.
**Amendment (spike S-1, 2026-10-10).** The default flips to **`afterMul`**: `value = raw / scale × 10^−dec × mul + shift`. Real logger data settles it by physical evidence, as this entry requires (`docs/spikes/S-1.md` §7). In `20251009-1552000.ld` (C185 logger data) the channel `Gbox Oil Temp` has `shift 32, mul 9, scale 5, dec 1`, unit °F, raw counts in 0.1 °C: afterMul gives 75.2 °F (24.0 °C) at a cold start where coolant reads 24.0 °C and inlet air 25.3 °C, while beforeMul gives 331.2 °F (166 °C), which is not physical. `20260420-0120200.ld` (C127, as downloaded) corroborates it: `Ambient Air Temp` with raw 0 decodes to 32 °F under afterMul and 288 °F under beforeMul. The field set is the °C to °F conversion `F = C × 9/5 + 32`, which only afterMul expresses; the user's converter and i3rs also compute afterMul. A vendor-software CSV export of either file (both come from MoTeC devices, so D-038 (a) allows it) is still welcome as confirmation and is recorded in `CLEANROOM.md` when made; it is no longer required. `ld.shiftOrder` stays the dialect option, so `beforeMul` remains selectable per file and per fixture. `tools/ld_ref_decode.py` reports both orders and is not changed to agree with the reader (D-041); oracle dumps used for fixtures name the order explicitly. Q-06 is answered.
**User must confirm:** no (Q-06, which supplied the files, is answered by spike S-1).

#### D-038 i2 EULA field of use (amends D-005, D-004)
**Context.** The installed EULA licenses i2 only for use with data generated by MoTeC logging devices or MoTeC-licensed third-party software, and forbids reverse engineering; D-005 considered only the second clause. AC-M5-08, the v1.0 demo and 05 §14 item 8 ran the user's i2 on files Esses wrote.
**Decision.** (a) i2 is run only on files from MoTeC logging devices; files from other producers, ACC included, only if the user confirms that the producer is MoTeC-licensed. (b) i2 is never run on files Esses wrote; export verification is re-import by Esses plus decoding by pinned i3rs and motec-i2 (MIT). (c) Behaviour observation (D-005 (3)) waits for the user's own legal check that black-box observation is not "reverse engineering" under the EULA (Q-03 (3) was answered yes on 2026-10-10; the check is the user's own responsibility and is not recorded as performed); until then the D-017, D-020 and D-047 defaults are recorded "chosen, not observed". (d) The rule is written in `CLEANROOM.md` and in risk R-15.
**User must confirm:** **confirmed by the user on 2026-10-10 (Q-03).** (a) and (b) stand as written. For (c) the user answered yes to observation on their own install: black-box only, never disassembly; the legal check is the user's own responsibility and is not recorded as performed. The D-017, D-020 and D-047 defaults therefore stay "chosen, not observed" only until each observation runs (scheduled in `OPEN_QUESTIONS.md` section B). Under (a) the user answered the ACC question the same day: ACC files are not MoTeC-licensed, so i2 is never run on ACC data, not even as an oracle; ACC exports are ordinary third-party fixtures (D-040), private for now (Q-04).

#### D-039 Research-phase use of the user's i2-written files (amends D-005)
**Context.** `background/research/config_model.md` (2026-10-09) used project files that the user's i2 wrote under `Documents\MoTeC\i2\Workspaces` and `%APPDATA%\MoTeC\i2\1.1` (D-005 category 2) before the user answered, and 03 §15 and 06 §23 cite them as provenance. Under the old "no to all three" default those lines would be invalid.
**Decision.** The three D-005 answers are blocking before the first commit of `CLEANROOM.md` (all three were given on 2026-10-10, so it is no longer blocked), which records that category (2) was used for research on 2026-10-09. If (2) is no, every provenance line that cites user files is re-sourced from help text or shipped XML, or marked "unsourced, chosen default". The user-file-derived sections of `config_model.md` never enter the public repository.
**User must confirm:** **confirmed by the user on 2026-10-10 (Q-03 (2)): the research-phase use stands.** `CLEANROOM.md` records the 2026-10-09 use with the 2026-10-10 consent date, and the user-file-derived sections of `config_model.md` still never enter the public repository. With all three D-005 answers given, the first commit of `CLEANROOM.md` is no longer blocked.

#### D-040 Fixture licensing and the public/private split (amends D-033)
**Context.** D-033 and 03 §14.2–14.3 planned the ACC workspace maths (authored by Kunos and/or MoTeC, no licence) and vendor template maths as in-tree parser fixtures, and AC-M0-01 required sim-to-motec exports (no licence) in the public job. Owning a licensed copy grants no right to redistribute, and golden JSON that names items would reproduce their content.
**Decision.** Vendor and ACC maths and workspaces live only in `fixtures/private`; their public goldens record counts only (items, OK/warning/error), never names or expressions. The public parser corpus is the project's own default module, hand-written i2-syntax cases and fuzz-generated expressions. Third-party exports are in-tree only when their licence permits redistribution. sim-to-motec output is private, and `CLEANROOM.md` records the project as "read only, unlicensed, no code or template copied". Sim dialects are covered publicly by synthetic families from our writer, parameterised from documented dialect differences. Vendor templates are "vendor files on the user's install", not "user-owned". The private submodule is checked out only with a deploy-key secret, and `fixture verify` reports skipped private fixtures so fork PRs stay green.
**User must confirm:** no (which of the user's own files are public is Q-04, answered 2026-10-10: none for now, revisit before the first public release).

#### D-041 Writer-independent oracles for synthetic fixtures (amends D-033)
**Context.** Synthetic `.ld` families are written by our `IWriter` and read by our reader, so an error made in both (scaling order, `dtype_a` semantics) passes the round trip; `fixture add` writes expectations from the current reader.
**Decision.** Expected channel values for synthetic families come from `tools/ld_ref_decode.py`, written from the public layout notes independently of `esses_formats`, or from a pinned i3rs/motec-i2 decode recorded once. `esses-cli fixture add --oracle ref-decode|<tool>` records which one. The reader's own output is never the oracle for a dialect claim, and a dialect claim counts as settled only with a real-file or third-party-decoder oracle (the scaling order needs a real file, D-037).
**User must confirm:** no.

#### D-042 Budget measurement on one reference machine (amends D-025, D-033)
**Context.** D-025 gated MVP budgets "on the developer's laptop class" and D-033 "on a fixed runner class (±15 %)". GitHub-hosted runners are shared VMs whose run-to-run variance often exceeds 15 %, 07 §1.5 forbade cutting budget criteria, and absolute limits were mixed with relative tolerances.
**Decision.** Every budget row is an absolute limit, checked as the median of ≥ 5 repetitions on one reference machine: the developer's machine registered as a self-hosted runner, or a pinned dedicated runner, with CPU, core count, RAM and OS recorded in `bench/workloads.md`. The self-hosted job runs only on pushes to `main`, nightly, on tags and on manual dispatch, never on fork pull requests. Hosted runners run the same benchmarks as a non-blocking ±30 % regression alarm against their own baselines. "Cold" means a fresh process with the OS file cache flushed. A budget may move one milestone when the reference machine, not the code, causes the failure. Resolves 07 §15.1 and 08 §13.9.
**User must confirm:** no (the machine is Q-07).

#### D-043 Eviction staging moves to M5 (amends D-011)
**Context.** The MVP workload RW-2 is fully resident (384 MB with pyramids), while RW-1 (2.85 GB raw) is a v1.0 workload (01 §7.2). M4's "2 GB `.ld` to first paint < 3 s cold" was stricter than the v1.0 row (1 GB ≤ 3 s from M5), the stress-file size differed between 01 and 07, and eviction plus stress cost 18 h in an over-committed plan.
**Decision.** LRU eviction of unpinned chunks, `OutOfBudget` (D-053), the RW-1-class stress fixture (a synthetic 2.85 GB `.ld`, the only stress-file size) and the "1 GB `.ld` cold → first paint ≤ 3 s" gate land in M5 (AC-M5-09). Release 0.1 keeps D-011's M0–M3 behaviour (budget tracked, Messages warning) and adds a warning at open, with continue or cancel, when the probe estimate (raw + pyramids) exceeds the budget. **Conflict** with 01 §7.2 ("the stress fixture gates 0.1") resolved by this entry.
**User must confirm:** **yes** (Q-11; it removes large-file support from 0.1).

#### D-044 M0–M2 resequencing and the M1 maths subset (amends D-016)
**Context.** M0 packed the whole Qt-free foundation plus the CSV engine into 100 h; M1 built most of the shell, the full parser and three corrected-speed methods, with 37 h for about twelve UI classes. D-016 requires the built-in Corr Speed/Dist module when distance mode ships, and 03 §14.4 put the parser in M1.
**Decision.** M0 keeps the `.ld/.ldx` reader and writer, store, units, model, sidecar, CLI `info/channels/laps/validate/fixture add|verify`, the synthetic generator with the reference decoder, the macOS core runner (D-034) and a TSan job on `esses_base`. `TextEngine`, `motec.csv`, `generic.csv`, `Dictionary`, `UnitMappingProfile`, their schemas and fuzz targets, and `fixture diff` move to M2. M1 keeps D-016's built-in module with only the engine it needs (`MathGraph` define/require/version, `PlanProvider`, `EvalContext`, `fastestWheel` with clamps). `Parser`, `TypeChecker`, `esses-cli eval`, the elementwise budget and `LapStretch` move to M2; `alternateList` and `gCorrection` move to M3 with the dialog. The `QDockWidget` fallback is built only for an OS where spike S-3 fails (D-026's wording). `PropertiesDialog` first appears in M3, Data-window filters and Channels-window i2 search move to M2, and overlapped mode moves to M3. The review's alternative, computing Corr Speed directly in `esses_analysis` and wrapping it in M2, is rejected because D-016 makes the module the single data path for distance consumers. **Conflict** with 03 §14.4 (parser in M1) resolved by this entry.
**User must confirm:** no.
*Amended by D-154 (A.12, proposed): the `alternateList` fallback is used from M1 and its default list ends with `Speed`; D-153 (decided 2026-10-11): ADS is the default dock backend and the `QDockWidget` fallback is built on every OS, not only where S-3 fails.*

#### D-045 0.1 re-baseline: test share and contingency (amends D-035's timing)
**Context.** The 30-week / 750 h plan applied ×1.2 where the judges estimated parity-first at ~1.5× optimistic, held tests near 20 % and had no contingency, while documents 01–08 added infrastructure and test obligations; the review estimated 1,100–1,300 h for the design as written.
**Decision.** Tests are 30 % of every milestone. Milestones: M0 4 weeks / 100 h, M1 8 / 200, M2 9 / 225, M3 11 / 275, M4 4 / 100 (900 h), followed by a 15 % contingency line (135 h), so 0.1 lands at ≈ 42 calendar weeks. M5 is 13 weeks plus 15 % (v1.0 ≈ 57); M6 and M7 are 8 weeks each plus 15 % (v1.1 ≈ 75). R-01 fires when M0 exceeds 130 h, and M1–M4 are then re-planned with M0's actual/plan ratio.
**User must confirm:** **confirmed by the user on 2026-10-10 (Q-05): re-baseline accepted** (≈ 42 calendar weeks to 0.1, tests 30 %, 15 % contingency).

#### D-046 Optional 0.1 cut list (proposed amendment to D-035/D-027)
**Context.** M3's estimates for `time_distance` completion, reports, track generation and the summary components were 1.5–2× low, and the Saturday story does not need every item that D-027 and D-035 keep. These items go beyond D-035, so the user decides.
**Decision (proposed).** Move to v1.0: Speed+LatG track generation (GPS stays); Time Report eclectic column, rolling minimum and % bands (plain section times stay); annotations, status strip and device events; animation; `LayoutEditor` (canvas drag/resize stays); scatter interval average, tiling and colour channel; histogram text and line styles and tiling; Channel Report transposed layout. This saves ≈ 50 h of M3 (0.1 ≈ 39–40 weeks; v1.0 +2 weeks). Default if unanswered: not adopted, and the list becomes M3's ordered may-slip list.
**User must confirm:** **yes** (Q-10).

#### D-047 Values-window default content
**Context.** 06 §8 follows parity-first §5.10, which D-001 adopts verbatim: rows are the focused component's channels, with an "All channels" toggle. 07 stated "all channels of the Main source"; i2's own rule is undocumented.
**Decision.** Rows are the focused component's channels (`ComponentWidget::channelNames()`); the header toggle "All channels" lists every channel of the Main source and persists per user (`layout.json` `docks.values.allChannels`); with no focused component, the last focused component's list stays. **Conflict** (06 vs 07) resolved for 06.
**User must confirm:** no (observation permitted since 2026-10-10 and scheduled for M3, Q-08).

### A.2 Architecture and runtime (01)

#### D-048 Layer ordering vs link edges
**Context.** Outline §1.2's chain `base ← units ← store ← … ← cli` reads like a list of link edges. The §1.1 table and D-007 give `esses_store` no `esses_units` link and `esses_project`/`esses_plotcore` no `esses_maths` link.
**Decision.** The chain is a layer ordering; link edges are the §1.1 "Depends on" column; maths results reach the plot as `Column`s through the shared `ChunkCache`. Outline §1.2 is updated. *Amended by D-122: `esses_project` does link `esses_maths` (same-layer edge); `esses_plotcore` still does not.*
**User must confirm:** no.

#### D-049 `Executor` is test-only
**Context.** The outline listed an `Executor` interface in `esses_base`. D-031 names only `TaskPool`.
**Decision.** `Executor` is a test-only inline executor and `TaskPool` is the only production implementation. The outline is updated.
**User must confirm:** no.

#### D-050 `SelectionSnapshot` placement
**Context.** D-017 places `SelectionSnapshot` under the Qt `DataSelectionModel`. The outline defines the struct in `esses_plotcore`.
**Decision.** The POD lives in `esses_plotcore` (Qt-free, copied to workers) and `DataSelectionModel` owns the live instance. Maths receives its own copy, `MathSelection` (03 §7.5), built by `MathsService`, because `esses_maths` cannot see `esses_plotcore`.
**User must confirm:** no.

#### D-051 `ErrorCode` and the C-ABI status
**Context.** `Result<T,Error>` needs a stable code set. Ecosystem-first's ABI status list is a subset of what the libraries need.
**Decision.** `ErrorCode {Ok, Io, Format, Unsupported, NotFound, Invalid, Cancelled, OutOfBudget, OutOfMemory, Cycle, Parse, Internal}`; `esses_status` in `sdk/esses_abi.h` mirrors it 1:1.
**User must confirm:** no.

#### D-052 `TaskKey` naming and coalescing
**Context.** D-031 coalesces requests by key. It fixes neither the key names nor the two coalescing behaviours.
**Decision.** Prefixes `open/`, `chunk/`, `summary/`, `distidx/`, `maths/`, `trace/`, `geom/`, `report/`, `export/` (extended by D-123 with `ranges/` and `slab/`). With `chunk/`, `summary/`, `distidx/`, `maths/` and `open/`, all requesters share one result; with `trace/`, `geom/` and `report/`, a newer request replaces (cancels) the older one.
**User must confirm:** no.

#### D-053 `OutOfBudget` when pinned bytes exceed the budget
**Context.** With eviction, unpinned chunks can be freed, but pinned bytes alone may still exceed the budget.
**Decision.** In that case `ChunkCache::allocate` fails with `OutOfBudget`, and the requesting task fails visibly with a Messages row instead of silently exceeding the budget. This applies from M5, when eviction arrives (D-043); before that, the budget only warns.
**User must confirm:** no.

#### D-054 Pyramid entry type
**Context.** `MinMaxPyramid` entries for F64 and I32 columns were unspecified. Exactness comes from the raw-sample descent of D-012.
**Decision.** Entries are `MinMax{float, float}` for every dtype, including F64; exact values always come from raw samples. *Entries of wider dtypes are rounded directionally (`lo` toward −∞, `hi` toward +∞) so buckets stay conservative (01 §7.1); 02 §3.1 and 04 §4.1 declare the same `float` entries (`OPEN_QUESTIONS.md` F-2, applied).*
**User must confirm:** no.

#### D-055 Per-user directories, portable mode and `settings-1`
**Context.** The outline lists per-user settings without a schema id or directory rules. 01 §9 and 06 §18 list different `settings.json` contents.
**Decision.** Per-OS config, cache, logs, data, plugins and projects directories, with `ESSES_*_DIR` overrides; portable mode when `portable.marker` sits next to the executable. The per-user `settings.json` has schema `settings-1`, whose content is the union of 01 §9 (budget, time format) and 06 §18 (scheme, keymap name, MRU, importer overrides, palette history, geometry); 02 owns the schema text.
**User must confirm:** no.

#### D-056 Logging
**Context.** There was no logging convention. The CLI and the GUI need different sinks.
**Decision.** Named categories; sinks are stderr for the CLI, a rotated 2 MB × 5 file for the GUI, and a `MessagesSink`; level selection uses `ESSES_LOG=category:level`.
**User must confirm:** no.

#### D-057 Messages model
**Context.** Diagnostics from import, maths, budget and project code needed one home. Modal dialogs would interrupt analysis.
**Decision.** `MessagesModel` origins `{Import, Maths, Budget, Project, System}`, a 5,000-row session cap, and no modal dialogs for diagnostics.
**User must confirm:** no.

#### D-058 Layer release for hidden worksheets
**Context.** 01 §7.1 releases a hidden worksheet's `QImage` layers after 30 s, while 04 §6.2 releases them on `hideEvent`. 04 also asked whether the Dynamic layer should stay a cached `QImage`.
**Decision.** Layers of components on a hidden worksheet are released 30 s after it is hidden; showing it again before then reuses them, and after that rebuilds them. The Dynamic layer stays a cached `QImage` (three layers, D-025). *The last sentence is superseded by D-137 (only the Static image is cached).* **Conflict** resolved for 01, which avoids rebuilds on quick tab flips.
**User must confirm:** no.

#### D-059 Import Log records every heuristic
**Context.** Outline §9 requires the Import Log to be the dialect audit trail. `ImportMessage` severities covered only warnings.
**Decision.** `Severity::Info` records every heuristic decision (dialect choices, `ld.shift_nonzero` once per file, sentinels), so the log is complete rather than exceptions-only.
**User must confirm:** no.

### A.3 Data model and persistence (02)

#### D-060 Channel status vs maths state
**Context.** The outline's `Channel::Status {Ok, Pending, Error}` has no Warning, Evaluating or Disabled state. D-013 also puts `statusText` in the descriptor.
**Decision.** `Channel::status` (enum `ChannelStatus`) stays `{Ok, Pending, Error}` and mirrors `desc.statusText`. `esses_maths::MathState {Pending, Evaluating, Ok, Warning, Error, Disabled}` maps onto it as 03 §7.3 specifies.
**User must confirm:** no.

#### D-061 `track-1` ownership
**Context.** Outline §5 made `esses_analysis` the owner of `track-1`. Outline §4 listed `TrackMapFile` under `esses_project`.
**Decision.** Schema and geometry belong to `esses_analysis::TrackMap`; the file wrapper `TrackMapFile` stays in `esses_project`.
**User must confirm:** no.

#### D-062 `Dtype::I64` for time columns
**Context.** D-008 requires an exact I64 µs column for `TimestampedSeries`. D-009 fixed `Dtype {F32, F64, I32}`.
**Decision.** Add `Dtype::I64`, restricted to time columns: it is never a `ChannelDescriptor` dtype, has no pyramid and is always `AllValid`.
**User must confirm:** no.

#### D-063 `ChannelId` for non-logged sources
**Context.** D-013 defines the deterministic id only for logged channels. Maths, alias, mapped, detail, constant and setup channels need stable ids too.
**Decision.** A per-source derivation table, held in 02: each source kind hashes its own defining key in place of the logged name (global maths: module id; local maths: "local"; similarly for Alias, Mapped, Detail, Constant and Setup).
**User must confirm:** no.

#### D-064 Marker group, device events and unknown classes
**Context.** D-014's `Marker` hashes a group that it does not store. 02 adds `MarkerClass::DeviceEvent`, while 05 says device events are not markers and adds `LayerInfo::events[]` (`EventInfo`) to `probe-1`; 04 draws device events from the marker set.
**Decision.** `Marker` gains `group` ("" for intrinsic, "Beacons", "StageMarkers"). At the importer boundary, device events travel as `EventInfo` in `probe-1` `LayerInfo::events[]`. `esses_model` turns each into a `Marker{cls = DeviceEvent, group = "", prov = Logged}` that no `IRangeRule` consumes and that 04 draws (SHIFT+E). Unknown `.ldx` classes become `MarkerClass::Other` and round-trip. **Conflict** (02 and 04 vs 05) resolved by keeping both representations, one per layer. *Superseded by D-132: 02, 04 and 05 now agree on one representation (`DeviceEvent` markers in group `DeviceEvents` reported in `probe_json` `markers[]`; no `EventInfo`, no `MarkerClass::Other`; unknown classes opaque).*
**User must confirm:** no.

#### D-065 `MarkerEdit` restore and orphans
**Context.** D-014 lists `restore` as an op without saying whether it is stored. Marker ids can also disappear when a companion file changes.
**Decision.** `restore` is a command that deletes the marker's `offset`/`ignore` entries and is never persisted; a sidecar holds at most one offset and one ignore per marker id. Edits whose marker no longer exists are kept and reported, never dropped.
**User must confirm:** no.

#### D-066 `RangeId` for derived ranges
**Context.** D-014 defines identity only for marker-bounded ranges. Section and Data ranges have no markers of their own.
**Decision.** Section and Data ranges use `hash(group path, start-marker id of the enclosing marker-bounded ancestor, qualifier = section name)`.
**User must confirm:** no.

#### D-067 Resampling boundary rules
**Context.** The `Interp` policies lacked edge semantics. Gaps and series ends must behave the same in every view.
**Decision.** Outside `[tFirst, tLast]` every policy returns invalid; `Closest` ties go to the earlier sample; `Linear` with an invalid neighbour is invalid.
**User must confirm:** no.

#### D-068 AFR modelling
**Context.** D-010 says AFR = lambda × a vehicle constant. It does not say where the constant lives.
**Decision.** Fuel-specific AFR units stay fixed-scalar lambda aliases for import. The vehicle constant comes from a shipped circuit maths item `AFR = 'Lambda' * 'Stoich Ratio'` over a new numeric detail `Stoich Ratio` (default 14.7).
**User must confirm:** no.

#### D-069 Sidecar write policy
**Context.** D-015 says the sidecar is written atomically but not when. D-019 says autosave never rewrites project files.
**Decision.** The sidecar is written atomically on every committed sidecar command (coalesced over 500 ms) and on source close. It is not journaled because it lives beside the log, and an empty document deletes the file.
**User must confirm:** no.

#### D-070 Journal entries and backup retention (amends D-019)
**Context.** D-019 fixes a write-ahead journal and "previous copy" backups. It defines no entry format or retention.
**Decision.** `journal-1` entries hold the full canonical document per dirty file, with `baseHash`/`hash` for conflict detection. An explicit Save performs the atomic writes, then the backup copies, then deletes the journal. `backup/` keeps the last 5 copies per file.
**User must confirm:** no.

#### D-071 Per-user layout file and workbook id
**Context.** The outline keys dock perspectives by "workbook id" without defining it. 02 §6.7 and 06 §3.4 give different `layout-1` field sets.
**Decision.** `WorkbookFile.id` is a UUID assigned on creation (derived from `Hash128(file name)` when absent) and keys the perspectives. `.local/layout.json` (`layout-1`, schema owned by 02) holds perspectives, dock states, window geometry, `lastView {workbook, worksheet}` and the active workbook and worksheet. `.local/settings.json` holds per-user project settings. The workbook's `activeWorksheet` is only the first-open fallback, and zoom, cursor and datum are never persisted. 06's example must validate against 02's schema.
**User must confirm:** no.

#### D-072 `$schema` placeholder form
**Context.** Until a project domain exists (D-003), 02 proposed `esses:<name>-<major>` and 08 proposed `urn:esses:<schema>-<n>`. Every file needs one form.
**Decision.** `urn:esses:<schema>-<major>`. `SchemaRegistry` also accepts the later https form and normalises on save; a newer major refuses to open (`E_SCHEMA_TOO_NEW`); removed fields migrate into an `x-esses-legacy` bag. **Conflict** resolved for 08 because it is a valid URN.
**User must confirm:** no.

#### D-073 Names and schema ids added to the outline
**Context.** Outline §10 allows a new name only when it is added to the outline. The section writers introduced about forty.
**Decision.** They are registered in outline §4 ("Names added during documentation") and §5: schema ids `settings-1`, `rule-1`, `units-1`, `common-1`, `schemas/components/<type>-<version>.schema.json`, `importer-profile-1`, `dictionary-1`, `plugin-1`, `maths-case-1`, `mathfn-manifest-1`, `builtin-<id>-<version>`, and the `project.json` key `builtinMaths`.
**User must confirm:** no.

#### D-074 Alias dps and override unit spelling
**Context.** D-013 mentions only `colourIndex` on aliases. Overrides convert to base units on read, which loses the spelling the user wrote.
**Decision.** Aliases also carry `dps`. Overrides keep the written unit symbol in memory so that round trips are byte-identical.
**User must confirm:** no.

#### D-075 Stint and pit thresholds
**Context.** The beacon-pair rule needs stint and pit thresholds. The values come from motorsport-telemetry-rs.
**Decision.** A gap over 10 s or a counter reset starts a stint, and 15 s or more below 1 km/h on Corr Speed inside a lap marks a pit (as the `pitStop` annotation only; the lap keeps its class, number and trust, D-132). Both are `rule-1` parameters, checked against the user's real logs in M1 without involving i2.
**User must confirm:** no.

#### D-076 Lap numbering for out, in and partial laps
**Context.** i2's numbering of non-flying laps is undocumented. The Data window shows these numbers.
**Decision.** Out lap = 0, in lap = n + 1, and a partial lap keeps the number it would have consumed. Rule templates drive the numbering, so an observed difference is a data edit.
**User must confirm:** no (observation permitted since 2026-10-10 and scheduled for M1, Q-08).

### A.4 Maths (03)

#### D-077 `ExtentMode::Ordered`
**Context.** D-022's "forward IIR orders chunk-independent with state carry" contradicts itself. State carry implies ordering.
**Decision.** A third extent mode between Parallel and Whole: chunk by chunk on one worker, each chunk sealed as it completes. Forward IIR uses it.
**User must confirm:** no.

#### D-078 Optional `Windowed` reclassification
**Context.** D-022 classes `time_shift` and `previous_sample` as Stateful/whole-extent. Both can in fact be computed with a halo.
**Decision.** They stay Stateful. Reclassifying them to Windowed (halo = shift, bit-identical) is permitted as an optimisation when the shift is ≤ 65,536 samples.
**User must confirm:** no.

#### D-079 Math window when a source does not hold Main
**Context.** Ungated statistics need a window. Sources other than Main's have no Main range.
**Decision.** Use the earliest-numbered selected overlay in that source, else the whole layer extent; the choice is part of `selectionKey`.
**User must confirm:** no.

#### D-080 `stat_*` and running-function readings
**Context.** The help text leaves segment and running semantics open. The running-function names are guesses.
**Decision.** `stat_*` are segment-constant two-pass statistics over gated samples per reset segment (ungated: over the math window, Scalar, `SelectionDependent`; with reset: `LapScalar`). `running_min/max` are cumulative. Names resolve through `MathsCompat::functionAliases`.
**User must confirm:** no.

#### D-081 `variance_*` reading
**Context.** D-023 defines the functions only by reference to the graph feature. Their value per sample needs a definition.
**Decision.** `variance_dist(x)`/`variance_time(x)` = x on the Main lap minus x on the reference overlay at equal distance/time from lap start, via the two `DistanceIndex` tables; invalid outside the Main range or without a reference overlay.
**User must confirm:** no.

#### D-082 Remaining maths compatibility readings
**Context.** Several function semantics are ambiguous in the help text. D-020 requires each to be listed with a test.
**Decision.** As listed in 03 and `docs/maths-compat.md`, each with a case file: `edge_delay` rise/fall with `time_valid ≡ edge_delay(rise)`; positive `time_shift` delays; `pulse_width` is the width of the most recently completed high pulse; `bit_decode(x, bit)` and `(x, lowBit, count)`; `integrate_dist` shares `integrate`'s arities; `derivative(x, y[, n])` is the with-respect-to form; tables clamp out of range; IIR passes are primed with the first/last valid sample; window edges truncate and exclude invalid samples; rounding is half away from zero; text comparison is exact and case-sensitive; downsampling is point sampling; scientific-notation literals are decimal (seconds) in window arguments.
*Refined by D-169 (M2, proposed): the IIR readings (RC form, Chebyshev ripple, `fc`, normalisation, band order, priming).*
**User must confirm:** no.

#### D-083 `esses.gps_corrections` registered disabled
**Context.** The module's dialog is v1.0 (D-027). Without settings it cannot produce meaningful output.
**Decision.** It is registered on open but disabled until settings exist; once enabled, its outputs shadow the input GPS channels by default. Amendment: the zero-fix validity step (latitude and longitude both 0 marked invalid) is not a setting and runs from M2 even while the module is disabled (03 §9.2, OPEN_QUESTIONS F-11).
**User must confirm:** no.

#### D-084 G-correction and Corr Dist integration
**Context.** D-016 names the corrected-speed methods. It does not give the algorithms.
**Decision.** G-correction predicts speed from longitudinal G (lock threshold 2 m/s, recovery 0.5 s). `Corr Dist` integrates by the trapezoid rule and holds through invalid speed samples, while `integrate()` keeps the rectangle rule. Both are documented as "results will not be bit-identical to i2". `gCorrection` ships in M3 (D-044).
*Refined by D-194 (M3, proposed): the start, invalid samples and linear blend of `gCorrection`, and its inputs in `compute()`.*
**User must confirm:** no.

#### D-085 `Type::scaleToBase` and unknown dimensions
**Context.** Unit-scale mismatches such as `'A'[km/h] + 'B'[m/s]` are legal in i2. They are still worth reporting.
**Decision.** `Type` gains `scaleToBase`; fractional powers give `Dimension::unknown()`; scale mismatches are Info diagnostics (`I-RESULT-SCALE`, `W-UNIT-SCALE`), and no implicit conversion is ever inserted. `esses_units` exposes `Dimension::pow/unknown`.
**User must confirm:** no.

#### D-086 C-ABI maths validity and plugin descriptor
**Context.** Plugin maths functions need validity data and a registration point. The ABI is frozen at v1.0.
**Decision.** At the ABI, validity is one byte per sample in `esses_math_array_v1`, converted by the host from the bitmap; `esses_plugin_desc_v1` gains `mathfns`/`mathfn_count`. *Validity part superseded by D-131 (one LSB-first bitmap for importers and maths arrays).*
**User must confirm:** no.

#### D-087 Deterministic random functions
**Context.** Cached chunks must be reproducible. Evaluation order varies between runs.
**Decision.** `rand_val`/`rand_chan` use splitmix64 seeded by definition hash, source id and sample index.
**User must confirm:** no.

#### D-088 Whole-extent spill and halo limit
**Context.** Whole-extent plans on long logs can exceed memory. Very large halos make chunked evaluation pointless.
**Decision.** Whole-extent stage outputs above 256 MB spill to temporary `ChunkCache` chunks; a windowed halo above 65,536 samples forces whole-extent evaluation.
**User must confirm:** no.

#### D-089 i2 maths importer placement
**Context.** The outline did not place the v1.0 i2 maths XML importer. `esses_project` owns `maths-1` loading.
**Decision.** `esses_project::I2MathsImporter`, built on pugixml. *Superseded in part by D-122: the class is `CfgMathsImporter` in `libs/project/src/cfgimport/`, because the identifier lint rejects `I2…`.*
**User must confirm:** no.

#### D-090 CLI subcommand set and staging
**Context.** D-007 lists the subcommands without milestones. The writers added `maths check`, `maths functions`, `units scan` and `bench formats`.
**Decision.** M0: `info/channels/laps/validate/fixture add|verify`. M1: `convert/bench` and `esses-render`. M2: `eval`, `export`, `maths check`, `maths functions`, `units scan`, `fixture diff`, and `bench formats` with the format benchmarks. All `--json` output is canonical.
**User must confirm:** no.

### A.5 Rendering and components (04)

#### D-091 Cursor and datum are Main-lap-relative
**Context.** D-018 says "Main-lap time". It does not fix absolute versus relative time.
**Decision.** `ViewState.cursorT`/`datumT` are µs since the Main range start, which makes N/P preservation and per-slot conversion trivial.
**User must confirm:** no.

#### D-092 F9 fallback extent
**Context.** D-016's fraction rule does not say which distance extent to show when the Main lap has no `DistanceIndex`. The axis still needs bounds.
**Decision.** Use the bound track-map length if there is one, else 1.0 with distance labels suppressed and the banner shown; `zoomFrac` is maintained continuously so round trips are lossless.
**User must confirm:** no.

#### D-093 Offset nudge steps and drag commits
**Context.** i2 documents only "arrow" for alignment offsets. 04 §3.3 uses one device pixel of domain units, while 07 AC-M3-09 used one sample / 0.25 m.
**Decision.** CTRL+SHIFT / ALT+SHIFT + LEFT/RIGHT nudge by one device pixel of domain units at the current zoom, and + UP/DOWN by ten. Drag offsets live in `WorksheetViewModel::pendingOffsets` and commit to the sidecar as one undo command on release or after 250 ms idle. **Conflict** (04 vs 07) resolved for 04; AC-M3-09 is updated. *Step size superseded by D-138 (one sample period / 0.25 m), which 04, 06 and 07 now use.*
**User must confirm:** no.

#### D-094 M4 vertex order and summary-path bound (amends D-012)
**Context.** D-012 claims pixel-exact decimation, but the order of min and max within a column cannot be recovered from a pyramid. Full-extent draws over non-resident chunks also use the `SummaryPyramid`.
**Decision.** Min, max, first and last stay exact; vertices are emitted as (lo, hi) when first ≤ last, else (hi, lo). The summary path (`outing`, full-extent draws over non-resident chunks) may place column edges within ≤ 1/8 px, the only documented bound.
**User must confirm:** no.

#### D-095 Overlapped-mode Lo/Hi
**Context.** The research left open how overlapped group bounds are stored. Panels already persist `edges`.
**Decision.** Fractions of the trace-area height (0 = top), persisted alongside panel `edges`.
**User must confirm:** no.

#### D-096 Mixed manual and automatic scaling in a group
**Context.** The research left open how a Channel/Auto group combines channels with and without overrides. Both kinds can share an axis.
**Decision.** Channels with an override contribute the override and the others their autoscale, combined per channel.
**User must confirm:** no.

#### D-097 Time Report rolling minimum
**Context.** The help text is ambiguous about the rolling minimum. Two readings give different rows.
**Decision.** The rolling minimum is the minimum-sum window of `sectionsPerLap` consecutive sections over the chronological concatenation of all section times in scope, displayed rotated to section positions.
**User must confirm:** no.

#### D-098 Overlay scope of gauges and `values_list`
**Context.** The research asks which gauge types are restricted. Gauges have room for few values.
**Decision.** Numeric and bar gauges and `values_list` show Main plus the reference overlay (Overlay 0, else the lowest-numbered overlay) only.
**User must confirm:** no.

#### D-099 `DistanceIndex` time unit
**Context.** D-016 stores `tAtD[]` in seconds. Every other time is `Micros`.
**Decision.** `tAtD[]` stays in F64 seconds; `DomainMapper` converts with `llround(x · 1e6)`; 03 confirms.
**User must confirm:** no.

#### D-100 Overlay 1..n default style
**Context.** D-017 leaves the colours of overlays ≥ 1 to verification. The renderer needs a default now.
**Decision.** Plain channel colours, with scheme `overlayStyle {source: channel, lightness: 0, dash: solid}`; an observed difference becomes a scheme edit.
**User must confirm:** no (observation permitted since 2026-10-10 and scheduled for M1, Q-08).

#### D-101 Time-of-day axis
**Context.** The `TOD.*` axis mode is documented only through workbook attributes. No MVP story needs it.
**Decision.** It arrives in v1.0; imported attributes are kept in `x-i2` until then.
**User must confirm:** no.

### A.6 Formats and plugins (05)

#### D-102 Sectioned text profiles (amends D-029)
**Context.** Pi Toolbox ASCII has per-channel blocks. D-029's table-shaped profile schema cannot express them.
**Decision.** `importer-profile-1` gains `layout: "table" | "blocks"` and a `blocks {sectionRegex, metadata, events, channels}` object.
**User must confirm:** no.

#### D-103 Vendor-mark allow-list
**Context.** The MoTeC CSV writer must emit the literal format name on line 1, and the `motec.csv` sniff regex contains it. 08 also proposed allow-listing a v1.0 `i2config/` source directory, or naming it `cfgimport/` instead.
**Decision.** The D-003 allow-list gains exactly the writer's format-name constant and the sniff regex. The v1.0 i2 configuration importer lives in `libs/formats/src/cfgimport/`, so the directory needs no entry. **Conflict** (08 offered both options) resolved for the smaller allow-list. *Superseded by D-122 (importers in `libs/project/src/cfgimport/`) and D-125 (the five allow-list classes).*
**User must confirm:** no.

#### D-104 Lap counter as a lap source for `.ld`
**Context.** M1 logs may carry an I32 lap counter or a 5 Hz beacon channel but no `.ldx`. D-014's rules consume markers only.
**Decision.** Release 0.1 derives laps from `.ldx` and logged markers only; a file with neither gets `WholeOutingRule` and a Messages hint. At v1.0, `motec.ld` gains an optional `lapSource {column, kind: lap_counter_edge | beacon_event}` import option, off by default.
**User must confirm:** no.

#### D-105 Session-less ABI `probe`
**Context.** D-028's `probe(ByteSource&)` is session-less. Ecosystem-first used `probe(session)`.
**Decision.** Keep it session-less; plugins that would parse a header twice may cache by path and hash. This is reconfirmed at the v1.0 ABI freeze.
**User must confirm:** no.

#### D-106 MoTeC CSV lap table
**Context.** The `motec.csv` `lap_table` lap source is a placeholder. No available sample has lap rows.
**Decision.** It stays a placeholder until verified against the user's own i2 CSV exports of MoTeC-device logs (Q-03 (1), D-038); until then MoTeC CSV files get laps only from beacon or lap columns. Q-03 (1) was answered yes on 2026-10-10, so the check is scheduled for M2 with the `motec.csv` importer (D-044).
**User must confirm:** no.

#### D-107 Converter harvest gating details (extends D-028)
**Context.** The harvest table assumes the parsers are the user's own work. Wrapped parsers may need a `QCoreApplication`, which the Qt-free CLI cannot provide.
**Decision.** BLF, RPK and Emtron stay wrap-only plugins until their provenance is clear. Step-1 plugins must not require a `QCoreApplication`; a plugin that does is GUI-only, and `esses-cli` reports it as unavailable.
**User must confirm:** no (the provenance answer is Q-15).

### A.7 UI shell (06)

#### D-108 W lives in `WorksheetViewModel`
**Context.** The outline listed W among the `DataSelectionModel` slots. W changes zoom and cursor, not selection.
**Decision.** W is `WorksheetViewModel::zoomToMainRange`; outline §4 is corrected.
**User must confirm:** no.

#### D-109 One selection slot per range
**Context.** i2 does not document what happens when a range already in one slot is assigned to another. The selection model needs a rule.
**Decision.** A range occupies exactly one slot; setting Main to the current Overlay 0 or overlay k removes it from that slot.
**User must confirm:** no (observation permitted since 2026-10-10 and scheduled for M1, Q-08).

#### D-110 N/P, CTRL+F and CTRL+ALT+F rules
**Context.** i2's "fast laps" definition and its wrap behaviour are undocumented. These keys are used constantly.
**Decision.** N/P do not wrap and stay within the Main source. CTRL+F does nothing when there is no trusted flying lap. CTRL+ALT+F selects the fastest lap as Main plus all trusted flying laps within `fastLapTolerancePct` (default 1 %, per project, shared with the "Within 1 %" filter).
**User must confirm:** no.

#### D-111 F4 hides the reference overlay everywhere
**Context.** F4 hides overlays without deselecting them. Variance, Δ columns and reports also read the reference overlay.
**Decision.** While `overlaysHidden` is set, `referenceOverlay()` is empty for variance, Δ columns and reports, not only for drawing.
**User must confirm:** no.

#### D-112 Key 0 for worksheets and workbooks
**Context.** i2 documents keys 1–9 for worksheets and workbooks. A tenth tab needs a key.
**Decision.** 0 selects the tenth worksheet tab; CTRL+0 selects the tenth workbook.
**User must confirm:** no.

#### D-113 Single-instance forwarding
**Context.** i2's behaviour on a second launch is unknown. File associations launch the app once per double-click.
**Decision.** A second launch forwards `-w/-l` to the running instance via `QLocalSocket`; `--new-instance` opts out.
**User must confirm:** no.

#### D-114 Reserved, undocumented i2 views
**Context.** The research does not document the Data Sets / Range Key view, the Telemetry View or per-worksheet key bindings. None is in the MVP.
**Decision.** None of them is in the MVP; per-worksheet key bindings get a reserved slot (`WorksheetDef.keys` with the `keymap-1` row shape).
**User must confirm:** no.

### A.8 Roadmap, repository and build (07, 08)

#### D-115 Roadmap conventions
**Context.** 07 introduced conventions that D-034 and D-035 do not state. They affect tags, spikes, fuzzing and keymap tests.
**Decision.** Each milestone is tagged `v0.1.0-mN`. The four M0 week-1 spikes sit inside M0 (2.5 days). `fuzz_maths_parser` starts in M2 with the parser. Keymap rows carry `"status": "planned"` with a milestone and are exempt from the one-test-per-row rule until that milestone.
**User must confirm:** no.

#### D-116 Linux and macOS at 0.1
**Context.** The outline's M4 acceptance required packages to launch on Ubuntu and macOS VMs. The old open question 9 left their release status open.
**Decision.** AppImage and DMG are built and launch-tested and labelled "builds green, not supported" unless the user decides otherwise; without macOS hardware, the DMG may slip to v1.0.x.
**User must confirm:** **yes** (Q-12).

#### D-117 Vendor sample files
**Context.** The importer samples shipped with i2 Pro (the install's `Samples/` folder, including four 2009 Conversion Wizard samples; never copied) are vendor-shipped. The real-file checks of WU-76 need files the user owns.
**Decision.** Vendor sample files are never committed in any form. They are used locally as oracles only if the user permits (Q-06), and the use is recorded in `CLEANROOM.md`. The user permitted it on 2026-10-10: the samples may be used for local/private testing, as test inputs and oracles, and are still never committed or redistributed (D-004). The WU-76 checks run on the user's own MoTeC-device files during spike S-1.
*Extended by D-146: the vendor T2 test models and the two sample `.ldx` files found on the user's machine count as vendor sample files.*
**User must confirm:** no.

#### D-118 Golden conventions
**Context.** D-033 names only PNG goldens. D-025 does not name the test font.
**Decision.** `DrawList` JSON goldens run in the Qt-free `core` job as the strict, font-independent companion to the PNG goldens. Goldens use a redistributable test font (DejaVu Sans, Bitstream Vera licence) kept under `tests/golden/fonts/` for tests only. *Font superseded by D-127 (Noto Sans, OFL-1.1).*
**User must confirm:** no.

#### D-119 ADS acquisition
**Context.** D-026 allows vcpkg or FetchContent. vcpkg's ADS port depends on vcpkg's own `qtbase`, against D-006.
**Decision.** FetchContent at a pinned tag, built as a shared library.
**User must confirm:** no.
*Amended by D-153 (A.12, decided 2026-10-11): ADS 5.1.1 is vendored under D-147 (the user approved the download) instead of fetched, and built through its own CMake project as a shared library.*

#### D-120 Build and repository conventions
**Context.** 08 needed details that D-034 leaves open. They do not change any ADR.
**Decision.** Top-level `schemas/`, `bench/` and `third_party/` directories; Catch2 and Google Benchmark as a vcpkg `tests` feature; `-Werror` only in CI presets; the AppImage builds on ubuntu-22.04 (older glibc) while tests run on 24.04.
*Amended by D-147, D-148 and D-149 (M0 as built): there is no vcpkg manifest, so Catch2 is vendored (3.7.1 amalgamated) and Google Benchmark will be vendored when `bench/` needs it; the per-component `ESSES_WITH_<LIB>` switches join the build options.*
**User must confirm:** no.

#### D-121 `.ld` file association
**Context.** D-034 says only ".ld association". The user may also keep i2 installed.
**Decision.** The installer's association task is unchecked by default when an existing `.ld` association is detected, so an i2 installation is not taken over.
**User must confirm:** no.

### A.9 Final cross-document revision (proposed, 2026-10-10)

These entries record choices the documents adopted during the final cross-document revision and editorial pass, after A.1–A.8 were written. Each states the ADRs it amends; those ADRs carry an "Amended by (A.9, proposed)" line. Where a revised document disagreed with an earlier addendum entry, the entry below is the newer choice and the older entry is marked.

#### D-122 i2 configuration importers live in `esses_project` (amends D-007, D-048, D-089, D-103)
**Context.** D-089 placed the v1.0 maths XML importer in `esses_project` as `I2MathsImporter`, a name the identifier-token lint rejects (00 §8 rule 3). D-103 put "the i2 configuration importer" in `libs/formats/src/cfgimport/`, but `esses_formats` may depend on neither `esses_project` nor `esses_maths`, which the workbook and maths importers need. D-048 says `esses_project` does not link `esses_maths`, yet `esses_project` loads `maths-1` documents into `MathModule`. 01 §12 D-1, 06 §16 rule 9 and 08 §3.5 put every importer in `esses_project`; a revision of 03 §12 had moved the maths importer into `esses_maths`.
**Decision.** All v1.0 i2 configuration-file importers (`.i2wkb`/`.i2wsp` → `workbook-1`, `.mcs`/`.mfs` → `scheme-1`, maths XML → `maths-1`) live in `esses_project` under `libs/project/src/cfgimport/`, with the neutral names `CfgWorkbookImporter`, `CfgSchemeImporter` and `CfgMathsImporter` and pugixml as a private v1.0 dependency. `esses_project` links `esses_maths` as a same-layer edge (layer 2); `esses_maths` never links `esses_project`, and `esses_plotcore` does not link `esses_maths`. `MathModuleIo` (`maths-1` ⇄ `MathModule`) stays in `esses_maths`. D-007's Qt-free set is unchanged. **Conflict** (03 §12 against 01, 06 and 08) resolved for the majority; moving the maths importer into `esses_maths` was the alternative listed in `OPEN_QUESTIONS.md` (F-1) and is not adopted.
**User must confirm:** no.

#### D-123 Threading and runtime refinements (amends D-031, D-052)
**Context.** D-031 names `std::jthread` workers, `Dispatcher::post(std::function<void()>)` and a synchronous lap rebuild. 01 §5 found that `std::jthread` may be missing from the default libc++ on the `macos-14` runner (to be confirmed), that closures could outlive their targets, that a fixed pool deadlocks when workers wait for queued work, and that workers could see references to GUI-lifetime state.
**Decision.** (a) Workers are `std::thread`s with an atomic stop flag, joined by `~TaskPool`; cancellation uses our `CancelToken`. (b) `Dispatcher::post(QObject* context, fn)` skips a closure whose context is gone. (c) An explicit runtime (`AppRuntime` in the GUI, constructed after `QApplication`; a plain struct in `esses-cli`) owns `TaskPool`, `ChunkCache`, `Dispatcher` and the sinks; `TaskPool::instance()` and `ChunkCache::instance()` return the installed objects; `main()` calls `rt.shutdown()` after `app.exec()` (cancel all, join, remove the Messages sink, close the dispatcher, clear the cache). (d) **Rule W:** no pool thread waits for a task that is not running: `Column::require` materialises inline with per-chunk `{Absent, Building, Sealed}` states, `Future::get` runs a still-queued task itself, nested maths recurse inline, and the open pipeline is split into task A and task B around the Mapping panel. (e) Blocking-call assertions check `esses::ThreadRole::isNoBlock()` (`esses_base`), set only on the app's GUI thread. (f) No worker-visible reference or raw pointer to GUI-lifetime objects: shared inputs are `std::shared_ptr<const T>`, and model status is written on the GUI thread only. (g) Lap rebuild: phase 1 (markers and details) runs synchronously on the GUI thread; phase 2 (stint/pit annotations, track sections) is a worker task. (h) `Column::tryPin(chunkNo, Priority) → std::optional<ChunkHandle>` and `Column::prefetch` replace the raw `tryChunk` accessor and the unpinned `at` (02 §3.2). (i) `TaskKey` additions to D-052: share keys `ranges/<sourceId>/<layer>/<markerSetHash>` and `slab/<sourceId>/<slabNo>`; one `summary/<sourceId>` task per source; `open/<canonical path>/<size>/<mtime>/{a,b}`; maths keys end in `/c<chunkNo>` or `/all`; replace keys are latest-wins on the queue (at most one running and one pending per key).
**User must confirm:** no.

#### D-124 Build and CI refinements (amends D-034)
**Decision.** Iir1 and json-schema-validator are compiled from vendored sources as static libraries; only pocketfft is header-only. Windows uses the `x64-windows-static-md` triplet and one `bin/` output directory. The vcpkg `files` binary cache is persisted with `actions/cache` (not `x-gha`). Required checks are `core` ×3, `qt-app` on Windows and Linux, `sanitize`, `fuzz-smoke` and `lint`; macOS `qt-app` runs nightly. DMG and AppImage are built best-effort at 0.1 with runner smoke tests (a macOS VM is permitted only on Apple hardware); the Windows installer, portable zip and CLI tarballs are release-blocking. AC-M4-04 adds a Windows VM with an old VC++ redistributable. A nightly Linux TSan build with an instrumented Qt covers `QtTextMetrics` (04 §5.4).
*Amended by D-147 and D-150 (M0 as built): there is no vcpkg triplet or vcpkg binary cache, because every library dependency is vendored and built in the tree; the `qt-app` job, and with it the `qt-app` required checks, starts in M1.*
*Amended by D-169 (M2, proposed): Iir1 is not vendored; the IIR filters are designed in-tree.*
**User must confirm:** no (the Linux/macOS release status is Q-12).

#### D-125 Vendor-mark allow-list classes and identifier linting (amends D-003, D-103; touches D-019, D-035)
**Context.** D-003 allow-lists only importer ids and dialog labels. The MoTeC CSV writer must emit the format name (WU-70), D-019's round-trip bag is spelled `x-i2`/`xI2`, and prose files name the vendor. D-103 added only the CSV constant and the sniff regex.
**Decision.** Exactly five classes (00 §8 rule 2, 08 §5): (a) importer ids `motec.ld`, `motec.csv` (and the v1.0 configuration-importer ids) with their dictionary and profile file names; (b) dialog format labels; (c) the MoTeC CSV writer's format-name constant and the `motec.csv` sniff pattern; (d) the D-019 bag: the JSON key `x-i2` in `workbook-1`/`maths-1` documents, schemas and fixtures, and its C++ member `xI2`; (e) prose in `CLEANROOM.md`, `NOTICES.md`, `TRADEMARK.md`, `docs/`, fixture READMEs and `fixture.json` `source` fields. The lint also splits identifiers into tokens, so `I2MathsImporter`, `xI2` and `IMtc…` are caught. Option for the user: rename the bag to a neutral key `x-import` with a `format` field, which removes class (d); the default keeps `x-i2`.
**User must confirm:** yes (the optional rename only; Q-17).

#### D-126 Qt 6.12 installed locally in M0 week 1 (amends D-006; changes the Q-13 default)
**Context.** D-006 leaves the local kit to the developer, who has 6.7.2; CI builds on 6.12, so under the old Q-13 default (6.7.2 until M4) CI-only failures could appear for months.
**Decision.** Install Qt 6.12 locally in M0 week 1 (aqtinstall, about an hour) beside 6.7.2, with a `dev-612` user preset (08 §3.7.4, §12). Golden PNGs are still never regenerated locally. This becomes Q-13's default.
**User must confirm:** no (Q-13 stays open for the user's preference).

#### D-127 Goldens and fixtures (amends D-033, D-040, D-118)
**Decision.** (a) Pixel goldens are authoritative on ubuntu (Qt 6.12, offscreen) only and are regenerated only from CI artefacts; Windows and macOS run render smoke tests and the `DrawList` JSON goldens; `time_distance` is capped at 12 named cases. (b) The golden font is Noto Sans (OFL-1.1), the shipped schemes' family, in `tests/golden/fonts/` with an `OFL-1.1` REUSE annotation. This replaces D-118's DejaVu Sans, because D-025 asks for the scheme's default font. (c) Game-generated files, ACC exports included, are ordinary third-party fixtures (D-040) and stay private for now, like every real file (Q-04); one enters the public tree only if its producer's terms permit redistribution, recorded in `CLEANROOM.md`. ACC is not MoTeC-licensed (the user's answer of 2026-10-10), so i2 is never run on ACC files (D-038 (a)). Public CI uses the `synthetic-acc-dialect` family written by our own writer. (d) `fixture-1` is defined once, in 05 §9.1 (`input{file, sha256, bytes, remote}`, `importOptions`, a `dialect` string, `oracle` with per-dtype tolerances), and `expected.laps.json` (schema `laps-1`, the `esses-cli laps --json` output) joins the expectation set.
**User must confirm:** no (which files are public is Q-04, answered 2026-10-10: none for now).

#### D-128 Clean-room controls for AI-assisted work (amends D-004)
**Decision.** `CONTRIBUTING.md`, `CLEANROOM.md` and `CLAUDE.md` carry the AI-provenance rule: never paste i2 help text, shipped files, vendor XML or GPL source into a prompt that generates committed code or text. Forbidden paths (`research/`, `help_text/`, `conv_samples/` and vendor file types) fail `lint`. `tools/check_text_overlap.py` (local or private, on the release checklist) flags overlaps of 12 or more words with the reference corpus. Ported MIT/BSD files keep the upstream copyright line and get a `ported-from` row in `CLEANROOM.md`. The `CLEANROOM.md` table has a Category column: help page, public release notes, vendor-shipped template, file written by the user's install (with the D-005 (2) consent date), or observed on a licensed install.
*Amended by D-145: two further categories, user-contributed own work and user-owned device logs, and the values rows already use (public project, public standard, own design, rule).*
*Amended by D-151: `CLAUDE.md` is written in M1; until then `CONTRIBUTING.md` and `CLEANROOM.md` carry the AI-provenance rule.*
**User must confirm:** no.

#### D-129 Exact `indexAt` and `Rate` ordering (amends D-008)
**Context.** D-008's `floor((t − t0)·num / (1'000'000·den))` is not the inverse of its rounded `timeAt` for non-integer periods (`{512,25}`, `{30,1}`).
**Decision.** With `D = 1'000'000ull·den`: `timeAt(i) = t0 + floorDiv(2·i·D + num, 2·num)` and `indexAt(t) = floorDiv(num·(2(t − t0) + 1) − 1, 2·D)`, the largest `i` with `timeAt(i) ≤ t`. All products are formed in 128 bits (MSVC intrinsics, `__int128` elsewhere), every division floors toward −∞, and `Rate` compares by value (`{2,2} == {1,1}`) (02 §1).
**User must confirm:** no.

#### D-130 Source identity (amends D-011, D-013, D-015)
**Decision.** `SourceId` is the quick content hash: xxh3-128 over the file size, the first and last MiB and the format header, with mtime excluded so a copied or moved log keeps its id. It is final before pipeline step 3 and keys sidecar matching and caches. The full-content xxh3-128 is computed at `Background` after first paint and stored in provenance and in the sidecar's `source.contentHash`; a mismatch logs `source.changed`. Open coalescing keys on the canonical path, size and mtime. `SourceId` does not hash the importer id; `ChannelId` and `ColumnKey` include it (02 §4.1, 05 §2, 00 §10; `OPEN_QUESTIONS.md` F-3, applied).
**User must confirm:** no.

#### D-131 C ABI refinements (amends D-028, D-051, D-086)
**Decision.** One header, `sdk/esses_abi.h`, holds the importer, writer and maths-function ABI. Every field is fixed-width, and no enum type appears in a field or parameter. `esses_status` mirrors `ErrorCode` 1:1 by value (0–11, `ESSES_E_INVALID` for bad arguments); `esses_dtype` is `ESSES_DT_F32/F64/I32`. Validity has one bitmap representation (LSB first, `NULL` = all valid) for importer blocks and maths arrays, replacing D-086's byte-per-sample mask. A per-call `esses_call_v1` carries cancellation and progress; `esses_math_ctx_v1` uses the host's `esses_host_v1` for `alloc`/`free`/`log`; there is one maths state per worker thread and never a concurrent `evaluate` on one state. Descriptor strings are plugin-static, and only out-parameter strings use `host->alloc`. Any minor version is accepted, and the caller owns an out-struct's `struct_size`. A plugin that returns `ESSES_E_INTERNAL` is disabled, never unloaded. Qt is allowed in plugins only under the `qt` modes `none | static | infix | dynamic`; `dynamic` needs `minVersion` ≤ the host's Qt. The alternative (a separate `esses_abi_common.h` with a shorter status list and a mapping table, 03's earlier proposal) was listed in `OPEN_QUESTIONS.md` (F-13) and is not adopted.
**User must confirm:** no.

#### D-132 Model rules: laps, groups, device events and aliases (amends D-013, D-014, D-064, D-075)
**Decision.** (a) Beacon-pair numbering starts at the `Start Lap` detail when present, else at 1. Whether `Start Lap` numbers the first flying lap or the out lap is a D-005 (3) verification item, permitted since 2026-10-10 (Q-03) and scheduled for M1. (b) Stint and pit are phase-2 annotations (`Range::stint`, `Range::pitStop`) that never change class, number or trust. 15 s or more stationary sets `pitStop` only; stints split on counter resets, `RESET` or a data gap over 10 s, never on ordinary beacon intervals. `LapClass::Pit` stays reserved (amends D-075's "marks a pit"). (c) A `detail-group` rule kind produces the `Events`, `Vehicles`, `Sessions`, `Driver`, `Vehicle Id` and `Vehicle Number` groups. `track-sections` emits per-set groups with per-category children (`…:Default:Corners`, `…:Default:Straights`, `…:<set>:<category>`), so `range_is("Outings:Laps:Track Sections:Default:Corners")` resolves; on laps that do not start at a `BCN`, sections are placed backwards from the end beacon. (d) Device events are `Marker{cls = DeviceEvent, group = "DeviceEvents", comment = message}`, reported by importers in `probe_json` `markers[]`; `MarkerClass::DeviceEvent` extends D-014's eight classes. D-064's `EventInfo`/`LayerInfo::events[]` representation and `MarkerClass::Other` are withdrawn; unknown `.ldx` classes are kept opaque in `companionOpaque` and are not markers. (e) `MarkerEdit` gains `offsetDistance {dM}`, which applies per layer to every `BCN` and is resolved through `Corr Dist` by a worker. (f) Aliases keep D-013's order (Mappings → Aliases → canonical → logged). An alias candidate may name another alias, with cycle detection, and `Error` or `Missing` candidates fall through to the next.
**User must confirm:** no.

#### D-133 One workspace-level view (amends D-018)
**Context.** D-018 gives each worksheet its own `ViewState`. i2 has one current zoom level, drawn in the zoom summary bar above all worksheets, and remembers only the time/distance mode per worksheet.
**Decision.** `zoom` (with `zoomDomain` and `zoomFrac`), `cursorT` and `datumT` live in one `WorkspaceViewModel`; `domain` and `offsetAxis` stay per worksheet. Switching to a worksheet in the other domain applies the F9 conversion to the shared zoom. `ViewState` remains a value type composed per worksheet. D-005 (3) verification item, permitted since 2026-10-10 (Q-03) and scheduled for M1: does i2 keep cursor and datum across worksheet switches?
**User must confirm:** no.

#### D-134 `DistanceChain` (extends D-016)
**Decision.** An outing-level piecewise distance mapping is built from the per-lap `DistanceIndex` tables and their stretch factors. `DomainMapper` uses it in distance mode, so F2, panning into adjacent laps and Main = Outing work. The view is clamped to the outing extent, and F2 shows `[outing.start − Main.start, outing.end − Main.start)`. Inside the origin lap the chain equals that lap's `DistanceIndex`.
**User must confirm:** no.

#### D-135 Fastest-lap candidate rule (corrects D-017's context)
**Decision.** The candidates for CTRL+F, CTRL+ALT+F, the Fastest, Fastest 3 and Within 1 % filters and the Δ Fastest column are the Main source's trusted `Flying` laps if any trusted lap exists, otherwise its `Flying` or `Partial` laps. `Out`, `In` and `Pit` laps are never candidates (i2 release note TT 2485). This replaces the reading "untrusted laps are never fastest".
**User must confirm:** no.

#### D-136 Schema-generated property grids for 0.1 (optional reversal of D-027)
**Context.** D-027 keeps hand-built i2-style tabs. 06 §12 binds them through one `FieldBinder` with a schema-driven round-trip test, which removes most bespoke code.
**Decision (proposed, default not adopted).** Keep D-027. If the user prefers, 0.1 may instead generate every property page from the body schemas, which trades i2 tab ergonomics for less UI code.
**User must confirm:** yes (optional; Q-18).

#### D-137 Static-only layer image (amends D-025, D-058)
**Context.** A full-size cached Dynamic image costs about 200 MB of memory traffic per cursor move at 3,840 × 2,160, beyond the 4 ms budget (04 §6.2).
**Decision.** `LayerCache` caches only the Static `QImage`; the Dynamic and Chrome layers are `DrawList`s built on the GUI thread and painted directly under a damage region. Layer memory is 1 × 4 B × device area per component. D-058's 30 s release of hidden worksheets' images stands; its "Dynamic layer stays a cached `QImage`" clause is withdrawn.
**User must confirm:** no.

#### D-138 Offset nudge step (amends D-093)
**Decision.** CTRL+SHIFT or ALT+SHIFT + LEFT/RIGHT moves an offset by one step: one sample period of the active channel in that slot's source in time mode (rounded to whole µs, minimum 1 µs), or 0.25 m in distance mode. + UP/DOWN moves ten steps. The step does not depend on zoom. Drag commits are unchanged (`pendingOffsets`, one undo command on release or after 250 ms idle). See 04 §3.3, 06 §24 item 5 and 07 AC-M3-09.
**User must confirm:** no.

#### D-139 0.1 maths staging (amends D-022)
**Decision (proposed, 03 §6.0).** Through 0.1, one task evaluates each maths node over the whole layer extent (key `…/all`) and seals it into ordinary chunks; parallelism is across nodes only. The chunk-parallel evaluator (register bytecode, cumulative halos, `Ordered` checkpoints, per-chunk tasks, the chunk-order determinism test) comes at v1.0. AC-M2-10's "multi-core evaluation" is met by parallelism across nodes; 07 §5 and §8 deliverables state this staging (`OPEN_QUESTIONS.md` F-10, applied).
**User must confirm:** no (listed in `OPEN_QUESTIONS.md` as design confirmation F-10, now applied).
**Refined by (A.13):** D-191 (the node's task streams the plan in pieces with the same values, and the node's later chunks are sealed by idle workers; still one task per node and parallelism across nodes).

#### D-140 Node versions and the `DistanceIndex` key (amends D-016, D-022)
**Decision (proposed, 03 §7.2).** A maths node's version hashes exactly the logged columns, range-group versions, details and constants it reads, never `DataSource::version`. `RangeGroupTree` keeps a version per group path (`Outings:Laps` separate from each `Track Sections` group). `DistanceIndex` is keyed by `(Corr Dist column version, lap start/end marker ids and times, stretch settings)`. So an annotation invalidates no maths, and track-section rebuilds never invalidate `Corr Dist`.
**User must confirm:** no.

#### D-141 Units table provenance (refines D-010)
**Decision.** `data/units.json` is our own table, taken from the SI Brochure, NIST SP 811 and standard automotive units and sized by need. i2-only spellings are defined from their physical meaning and flagged `"compat": "i2"`. i2's `units.xml` is consulted only for alias spellings and the affine `scalar/offset` model; no table is copied (02 §2.3).
**User must confirm:** no.

### A.10 Implementation decisions (2026-10-10)

These entries record decisions taken from the user's instructions once M0 implementation started. Each states the ADRs it amends; those ADRs carry an "Amended by (A.10)" line. D-143 to D-146 reconcile the design with spike S-1 (`docs/spikes/S-1.md`) and the converter port specification (`docs/formats/ld-converter-port.md`). The `.ld` layout facts they establish are recorded as amendments to 05 §3 and §5 and 02 §4 rather than as entries here, and D-037 carries its own S-1 amendment.

#### D-142 The M0 `.ld` reader and fixture writer are ported from the user's converter (amends D-028; keeps D-041)
**Context.** User instruction, 2026-10-10: "my logfileconverter already understands how to write the files, so reading should be inversed basically". The user's Log File Converter (Qt C++ version) writes `.ld` files in `src/formats/LdWriter.cpp` and reads them in `src/parsers/LdParser.cpp`; it has no `.ldx` code. D-028 harvests the converter only after v1.0 (outline M6), gated by Q-15, and 05 §3 planned the M0 reader as an independent implementation from public documentation.
**Decision.** (a) The M0 `motec.ld` reader is ported from `LdParser.cpp` and written as largely the inverse of `LdWriter.cpp`; the `.ld` part of the `.ld`+`.ldx` fixture writer (05 §5) is ported from `LdWriter.cpp`. The `.ldx` parser and writer are new code per 05 §4 and §5 item 7. (b) The user is the author of that code and contributes it under MIT (D-002), so Q-15 is not needed for these two files; the rest of the converter still waits for Q-15 and D-107. (c) The port replaces Qt types as in D-028 step 2 (05 §11), passes the `qt-free-check` and meets the contracts of 05 §3 to §5 (sniff, dialect flags, `ld.shiftOrder` with default `beforeMul`, `LdChunkProvider`, `ImportLog` codes). Where the converter's layout goes beyond those sections, the extra structures are documented in `docs/formats/ld.md` with their source. (d) D-041 still holds: `tools/ld_ref_decode.py` is written independently from the public layout notes, not from the converter or the port, and produces the oracles of the synthetic families, so an error shared by the ported writer and the ported reader cannot pass AC-M0-03. The shift-vs-mul order still needs a real MoTeC-logger file (D-037). (e) `CLEANROOM.md` gets a `ported-from` row for each ported file (D-128): the converter file, its version or file date, licence MIT as contributed by its author, and the user's copyright line, which the ported file keeps beside ours. The C++ files say they mirror the converter's earlier Python code (`parsers/ld_parser.py` and the `ConversionWorker` `write_ld_file` method), so the row names those predecessors too. Converter knowledge of i2 behaviour (for example display labels) enters only under an existing `CLEANROOM.md` category, and any part whose lineage turns out to reach gotzl/ldparser (GPL-3) or another non-MIT source is rewritten from the public layout notes instead.
**Consequences.** M0 starts from working `.ld` code, so the 16 h `esses_formats` line of 07 §3.4 is expected to be an upper bound. The reader's clean-room standing changes from "independent implementation from public documentation" to "port of the maintainer's own MIT code, checked against public documentation and an independent decoder"; 05 §3, §5, §11 and §13, 07 §3 and 08 §11 say so.
*Amended by D-144 (2026-10-10): until the user confirms that the converter's `.ld` code is their own work, MIT-licensed and not derived from gotzl/ldparser, nothing is ported verbatim. (a), (b) and (e) are suspended, and the reader and writer are new code written from `docs/formats/ld-converter-port.md`, `docs/formats/ld.md` and the public MIT notes. (c) stands with the default `afterMul` of D-037's S-1 amendment, and (d)'s shift-vs-mul clause is settled.*
**User must confirm:** no.

#### D-143 The `.ld` pro-logging field is never computed or written (amends D-004)
**Context.** The u32 at header offset 0x5DE, which the public notes call "pro logging", is non-zero on 84 of the 87 `.ld` files of spike S-1 (`docs/spikes/S-1.md` §4). The user's converter computes a value for it when it writes a file, and the value acts as a licence token for the vendor's analysis software (`docs/formats/ld-converter-port.md` §1.13). 05 §3.2 kept the field as "semantics unknown, preserved", and 05 §5 item 8 asked a round trip to re-emit unknown header bytes verbatim, which would copy such a token into a file Esses wrote.
**Decision.** (a) Esses never computes, derives, validates or reproduces this field, and no Esses document, code comment, test or fixture states how a value is derived: working around a vendor's licence check is outside the project's scope (D-004). (b) The reader records only whether the field is zero or non-zero, as the `pro-logging` dialect flag (05 §3.8); the raw header kept for round trips (`Provenance_Source::rawHeader`, 02 §4.2) holds 0 in those four bytes. (c) The writer writes 0 at 0x5DE in every file, fixtures, exports and round trips of a file whose field is non-zero included; round-trip acceptance (05 §5 item 8, §14 item 3) excludes bytes 0x5DE to 0x5E1. (d) The converter's routine is neither ported nor restated (port specification Q-W36), and no observed non-zero value is recorded in `docs/formats/ld.md`, fixtures, expectations or producer fingerprints. (e) This supersedes the port specification's §1.13 rule that a verbatim header round trip preserves the value.
**Consequences.** Every file Esses writes carries 0 in the field, and Esses accepts however vendor software treats such files. Producer fingerprints (05 §3.8) never use the field's value.
**User must confirm:** no.

#### D-144 The `.ld` reader and writer are written fresh while the converter's licence and lineage are unconfirmed (amends D-142, D-028)
**Context.** D-142 ported the converter's `LdParser.cpp` and `LdWriter.cpp` on the basis that the user wrote them and contributes them under MIT. The port specification (`docs/formats/ld-converter-port.md` §0, §8 item 1) records that the MIT grant came from the orchestrating task text rather than the user's own words, that the converter tree has no `LICENSE` file, and that the author has not yet confirmed that its `.ld` code does not derive from gotzl/ldparser (GPL-3). The specification restates the converter's layout knowledge, quirks and inverse mapping as facts, so new code can be written from it.
**Decision.** (a) No converter code is pasted, translated line by line or otherwise reused verbatim in Esses until the user confirms in writing (a `LICENSE` file in the converter tree or a statement recorded in `CLEANROOM.md`) that `LdParser.cpp`, `LdWriter.cpp`, `parsers/ld_parser.py`, `write_ld_file` and the author's layout notes are the user's own work, licensed MIT, and not derived from gotzl/ldparser or another GPL source. (b) Until then the `motec.ld` reader and the `.ld` part of the fixture writer are new code written from `docs/formats/ld-converter-port.md`, `docs/formats/ld.md` and the public MIT notes (Friss/i3rs `docs/ld-file-format.md`, afonso360/motec-i2, tobi/motorsport-telemetry-rs); gotzl/ldparser is never a source. The converter's format knowledge enters only as facts restated in the specification and is recorded in `CLEANROOM.md` under the category user-contributed own work (D-145). (c) D-041 is unchanged: `tools/ld_ref_decode.py` stays independent and is never edited to agree with the reader; when the two disagree, the real files and the vendor-software CSV exports listed in `docs/spikes/S-1.md` decide which one is wrong. The converter's outputs are user-owned round-trip inputs, never oracles for dialect claims. (d) After the confirmation, verbatim reuse becomes possible under D-142 (e)'s `ported-from` rows; code already written fresh stays as it is unless a later change says otherwise. (e) D-142 (a), (b) and (e) are suspended until (d); D-142 (c) and (d) stand, with the D-037 default now `afterMul`. (f) The confirmation is part of Q-15, which therefore no longer waits for the M6 harvest.
**Consequences.** M0 writes about 3,700 production and 1,400 test lines for `.ld` (port specification §5), so the 16 h `esses_formats` line of 07 §3.4 is no longer an upper bound and the writer's dialect options may slip to M1. `CLEANROOM.md` gets no converter `ported-from` row while (a) holds. 05 §3, §5, §11 and §13, 07 §3, 08 §11 and the design `README.md` follow.
*Amended 2026-10-11 (user statement): "the python program is now deprecated and left behind for the qtc++ version; the qt C++ has the conversion mappings from specific formats to .csv or .ld". The converter's Python version is therefore not used as a source or a reference. The Qt C++ tree (`<converter>\QT C++ Version`), including its per-format JSON mapping files such as `haltech_mappings.json`, `MaxxECU_mapping.json`, `emtron_mappings.json`, `Emublog_mapping.json`, `cosworth_mappings.json`, `Syvecs_SD_scaling.json`, `iracing_ibt.json` and `windarab_txt.json`, is the reference for the later input-format phase (the harvest of D-028 and D-107, and the text profiles of D-029). The statement does not say that the converter is the user's own work or that it is licensed MIT, so (a) to (f) still apply: nothing from the Qt C++ tree, its mapping files included, is reused verbatim until the user confirms authorship and licence (Q-15, still open), and its format knowledge enters Esses only as restated facts recorded in `CLEANROOM.md` as user-contributed own work (D-145). (a) still names the Python predecessors, because the C++ `.ld` files say they mirror them.*
**User must confirm:** no (the converter confirmation itself is Q-15).

#### D-145 `CLEANROOM.md` categories for user-contributed work and user-owned device logs (amends D-128)
**Context.** D-128 gives the `CLEANROOM.md` Category column five values. Spike S-1 and the port specification produce provenance of two other kinds: format knowledge the user contributes from their own converter, and facts observed by decoding the user's own logger files (port specification §7, §8 item 2). Rows already in `CLEANROOM.md` also use public project, public standard, own design and rule.
**Decision.** The Category column takes D-128's five values plus two: **user-contributed own work**, for code, notes or format knowledge that the user wrote and contributes, where the row names the work and its version and states whether its licence and lineage are confirmed (D-144); and **user-owned device logs**, for facts observed by decoding the user's own logger files and the outputs of the user's own tools, which are never committed (Q-04), where the row names the class of files and never their content. The values public project (with its licence), public standard, own design and rule (a policy row with no external source) are recognised as already in use. Vendor sample files keep their own category, vendor-shipped files (D-117, D-146).
**User must confirm:** no.

#### D-146 Vendor T2 test models count as vendor sample files (extends D-117)
**Context.** Spike S-1 found four T2 test-model `.ld` files under the user's `Documents\MoTeC\T2\Test Model\` and two sample `.ldx` files under `C:\MoTeC\Logged Data\Samples\`. They were produced by the vendor, not logged by the user's devices. D-117 lets the importer samples shipped with i2 Pro be used for local testing (Q-06), and S-1 asked whether the T2 test models fall under that permission (`docs/spikes/S-1.md`, proposed `CLEANROOM.md` rows).
**Decision.** Vendor-produced sample and test-model files found on the user's machine, the T2 test models and the two sample `.ldx` files included, are vendor sample files under D-117: local, private testing and oracle use only, never committed or redistributed, and nothing derived from their content (sample values, dumps, expectations) leaves `fixtures/private`; inventory facts such as file names, sizes and channel counts may appear in spike reports. Each use is recorded in `CLEANROOM.md`. They are not user-owned device logs (D-145).
**User must confirm:** no.

### A.11 M0 as built (2026-10-11)

These entries record where M0, as built and accepted on the developer machine on 2026-10-11 (`docs/milestones/M0.md`), departs from the design. Each states the ADRs it amends; those ADRs carry an "Amended by (A.11)" line. M0.md §5 lists six deviations: 1 is D-150, 2 is D-147, 3 is D-151 and 4 is D-152. Deviation 5 (the `.ld` reader rules amended after the acceptance review: storage dtype of scaled integers and of u32, C-series record rules keyed on the logger layout, the [mi2] fingerprint, reversed gap pairs, the terminator rule and four new Info codes) is recorded, as A.10 records layout facts, as dated amendments to 05 §3.5 and §3.8 and in `docs/formats/ld.md`. Deviation 6 (device-event labels in Esses' own wording instead of the converter's) applies D-144 (b) and needs no entry. D-148 and D-149 record two build choices that M0.md does not list as deviations but that the design did not specify.

#### D-147 Dependencies are vendored under `third_party/`; there is no vcpkg manifest (amends D-034, D-120, D-124)
**Context.** D-034 takes the library dependencies from a vcpkg manifest, D-120 makes Catch2 and Google Benchmark a vcpkg `tests` feature, and D-124 sets the `x64-windows-static-md` triplet and a vcpkg binary cache in CI (08 §3.6, §3.7.1, §4.2, §4.3). M0 was built without vcpkg (`docs/milestones/M0.md` §5 deviation 2). In M0 phase A (2026-10-10) nothing could be downloaded, so pugixml and xxHash were copied from sources already on the developer's machine; later that day the user explicitly approved downloads, and the other libraries were downloaded from their official upstream repositories at fixed tags (phase B).
**Decision.** (a) Every library dependency is vendored under `third_party/<lib>/` as unchanged upstream files with the library's own licence file. There is no `vcpkg.json`, no `vcpkg-configuration.json`, no vcpkg toolchain or triplet in the presets and no vcpkg cache in CI; the build fetches no library at configure or build time, and CI downloads only its toolchain (actions, apt packages, the reuse tool). (b) M0 vendors pugixml 1.13 and xxHash 0.8.3 (local copies, no download) and nlohmann/json 3.11.3, json-schema-validator 2.3.0, CLI11 2.4.2, Catch2 3.7.1 and fast_float 6.1.6 (downloads the user approved on 2026-10-10). fast_float is used from M0 for every floating-point parse in `esses_formats`, because Apple Clang 15's libc++ lacks `std::from_chars` for `double`. (c) `third_party/README.md` is the single provenance record: version, licence file, route (local copy or approved download), source of the copy, upstream URL and the SHA-256 of every vendored file as committed. It replaces the per-library `UPSTREAM.md` of 08 §3.7.2; licences and copyright lines are in `NOTICES.md`, `REUSE.toml` and `LICENSES/`. (d) `cmake/third_party.cmake` builds each library itself, never through its upstream CMake files, as `esses::<lib>` targets with SYSTEM include directories and without the project's warning flags. (e) Updating a library touches only its directory, `third_party/README.md`, `NOTICES.md` and `REUSE.toml`. A later dependency (Google Benchmark, zlib, Eigen, pocketfft, Iir1, sqlite3, matio, Lua and sol2) is vendored the same way when its milestone first needs it, and each download needs the user's approval. Qt (D-006) and ADS (D-119) keep their own acquisition routes. Adopting a package manager later needs a new entry amending this one.
**Consequences.** Builds are reproducible offline. Every vendored library is header-only or a static library built in the tree, so no third-party DLL has to be found at run time and D-124's triplet has nothing left to choose. Updating a dependency is a reviewed copy with new hashes instead of a baseline bump. 08 §1, §3.2, §3.6, §3.7, §4 and §12, 07 §3, 01 §1 and the outline's M0 row follow.
**User must confirm:** no.
*Amended by D-153 (A.12, decided 2026-10-11): ADS 5.1.1 is vendored here too, unmodified, but built by its own CMake project as a shared library (`cmake/qtads.cmake`), the one exception to (d); it is the one third-party library found at run time, beside the executables in `<build>/bin`.*

#### D-148 Catch2 3.7.1 amalgamated, with the project's own test discovery (refines D-033; amends D-120)
**Context.** D-033 picks Catch2 v3, D-120 and 08 §3.7.1 take it from a vcpkg `tests` feature (3.5 or later), and 08 §6.2 registers each `TEST_CASE` with Catch2's `catch_discover_tests()`. M0 phase A could only use a local copy of Catch2 2.13.10 (single header). A Catch2 test spec matches names case-insensitively after trimming, treats a leading or trailing `*` as a wildcard and an `exclude:` prefix as a negation, and Catch2 v3 accepts one name twice when the tags differ, so a ctest entry built from a test name can run the wrong test case, several of them or none.
**Decision.** (a) Tests use Catch2 3.7.1 in its amalgamated form (`third_party/catch2v3/src/catch_amalgamated.{hpp,cpp}`, vendored per D-147), built as `esses::catch2` with Catch2's default `main()` (`wmain()` on Windows, so a non-ASCII test name reaches Catch2 as UTF-8) and as `esses::catch2_nomain` for `esses_add_test(... CUSTOM_MAIN)`. Tests include the modular v3 headers through generated forwarders, so a later move to the non-amalgamated Catch2 needs no test change; `CATCH_CONFIG_ENABLE_ALL_STRINGMAKERS` is on. Catch2 2.13.10 is retired and no longer in the tree. (b) Test discovery is the project's own (`cmake/catch_discover.cmake`, `cmake/catch_discover_tests.cmake`): after each link it lists the executable through Catch2's JSON reporter and registers one ctest entry per `TEST_CASE`, named `<library>.<test name>`, with the quoted and escaped name as the test spec and the tags `[unit]`, `[property]`, `[slow]` and `[oracle]` as ctest labels. (c) A name that a Catch2 test spec cannot select exactly fails the build with a message naming the test: a leading or trailing `*`, an `exclude:` prefix, a line break, or two names in one executable that are equal once case and surrounding spaces are ignored. `tests/catch_discovery_tests.cpp` holds the regression cases.
**Consequences.** Each ctest entry of a Catch2 executable runs exactly one test case, so a passing or failing entry names one test. Test names must be unique per executable regardless of case. 08 §3.7 and §6.2 and the test row of the port specification (`docs/formats/ld-converter-port.md` §5) follow.
**User must confirm:** no.

#### D-149 Per-component build switches `ESSES_WITH_<LIB>` (extends D-120)
**Context.** 08 §3.2 has `ESSES_WITH_*` options only for optional v1.0 and v1.1 features (catalogue, MAT export, scripting). In M0 the libraries were written one after another and partly in parallel, and a configure had to succeed while a sibling library was still incomplete.
**Decision.** `CMakeLists.txt` has the options `ESSES_WITH_UNITS`, `ESSES_WITH_MODEL`, `ESSES_WITH_FORMATS` and `ESSES_WITH_CLI`, all ON by default and honoured by the guarded `add_subdirectory()` calls in `CMakeLists.txt` and `libs/CMakeLists.txt`. Switching one off leaves out `libs/units`, `libs/model`, `libs/formats` or `apps/cli`; the C++ developer tools and the fixture corpus tests follow the CLI. A component that DEPS on a switched-off one fails the configure-time layer check with a message naming the switch, and a `lint` negative test covers that message. CI configures with the defaults, so every switch is ON there. A later library may get a switch on the same pattern.
**Consequences.** Work on one library is not blocked by a broken sibling, and a default configure is unchanged. 08 §3.2 lists the switches; `tools/dev/README.md` documents their use.
**User must confirm:** no.

#### D-150 The `qt-app` CI job starts in M1 (amends D-034, D-124)
**Context.** 07 §3.1 asked M0 for a `qt-app` skeleton building an empty `esses_app` with Qt 6.12, so that the Qt path is green before M1, and D-124 lists `qt-app` on Windows and Linux among the required checks. M0 wrote no Qt code (`docs/milestones/M0.md` §5 deviation 1).
**Decision.** The `qt-app` job is added in M1 together with the first Qt target and the empty `esses_app`. Until then `.github/workflows/ci.yml` runs `core` (three OSes), `lint`, `sanitize` and `fuzz-smoke` (with a Windows MSVC variant), and the required checks of 08 §4.4 are those jobs. `ESSES_BUILD_QT` defaults to OFF in the M0 `CMakeLists.txt` because no Qt layer exists yet; the default ON of 08 §3.2 applies once M1 adds the Qt layers. D-124's other required checks are unchanged.
**Consequences.** The Qt 6.12 CI path (install-qt-action, ADS, offscreen Qt Test) is first exercised in M1, so a Qt-only CI failure can first appear there. 07 §3.1, §3.2 and §4.1 and 08 §4 follow.
**User must confirm:** no.

#### D-151 `CLAUDE.md` and `AGENTS.md` are written in M1 (amends D-128)
**Context.** 08 §1 and §11 put `CLAUDE.md` and `AGENTS.md` (layer map, naming and Qt-free rules, where tests live, the AI-provenance rule, links to the ADRs) in the repository from the start, D-128 has `CLAUDE.md` carry the AI-provenance rule, and 07 R-08 counts both files as a bus-factor mitigation. They are not an M0 acceptance criterion and were not created (`docs/milestones/M0.md` §5 deviation 3).
**Decision.** Both files are written in M1 from the outline and the M0 tree. Until they exist, `CONTRIBUTING.md` and `CLEANROOM.md` carry the AI-provenance rule, as D-128 also requires.
**Consequences.** No M0 criterion changes; the R-08 mitigation starts one milestone later. 07 §4.1 lists the files among the M1 deliverables.
**User must confirm:** no.

#### D-152 `esses-cli fixture add` defaults (applies D-040, D-041)
**Context.** The 07 §3.6 step-4 command as first written had no `--licence`, and `fixture add` defaulted to the licence `LicenseRef-unknown` and to strict validity. The M0 acceptance review found that a public fixture could be added without a licence, that a refused command left a directory behind, and that an input with gap blocks failed its own `fixture verify`, because the reference decoder cannot see gap blocks (D-041; `docs/milestones/M0.md` §3 and §5 deviation 4).
**Decision.** (a) A public fixture needs `--licence`, checked before anything is written, so a refused command leaves nothing behind. (b) With `--oracle ref-decode`, `oracle.validity` defaults to `finite-only`; the synthetic-fixture generator passes `--validity strict` because it supplies the validity itself. 07 §3.6 step 4 carries `--licence`.
**Consequences.** Every public fixture states its licence (D-040), and a fixture made with the reference decoder verifies whatever gap blocks its input holds. `cli.demo_script` covers both rules.
**User must confirm:** no.

### A.12 M1 as built (proposed, 2026-10-11)

These entries record where M1, as built and reviewed on the developer machine on 2026-10-11 (`docs/milestones/M1.md`), departs from what a design document or an acceptance criterion says. Each states the ADRs it amends; those ADRs carry an "Amended by" line. Lower-level departures (added accessors, argument types, helper classes the documents name without declaring) are recorded in the "Deviations from the design text" section of each library's `README.md` and summarised in M1.md §5.

#### D-153 Dock backend: ADS by default, the `QDockWidget` backend kept as the fallback (amends D-026, D-044, D-119, D-147)
**Context.** D-026 wraps ADS in `DockHost` with a `QDockWidget` fallback for an OS on which ADS proves unusable; D-044 and 07 §4.1 build that fallback only for an OS on which spike S-3 fails; D-119 fetches ADS at a pinned tag. D-147 vendors every dependency and makes each download subject to the user's approval. M1 was first built without ADS: `DockHost` got a backend seam (`IDockBackend`) and `QDockWidgetBackend` (docked, pop-up, a fly-out emulated with side tool bars, perspectives, Reset Dock Windows, ESC returning the focus), and this entry was proposed asking the user either to approve the ADS download or to accept that backend for 0.1. The user approved the download on 2026-10-11, and ADS 5.1.1 was vendored unmodified under `third_party/qtads` (archive SHA-256 in `third_party/README.md`).
**Decision.** (a) ADS is the default `DockHost` backend (`AdsBackend`; `libs/widgets/src/shell/ads_backend.cpp` is the only translation unit that sees ADS). It is built as the shared library `qtadvanceddocking-qt6` (LGPL-2.1-or-later), aliased `esses::qtads`, and linked privately by `esses_widgets` only (`cmake/qt-free-check.cmake`, `tools/check_layering.py`). (b) The `QDockWidget` backend is kept on every OS, not only where S-3 fails as D-044 had it: the environment variable `ESSES_DOCK_BACKEND=qdockwidget` selects it at run time, and the CMake option `ESSES_DOCK_BACKEND=qdockwidget` builds without ADS for an OS on which ADS proves unusable (D-026's fallback rule). AC-M1-13's suite, `widgets.tst_dock_host`, runs on every backend the build has. (c) Acquisition (amends D-119 and 08 §3.7.3): ADS is vendored under D-147 instead of fetched, and it is built by its own unmodified CMake project (`add_subdirectory` with the options it declares: `BUILD_STATIC OFF`, `BUILD_EXAMPLES OFF`, `QT_VERSION_MAJOR` from our `find_package(Qt6)`; `cmake/qtads.cmake`), the one exception to D-147 (d). Its output directories are redirected to `<build>/bin`, beside every executable and test. ADS 5 names its target `qtadvanceddocking-qt6`, where 08 §3.7.3 has ADS 4's `qt6advanceddocking`, and the tree links the alias `esses::qtads`, where 08 has `esses::ads`. (d) A perspective names the backend that wrote it (`{"backend": "ads", ..., "state": <CDockManager::saveState>}` or `{"backend": "qdockwidget", ...}`); each backend refuses the other's, and `layout-1` records `dockBackend` and, for ADS, `adsVersion`.
**Consequences.** Fly-outs are ADS's auto-hide side bars; the fallback keeps its emulation. A layout written by the other backend is replaced by the default layout, with an Info row in Messages naming the backend that wrote it. Spike S-3 has its Windows result (`docs/spikes/S-3.md`); the Linux and macOS results come from the `qt-app` CI runs, and an OS on which ADS fails is configured with the CMake option. `NOTICES.md` carries the LGPL notice and the replacement instructions for the ADS library (08 §9). 07 §4.1's widgets row, AC-M1-13 and R-10 read with this entry.
**User must confirm:** no. Decided by the user on 2026-10-11 (ADS download approved; ADS is the default; the `QDockWidget` fallback is kept).

#### D-154 The default `alternateList` ends with `Speed`; M1 uses the `alternateList` fallback (amends D-016, D-044)
**Context.** D-044 moves `alternateList` and `gCorrection` to M3. The public ACC stand-in (`fixtures/motec.ld/synthetic-acc-dialect`, the sim-exporter dialect) logs its road speed as `SPEED` and has no wheel speeds. None of 03 §9.1's default channel lists names that channel, so the M1 data path (ACC `.ld` → Corr Speed/Corr Dist → distance mode; AC-M1-01, -09 and -10) showed "no distance data".
**Decision.** (a) From M1, `fastestWheel` falls back to `alternateList` (`W-CSD-FALLBACK`) when one of its channels is unusable, as 03 §9.1 item 1 describes. The `alternateList` method (chosen in the dialog) and `gCorrection` stay in M3. (b) The module's default `alternateList` is 03 §9.1's list with `"Speed"` appended (11 entries). Lookups are case-insensitive and a channel whose unit is not a speed is skipped, so the entry has no effect on a file without such a channel. An importer `Dictionary` (M2) or a project alias can override it.
**Consequences.** The ACC stand-in has Corr Speed and Corr Dist, and AC-M1-01, -09 and -10 pass on it. The schema example `default.json` lists the 11 entries. 03 §9.1's default object and item 2 and 07 §4.1's analysis and maths rows are updated.
**User must confirm:** no.

#### D-155 Budget rows come from the benchmark executables; there is no `esses-cli bench` (amends 07 §4.5)
**Context.** 07 §4.5's demo prints the budget rows with `esses-cli bench --suite plot --json`. The measured paths need Qt Gui (the raster backend and `TimeDistanceGraph`), which the Qt-free `esses-cli` may not link (01 §2.2). The benchmarks are therefore separate executables under `bench/`: `esses-plot-bench` (AC-M1-02) and `esses-open-bench` (AC-M1-03). Each takes `--runs`, `--json`, `--gate` and `--smoke`, and they run under ctest (`bench.*`) and in the `bench` workflow.
**Decision.** The demo and CI use the benchmark executables. No `esses-cli bench` subcommand is planned while the measured paths need Qt. `tools/bench_compare.py` (08 §6.9) will read their JSON when it is written, and until then the `bench` workflow skips the steps that need it with a notice.
**Consequences.** 07 §4.5 names the two executables, and `apps/cli/README.md` lists no `bench` subcommand.
**User must confirm:** no.

#### D-156 The shell (`MainWindow`, `LayoutFile`) lives in `esses_widgets` (amends 01 §2.1)
**Context.** 01 §2.1 places `MainWindow` in `esses_app` and the outline places `.local/layout.json` handling under `esses_qtmodel`. 07 §4.1 lists `MainWindow` under `esses_widgets`, the `tests/ui` suites must construct the real shell from a library, and the perspectives `LayoutFile` stores are produced by `DockHost` (a widget).
**Decision.** `MainWindow`, `DockHost`, `KeyEventRouter`, `CommandActions` and `LayoutFile` (`layout-1`) live in `esses_widgets`. `apps/app` contains only `main()` (start-up order, command line, `AppRuntime`).
**Consequences.** `tests/ui` links `esses_widgets` and drives the same window the application shows. `esses_app` stays a thin executable. `schemas/layout-1.schema.json` is still to be written (M2).
**User must confirm:** no.

#### D-157 `DistanceIndex::timeAt` is exact between the 0.25 m steps (refines D-016)
**Context.** D-016 defines `tAtD[]` at 0.25 m as the first time each distance is reached, so that stopped-car plateaus are handled, and 04 §3.1 reads `timeAt(d)` from that table. Interpolating linearly between two steps places a distance inside a step in which the car stopped up to the whole stop later than its first reach. AC-M1-08's plateau rule therefore held only at the grid points.
**Decision.** `tAtD[]` stays the 0.25 m table (seconds, D-099). `timeAt(d)` uses it to find the sample segment that first reaches `d` (a binary search over the segments that start in that step) and interpolates on that segment. At the grid points the result equals `tAtD[k]`.
**Consequences.** The first-reach rule holds for every distance. A query costs O(log s) for the s sample segments in its step, which is usually one or two. `DistanceChain`, F9, variance and the distance-mode cursor values inherit the exact rule.
**User must confirm:** no.

#### D-158 Marker order: at equal times `EOL` before `SOL` (amends 02 §4.3 behaviour 8)
**Context.** 02 §4.3 behaviour 8 sorts a layer's markers by `(t, cls, id)` with the classes in declaration order, so a `SOL` sorts before an `EOL` at the same time. When one outing ends exactly where the next starts (companion or user markers), the whole-outing rule then emitted a zero-length second outing and left the rest of it outside any outing, and the beacon-pair rule dropped the first outing's in lap and the second's out lap.
**Decision.** At equal `t` the order is `EOL`, then `SOL`, then the other classes in declaration order; ties within a class are broken by `id` as before. Separately, the beacon-pair rule closes an outing that is still open when a new `SOL` arrives (an implied `EOL` at that time, with a nil end marker), and a missing final `EOL` is placed at the layer duration rather than at the latest marker of any kind.
**Consequences.** Only layers with coincident `EOL`/`SOL` markers sort differently; no public fixture has any. `markerSetHash` follows the new order for those layers.
**User must confirm:** no.

#### D-159 Shipped data: `<exe dir>/data` first on every OS (amends 01 §9)
**Context.** 01 §9 gives one shipped-data location per OS: `<exe dir>\data\` on Windows, `<prefix>/share/esses/data/` on Linux, `Contents/Resources/data/` in a macOS bundle. The build copies `data/` to `<build>/bin/data` on every OS, so Linux and macOS build trees could not find `units.json` (PR #1 `qt-app` ubuntu run: `esses-render`, both bench smokes and render parity failed).
**Decision.** After the `--data-dir` flag and `ESSES_DATA_DIR`, `esses::Paths` takes the first existing directory of: `<exe dir>/data` (build tree or portable layout, every OS); on Linux and on macOS outside a bundle `<exe dir>/../share/esses/data`; inside a macOS bundle `Contents/Resources/data`, then the `data/` next to the bundle. When none exists it returns the installed location so error messages name the expected path. Windows is unchanged.
**Consequences.** Build trees, portable layouts and installs resolve the same way on all OSes; `libs/base/tests/paths_tests.cpp` covers each layout. Installed packages (08 §7) must put data under `share/esses/data` or `Resources/data`, as before.
**User must confirm:** no.

#### D-160 DCO sign-off suspended while the maintainer is the only contributor (amends D-002, D-004)
**Context.** D-002 and D-004 require a `Signed-off-by:` line on every commit (DCO, no CLA). The sign-off is a personal certification, so an agent must not add it in the maintainer's name; PR #1 therefore failed the `dco` check on every commit. The maintainer is the only contributor today.
**Decision.** By the user's instruction on 2026-10-12 ("drop dco for now"), the requirement is suspended. The `dco` CI job stays in `.github/workflows/ci.yml` but runs only when the repository variable `ESSES_REQUIRE_DCO` is `true`. CONTRIBUTING.md, GOVERNANCE.md, AGENTS.md, CLAUDE.md and the pull request template say so. Before outside contributions are accepted, the maintainer sets the variable and the requirement applies again; no CLA either way.
**Consequences.** Commits made so far carry no sign-off and need no history rewrite. Re-enabling is one repository setting. The no-relicensing policy and the MIT licence are unchanged.
**User must confirm:** no (decided by the user, 2026-10-12).

### A.13 M2 (proposed, 2026-10-12)

These entries record where M2, as built, departs from what a design document or an acceptance criterion says (`docs/milestones/M2.md`). Each states the ADRs it amends; those ADRs carry an "Amended by" line.

#### D-161 `esses_qtmodel` links Qt Gui for the undo framework only (amends D-007, D-032; 01 §2.1–2.2, 06 §1)
**Context.** D-032 puts one `QUndoStack` on `ProjectSession` in `esses_qtmodel` and rejects a second, toolkit-free undo abstraction; D-007 and 01 §2.1/§2.2 rule 4 limit `esses_qtmodel` to Qt Core, on the premise that the undo framework is part of it. In Qt 6 `QUndoStack`, `QUndoCommand` and `QUndoGroup` belong to the Qt Gui module (they left Qt Widgets in Qt 6.0), so the M1 configure checks (`cmake/qt-free-check.cmake`, `tools/check_layering.py`) refuse them there, and M1 left the placement open (`docs/milestones/M1.md` §6 item 9). Options: (a) link Qt Gui in `esses_qtmodel` and restrict what may be used from it; (b) a Qt-free undo stack in `esses_project` behind a thin wrapper for the GUI; (c) move the stack and every command into `esses_widgets`.
**Decision.** (a), the smallest change that keeps D-032 as written. `esses_qtmodel` links `Qt6::Gui` and may include from Qt Gui only `<QUndoStack>` and `<QUndoCommand>` (also spelled `<QtGui/...>`); every other Qt Gui header (`QImage`, `QColor`, `QFont`, `QGuiApplication`, `QAction`, ...) stays forbidden there, and Qt Widgets, OpenGL, Svg and PrintSupport remain forbidden as before. `cmake/qt-free-check.cmake` allows the link; `tools/check_layering.py` rule 6 enforces the header list against the Qt Gui header directory the configure step records (`esses_targets.json` `qtGuiHeaderDirs`), with a self-test; the configure negative test that refused Qt Gui now refuses Qt Widgets (`lint.configure_check.qt_module_qtmodel_widgets`) and a positive case accepts Qt Gui (`qt_module_qtmodel_gui`). The undo commands (`libs/qtmodel/include/esses/qtmodel/undo_commands.hpp`) and their Qt Tests stay in `esses_qtmodel` and run under `QCoreApplication`: the undo framework needs no GUI application. The Edit-menu actions (`QUndoStack::createUndoAction`, which returns a `QAction`) are created in `esses_widgets`.
**Consequences.** D-032 holds unchanged: one `QUndoStack` per `ProjectSession`, commands wrapping the Qt-free documents, no second abstraction. Option (b) would have reopened D-032 and re-implemented merging, macros and the clean state; option (c) would have moved document logic into the widget layer and turned the AC-M2-09 property test into a widgets test. `esses_qtmodel`'s link closure now contains Qt Gui, which every executable that links it (`esses-render`, `esses`, the Qt tests) already had; the Qt-free libraries and `esses-cli` are unaffected. 01 §2.1, the outline's §1.1 table, 06 §1 and `AGENTS.md` §3 read "Qt Core, plus Qt Gui's undo framework (D-161)".
**User must confirm:** no.

#### D-162 `project-1` minor 2: Data-window filter keys are the hash text; the first real migration (amends D-019; 02 §6.2)
**Context.** 07 §5.1 asks for `SchemaRegistry` and `Migrations` "with one real migration". 02 §6.2's key table gives the Data-window filter keys as "`SourceId` hex or `default`", while its example and the M1 `project-1` example key them `xxh3:9f3c…`, the form every other hash of `project-1` (`recentSources[].hash`, `lastSelection.*.source.hash`) and `sidecar-1` (`source.hash`) uses; `project-1` minor 1 left the key form open. M2 is the first build that writes the filters (the Data-window filters moved from M1 to M2, D-044).
**Decision.** `project-1` minor 2 fixes the keys to `"default"` or the source's quick hash as `"xxh3:<32 lower-case hex digits>"` (`model::hashText`), enforced by the schema's `propertyNames`. The registered step `project-1` 1 -> 2 rewrites a key of 32 bare hex digits (either case) to that form, keeps an already prefixed twin when both exist, and moves every other key, with its filter, to `x-esses-legacy.dataWindowFilters` (02 §6.6 rule 3: nothing is deleted). `ProjectFile::kSchemaVersion` is 2; a minor-1 `project.json` loads migrated and dirty, and its first explicit Save keeps the minor-1 bytes in `backup/` (D-070).
**Consequences.** The whole migration path (migrate -> parse -> validate -> dirty -> backup) runs on a shipped document kind from M2, and `esses-cli validate` migrates minor-1 files before validating them. Whoever persists the Data-window filters writes `hashText` keys. 02 §6.2's key table reads "`default` or `xxh3:<32 hex>`".
**User must confirm:** no.

#### D-163 Load pipeline of the project documents: typed reader before the schema, newer minors kept or refused (amends D-019; 02 §6.1, §6.6; closes R-38)
**Context.** 02 §6.1 writes `Document<T>::load` as "validate -> migrate -> parse" and §6.6 rule 2 as "migrate -> validate -> parse". The M1 typed readers are lenient where the form does not matter (§6.6 rule 4: a missing `$schema`; a workbook without `id` or `name` gets one; an explicit `null` reads as absent) and strict on closed records, with the pointer of the offending member; the schemas require those members, so validating the raw input first would refuse what the readers accept and report other pointers. The members M1 keeps verbatim (`options`, `dataWindow`, `print`, unknown members) were checked by nothing on load. R-38 (M1 review): a document of a newer minor was written back as `schemaVersion` 1, and a member a newer minor adds to a closed record failed as `Invalid`.
**Decision.** (a) Every `esses_project` loader runs: JSON syntax -> header (another schema `Invalid`, another major `Unsupported`, absent header read as `<name>-1` and logged) -> the registered `Migrations` up to the kind's current minor -> the typed reader -> `SchemaRegistry` validation of the document as it will be written (the typed result serialised), `Invalid` with the schema's pointer. This checks the verbatim members against the schema and keeps the readers' leniencies; without a schemas directory the typed reader is the only check. (b) A newer minor is read as it is and its `schemaVersion` is kept on save, never downgraded; when the reader or the schema refuses it, the error is `Unsupported` with the original pointer ("written by a newer build ... left untouched"), so such a file is neither opened in part nor overwritten with a loss. (c) `esses-cli validate` runs the same header and migration steps without writing (02 §6.6 rule 5): a newer major fails at `/$schema`, the migration log is printed, every error carries its pointer.
**Consequences.** The invalid schema examples of the verbatim members fail on load with the schema's pointer; the pointers of typed members stay the readers'. Every document type carries `schemaVersion`. The sidecar (`esses_model`, below `esses_project`) keeps its own reader with the newer-minor rule it has had since M0.
**User must confirm:** no.

#### D-164 Maths dimension warnings: bare literals and undeclared result units (refines D-021; 03 §3.2 rules 4 and 10)
**Context.** 03 §3.2 rule 4 raises `W-DIM-MISMATCH` when the dimensions of the operands of `+ -` differ and both are known. A numeric literal is dimensionless, so `'Speed'[km/h] > 100`, `'Brake Temp'[deg C] - 5` or `max('Speed'[m/s], 0)`, the everyday form of pasted i2 maths where "numbers are numbers once annotated" (D-021), would each carry a warning. Rule 10 compares the item's `resultUnit` with the inferred dimension; `MathItem`'s defaults (unit `""`, quantity Unitless) cannot be told apart from an explicit unitless declaration, so every item and every `esses-cli eval` expression without a declared unit would carry `W-RESULT-DIM`.
**Decision.** (a) A bare numeric literal, and an expression of literals only, has no unit: it takes the dimension of the operand it meets in `+ -`, the relational and equality operators, `min`, `max`, `hypot`, `atan2` and the branches of `choose`, and raises no `W-DIM-MISMATCH` or `W-UNIT-SCALE` against it; in `* /` it leaves the other operand's dimension and unit unchanged. A constant, a detail or a reference keeps its unit and is checked as before. (b) The declared result unit is checked (`W-RESULT-DIM`, `I-RESULT-SCALE`) only when the item sets its unit or its quantity; `esses-cli eval` declares one only with `--unit`. Both readings are the `docs/maths-compat.md` rows `lang.literal-dimension` and `lang.result-unit`.
**Consequences.** Literal arithmetic does not erode AC-M2-01's share of Ok items; a mismatch between two annotated references, constants or details is still a warning. An item that leaves its unit and quantity at the defaults for a dimensioned expression gets no `W-RESULT-DIM`; its output is stored as the numbers, as before. Strict mode (D-021) is unchanged.
**Amended by (A.13):** D-185 (d) (a literal factor other than ±1 in `* /` makes the scale unknown: no `I-RESULT-SCALE` through a conversion by hand).
**User must confirm:** no.

#### D-165 Maths diagnostics beyond 03 §2.5, and the nesting limit (refines D-020; 03 §2.5, §14.3)
**Context.** 03 §2.5 lists the syntax codes to reproduce (`E-QUOTE-MISSING` ... `E-UNIT-UNKNOWN`) but none for other malformed input (an unexpected token, an empty expression, a stray character, an unclosed comment, a malformed number, an empty name) or for a call that cannot be evaluated, and nothing bounds the nesting the parser must accept, while the fuzz target of 03 §14.3 requires no crash and bounded memory.
**Decision.** Error codes added (ours): `E-SYNTAX` (any other unexpected token, saying what was expected), `E-EXPR-EMPTY`, `E-UNEXPECTED-CHAR` (outside quotes anything but the ASCII tokens, with a fix for typographic quotes and for `=`), `E-COMMENT-UNCLOSED`, `E-NUMBER-FORMAT` (`2x`, `1e`, out of range), `E-NAME-EMPTY` (`''`), `E-EXPR-TOO-DEEP` (a tree deeper than 256 levels or a parser recursion beyond 256), and `E-KERNEL-UNAVAILABLE` from `compile()` for a call that has no kernel. The parser recovers after every error (`Engine RPM` continues as `'Engine RPM'`, `'X' km/h` as `'X'[km/h]`, skipping to `)` or `,` otherwise), so one pass reports every independent error, in source order. The list is in `libs/maths/README.md` and `libs/maths/src/lang/codes.hpp`.
**Consequences.** The Maths Editor and Channel Status show a precise code for every input. Hostile input cannot exhaust the stack: the type checker is iterative and `print` and `compile` recurse at most 256 levels. An expression nested deeper than 256 levels, which no known maths does, must be split into items.
**User must confirm:** no.

#### D-166 `esses-cli eval` evaluates on the calling thread, over logged channels and details (amends 03 §13)
**Context.** 03 §13 has `eval` evaluate "on the TaskPool with synchronous wait". In M2 the language and the 0.1 whole-node evaluation (`evaluateWhole`, D-139) exist before `MathGraph::define`, the shadowing chains and `MathsScheduler`; on the whole-node path one evaluation is one task in any case.
**Decision.** `eval` resolves names through the CLI's scope over the opened source (channels by name, then by logged name; numeric and text details), compiles, and calls `evaluateWhole` on the calling thread (the CLI may block, 03 §7.4) at the plan's rate, else the layer's highest logged rate. Maths outputs (other items, the built-in `Corr Speed`/`Corr Dist`) are not resolvable in `eval` until it evaluates through `MathGraph`.
**Consequences.** The CLI runs the same plan and evaluation code as the application; its timing (`--timing`) is one core's. `eval` gains maths outputs and the TaskPool when `MathGraph::define` lands.
**User must confirm:** no.

#### D-167 Text engine as built: rules that differ from 05 §7 (amends D-029; 05 §7.1–7.3)
**Context.** The M2 text engine (`libs/formats/src/text/`, `docs/formats/text.md`) implements 05 §7. Writing it against the self-authored fixtures, the fuzz targets and the 300 MB benchmark showed rules of 05 §7 that cannot hold as written: the `@auto` names row needs a following line "≥ 80 % numeric", which a table with one text column in three never has; `decimal: auto` counts every numeric token, so whole-number columns hide a decimal comma; the rate search takes the smallest denominator within 0.1 %, which is 41/2 for 20.48 Hz; classifying columns from "the first 256 values" makes a column that is zero at standstill an integer column; `std::regex` has no portable multi-line flag (MSVC's `^` matches at every line start, libstdc++'s and libc++'s only at the start) and recurses per character on some libraries; 05 §7.2 writes `capture` as detail id → regex while its own legacy example maps file key → detail id; and the slab share key of §7.1 needs a way to name sibling columns for `ChunkCache::insertGroup` without keeping them alive.
**Decision.** (a) `@auto` names row: the next data line must be at least half numeric. (b) `decimal: auto` is `,` when, among the tokens that carry a separator, at least 90 % carry one `,` (whole numbers say nothing). (c) Rate inference checks each candidate of 05 §7.1 (smallest denominator first) against the sampled rows with the 0.6/rate rule and takes the first that fits. (d) Columns are classified from the first 256 rows and every 1,000th row; whole numbers give `Integer` (F32, F64 above 2^24), else `Real` (F64 above 7 significant digits); more than half non-numeric gives `Enum`. (e) The sniff regex sees the first 4 KiB with `^` anchoring at every line start on every library (a search, then for a pattern starting with `^` one attempt pinned to each later line start); every other profile regex sees one line or one name; subjects are cut to 4 KiB and a regex error counts as no match. (f) `capture` maps file key → detail id or `@input` when `keyValueColumns` is set, and detail id or `@input` → regex otherwise. (g) One slab decoder per source sweeps a chunk of a row set for every column: the requester's chunk goes to its builder, siblings whose threads already wait for the same sweep get theirs handed over, the rest are sealed with `insertGroup` through the new non-owning `store::WeakColumn`; the share key `slab/<sourceId>/<k>` is that decoder's in-flight table, not a TaskPool key; sparse columns, whose chunks do not align with the dense slabs, decode per column. (h) The `blocks` layout (D-102) is parsed and validated but opening such a file is Unsupported until the Pi profile (v1.0). (i) Text files are read through the mapping without the `.ld` reader's page-fault guard (the decode owns objects with destructors, which the guard's longjmp cannot unwind).
**Consequences.** Tables with text columns and decimal-comma files with integer columns open with the generic profile; 20.48 Hz exports read uniform without the details; results are the same on MSVC, libstdc++ and libc++, and pass 1 stays linear in the file (fuzz targets, 60 s each, clean). A file truncated in place while open may fault on POSIX; writers that replace files atomically are safe. `esses_store` gains `WeakColumn` (additive). The Pi profile and its fixture land with the `blocks` reader.
**Amended by (A.13):** D-190 (g): text files are read through the page-fault guard after all (windowed guarded copies in both passes); (f) and (i)-(k) add the ragged sparse cell, the quoted line break, the odd UTF-16 byte and the regular-expression limits.
**User must confirm:** no.

#### D-168 Open-pipeline step 4, the Mapping-panel hand-off and the shipped text profiles as built (amends D-028, D-029; 05 §2, §8)
**Context.** 05 §2 step 4 lists the dictionary (4a), the unit map (4b) and the resolver's Mappings (4c) and ends task A when "a unit or name" is unresolved; 05 §8.2 calls a profile complete "when every channel has a quantity and unit". Read literally, every text file without a units row (most generic CSVs) would stop at the Mapping panel for every column. The open pipeline of M0 also refused timestamped channels, had no way to apply the text profile's column overrides, and the registry had no data directory to find the shipped profiles in.
**Decision.** (a) Step 4a applies the reader's column hint (the text profile's `columns`: name, quantity, unit, interpolation, kind, dtype) and then the importer's dictionary entry (`IReader::columnHint`, `IReader::dictionary`; `ImportOptions::applyDictionary`). (b) Step 4b tries the importer's unit map first (`ImportOptions::unitMap`, else `OpenContext::unitMaps[<file stem>]`, most specific rule first), then the unit registry; a rule's `analysisName` becomes a load-time Mapping. (c) Step 4c applies `OpenContext::mappings` and those analysis names with `model::applyMappings`. (d) A channel is **unresolved** when its vendor unit is known to neither, or when a hint or the dictionary names a quantity without a unit; a channel with neither unit nor quantity opens unitless and does not stop the open. With a `MappingHook`, task A ends with a `PendingResolution` (rows with up to five decoded values); `resume` runs task B on a copy of the state with the `MappingDecision` (its unit map, its mappings; what stays unresolved opens unitless; `saveUnitMap` is the caller's to honour, the registry knows no project). Task keys `open/<path>/<size>/<mtime>/a` and `/b`. (e) Timestamped channels open as `TimestampedSeries`, the time column from `IReader::makeTimesProvider` (channels that share a provider share one I64 Column) with `IReader::timeRange`, else from `readTimestamps`; `IReader::attachColumns` gives a reader its Columns for `insertGroup`. (f) `ImporterRegistry::withBuiltins` registers the shipped `motec.csv` and `generic.csv` profiles with their dictionaries (build-time embedded copies of `data/profiles-text/` and `data/dictionaries/`) and the `motec.csv` and `csv` writers; `addProfile` refuses an id collision and any `importerId` but the built-in `motec.csv`, so `SourceLoader::registerTextProfiles` adds the user's profiles only. (g) fixture-1 gains the oracle kind `generator` (the values a fixture generator printed into a self-authored text input, `tools/gen_text_fixtures.py`). (h) The first-paint budget of the 300 MB CSV (07 §12) is measured by the Qt-free `bench/formats_bench` up to "every chunk of 8 channels resident" (the formats share); the graph's build and raster of the eight traces is measured by `open_bench`.
**Consequences.** AC-M2-11's blocking rule holds for files that state units the registry does not know; unit-less generic CSVs open at once and are mapped later. The registry's M2 members are implemented (`addProfile`, `load(…, MappingHook)`, `resume`); `OpenContext` gains `mappings` and `unitMaps`, `IReader` five defaulted hooks, `open_code` `open.unitmap_load_failed`. The registry tests that asserted "no text importer" in M0 now assert the M2 set.
**User must confirm:** no.

#### D-169 IIR filters designed in-tree, not through Iir1; the Chebyshev reading (amends D-124, 03 §1, §5.3; refines D-082)
**Context.** 03 §1 lists Iir1 (MIT, vendored) as a dependency of `esses_maths` and 03 §5.3 builds the Chebyshev filters "via Iir1"; D-124 compiles it from vendored sources. Iir1 is not vendored (`third_party/README.md`), and adding a dependency needs the maintainer's approval for the download (`AGENTS.md` §2). 03 also leaves open what the ripple percentage measures, where `fc` lies for a Chebyshev filter, how the gain is normalised and what the order of a band filter counts.
**Decision.** (a) The IIR kernels (`libs/maths/src/kernels/iir.cpp`) design and run every filter in-tree: a filter is a list of passes, each a cascade of second-order sections in transposed direct form II with F64 state, run forward or backward over the whole extent (03 §6.0). The first-order RC sections are the classic discretisation (low pass `y += a (x - y)`, `a = dt / (RC + dt)`; high pass `y = b (y' + x - x')`, `b = RC / (RC + dt)`; `RC = 1 / (2 pi fc)`). The Chebyshev type I filters come from the normalised analogue prototype (poles from the textbook formulas; ripple 0 gives the Butterworth poles) through the bilinear transform with pre-warped edges; band pass and band stop use the prototype of the given order (twice its poles), centred on `sqrt(f_lo f_hi)` of the pre-warped edges with their difference as width. (b) Readings (`docs/maths-compat.md` `iir.rc-form`, `iir.cheby-design`, `iir.priming`, `iir.nyquist`): ripple r % makes the pass-band gain ripple between 1 and 1 + r/100; `fc` is the edge of the ripple band (Butterworth: the -3 dB point); every section is normalised to unit gain at DC (low pass, band stop), at half the rate (high pass) or at the centre (band pass), so a constant passes a low pass unchanged; a pass is primed section by section with the steady state of its first valid input sample (backward: its last), holds its state through invalid samples and re-primes after a gap of more than 1 s; a cutoff at or above half the result rate is `E-ARG-VALUE` at evaluation (D-170). (c) `tools/maths_ref.py` checks the design independently: the whole transfer function multiplied out and run as one direct-form-I recursion, closed forms (the Butterworth biquad, -3 dB at `fc`, the ripple peak, unit gains); the case corpus compares the two on every order, ripple and band.
**Consequences.** `esses_maths` gains no third-party dependency; 03 §1's Iir1 entry and D-124's "compiled Iir1" no longer apply (the CMake comment of `libs/maths` says so). Results will not be bit-identical to i2's filters; the readings are "chosen, not observed" until compared on the maintainer's install (D-038). An Iir1 port, if the maintainer wants one later, replaces the design behind the same kernel and is checked by the same cases.
**User must confirm:** no.

#### D-170 Maths kernels as built: failures known at evaluation, `integrate_dist`'s Corr Dist input, the case corpus (amends D-022, D-033; 03 §6.1, §14.1)
**Context.** 03 §6.1's `Kernel::run` cannot fail, yet some failures are known only when a node is evaluated: a filter cutoff at or above half the result rate (the rate is the item's or the highest input rate), and a range path, which 03 §2.4 rule 3 validates against the source's `RangeGroupTree` at evaluation (`E-RANGE-UNKNOWN`, listing the available paths). `integrate_dist` reads the source's `Corr Dist` (03 §5.4), which is not one of its arguments, so a node version that hashes only its plan's inputs would not follow it. 03 §14.1 puts the case files in `tests/maths/*.case.json` with expected `values` plus `valid` flags, no layer extent, no way to state a table item or an expected failure, and has the expected arrays of windowed, stateful, IIR and resampling cases generated by a brute-force evaluator under `tests/maths/reference/`.
**Decision.** (a) `Kernel` gains `virtual Result<void> status() const` (default success): `evaluateWhole` checks it after `reset()` and after `run()` and ends the evaluation with that error, its diagnostic code in `context.detail` (`E-ARG-VALUE`, `E-RANGE-UNKNOWN`), which the host turns into the node's Error status. `analysis::RangeTables` gains `contains(path)` and `paths()` for that check and its message; paths match exactly. (b) `compile()` appends the scope's `Corr Dist` (its chain head, base metres, gathered `Linear`, F64) as the last input of an `integrate_dist` stage, so the node version and the plan's inputs include it; without it the item is `E-NO-DISTANCE` at compile time. (c) The case corpus is `tests/maths/cases/<id>.case.json`, schema `maths-case-1` (written in M2): `durationUs` (the layer extent), an optional `item` (a `table2d` whose axis expression is the case's expression), `constants`, `details`, `ranges` (with their parent groups), `selection` (Main and reference with offsets and stretch), and `expected` with the result `rate`, `values` with `null` at invalid samples and a `tolerance`, or an `error` code, plus diagnostic counts. Every expected value comes from `tools/maths_ref.py`, an independent per-sample reference in Python written from 03 §5 and `docs/maths-compat.md` (it replaces `tests/maths/reference/`; `--check` confirms the committed files, `--self-test` checks the reference); the Catch2 test `maths cases: every tests/maths/cases/*.case.json passes` runs each file through `Parser`, `compile` and `evaluateWhole`. (d) `mathfn-manifest-1` (written in M2) is the shape of one `FunctionManifest`; the intrinsic manifests take their arguments and arities from the language's signature table and validate against it.
**Consequences.** Channel Status shows the precise code of an evaluation-time failure; `Corr Dist` edits re-version `integrate_dist` nodes; AC-M2-02 is one ctest entry over the corpus (238 cases at M2) with the readings of `docs/maths-compat.md` named by case id. A case file is regenerated, never edited by hand: changing a reading changes `tools/maths_ref.py`, the files it writes and the compat row together.
**User must confirm:** no.

#### D-171 `MathGraph` as built: the ends of a shadowing chain, disabled elements, conditions, the descriptor unit and the M2 graph surface (refines D-022, D-024; amends 03 §4.2–4.3, §7.1, §8, §9.2)
**Context.** 03 §4.3 orders a name's chain "logged → local self-maths → global self-maths (module order) → consumers" but places neither the built-in outputs (`esses.gps_corrections` shadows the GPS latitude and longitude, 03 §9.2) nor the transient module (filters made from a channel label, 03 §4.2), and says nothing about a disabled element. The module `condition` is "an expression over details, Bool scalar" without saying how it is evaluated or what a condition that cannot be evaluated does. Alias fall-through (03 §3.3 step 2) skips "a candidate whose chain head has status Error", but names resolve when the graph is built and most Error states are known only after an evaluation. 03 §7.1's surface has one `ChannelKey` per name, so Channel Status cannot list shadowed elements or ignored duplicates, and has no Recalculate (`MathsService::invalidate`, 07 §5.1) and no setter for the project's mappings, aliases or compat switches. 03 §8 gives a maths output `unit = resultUnit`, while `compile()` stores the values in the base unit of the result unit (03 §6.1). 03 §9 has no way for a source to override a built-in module's project settings or to show "Disabled (no GPS channels)".
**Decision.** (a) Chain order: logged N → built-in outputs named N (registration order) → local items → global items (project order; the first enabled item of a name per module, later ones `W-DUP-IGNORED`, status Warning, never evaluated) → transient items: the GPS cleaning applies before any user maths, a quick filter on top of everything. (b) A disabled element (item or module disabled, condition false, a built-in output without its required inputs) stays in its chain with status Disabled and is skipped: consumers read the last enabled element, a self-maths element the enabled one before it, else the logged channel. (c) The condition is type-checked against the source's details and the module's own constants and must fold to a constant; one that does not parse or fold counts as false, with `E-CONDITION` on the module's items. (d) Alias fall-through skips a maths candidate whose definition has an error (syntax, type, compile, cycle); errors found only by an evaluation do not move an alias. (e) A maths output's descriptor carries the unit its values are stored in: the base unit of the declared result unit, else the expression's inferred unit; self-maths items (filter, scale_offset, an expression without a declared unit that reads its own name, a built-in output named like an input) keep the shadowed channel's descriptor (unit, quantity, DPS, colour) as 03 §8 says. (f) A `builtin` item of the local maths (else of an enabled global module) overrides the registered module's settings for its source; a disabled one disables the module's outputs unless `IBuiltinModule::disabledItemDisablesOutputs()` is false (`esses.gps_corrections`: `enabled` gates the v1.0 steps only). `BuiltinOutput` gains `required` and `missingSummary`; prefixed GPS outputs are stored in the Angle base unit. (g) Surface: `MathGraph` adds `nodes()` (`MathNodeInfo` with the `MathInput` of every referenced name and its `ResolutionTrace`), `status(NodeId)`, `setResolverTables`, `setCompat`, `setDistanceIndexes`, `invalidate(moduleId)` and `evaluationCount()`; `NodeId::kIgnored` marks an ignored duplicate; `define`, `remove` and `setEnabled` act on the graph's copy of the module; `ColumnHandle` adds `selectionKey`; `MathsScheduler::submit` keys a task `<prefix>/all` (`/c<n>` on the v1.0 chunk path). New codes: `E-CONDITION`, `E-GPS-SETTINGS`, `E-GPS-NO-CHANNELS`; `E-EXPR-TOO-DEEP` also for maths items that read each other more than 256 levels deep. (h) Node versions also hash the node's identity (origin, module, name), its result rate and a definition error's text; tasks of one version and selection share their sealed result, so a require that races a completion never evaluates twice.
**Consequences.** Disabling an item restores what it shadowed; the Channel Status window and the Maths Editor get one row per item with its resolution; Recalculate re-evaluates without re-versioning. A Warning-only definition shows its diagnostics while Pending. Readings (a)–(d) are chosen, not observed (D-005 (3) permits the observation; not run yet), and are listed in `CLEANROOM.md`.
**User must confirm:** no.

#### D-172 Undo, persistence and the journal in `esses_qtmodel` as built: whole-document commands, view state kept out of undone edits, workbook and Mapping-panel edits, dirty state, save and autosave rules (refines D-019, D-032, D-070; amends 02 §6.8, §7)
**Context.** D-032 and 02 §7 list the undoable edits but not how a command restores a document, and 02 §7 lists `linkTo`/`domain` among the undoable workbook edits while D-032 and 06 §1 rule 4 make the domain view state. A `SetProjectOptionsCommand` that restored the whole `project.json` would also rewind what lives there but is not an edit: the last selection, the Data-window filters, the MRU and the lap-stretch toggle (view state, 02 §7). 06 §5.2 makes the Data window's trust toggle an undoable `SetRangeTrustCommand`, but `DataSelectionModel` does not know the `SourceLoader`. 05 §8.1 makes the Mapping panel's decisions undoable project edits without saying which documents they touch. 02 §6.8 says the journal holds "every dirty project file" without defining dirty, and says nothing about files the project stops naming (a renamed or removed workbook, module or unit map), about a document that could not be read, or about autosave while the recovery prompt is open. The track-map document (sections) arrives in M3 and the scheme editor at v1.0 (06 §13.9).
**Decision.** (a) Every command holds the document before and after by value (`DocumentCommand<Doc>`, `SidecarCommand`); replays are exact by construction (AC-M2-09's property test checks 30 randomised sequences against an independent replay). (b) `ProjectSession::setProjectFile` keeps the session's `lastSelection`, `recentSources`, `dataWindow`, `options.lapStretch.enabled` and the members other documents own (`workbooks`, `mathsOrder`): undoing an options edit never rewinds view state. (c) The F9 domain toggle stays view state (D-032 wins over 02 §7); a worksheet's persisted `linkTo` and default `domain` change only through workbook edits. (d) The workbook list is a document of its own (`WorkbookSet`: documents and file names) with add, remove (never the last), rename (the file follows the name; the layout follows the workbook id, D-019) and move commands; worksheet add/remove/rename/move and component paste/delete are `WorkbookCommand`s, a paste giving fresh component ids. (e) `connectSourceLoader` turns `DataSelectionModel::setTrusted` into a `SidecarCommand` on the session's stack when the selection has a session. (f) `applyMappingDecision` pushes one macro, "Resolve mapping": the decision's analysis names as project Mappings rows and, with Save, the unit map of the importer (`SetUnitMapCommand`); then task B runs. (g) A document is dirty for the journal when its canonical text differs from the text read from its file; project.json counts its view state, so the journal keeps the selection (06 §5.5). `isDirty()` is true after any edit and false again at the stack's clean index, except after a recovery or a migration, which only a save clears. (h) Save writes only documents whose text changed (D-070 backup of the previous file), retires the files of workbooks, modules and unit maps the project no longer names to `backup/` and deletes them, never writes over a document that existed but could not be read, and clears the journal only when every write succeeded. (i) Autosave appends nothing when the dirty set equals the last entry, and nothing while a recovery decision is outstanding (`JournalRecovery::Later` keeps the journal untouched); the prompt is either a hook (`setRecoveryPrompt`) asked on open or the `recoveryAvailable()` signal. (j) Track-map section edits and scheme edits get their commands with their documents (M3, v1.0).
**Consequences.** Undo/redo never disturbs selection, filters or the stretch toggle; a crash loses at most one autosave interval of edits and selection; an unreadable file is preserved for the user to repair; a project folder holds no stale workbook or module files after a save. The view state in `project.json` makes a selection change "dirty" for the journal but not for the Save prompt.
**Amended by (A.13):** D-187 ((h) Save journals first and clears only its own session's files, a case-only rename is never retired; (i) autosave continues beside a deferred recovery, Recover adopts the replayed journal); D-190 ((a)-(b), (d): applied entries are not replayed, ordered and atomic clears, session-tagged file names, project.json only after every other document, the backup copy first, one replay per open).
**User must confirm:** no.

#### D-173 `MathsService` and Channel Status as built: GPS corrections where GPS is logged, local maths from the sidecar, the live check's scope, the rows and kinds of Channel Status (refines D-013, D-016, D-022; amends 03 §9.2, §11, 06 §10)
**Context.** 07 §5.1 registers `esses.gps_corrections` with its zero-fix step always on; registered for every layer, a source without GPS shows two Disabled outputs ("GPS Latitude", "GPS Longitude") in the Values window, the Channels tree and Channel Status. 03 §4.2 keeps local maths in the sidecar's `localMaths`, while the M2 contract gave `MathsService::setLocalModule` as the way in. 03 §11 item 2 checks the Maths Editor's text "against the Scope of the current Main source", but `MathGraph` exposes no scope for text that is not an item. 06 §10 lists the kinds Expression, Filter, Alias, Mapping, Built-in and Constant and the "(missing)" display of missing workbook references without a kind for them. A completion that applies its finished upstream nodes too can announce one output twice.
**Decision.** (a) `esses.gps_corrections` is registered for a layer that logs both inputs of its default settings (by name or logged name); its outputs then shadow the logged ones as 03 §9.2 says. (b) A source's local maths is its sidecar's `localMaths` of each layer, re-read on every new snapshot (so `SetLocalMathsCommand` takes effect through the sidecar commit); `setLocalModule` overrides it for every layer until cleared. (c) The live check (`MathsService::check`, `LiveDiagnostics` with the 150 ms debounce) resolves, in 03 §3.3 order, the Main source's enabled maths outputs, its logged channels through the project's mappings and aliases (`model::ChannelResolver`), its details and the enabled global modules' constants; without a Main source it is the data-less scope (`I-UNRESOLVED`). (d) Channel Status rows: every maths item and built-in output (shadowed elements and ignored duplicates included), the constants of the enabled global modules (Source "Project"), every alias per open source (resolved through the logged channels or, failing that, a maths candidate), every mapping per source that logs its name (an unused mapping is one "Project" row, "Not used") and every channel a workbook component references that resolves nowhere (kind "Reference", "(missing)"); item kinds also show "Scale & Offset", "2D Table", "3D Table", "Plugin" and "Regression". Status texts follow 03 §7.3; refreshes are coalesced to 10 Hz and emit `dataChanged` when the rows stay. (e) `channelReady` fires once per (output, version), whichever completion applied it; `require()` announces Pending → Evaluating. (f) `connectMathsSelection` feeds the selection (a `MathSelection` keyed over Main, reference and overlays) and the Main/reference distance indexes to every graph; `connectProjectSession` feeds the modules, compat switches, mappings, aliases and corrected-speed settings.
**Consequences.** Sources without GPS look as in M1; local maths edits are undoable sidecar edits; the editor's live line matches what the graph will report for logged channels, details and constants (a maths item's own scope adds its module's constants and self-maths, which the check does not model). Readings (a) and (d) are ours, not observed.
**User must confirm:** no.

#### D-174 Lap stretching in 0.1: the track length is the source's `Venue Length` until track maps; factors published as `Range::stretch`; the toolbar toggle is view state (refines D-016, D-140)
**Context.** D-016 stretches each lap "to the track length" within a tolerance; the track length is the track map's (concepts research), and track maps arrive in M3. `Range::stretch` is "derived by LapStretch, never persisted", and the distance chains, the domain mappers and the maths selection already multiply by it, while `DistanceIndexKey` includes the stretch so a change would rebuild every index. 02 §7 makes the stretch tolerance an undoable options edit but does not place the toolbar toggle.
**Decision.** (a) The track length of a source is an explicit value (`LapStretchService::setTrackLength`, the track map's from M3) else its `Venue Length` detail (from the file's venue block, in metres); without one nothing is stretched. (b) `LapStretchService` computes each lap's factor from its unstretched `DistanceIndex` (`analysis::LapStretch::row`) and publishes the factors as `Range::stretch` through `SourceLoader::setRangeStretch` (a new snapshot, group versions unchanged, so nothing that reads the ranges is invalidated); a lap whose index is pending keeps its factor until it arrives. (c) `DistanceService` copies a published index with the new scalar when only the stretch of its key changed (D-140: a shared index never changes), so a factor change rebuilds nothing. (d) The toolbar toggle (`data.lap_stretch_toggle`) is view state: written to `options.lapStretch.enabled` without the undo stack (like the filters); the tolerance stays an undoable options edit.
**Consequences.** Laps line up in distance mode from M2 for logs that carry a venue length; M3's track maps replace the source of the length without other changes.
**User must confirm:** no.

#### D-175 Channels-window search: the query is split like the names; loose is a substring per word, still in order (refines 06 §7 item 3)
**Context.** 06 §7 item 3 splits the query on whitespace only, yet its example `temp (ecu)` must find `Water Temp (ECU)`, whose words are split on brackets; "loose name match" switches "each word to a substring match" without saying whether order and distinctness still hold.
**Decision.** The query is split on the same separators as the names (whitespace, `_`, `-`, `/`, `(`, `)`, `[`, `]`); each query word must match a distinct name word after the previous match (a greedy left-to-right match); loose matching replaces the prefix test by a substring test and keeps the order. The option is `options.general.looseChannelSearch` (`ProjectSession::looseChannelSearch`).
**Consequences.** `W S FL`, `temp (ecu)`, `pres front` behave as 06 describes; `speed wheel` does not find `Wheel Speed FL`. The Channel Editor's filter and the Maths Editor's auto-complete (v1.0) use the same function.
**User must confirm:** no.

#### D-176 (number not used)
The number was reserved while the M2 work was split between parallel tasks and no entry was written under it; it
stays unused so that the later numbers keep their references.

#### D-177 The maths editors as built: auto-save on close through the project save, the apply and close rules, the item pages of 0.1, Channel Status and Channel Editor routing, the active-channel commands (refines D-013, D-019, D-021, D-027; amends 03 §11, 06 §10, §13.1, §13.2, §14.4)
**Context.** D-019 saves the maths modules "on editor close", 03 §11 item 9 writes the module file there, and 06 §13.2 item 6 adds "and on every Apply"; `ProjectSession` saves the project's documents together (backups, the journal cleared, the clean index of the undo stack) and keeps the module order in `project.json`, so a module file alone cannot be written consistently. 03 §11 item 3 refuses to close "while the edited item has an Error" without saying whether an item stored with an error (pasted, imported, or broken by a renamed channel) traps the dialog, nor whether Add creates an item before it is valid. 03 §11 item 8 and 06 §13.2 item 3 list item pages and fields (2D table grid, built-in settings, enumeration, Logged/Telemetry, the full list "in a popover") that 07 §5.6 lets slip or that need documents not built yet. An item has one status per open source, Recalculate is per module in `MathsService`, and transient maths has no document to edit. 06 §10's double-click opens "the Maths Editor at the item, the Alias editor or the Mapping panel" without saying how a dock reaches them; 06 §13.1 lists "every channel of every open source" without the maths outputs whose units (Corr Dist) item 5 makes editable there. 06 §14.4's active-channel rows act on "the focused component's active channel", which a component may leave unset, and CTRL+SHIFT+Z takes "the displayed value at the cursor" (03 §4.2) without saying what a second zero does.
**Decision.** (a) Auto-save on close: when the dialog pushed a global-module edit since it was opened and a project is open, closing it calls `ProjectSession::save()` (atomic writes, D-070 backups, journal cleared); nothing is saved after an Apply (the journal covers it), and closing without a module edit saves nothing. Local maths is in the sidecar at every commit; transient maths is never saved. (b) Apply, leaving the item (another row, module, `openAt`, a structural edit) and closing apply a page with unapplied changes when it has no Error; with an Error, the apply, the switch and the close (button, window close and ESC) are refused with the diagnostic in a bar that offers Revert. Only unapplied changes block: an item stored with an error can be shown and left. A new item or constant exists only once applied (Add opens a dirty page; Revert drops it). Warnings and Info never block. (c) Item pages of 0.1: Expression (name, quantity, result and display units, rate Auto or Hz, DPS, colour index, interpolation, comment, the expression), Constant (name, value, unit, comment), Filter (channel, points or seconds) and Scale & Offset (channel, scale, offset with its unit); 2D/3D tables, built-in, plugin and regression items are listed and preserved but not edited; the enumeration name and the Logged/Telemetry flags are kept as stored; the full diagnostics list is a list under the line, not a popover. The status column shows the Main source's status (the source of a local module for its items); Recalculate invalidates the current module (`MathsService::invalidate`). The Transient node lists the filters of channel labels read-only. The Filter and Scale & Offset pages check the channel they read (`'Name'`: E-UNKNOWN-CHANNEL with data, I-UNRESOLVED without). (d) Channel Status: double-click or ENTER emits `openItemRequested` and invokes `tools.maths_editor [module, item]`, `tools.aliases [alias]` or `tools.mappings [logged name]` when registered and enabled (the mapping's logged name is the first quoted name of its "Resolved to"); built-in outputs and missing references open nothing; the filter box is the Channels-window word-prefix search on the channel name or a substring of the other text columns; "Errors only" keeps the rows with `ErrorRole`. (e) Channel Editor: the rows are the channels of every open source plus the maths outputs that are not self-maths of a logged channel (Category "Maths"); without an override the Unit cell is the stored unit; a display unit of another family than a file's channel is not applied to that file's row; at edit time a conflicting unit is refused with the file names (the at-open removal of 06 §13.1 item 6 is the open pipeline's, not the editor's). The Aliases and Mappings editors are public classes (`AliasesDialog`, `MappingsDialog`). (f) Active channel: `ActiveChannelCommands` registers the nine M2 `channel.*` rows and acts through a `Host` the shell supplies; the active channel defaults to the first visible trace; next/previous skip hidden traces; hide moves the square to the next visible trace and remove to the neighbouring visible one, dropping an emptied group; CTRL+SHIFT+F replaces the transient Filter of the channel (one per channel) and keeps the transient module itself, since `MathsService` only takes whole modules; CTRL+SHIFT+O and CTRL+SHIFT+Z replace or adjust the channel's `scale_offset` item of the local maths instead of stacking a second one (which W-DUP-IGNORED would ignore); zero at cursor uses the value as displayed (`ValuesModel`, rounded to the channel's decimals) in the trace's display unit, offset applied as a delta, so a second zero subtracts what is displayed now. (g) `ChannelEditor` keeps its header in `include/esses/widgets/editors_data/` and is implemented in `src/editors_maths/`.
**Consequences.** A maths edit reaches disk with the rest of the project's edits when the editor closes (the same as Ctrl+S then); the editor can always be closed after Revert; an empty "New Expression" never lands in a module. The table, built-in and enumeration pages, auto-complete and Insert Function remain v1.0 (or M3 for the table grid, 07 §5.6). Channel Status and the editors meet through commands, so the dock works before and after the shell wires the editors. Zeroing twice converges on the displayed zero; a filter from a channel label can be changed without accumulating filters. Readings (b) for stored errors and (f) are ours, not observed.
**Amended by (A.13):** D-190 (c) (the save on close writes the modules and their order only, through `ProjectSession::saveMathsModules`).
**User must confirm:** no.

#### D-178 The shell's M2 wiring as built: Options, schemes, Edit-menu undo, text-field chords, the recovery prompt and leaving a project; keymap rows of the milestone in progress (refines D-019, D-032, D-161; amends 06 §2.1, §13.7, §14.2, §16, §18)
**Context.** 06 §13.7 persists every option in `project.json` "unless marked *user*", but "Show scheme selector on toolbar" is neither in 02 §6.2's key table nor marked, and "Follow OS light/dark" is *user* without saying what ends it (06 §16: "unless the user chose a scheme explicitly"). The lap-stretch tolerance (D-016, `options.lapStretch.tolerancePct`) is an undoable option (D-174) with no editor in 0.1 (the Corrected Speed dialog, 06 §13.6, is later). The Fonts and Keys tabs may slip (07 §5.6), and Keys "Import" does not say when an imported keymap applies, as the binder is built at start-up. D-161 creates the Edit-menu actions with `QUndoStack::createUndoAction`, while 06 §1 rule 5 makes every action a registry command with no shortcut of its own. 06 §13.2 item 6 makes CTRL+Z inside a text field text undo, but the router of 06 §14.2 (M1) sends CTRL chords to the binder first, also in other windows. 06 §18 Exit saves or asks for a dirty project only, yet the last selection and the Data-window filters are view state in `project.json` that marks nothing dirty (D-172 (g)), so a plain exit would lose what AC-M2-06 restores; it does not cover leaving a project for another. Step 3 asks Recover / Discard before steps 5-6 without saying how the start-up continues meanwhile or which selection is restored after a recovery. `check_keymap_coverage.py` refused an implemented row whose milestone is after the current (closed) one, so an M2 command with a handler could not get its `KEYMAP_TEST` row before M2 closes.
**Decision.** (a) Options (`OptionsDialog`, window-modal, geometry in `layout.json` `dialogs.optionsDialog`): Apply and OK write the project options the user changed, and only those (an absent member stays absent), as one `SetProjectOptionsCommand` "Change options", never the view state; an invalid field refuses Apply and OK with the reason. The Analysis tab also edits `lapStretch.tolerancePct`. User settings: `language` (English only), `ui.schemeSelectorOnToolbar` and `scheme.followOs`, both off by default. Fonts and Keys are built read-only in M2; Keys imports a `keymap-1` file into `<config>/keymap.json` after checking it against the shipped keymap for conflicts (it applies at the next start), exports the effective keymap and reveals the configuration folder. (b) Schemes (`SchemeSelector`): the shipped light and dark schemes, then the user's `<config>/schemes/*.scheme.json` by name (a file that does not load or repeats a listed name is left out with a Messages row); an explicit choice turns "follow OS" off; following the OS applies the shipped scheme of `QStyleHints::colorScheme()` when the OS reports one; Duplicate writes "<name> copy" to the user folder and selects it. The application palette is mapped from the active scheme (06 §16 rule 6) at start-up and on every change. (c) Undo: Edit > Undo/Redo and the toolbar buttons are the registry commands `edit.undo`/`edit.redo` (`registerEditCommands`) shown through `CommandActions`, their tool tips naming the edit; `createUndoAction` is not used. A component edit from the canvas is a `WorkbookCommand`; after every document change (undo, redo, recovery) the shell re-syncs the workbook selector, the worksheet tabs and the shown worksheet. The platform's text-editing chords (undo, redo, cut, copy, paste, select all) in an editable text field go to the field first, wherever it is. (d) Recovery and leaving: the prompt is a window-modal `RecoveryDialog` (Recover / Discard / Later; closing it is Later) opened once `openProject` has returned, never a nested event loop; `SessionRestore` (the `-l` logs, which `esses` now hands to `openProject` with `-w`, else the previous data, then the selection) starts after the answer, so a recovered `lastSelection` is the one restored. Leaving a project with edits (exit, or opening another project) saves it when `options.general.saveOnExit` is on (the default) and asks Save / Discard / Cancel otherwise or when the save failed; Discard also deletes the journal. Leaving with view-state changes only writes them without a question, unless a journal was put off with Later (it stays for the next open). The window title marks a dirty project with "*". (e) A keymap row may be `implemented` while its milestone is the one in progress (the one after `CURRENT_MILESTONE`); later milestones still fail; a menu-only row is tested by triggering its `QAction` (`tests/ui/keymap/tst_keymap.cpp`).
**Consequences.** Undo never disturbs the selection, the filters, the stretch toggle or the scheme; a text field keeps its own undo; a normal exit keeps the selection and filters AC-M2-06 restores, a kill keeps them in the journal; the scheme choice is per user, as 06 §16 says. M2 rows (`edit.undo`, `edit.redo`, `tools.options`, `data.lap_stretch_toggle`, `view.messages_window`, `view.reset_docks` so far) turn implemented with their tests as they land, and when M2 closes `CURRENT_MILESTONE` becomes M2 and every M2 row must be implemented. Readings (a) and (b) are ours, not observed.
**User must confirm:** no.

#### D-179 The data editors and the Data/Channels dock work as built: the Mapping panel's queue, Save and unit-map rows, the Lap Editor's rows and beacon edits, typed details, the filter UI and the proof of AC-M2-05/-11 (refines D-028, D-014, D-015; 05 §8.1, 06 §5.4, §6, §7, §13.3, §13.4, §13.8)
**Context.** 06 §13.8 and 05 §8.1 describe the Mapping panel's actions without saying what Save does to the pending
open, whether the "Apply to future files" column limits what is saved, where an analysis name is stored, or how
several pending opens share the panel; 06 §13.3 lists the Lap Editor's functions but not how ignored beacons sit in
the table, how a per-beacon offset applies to a user beacon (marker edits apply to the logged markers only, 02 §4.3)
or what "pending" means while no worker resolves `offsetDistance` (02 §4.3 behaviour 7a); 06 §13.4 does not say
which details are rows or how a typed number keeps its digits; 07 §5.2 proves AC-M2-05 and AC-M2-11 in
`tests/ui/lap_editor` and `tests/ui/mapping_panel`, which drive the shell, whose M2 wiring lands separately.
**Decision.** (a) The Mapping panel follows `SourceLoader::mappingRequired()` itself and queues further pending
opens behind the one it shows. Proposals come from the project's unit map of the importer, then the units table,
then the inferred quantity; units are offered as the canonical symbols of the row's quantity (compat units left
out). Resolve applies every row to the open and writes only the rows ticked "Apply to future files" into
`channels/unitmaps/<importerStem>.json`, as one "Resolve mapping" step; analysis names become project Mappings
(D-013), never unit-map `analysisName` members, so one mapping serves every importer. Save puts the complete rows
into the unit map (an undoable document edit written by the project's save and the journal) and leaves the open
pending: only Resolve or Open Anyway completes it (06 §13.8 item 4); AC-M2-11's "resolved or saved" reads as
"resolved, the answer optionally saved". Open Anyway applies the complete rows and opens the rest unitless. 05
§8.1's Status column is kept beside 06 §13.8's columns; Load and Save As (05 §8.1) are not built in 0.1. (b) The
Lap Editor lists the laps of the profile's primary group and, as greyed rows of class "Ignored", the ignored
beacons of its marker group (IGRDBCN, or a beacon with an ignore edit), in session-time order; absolute inserts are
in session time. A per-beacon time offset applies to the start beacon of the highlighted lap: a marker edit for a
logged or companion beacon, a move for a user beacon. The beacon offset by distance is entered and stored in
metres (`dMUnit` "m"), not in the Corr Dist display unit, and stays "pending" while no resolver exists (the laps do
not move; `docs/milestones/M2.md` open issues). Trust edits go through `DataSelectionModel::setTrusted`, the Data
window's command. The editor carries the lap-stretch check box (the toolbar's view state) and the stretch report.
SHIFT+F7 is `registerInsertBeaconCommand` (`cursor.insert_marker`) and the toolbar action follows the service
through `connectLapStretchToggle`; the shell registers both. (c) The Details editor's rows are the profile's
details, then every other id of the open sources except the derived ones; a number is typed `<value> [unit]` (the
profile's unit by default, a unit of the same quantity, the typed decimals kept as its display decimals); a Venue
edit emits `venueRenamed` and the prompt of 06 §13.4 item 3 waits for track maps (M3). (d) The Data window's
context menu offers the filters with the current kind checked and a non-modal lap-number editor; rows kept only by
a slot and filtered source rows carry a filter icon. (e) AC-M2-05 (the editor half) and AC-M2-11 are proved by
`widgets.tst_lap_editor` and `widgets.tst_mapping_panel` over the real loader, session and undo stack; the
`tests/ui` walkthroughs follow with the shell's wiring.
**Consequences.** A decision taken for one file can be kept for that file only; repeated imports stop at the panel
only for what the unit map lacks; unit maps never rename channels. The beacon offset by distance is recorded and
undoable but has no effect until its resolver lands.
**User must confirm:** no.

#### D-180 The shell's wiring of the M2 editors as built: the Mapping panel's place, the Channel Status dock, modeless editors on first use, and the proofs of AC-M2-05/-11 in `tests/ui` (refines D-177, D-178, D-179; 06 §2.2, §3.2, §10, §13, §14.4)
**Context.** D-177 and D-179 built the editors without the shell, leaving `setMappingHandOff` off (unknown units
opened unitless), the editors' `tools.*` rows, `cursor.insert_marker` and the nine `channel.*` rows `planned`, and
AC-M2-05/-11 proved below the shell. 06 §13.8 puts the Mapping panel "above the worksheet area" without saying how it
shares the central widget; 06 §3.2 lists `channelstatus` (Bottom, Closed) beside Messages; 06 §14.4 makes
`view.channel_status` an alias of `tools.channel_status`; 06 §2.2 files the active-channel rows under Component, while
`ActiveChannelCommands` registered them under a top-level "Channel" menu. Adding a closed dock moved the start-up
keyboard focus into the Data window, and exposed a slot of `WorksheetCanvas` that `QApplication::focusChanged`
could still reach while the canvas was being destroyed with the focus.
**Decision.** (a) `MainWindow` owns a `MappingPanel` between the workbook/worksheet bar and the canvas, hidden until
its `pendingChanged(true)` and hidden again on `false`, and turns the loader's Mapping-panel hand-off on. (b) The
`channelstatus` dock (Bottom, Closed) holds a `ChannelStatusWindow` over a `ChannelStatusModel` of the window;
`tools.channel_status` (CTRL+U, an optional channel argument selects its row) and `view.channel_status` both show,
focus and refresh it. (c) The Maths Editor (CTRL+M, `[module, item]` opens it at the item; its Main source follows
the selection), the Channel Editor (CTRL+E, `[channel]`), its Aliases (`[alias]`) and Mappings (`[logged name]`)
editors, the Details editor (CTRL+D) and the Lap Editor (menu only; with the lap-stretch service) are modeless
dialogs created on first use, shown, raised and activated by their command, their geometry kept in `layout.json`
`dialogs.<key>` when they close; editor messages go to Messages (origin Maths), refusals and hints to the status bar.
(d) `registerInsertBeaconCommand` (SHIFT+F7 over the workspace cursor) and `ActiveChannelCommands` (its `Host`: the
canvas's focused component, else the last one; the active worksheet; the Maths Editor; the Channel Status dock)
are registered; the active-channel rows sit in "Component/Active Channel". (e) The window gives the canvas the
focus at the end of its construction, and `~WorksheetCanvas` disconnects from `QApplication::focusChanged` first.
(f) The 19 rows (`tools.maths_editor`, `tools.channel_status`, `view.channel_status`, `tools.channel_editor`,
`tools.aliases`, `tools.mappings`, `tools.details`, `tools.lap_editor`, `data.filter_all_sources`,
`cursor.insert_marker`, `channel.*`) are `implemented` with their `KEYMAP_TEST` rows (D-178 (e)). (g) AC-M2-05 and
AC-M2-11 are proved in the shell by `ui.tst_lap_editor` (60-lap outing: Tools > Lap Editor, SHIFT+F7 in the graph,
CTRL+Z/CTRL+Y, a beacon time offset and Restore Logged Laps) and `ui.tst_mapping_panel` (a CSV beside an `.ld`,
Resolve, CTRL+Z/CTRL+Y, CTRL+S writing the unit map, a silent next open, Save then Open Anyway), as 07 §5.2 names
them; the widget-level suites of D-179 stay.
**Consequences.** Unknown units now stop at the panel in the application, as AC-M2-11 requires; every M2 editor of
06 §13 except the Track Editor (M3) and the Units List is reachable from its menu and key. A user who never opens
an editor pays nothing for it. The remaining `planned` M2 rows are the project/file rows (`file.*`, the workbook
selectors) and `tools.units_list` (`docs/milestones/M2.md` open issues).
**User must confirm:** no.

#### D-181 `esses-cli export` and `fixture diff` as built (refines D-090; 05 §9.2, 07 §5.1, §5.5)
**Context.** 07 §5.1 asks for `export` "project-aware CSV with maths" and `fixture diff`; 05 §9.2 has `export
<file> --format csv|motec.csv|motec.ld` as an alias of `convert` (not built) and `fixture diff <dir> [--channel x]`
explaining mismatches; the M2 demo exports `"Corr Dist,Lap Delta"` of `Outings:Laps/3` from a project. Neither
says which of two channels with one name is exported, which units are written, or how a selection-dependent maths
channel (`stat_*` without reset) gets its math window outside the GUI.
**Decision.** (a) `esses-cli export [<project>] <log> -o <file> [--channels a,b] [--range <group>/<n>] [--format
csv|motec.csv] [--rate <Hz>] [--units "<channel>=<unit>,..."] [--layer] [--importer] [--options] [--no-sidecar]
[--json]`. With a project folder (or its `project.json`): its mappings and unit maps apply at open, its maths
modules in `mathsOrder`, the sidecar's local maths, `builtinMaths` of the corrected speed and distance (settings and
enable flag), the GPS corrections where both GPS inputs are logged, its aliases and `options.maths.compat` (the
registration order of `MathsService`, D-171/D-173); without one, the built-in modules with their defaults. (b) A
requested name is the maths output when the graph has one (the head of its chain, what every consumer reads),
else the logged channel by name, then by logged name; an unknown name is exit status 1. Without `--channels`: the
logged channels no maths output shadows, then every maths output that is neither in error nor disabled. (c) Units:
a maths item's display unit, else its declared result unit; then the project's `overrides.json` display units; then
`--units`; else the stored base unit. (d) `--range` writes that range (time from 0 at its start) and is the Main
range of the maths selection (`MathSelection`, its key a hash of the group path and bounds). (e) The maths are
evaluated on a `TaskPool` of the process and the calling thread waits (03 §7.4). `motec.ld` output stays with
`convert` (M1 not built; v1.0 UI). (f) `fixture diff <dir>... [--channel <name>] [--json]` runs `fixture verify`'s
checks and explains every fixture: per channel the samples compared, how many differ, the largest F32/F64 ulp
distance and the first differing sample with both values; for `expected.probe.json`, `expected.markers.json` and
`expected.laps.json` the JSON Pointer of each difference (JSON Patch from expected to actual, 25 at most), which
covers missing markers and laps; exit 1 when a fixture differs.
**Consequences.** The demo's export runs with `'Corr Dist'` and any maths channel of the project (`Lap Delta` exists
only as a user's item: there is no built-in of that name in 0.1). Exported maths values are those the graph would
draw for that Main range. Tests: `cli.export.project` (relations checked by `apps/cli/tests/export_check.py`, not
from the code under test) and `cli.fixture_diff.explains`.
**User must confirm:** no.

#### D-182 Maths units found in the user's maths: `[ratio]` on a unitless channel, `l/100km` and `psi (sg)` (refines D-164, D-010; 03 §2.4 rule 5)
**Context.** The AC-M2-01 corpus (the user's i2-written maths, counts in `docs/milestones/M2.md`) had 512 items with
`W-UNIT-FAMILY` because generated table maths tags every unitless channel `[ratio]`, and two items with
`E-UNIT-UNKNOWN` on two units missing from `data/units.json` (a fuel consumption and a sealed-gauge pressure). 03 §2.4 rule 5 passes a unit of another family through
unconverted with a warning, "a unitless channel with a dimensioned unit too"; Ratio is dimensionless, and its base
`ratio` has the factor one, so nothing is converted either way. Fuel consumption (volume per distance) is the
reciprocal of Fuel Economy, so it cannot be a unit of that quantity.
**Decision.** (a) A unitless channel read in the base unit of a dimensionless quantity (`[ratio]`, `[rad]`,
`[lambda]`) is read as stored, typed in that unit, without `W-UNIT-FAMILY`; any other unit (`[%]`, `[deg]`) keeps
the warning. (b) `data/units.json` gains the quantity Fuel Consumption (dimension L², base `m3/m`, unit `l/100km`
with the aliases `L/100km`, `l/100 km`, `L/100 km`; 1 l/100km = 1e-8 m³/m) and the aliases `psi (sg)` and `psisg` of
`psi g` (sealed gauge: read against a sealed reference near one standard atmosphere, a gauge reading). Recorded as
`lang.unit-family` and `lang.unit-lookup` in `docs/maths-compat.md`.
**Consequences.** The corpus checks with 0 errors (100 % Ok or warnings only). Both readings are chosen, not
observed: an i2 observation that `[%]` on a unitless channel scales, or that `psi (sg)` is absolute, changes them.
**User must confirm:** no.

#### D-183 AC-M2-10 on the 0.1 whole-node path: measured, not met; the two-frame gate moves to M5 (proposed; amends 07 §5.2 AC-M2-10, §12; D-042, D-139)
**Context.** AC-M2-10 asks the 20 maths channels of `bench/workloads.md` on RW-2 to be ready within two frames (33.3
ms) on the reference machine, "multi-core evaluation met by parallelism across nodes on the 0.1 whole-node path
(D-139)". Measured on the reference machine (16 logical processors, 15 workers, Release, idle): median 112.9 ms of
7 (107.0 to 117.5). The 20 nodes take 385 ms of work when required one at a time; the slowest alone (`derivative(
smooth('Damper Pos FL', 5))` over 1.8 M samples at 1 kHz) takes 61 ms, so no scheduling of whole nodes can reach
33 ms; across 15 workers the set runs 3.4 times faster than serially, bound by memory traffic (each 1 kHz node
gathers its inputs into F64 buffers, evaluates F64 stages and seals F32 chunks plus a summary pyramid).
**Decision (proposed).** AC-M2-10 at M2 records the measurement (`bench.maths_bench`, `maths.rw2_20_ready`, with the
core count) instead of gating it, like `filter_lp` order -2; the two-frame gate, unchanged, applies from M5 with
the chunk-parallel evaluator (03 §6.1, §8.1 of 07), which also brings F32 buffers between stages. The 0.1 levers in
the order of their effect stay open issues of M2: the windowed kernels' per-sample cost (about 10 ns per sample for
`smooth`), the gather and seal copies, sealing a node's chunks on idle workers.
**Consequences.** 0.1 shows a maths-heavy worksheet Pending for about a tenth of a second after a change instead of
two frames; every other M2 budget holds. If the maintainer keeps the gate at M2, M2 stays open until the evaluator
work lands (estimated 2-3 days for the kernels and copies, without the chunk-parallel path).
**Also amends 07 §1.5 (proposed, part of the same confirmation).** 07 §1.5 lets a budget row move one milestone
only when the reference machine, not the code, is the cause; here the cause is the 0.1 whole-node evaluator. On
confirmation the AC-M2-10 row of 07 §12 moves to M5 although the code is the cause, and until M5
`maths.rw2_20_ready` is recorded without a limit (`bench/maths_bench.cpp` gives the row no limit then, so the
self-hosted gate passes on it). Until the maintainer answers, the row keeps its 33.3 ms limit and the self-hosted
bench job fails on it (Option B: M2 stays open for the evaluator work, and even then the slowest single node must
drop below 33 ms). Measured again after the M2 review fixes: see `docs/milestones/M2.md` §1.
**Status.** Withdrawn on 2026-10-13: D-191 met AC-M2-10 on the reference machine (about 28 ms against 33.3 ms), so the gate stays at M2 and the 07 §1.5 amendment proposed here does not apply.
**User must confirm:** no (withdrawn).
**Superseded in effect by (A.13):** D-191: the evaluator meets the 33.3 ms limit on this machine (`maths.rw2_20_ready` 28-29 ms, D-191's evidence), so recording the row until M5 and the 07 §1.5 amendment are no longer needed; proposed: withdraw this decision and keep the gate at M2.

#### D-184 AC-M2-14 reads the fixture's own channel name (amends 07 §5.2 AC-M2-14)
**Context.** AC-M2-14 evaluates `'Ground Speed' * 2` over `synthetic-acc-dialect`, which logs its road speed as
`SPEED` (D-154); `esses-cli eval` reads a log without a project, so no mapping, alias or dictionary renames it, and
the sim-exporter dialect has no shipped dictionary.
**Decision.** The criterion's command is `esses-cli eval "'SPEED' * 2" fixtures/motec.ld/synthetic-acc-dialect/
input.ld --json` (the ctest `cli.eval.json_speed_lap` reads `'SPEED'[m/s] * 2` over lap 1); with a project,
`'Ground Speed'` resolves through the project's mapping or alias, as in `esses-cli export`.
**Consequences.** The criterion measures what it was written for (rate, unit, values, the elementwise budget). In
the application a workbook that names `Ground Speed` shows it "(missing)" for this dialect until the project maps or
aliases `SPEED` (Corr Speed finds it through its alternate list, D-154); a shipped dictionary for the sim-exporter
dialect stays an open issue of M2. The command as 07 §5.2 writes it fails (`E-UNKNOWN-CHANNEL 'Ground Speed'`, exit
1): this entry changes an acceptance criterion, so it needs the maintainer's answer. The alternative that keeps
the criterion as written is a registry-level dictionary for importers without one (M2 open issue 7: `SPEED` ->
`Ground Speed` for the sim-exporter dialect).
**User must confirm:** yes (it changes an acceptance criterion). **Confirmed by the user on 2026-10-14.**

#### D-185 Maths engine corrections of the M2 review (refines D-021, D-022, D-164, D-170, D-171; amends 03 §3.2, §4.4, §5.2, §5.4, §5.6, §6.4–6.6, §7.2, §7.6, §8)
**Context.** The M2 review probed the engine through `esses-cli` and project exports and found results that 03 does
not allow or does not settle: an ungated `stat_*` (a `Scalar`) dropped the rate of its Series argument, so the plan
fell back to the layer's highest rate and gathered a 10 Hz channel upsampled to 100 Hz (other statistics, ten times
the work); an integer or Bool expression with a declared non-base unit was converted to base and then truncated
into an I32 column (`choose(c, 50, 0)` declared `%` read 0 %); an integer result beyond ±2^31 saturated silently;
+-inf from `0^-1`, `exp(1000)`, `x / 1e-320` or the F32 cast was stored as a valid sample; a literal factor
(`'Speed'[km/h] / 3.6` declared m/s) raised an `I-RESULT-SCALE` that told the user to declare the wrong unit; a table
axis was never checked against the axis unit; flipping `unaryMinusAbovePower` re-versioned every expression though
03 §2.3 affects only those with a sign and `^`; the synthesised `'N'` of filter and scale_offset items raised
`W-UNIT-MISSING` on every dimensioned channel (in text the user never wrote); a Filter item of 0 points was an error
though 03 §5.2 makes it a window of 2; a LapScalar result interpolated linearly between laps; `'P' =
previous_sample('P') + 1` without a logged `P` was `E-UNKNOWN-CHANNEL` where 03 §5.4 says `E-CYCLE`; a marker edit
that moved the math window's lap left ungated statistics on the old window with status OK; variance against a
reference lap in another source was all-invalid with status OK; a sparse timestamped channel got a non-round auto
rate, and a layer of timestamped channels only had no rate at all.
**Decision.** (a) Result rate: an explicit rate, else the highest rate among the plan's Series inputs, a channel
read only by a statistic included (03 §6.4 rule 2 as written); the layer's highest rate only for a plan without
Series inputs (rule 3). (b) Output dtype (03 §6.5): I32 for Bool results, enumerations and integer results that
provably fit (constants within the range; integer channels stored in at most 32 bits; those through `+ -`, the
unary operators, `& |`, `choose`, `min`, `max`, `abs`, `remainder`, the integer roundings, `bit_*` of a known width
under 32 and the order-preserving stateful functions); every other integer result is F64 (exact to 2^53) and keeps
the Integer sample kind; an integer or Bool result converted from a declared non-base unit is real (F32, or F64 by
the precision rules). (c) Every non-finite value is invalid: the overflowing operations (`/`, `^`, `power`, `exp`,
`exp10`, `sinh`, `cosh`, `tan`) give an invalid sample in evaluation and in constant folding alike, and
`evaluateWhole` clears any non-finite result after rounding to the output dtype (03 §5.1, §6.6). (d) A numeric
literal factor other than ±1 in `* /` beside an operand of known non-trivial dimension leaves the dimension and the
unit label and makes the scale unknown: users convert units by hand, so no scale is inferred through a literal and
`I-RESULT-SCALE` is not raised; `W-RESULT-DIM` still checks the dimension (refines D-164 and 03 §3.2 rules 5 and 10).
(e) A table2d axis expression is checked against the axis unit like a declared result unit (`W-RESULT-DIM`,
`I-RESULT-SCALE` with an axis wording); its numbers are still compared with the breakpoints as they are. (f) The
power switch is not hashed into definitions or compile signatures: the parse cache is keyed by the switches and
the canonical text of the parsed AST (in both) differs exactly for the expressions with a sign and `^`, so only
those re-version (03 §2.3 as written); function aliases stay hashed. (g) A synthesised filter or scale_offset
reference reads `'N'[u]` in the unit it is read in (an identity conversion; bare for a unitless channel), so no
`W-UNIT-MISSING` points into synthesised text. (h) A Filter item follows `smooth()`'s window rules: 0 and 1 points
and 0 s are a window of 2; a negative or non-finite window is `E-ARG-VALUE` (03 §5.2 as written). (i) A LapScalar or
Scalar result whose item leaves the interpolation at its default is Previous-interpolated (D-021, 03 §3.2 rule 3).
(j) A reference to an item's own name inside `previous_sample` with no logged or earlier chain element is `E-CYCLE`
(03 §5.4 as written); any other self reference without an earlier element stays `E-UNKNOWN-CHANNEL`. (k) The
selection key of a SelectionDependent node also hashes the math window resolved against the current snapshot (03
§7.6), and a new snapshot re-keys a node whose column was evaluated over another window (Pending, re-evaluated). (l)
Variance against a reference lap in another source that the host cannot read fails with the new code
`E-REF-SOURCE` (status Error) instead of an all-invalid output with status OK; `gap.variance-source` in
`docs/maths-compat.md`. (m) The nominal rate of a timestamped channel without an importer rate is one over its mean
interval rounded to whole Hz (a period below 1 Hz): 03 §6.4 rule 2 names the median, which needs the samples, and
the rebuild runs on the owning thread, which never reads them (01 §5.3); the layer's highest rate counts every
channel through the same helper. `esses-cli eval` uses the same rules.
**Consequences.** Statistics of slow channels are computed over the samples as logged; integer results never
saturate; plots never receive an infinity; the usual hand conversions are hint-free; a compat flip recomputes only
what it changes; Channel Status no longer shows a warning on every Filter and Scale & Offset item; per-lap constants
step; a moved lap re-evaluates the statistics over it. Every change has a regression test (`maths.math graph
review: ...`, `maths.checker: a literal factor ...`, the corpus cases `pow_zero_negative_invalid`,
`exp_overflow_invalid`, `f32_overflow_invalid`). Reading (m) is ours; an importer that knows the nominal rate gives
it in the descriptor.
**User must confirm:** no.

#### D-186 `scale_offset` is computed in the unit of its offset (amends 03 §4.4)
**Context.** 03 §4.4 synthesises a Scale & Offset item as `'N' * scale + offset_base` in the stored base unit, the
offset converted into it as a delta. For linear units that equals scaling in any unit; for an affine unit it does
not: a temperature stored in K and scaled by 1.02 reads 80 °C as 360.21 K = 87.06 °C, where a user who works in °C
(and the reference product, which has no base-unit storage) expects 81.6 °C.
**Decision.** The item computes `y = x * scale + offset` in the unit of its offset when that unit is of the input's
display family (the unit the Maths Editor and CTRL+SHIFT+Z write: the channel's display unit), and in the stored
unit otherwise (an offset without a unit, or in the stored unit itself); the offset is then used as written, and the
result is converted to the base unit for storage (`MathItem::resultUnit` of the synthesised plan names the unit it
is computed in). For linear units the result is the same as 03 §4.4's form; for affine units it is the scale the
user reads.
**Consequences.** A °C or °F channel scales as displayed; zeroing at the cursor (scale 1) is unchanged; an offset
given in K on a °C display applies in K. `maths.math graph items: scale_offset computes in the offset's unit ...` and
`maths.math graph review: scale_offset scales a temperature in the offset's unit`. Chosen, not observed.
**User must confirm:** yes (it changes the computation 03 §4.4 specifies). **Confirmed by the user on 2026-10-14.**

#### D-187 The journal as a per-session snapshot: write-ahead save, autosave beside a deferred recovery, Recover adopts, case-only renames (amends D-172 (h)–(i); 02 §6.8)
**Context.** The review reproduced four ways to lose data with D-172's journal: (1) a workbook renamed only in letter
case is written to the same file on a case-insensitive file system (Windows, default macOS), and the retire step
then backed up and deleted that file, leaving `project.json` naming a workbook without a file; (2) replay took the
newest entry per path across every journal file, but each autosave appends only the documents dirty at that moment,
so an edit journalled once and then undone came back after a crash; (3) "Later" on the recovery prompt suspended
autosave for the rest of the session and the next Save (or the save on exit, or closing the Maths Editor) deleted the
deferred journal; (4) a save that failed part-way left the older journal, whose entries replayed over the files that
save had written.
**Decision.** (a) Every journal file is its session's complete unsaved state; `Journal::append` deletes the files it
wrote before, and replay takes each session's newest file (a damaged entry there falls back to the same path in the
session's next older file), merging sessions newest entry per path by seq and time and logging a path two sessions
changed. (b) `ProjectSession::save()` first appends the snapshot of everything it is about to write, then writes,
and on success deletes only its own session's files; a journal whose recovery was put off stays. (c) Autosave keeps
journalling this session's own edits while a recovery is deferred (Later), beside the other session's files; a
dirty set back to the disk's contents deletes the session's files. (d) Recover adopts the replayed journal: the
session journals the recovered documents as its own and then deletes the replayed files, so they are never replayed
over a later edit. (e) Discard on the prompt deletes the other sessions' files; leaving a project without saving
(`discardSessionJournal`) deletes only this session's. (f) The retire step of Save never retires a file that is the
same file as one the save names (`std::filesystem::equivalent`): a case-only rename keeps its file. `Journal` gains
`JournalSessions {All, Own, Others}` for `pending`, `replay` and `clear`; journal-1 is unchanged.
**Consequences.** D-172's consequence holds again: a crash loses at most one autosave interval, deferred or not, and
a failed save loses nothing; journal folders hold one file per session instead of one per autosave. Two sessions
that changed the same document independently replay the newer one (the replay log says so). Tests:
`project.journal: ...` (per-session newest file, clear by session) and `qtmodel.tst_project_recovery`
(`caseOnlyRenameKeepsTheWorkbook`, `undoneEditIsNotReplayed`, `laterSurvivesEditsSavesAndCrashes`,
`failedSaveLeavesItsContentRecoverable`).
**Amended by (A.13):** D-190 (a) (an entry the disk already holds is not replayed and no conflict; a journal holding nothing else is deleted at open without a prompt).
**User must confirm:** no.

#### D-188 `esses-cli maths check` reports MathGraph's nodes (refines D-166; 03 §13, AC-M2-01)
**Context.** `maths check`, the tool behind AC-M2-01, parsed and type-checked each item against its own scope: it
could not report cycles between items, the graph's synthesis and compile errors (`E-ARG-VALUE` of a filter window,
`E-TABLE-AXIS`, `E-RATE-ZERO`, `E-NO-DISTANCE`), and it resolved a logged channel before a module item of the same
name, the inverse of the shadowing chain, so AC-M2-01's counts measured another resolver than the application's.
**Decision.** `maths check` loads every module given into one `MathGraph` in the order given (the project order)
and reports each item's node: its status (Disabled, Error, else Ok or Warning by its diagnostics) and the
definition's diagnostics. With `--log` the graph has the log's source and the built-in modules the application
registers (corrected speed and distance; GPS corrections where both inputs are logged); without it the graph has no
source: references are `I-UNRESOLVED`, an item that reads one is checked but not compiled, a module condition over
details holds, and nothing is evaluated. A module id given twice is renamed inside the graph only. Parse errors are
counted on the text the user wrote (expressions and table axes).
**Consequences.** AC-M2-01's private corpus re-checks with the application's resolver (counts in
`docs/milestones/M2.md`); cross-module constant ambiguity (`W-CONST-AMBIGUOUS`) and cross-module outputs now show.
`cli.maths cli: maths check finds cycles, synthesis and table errors through MathGraph`.
**User must confirm:** no.

#### D-189 Seven keymap rows of milestone M2 move to M3; the workbook rows are built (amends 06 §14.1's milestone column, D-178 (e))
**Context.** D-178 (e) makes `tools/check_keymap_coverage.py` require every row of the closing milestone to be
implemented with a `KEYMAP_TEST`. Nineteen M2 rows were planned at review: `view.workbook_n` (ten keys),
`view.prev_workbook`, `view.next_workbook`, `file.open_log`, `file.close_all`, `file.save_project`,
`file.new_project`, `file.open_project`, `file.close_project` and `tools.units_list`. The file rows need a project
or a file-dialog stub in the keymap fixture; `file.new_project`, `file.close_project`, `file.close_all` and the Units
List have no handler.
**Decision.** The workbook rows are implemented (`MainWindow::selectWorkbook`, wrapping at the ends) with their
`KEYMAP_TEST`s. The seven others move to milestone M3 in `data/keymaps/default.json` (status planned): CTRL+O and
CTRL+S keep working through their commands' default keys, and M3's keymap fixture adds a project and a dialog stub.
**Consequences.** `check_keymap_coverage.py --milestone M2` reports 0 findings; M3 inherits seven rows (recorded in
`docs/milestones/M2.md` §5 and §7).
**User must confirm:** no.

#### D-190 The rest of the M2 review: recoverable saves, bounded and guarded text reads, coalesced sidecar writes, cancellable opens, the Maths Editor's partial save (amends D-167, D-172, D-177 (a), D-187; 02 §4 sidecar rule 4, §6.8; 05 §7.1)
**Context.** The M2 review's last fourteen findings, checked against the code after D-185 to D-189. The journal: D-187 already journals a save ahead of its writes, but a failed or killed save still left entries whose document was on disk, which replay reported as conflicts against the older `baseHash`; `Journal::clear` deleted files one by one in directory order, so an interrupted clear could leave an older snapshot behind a newer one; two sessions that read the same highest seq wrote the same `<seq>.json`, the later rename replacing the other session's file; Save wrote `project.json` even after an earlier document failed (02 §6.8 rule 2 writes it last so that it never names a file that was not written); `saveWithBackup` replaced a file before copying the old one to `backup/` (D-070), and reported a failed copy as a failed save of a document that was in fact on disk. The Maths Editor's save on close (D-177 (a)) was the project's save: it committed every other pending edit, cleaned the undo stack and deleted the journal. The text engine: a line or a row of any length was split into a cell per delimiter (a 10 MB line of commas took 482 MB); a sparse timestamped column counted the ragged rows before its first empty cell in pass 1 but not in pass 2, so the probe's sample count, the decoded samples and the chunk boundaries disagreed (a repeated time at sample 65,536); both passes read the file mapping without the page-fault guard (D-167), so a CSV truncated in place killed the process on Linux; a header-only CSV failed under `@auto`; a UTF-16 file of odd length gained a row; a quoted field holding a line break split its row silently; dictionary `nameRegex` patterns were compiled for every channel and entry, and a nested quantifier in a user's profile can hang libstdc++ and libc++. The application: every sidecar edit wrote the sidecar synchronously on the GUI thread (02 §4 sidecar rule 4 coalesces over 500 ms); opens could not be cancelled, so an open started in one project published into the next and its Mapping-panel decision wrote into the next project's documents; autosave rebuilt every dirty entry every interval and the recovery prompt replayed the journal twice.
**Decision.** (a) Journal (amends D-172, D-187): replay leaves out an entry whose file already holds its document (`JournalReplay::applied`, no conflict), counts skipped files and entries (`skipped`), and a journal with nothing else to replay is deleted at open without a prompt; files are named `<seq>-<session tag>.json` (the session id's first eight hex digits; the untagged names of older journals are still read); `clear(All)` renames `.journal` to `.journal-clearing-<tag>-<stamp>` and then deletes it (a leftover folder is read by nobody and removed by the next clear), and every clear deletes oldest first, so an interrupted clear leaves each session's newest file, and deletes only the files it listed when it began, so a file another session writes meanwhile stays; Save writes `project.json` only when every other document was written, and retires the files the project no longer names only when every write, `project.json`'s included, succeeded (until then `project.json` on disk still names them). (b) `saveWithBackup` copies the previous version to `backup/` first; an error means the target was not written, unless the caller takes the backup's failure separately (`backupError`): then the document is written, the failure reported, and `ProjectSession` keeps the new baseline and logs "saved without its backup copy". (c) `ProjectSession::saveMathsModules` (amends D-177 (a)): the Maths Editor's save on close writes the module files that differ from disk and the module order into `project.json` as it is on disk, retires removed modules' files, journals ahead as Save does and re-journals the other edits against the new files; the undo stack, the other documents and the journal stay, and only a session left with nothing unsaved becomes clean. (d) Autosave compares the dirty set by hash before it builds any entry, and writes an unchanged set again when the session's newest journal file is gone (`Journal::ownFileExists`; another instance's Discard deletes it); the open's replay is made once and reused by the prompt and Recover. The serialisation and the write stay on the GUI thread in 0.1: `save()` is synchronous and its write-ahead must land before its writes, so a background journal writer needs an asynchronous save first (a serial persistence queue, later). (e) Text bounds: a first non-blank line longer than the 1 MiB header window is Format ("line N exceeds the maximum line length"); a longer line later ends the head; a names row of more than 65,536 cells is Format; `splitCells` stops at the cells the caller can use (65,537 in the header search, the header's count plus two after it, in both passes). (f) Sparse timestamped columns: in both passes a missing cell (a ragged row) is an invalid sample, as 05 §7.1 pass 2 says, and an empty cell is no sample; the review's proposal (a missing cell as an empty one) would contradict 05 §7.1 and the ragged-row rule of the dense columns. (g) Guarded reads (withdraws D-167's "no page-fault guard for text files"): both passes read a file mapping through `GuardedLines`, which copies 4 MiB windows through `MappedFile::guarded()` (a line over 16 MiB is cut to its first 16 MiB in both passes); hold carry-ins read backwards through guarded 64 KiB windows; UTF-16 is transcoded from a guarded copy; `formatHeader()` copies through the guard; pass 2 checks `changedOnDisk()` before each chunk (Io, `text.source_changed`). A fault is Io: the chunk is sealed invalid (`open.read_error`), the open fails. The registry's sniff still reads the first 64 KiB in place. (h) A header-only CSV under `@auto` (the whole file in the head, its last line or names and units at the end, the names naming the time column, no line that looks like a data row of them) opens with no samples and `text.no_data_rows`. (i) A quoted field holding a line break is not joined (05 §7.1 step 2 as built: one line per row); a head line that ends inside an open quote logs `text.quoted_newline` (Warning, the first line and the count). (j) A UTF-16 file of odd length drops its last byte (`text.encoding`, Warning, "odd byte count"). (k) Regular expressions: profiles and dictionaries refuse a repeated group that repeats inside unless each of its alternatives holds a character that must occur exactly once ("(a+)+", "(\w+\s?)*"; not "(\.[a-z]+)*"), a syntactic check; dictionary patterns are compiled once per process (a bounded cache); the sniff regex sees at most the first 16 lines of its 4 KiB. (l) Cancellable opens: `ImportOptions::cancel` carries the open task's token to the importer (the text engine's pass 1 checks it); `SourceLoader::cancelOpens()` fires every open's token, drops its result and every open waiting for the Mapping panel (`open.cancelled`, `open.mapping_cancelled`); `closeAll()` and a project change (`connectProjectSession`) call it. (m) Sidecar writes (02 §4 sidecar rule 4): `commitSidecar` publishes at once and queues the document; 500 ms after the first queued edit a Background task writes the newest document of each source, a per-source generation under a mutex keeping an older write from landing after a newer one; closing a source and destroying the loader write a queued document on the GUI thread; a failed write is an Import Log row `sidecar.write_failed` (`commitSidecar` no longer returns Io).
**Consequences.** A save that fails or is killed part-way recovers without false conflicts and never leaves an older snapshot that replays over newer files; sessions that pick the same seq keep their files; the backup copy exists before the file it preserves changes. Closing the Maths Editor no longer commits, cleans or deletes anything but the modules. Hostile text costs bounded memory (the cells a row can use, one 4 MiB window and one line of at most 16 MiB per decoding thread, 32 MiB of sample rows); sparse columns with ragged rows decode exactly what the probe announced; a CSV truncated under the mapping fails a chunk instead of the process on every OS. Tests: `project.journal: an entry the disk already holds ...`, `... two sessions that pick the same seq ...`, `... clearing the whole journal renames it away first`, `project.project file: saveWithBackup backs up before it writes`, `... a clear never deletes a file written after it listed the journal`, `... ownFileExists follows the session's newest file`, `qtmodel.tst_project_recovery` (`failedSaveLeavesItsContentRecoverable`, `savedJournalIsNotOffered`, `failedBackupStillSaves`, `promptReplayIsTheOneRecovered`, `saveMathsModulesLeavesOtherEdits`, `failedSaveRetiresNothing`, `autosaveRewritesADeletedJournal`), `widgets.tst_maths_editor` (`closeSavesOnlyTheModules`), `qtmodel.tst_source_loader` (`sidecarWritesAreCoalesced`, `closeAllCancelsOpensInFlight`), `qtmodel.tst_mapping_handoff` (`projectChangeCancelsOpens`), and `formats_text.*` (the hostile line and row, the ragged sparse column in one chunk and across chunks, the guarded windows against `LineCursor` and against the in-memory decode, a file changed on disk, the header-only file, the odd UTF-16 file, the quoted line break, the nested quantifiers and the 16-line sniff, a cancelled open).
**User must confirm:** no.

#### D-191 The 0.1 evaluator streams a plan in pieces and seals a node's chunks on idle workers (refines D-139; 03 §6.0; AC-M2-10)
**Context.** AC-M2-10 was measured at 113-120 ms against 33.3 ms (D-183). A profile of the 20-channel RW-2 set
(Release, 16 logical processors, 15 workers, timing per node and per stage) showed the time going to memory, not to
arithmetic: every 1 kHz node gathered its inputs into zero-filled whole-extent F64 buffers (14.4 MB per buffer at
1.8 M samples), wrote every stage into another, made four passes over the result to round it and one more to slice
it into chunks, and freed them all again; with 15 workers the fresh pages of those buffers faulted in one at a time
(one 14.4 MB buffer took about 10 ms to allocate), and the set moved about 3 GB through memory. On top, the kernels
had per-sample overheads: the default two-point derivative about 15 ns per sample, smooth 5.5 ns, the RC filters
5.5 ns per pass with their state in memory, the I32 rounding a library call per sample.
**Decision.** Without changing any result:
(a) `lang::PlanStream` evaluates a plan in pieces of 16,384 result samples. A stage that implements the internal
`kern::Streamable` (smooth, derivative, the forward-only filters, integrate, running, flip_flop, edge_delay,
time_valid, time_shift, pulse_width, previous_sample, range, table and the fused elementwise kernel) steps through
the extent with its state carried from one step to the next and reads its inputs at absolute sample indices from
sliding windows that keep one piece plus the context it needs on each side; a stage that needs its whole input
first (zero-phase and backward filters, the statistics, Variance) runs once over the whole extent as before. Buffers
are F64 with NaN at invalid samples and no bitmap. `Kernel::run` of a streamable kernel is one step over the view, so
each kernel's arithmetic has one implementation; `evaluateWhole` is a stream pulled to the end.
(b) `MathGraph` seals an expression node through `graph::StreamPlanProvider`: each chunk is pulled from a stream,
rounded as the output dtype stores it and written, then sealed by the store while its samples are in the cache; the
result never exists as a whole-extent buffer (`PlanProvider` keeps the built-in outputs). The provider is
chunk-independent, so `Column::require` offers the node's later chunks to idle workers, one priority band below the
node's so that they never run ahead of another node's task, while the node's task builds the chunks in order and
takes back those still queued. When every stage restarts inside the extent (stateless stages, and smooth and the
sliding derivative at their re-summation points: `kern::Streamable::restartPoint`) a worker evaluates its chunk with
an evaluation of its own restarted there; otherwise one stream runs in order under a lock and only the rounding,
writing and sealing spread.
(c) The kernels keep their operations and their order for every sample: smooth's add-one-drop-one update and the
two-point derivative run in loops without the validity tests where a step's input has no invalid sample (the
derivative's abscissa terms are exact: mu = i - 0.5, du = +-0.5, suu = 0.5, so x / 0.5 is computed as x * 2); the RC
passes keep their state in registers and several forward passes run interleaved per sample; a whole-extent filter
run reads its input in its first pass instead of copying it; the elementwise kernel reads F64 inputs without a
bitmap in place; the chunk write folds the rounding and the validity passes into the conversion (F32/F64: an invalid
sample is written as NaN and the store's seal makes the bitmap, 02 §3.1 behaviour 2; I32: truncated without the
library call).
**Evidence.** Unchanged results: `esses-maths-bench --digest` (a fingerprint of every output of the readiness set and
of 67 expressions over every kernel class with invalid runs, a re-prime gap and mixed rates, through MathGraph and
through `evaluateWhole`: 174 lines) is identical for the build before this change and after it; the 256 case files
and `maths_ref.py --check` pass; new tests compare the stream pulled in random pieces with the whole-extent
evaluation through `Kernel::run` with bitmaps, `restartAt` with the continuous stream, the provider's chunks
(samples, bitmaps, pyramids) with `PlanProvider`'s chunks of `evaluateWhole`, chunks built concurrently in any order,
and MathGraph's columns with `evaluateWhole`, bit for bit. Timing (Release, 16 logical processors on 8 cores, 15
workers): `maths.rw2_20_ready` medians of 28.3, 28.5 and 29.0 ms in three batches of 15 runs (26.9-32.0 ms over
all 45; 25.3-26.6 ms in earlier batches on a quieter machine) against 124.4 and 127.0 ms for the build before
the change in the same session; the set's serial cost (`--workers 1`) 175 ms, and the 20
nodes required alone 79.7 ms in all (402 ms before), the slowest `filter_hp('Damper Pos FR', 1, 2)` at 13.1 ms;
`maths.filter_lp_zero_phase_21600k` 225-234 ms instead of 398-411 ms.
**Consequences.** AC-M2-10 is met on this machine; D-183 (recording the row until M5, with its 07 §1.5 amendment)
is no longer needed, and the 33.3 ms limit of `maths.rw2_20_ready` stays. An evaluation holds a few hundred
kilobytes per stage instead of whole-extent buffers (whole-extent stages excepted), and a single node required alone
seals its chunks across idle workers (`derivative(smooth('Damper Pos FL', 5))` over 1.8 M samples: 64 ms before,
about 3 ms now). The public `Kernel`, `BlockCtx`, `ExtentMode` and `Plan` are unchanged; `kern::Streamable` and
`PlanStream` are internal and a first piece of the v1.0 chunk path (03 §6.1: restart points instead of
`saveState`/`loadState` for the kernels that have them). The remaining cost (serial, per kernel:
`docs/milestones/M2.md` open issue 5) is mostly the store's seal of the output chunks (NaN scan, pyramid levels,
summary, cache insertion: about 3.5 ns per sample, 45 % of the set) and the first touch of the chunks' memory; the
next levers are in `esses_store` and the elementwise interpreter's non-vectorised operators.
**User must confirm:** no (it supersedes the proposal of D-183, which the maintainer withdraws or confirms).

#### D-192 Public README is a work-in-progress notice; TRADEMARK.md withheld until a working model (amends D-004; AC-M0-10)
**Context.** The maintainer asked on 2026-10-13 to keep the project description and the trademark notice out of the public view until there is a working model, with only a work-in-progress notice in their place.
**Decision.** The root `README.md` says only that the project is a work in progress. `TRADEMARK.md` is removed from the repository. Their full texts are kept locally in `docs/unpublished/` (git-ignored) and are restored before the first public release. Other documents point to the trademark policy in `00_OVERVIEW.md` section 8 instead. The vendor-mark rules themselves are unchanged and still enforced by `tools/check_vendor_marks.py`.
**Consequences.** The repository history still contains the earlier README and TRADEMARK.md, and the design documents describe the project in full; hiding those as well needs a history rewrite and a decision on `docs/` before the repository is made public.
**User must confirm:** no (requested by the user, 2026-10-13).

### A.14 M3 (proposed, 2026-10-14)

#### D-193 The M3 contracts: component body schemas under `schemas/components/`, shared definitions repeated per schema instead of `common-1`, the names added, the dispatch points wired once (refines D-027, D-061, D-073; amends 02 §6.4, 04 §7.3, §16-17, outline §1.1 and §5)
**Context.** The M3 contracts (`docs/milestones/M3.md` §2) lay down the headers of every class of 07 §6.1 so that four implementing agents can work in parallel without editing a shared file, and the component body schemas AC-M3-08 needs. The design names `schemas/components/<type>-<version>.schema.json` and a `common-1` schema holding `BandAxis`, `ReportSpec` and `ColourBands` (02 §6.4, outline §5), but `tools/check_schemas.py` and the vendored json-schema-validator accept only local `#/definitions` references. Several 07 §6.1 deliverables have no class name (animation, the component clipboard), the gauges have no geometry builder although 04 §7.2 item 9 renders every drawn component through the same builders as `esses-render`, and the per-type dispatch (component registration, `OffscreenRenderer`'s builders, the channels a component shows, the rule factory, F5) sits in files every agent would otherwise touch.
**Decision.** (a) A component body schema is a fragment like the built-in settings schemas (no `$schema`/`schemaVersion` envelope) at `schemas/components/<type>-<version>.schema.json` with `$id` `urn:esses:components/<type>-<version>`; its examples are under `schemas/examples/components/<type>-<version>/`. `check_schemas.py`, `tests/schema_examples_tests.cpp` (which also validates every body of the shipped workbooks) and `esses-cli`'s data copy read the directory; `SchemaRegistry` does not load it yet. Bodies are closed (`additionalProperties: false`) at every level, so a schema is the complete property list that AC-M3-08 binds; the typed bodies still keep unknown members in `rest` when they read one. (b) `common-1` is not created: each component schema repeats the shared definitions it uses (`bandAxis`, `colourBands`, `scaleSpec`, `unitValue`, `unitSymbolOrEmpty`, `colour`, `channelName`, `dps`, `tdGroup`, `tdTrace`, `scaleLine`, `graphAxis`) and rule 4 of `check_schemas.py` keeps the copies identical. `ReportSpec` is the `report` member of the `channel_report` schema. (c) Persisted limits are `{v, unit}` only (D-010): the schema rejects M1's bare-number manual scale, which `TimeDistanceBody` still reads. (d) `track-1` (`schemas/track-1.schema.json`, D-061) is a document schema: lengths are `{v, unit}`, `lines.centre` is required, a section requires `id`, `name`, `startDist` and `endDist`, and `set` defaults to `Default`. (e) Names added (outline §4, "Names added by the M3 contracts"): `GaugeGeometry` with the typed bodies `GaugeNumericBody`/`GaugeBarBody`, `ScatterBody`, `HistogramBody`, `TrackPositionBody`, `OutingBody`, `ChannelUse`/`componentChannelUses`, `ScaleSpec`, `ColourBand`, `TrackFit`, the `BandLayout` and `KeyLayout` value types and the M3 `HitKind`/`HandleKind` values in `esses_plotcore`; `UnitValue`, `Band`, `SampleSpan` and the report, track and binning value types in `esses_analysis`; `ComponentBuildInputs::trackMap`; `ReportModel` (named by 04 §12) in `esses_qtmodel`; `AnimationController`, `ComponentClipboard`, `FieldKind`/`FieldBinding` in `esses_widgets`. `VarianceTable::build` takes a `VarianceRequest` (span, columns, alignment offsets) beside the two indexes. (f) The dispatch points are wired once by the contract: `registerBuiltinComponents` registers the ten MVP types, `OffscreenRenderer::builtinBuilders` maps `outing`, `scatter`, `histogram`, `track_position`, `gauge_numeric` and `gauge_bar` to their plotcore builders (the table components `values_list`, `channel_report` and `time_report` render through `QWidget::render`), `qt::componentChannels` falls back to `plot::componentChannelUses`, `makeRangeRule` hands `track-sections` to `TrackSectionsRule::fromDocument`, and `ComponentWidget::openProperties` calls `PropertiesDialog::openFor`. A stub builder fails with `Unsupported`, which `OffscreenRenderer` draws as its "Render failed" placeholder until the owner implements it; `fromRuleIds` keeps skipping the track-sections rule while its parser is a stub. (g) Key contexts (06 §14.2): `outing` Graph, `scatter` and `histogram` Summary, `track_position` Track, the reports and gauges Component. (h) `TrendFit` solves by QR on the design matrix without a new dependency: Eigen (outline §1.1) is not vendored and a download needs the maintainer's approval, and a Householder QR of at most seven columns is short.
**Consequences.** Parallel M3 work adds files in its own roots (`docs/milestones/M3.md` §2) and edits no shared `CMakeLists.txt`, registration or dispatch file. A body field added later is a schema change (a version bump of the type when it breaks old bodies) plus a `FieldBinder` row. A shared definition changed in one schema must change in all of them, which `check_schemas.py` enforces. `SchemaRegistry` gains the component directory when a loader validates bodies.
**User must confirm:** no.

#### D-194 Binning, variance and corrected-speed readings fixed in M3 (refines D-023, D-084, D-193 (h); 03 §9.1 items 2-3, 04 §7.3, §8.8, §10.1, §11.1)
**Context.** Implementing `BandAxis`, `HistogramBinner`, `ScatterBinner`, `TrendFit`, `VarianceTable` and the two remaining corrected-speed methods (07 §6.1) needs readings the design text leaves open: 04 §10.1 item 1 reads every scatter channel with its own `Interp` but the contract's input types carry none; 04 §7.3 and §11.1 do not say how a width that does not divide the range, `centred`, the palettes or the open bands generate; "percent of accepted samples" and "time" are undefined for samples outside every bin and for timestamped channels; 04 §8.8 applies the alignment offsets but is silent on lap stretch; 03 §9.1 item 3 gives the gCorrection recurrence but not its start, its invalid samples or the shape of the blend, nor what `compute()` does when one of its two channels is missing.
**Decision.** (a) Defaulted members carry the policies: `ScatterChannel::interp` (Linear), `ScatterInput::gatingInterp` (Previous, as the histogram's gating) and `HistogramInput::colourInterp` (Linear); every secondary read follows `store::Resampler`'s rules, invalid outside the channel's first..last sample. (b) `generateBands`: width mode makes `ceil((max - min) / width)` full-width bands from `min` (the last may end past `max`; an edge within rounding of `max` is `max`), and falls back to `bandCount` bands without a width; count mode makes equal bands; `centred` centres band k on `min + k * w` (width mode `floor((max - min) / width) + 1` bands, count mode `w = (max - min) / (bandCount - 1)`); the open bands are `[-inf, lo)` and `[hi, +inf)` with the end bands' colours; palette `twoColour` blends `colour1` into `colour2`, `rainbow` (and an unknown name) sweeps blue to red; persisted values convert through the registry (a width by scale only; an empty or unknown unit is taken as written; units of different families are Invalid); at most 10,000 bands. (c) The histogram counts a sample only in the band holding its value; percent is of the samples counted in some bin (the bins sum to 100); time is one period per uniform sample and, for a timestamped channel, the time to the next sample cut at the span end (0 for the series' last sample); the colour split uses the bars' y mode. (d) The scatter's union grid is the sample times of the highest-rate channel (x first on a tie), so x must be uniform; an invalid colour value keeps the point with z = NaN; `gather` reads every span it is given (a scatter has no untrusted exclusion); the interval-average bins are `[k w, (k + 1) w)`. (e) `TrendFit` solves by Givens rotations row by row on x scaled to about [-1, 1] (memory O(order²) for any number of points), r² is `1 - SSres / SStot` (1 for a constant y), `TrendType::None` is Invalid, and the equation text uses `std::to_chars` `%g` forms (locale independent) with zero coefficients left out. AC-M3-06's reference is `tools/trend_ref.py`, which solves the normal equations in exact rational arithmetic over the exact binary inputs and writes `libs/analysis/tests/data/trend_fit_cases.json`; ctest checks the file is current. (f) `VarianceTable` applies each lap's stretch as `DistanceChain` does (`timeAt(d / stretch)`, `distAt(t) * stretch`, as the maths `variance_*` kernels already do) and blanks a column outside the Main lap `[0, extent)` or the overlay's `[0, extent]`; the single-point lookups equal a one-column table with zero offsets. AC-M3-05's "no table without an overlay" is the time_distance builder's duty (one `build` per visible overlay). (g) `gCorrection`: the first valid wheel sample, a sample whose acceleration is invalid or outside its samples and the sample after an invalid wheel sample follow the wheel; an invalid wheel sample gives an invalid output; the blend starts at v_pred on the first sample back inside the band and decays the difference linearly to the wheel over `recoverSeconds` (≤ 0: straight back); leaving the band during the blend follows v_pred again. `compute()` with `gCorrection` reads its two channels without a fallback (the maths module's input list names exactly those two), so either unusable is NotFound `E-CSD-NO-SPEED` with `context.channel`; the longitudinal-G channel converts through the registry when it carries a unit (which must be an acceleration) and is taken in G otherwise or without a registry. `compute()`'s alternate list reads channels only up to the first usable one and passes the candidates to `alternateList`.
**Consequences.** Binners and variance are deterministic functions of the pinned data with documented edges, tested against brute-force references written independently of them (AC-M3-05, -07) and an exact-arithmetic one (AC-M3-06). The plotcore builders set the new `Interp` members from each channel's descriptor; leaving them at their defaults keeps the 04 behaviour for real channels. gCorrection keeps its documented "not bit-identical" status (D-084).
**User must confirm:** no.

#### D-195 Track maps as built: GPS and Speed+LatG generation, default sections with a minimum turn, the track-sections rule and its phase-2 pass, the track-map file (refines D-027, D-061, D-140; 02 §4.5, §6.9, 06 §13.5)
**Context.** 07 §6.1 and AC-M3-04 name the track deliverables without an algorithm: 06 §13.5 gives the two methods, the smoothing option, the 10 m minimum and default Turns/Straights, 02 §4.5 the groups and the backward origin rule, and the M3 contract left `TrackGenerator`, `SectionDetector`, `TrackSectionsRule`, `TrackMap`'s reader and writer and `TrackMapFile` as stubs. AC-M3-04 measures shape against ground truth, which needs a fixture whose truth is known independently of the code (D-041).
**Decision.** (a) GPS: the fixes in the lap window (the bounds interpolated) are projected onto the local tangent plane of the WGS-84 ellipsoid about `options.origin` or the lap's first fix; a gap of more than 1.5 sample periods is bridged by a cubic Hermite curve whose end tangents come from quadratic fits over 1.5 s on each side (`W-TRACK-DROPOUT` from 2.5 periods and 0.5 s); the loop is closed by joining its end to its start, and the closure error reported is the gap between the two interpolated bounds (spreading a gap of noise along the lap would add error); an adaptive local-linear Gaussian smoother along the path uses sigma = smoothingM / 2 in the corners and up to eight times that on the straights, where the curvature of a smoother pilot keeps the offset towards a curve's inside within 0.5 m; the line is resampled every stepM metres and a closed map's centreline repeats its start at dist = length; inner and outer lines carry the centreline distance of their nearest centreline point. (b) Speed+LatG: dead reckoning on the faster signal's samples with lateral acceleration positive to the left (ISO 8855; the new `TrackGeneratorOptions::invertLatAccel` reads the opposite convention) and the heading held below 2 m/s; loop closure removes a constant heading-rate bias so that the lap turns a whole number of revolutions, then spreads the remaining position gap along the distance travelled; the raw gap is the closure error reported (`W-TRACK-NOT-A-LOOP` when the lap turns less than half a revolution); the map's rotation is arbitrary. (c) Default sections: curvature from chords of +-smoothingM / 2; runs above 1 / cornerRadiusM; short runs absorbed shortest first (a short straight joins two corners turning the same way and is split between the two corners of an S-bend); a corner turning less than the new `SectionDetectorOptions::minTurnDeg` (15°) is part of the straight, because the residual wander of a GPS line with sigma 1.5 m otherwise produces dozens of false corners; a run across a closed map's start line is cut there; a generated id already held by a kept section gets a suffix. (d) `TrackSectionsRule`: the rule-1 members of 02 §4.5; the groups are the sets' parent `Outings:Laps:Track Sections` (emitted first, since `RangeGroupTree::setGroup` needs it), then each set followed by its category children in track order; a section range carries its lap's class, number, trust, enabled flag and start marker; its times come from the stretched lap index, and a section that reaches a lap bound ends there exactly; `TrackSectionsSettings` gains the group flags. `RuleRunner::runPhase2` gains an overload taking the track map and the lap-index lookup (a Track Sections group keeps its version while its ranges are unchanged, D-140), and phase 1 keeps the previous Track Sections groups while the laps they were built under are unchanged. `RuleRunner::fromRuleIds` now loads the rule; phase 1 ignores it. (e) `TrackMapFile::load` also applies `TrackMap::validate`, so a file with a section under 10 m is Invalid at its JSON pointer rather than repaired; lengths are written in metres as the contract header states (not in the unit read, D-010; a valid file's second round trip is stable); `save` creates `trackmaps/`. (f) AC-M3-04 is measured on `fixtures/track/synthetic-circuit` (CC0, `tools/gen_track_fixtures.py`: the true centreline integrated from a closed-form curvature profile; GPS at 10 Hz with white noise of sigma 1.5 m per axis and a 2 s dropout through a corner; IMU at 50 Hz with a 0.8 % speed error, lateral noise and a lateral bias); an apex is the point where half of a section's heading change is done, computed by the test; the Speed+LatG to GPS correspondence and the apex comparison use each map's fraction of its own length (the view lap stretching gives), because a dead-reckoned distance drifts by its speed error.
**Consequences.** On the synthetic circuit (this machine, 2026-10-14): (a) 0.80 m RMS from the true centreline (inner 0.50 m, outer 0.52 m; length -0.14 %); (b) 4.1 m RMS after the rigid alignment, seven corners in both maps, the worst apex 4.5 m apart, the closure error before correction 45.5 m. The thresholds stay provisional until a real GPS and IMU fixture (07 §6.2). The GUI wiring of the phase-2 pass (`SourceLoader` passing the venue's map and the lap indexes) belongs to the `esses_qtmodel`/`esses_widgets` track work; until it lands, the application builds no Track Sections groups.
**User must confirm:** no.

#### D-196 The plotcore M3 geometry as built: point-set tags, scenes apart from emission, summary scales for scatter, the outing's own layout, the track origin rule in plotcore (refines D-193; amends 04 §8.4, §9, §10.1, §11.1, §14, outline §4)
**Context.** The `esses_plotcore` part of 07 §6.1 (`BandLayout`, the `KeyLayout` columns, the full `HitTester`, `ScatterGeometry`, `HistogramGeometry`, `TrackGeometry`, `OutingGeometry`) is built in parallel with the `esses_analysis` binners it calls and with the time_distance completion that owns `TimeDistanceBuilder`. Several contract details leave a gap: a scatter click must move the cursor to the clicked point's time, but `HitResult` has no time and the widget never holds the gathered points; the GUI-thread Dynamic layer must place the scatter cursor ring on axes the worker scaled from the data; the DrawList goldens must not change whenever the binners change; `TrackMap::pointAt` cannot give the sub-polylines of sections and the zoom span; the outing graph shares time_distance's body but not its anatomy.
**Decision.** (a) `HitTester::addPoints` takes an optional tag per point and `nearestPoint` returns the found point's tag through an optional out-parameter; scatter tags each Main point with its Main-lap time (the cursor value a click sets). Only the Main sets are clickable (04 §10.1 item 7). (b) The scatter and histogram builders split into a frame (layout, scales, bands; reads no samples), a scene (worker: `ScatterBinner`, `TrendFit`, `HistogramBinner`) and an emitter (internal, `src/geometry/plot_scenes.hpp`); the DrawList JSON goldens drive the emitter with scenes computed by brute force in the test and bands given there. The emitter colours points by colour band with the BandAxis rule and evaluates trend polynomials by Horner itself, so the drawing does not depend on the analysis implementation of `binOf` or `evaluate`. A binner failing with Cancelled or Unsupported fails the Static build (Unsupported renders "Render failed" while a binner is a stub); any other failure drops that slot. (c) Scatter scales `channel` and `auto` both use the Main source's whole-column SummaryPyramid (the M1 autoscale stand-in, as time_distance), so the Dynamic ring lines up with the Static axes; "from the data shown" applies only while a summary is incomplete, and the Dynamic layer then draws no ring. Histogram y `auto` is 0 to the largest value of the tile's sets; manual y limits are taken as written (count, %, s). (d) The outing graph has its own layout (caption, a range band over the trace area with one cell per lap of the Main outing, key column, axis strip, panels by `edges`, x axis), not `TimeDistanceBuilder::layoutOf`, whose bands and columns the time_distance completion changes; its zoom handles are derived by the widget from `OutingGeometry::zoomRect()` because a Dynamic build carries no HitTester; overlays (enableOverlays, time mode) are aligned at lap start without the O-axis offsets. (e) `TrackGeometry` interpolates positions and sub-polylines along the centreline itself and applies the origin rule of 04 §14 item 5 there (a lap starts at a beacon when its layer holds an enabled BCN at its start time; a layer without BCN markers counts as starting at one); `fit` centres the bounding box of the rotated lines, manual scale multiplies the fit scale by `zoom` and shifts by `(panX, panY)` metres; `sizes`: `cursor` = dot diameter, `detail` = centreline width, `material` = extra width of the zoom span per side; only the first section set (Default) is drawn. (f) `KeyLayout`: value@datum and Δ sit right of value@main on each value line, the measurement lines (min, max, avg per slot) follow a row's value lines, and `KeyGeometry::valueLines` records the split; the default `KeyColumns` reproduce the M1 layout exactly. (g) `BandLayout`: overlapping range cells share the one 14 px row, later on top; cell edges are ticks; range labels elide, section labels appear only when they fit. (h) Histogram bins are equal-width slots labelled at their boundaries every `binLabelStep`; `stackLines` lifts trace n by n × 12 logical px; `condensedAxes` labels only the bottom row of tiles; the text style is a table of bins × sets. (i) Bodies keep the source's form of a BandAxis that reads back unchanged (`BandAxis::toJson` writes every member), so `parse(x).toJson() == x` holds for the component examples.
**Consequences.** The scatter widget sets the cursor from `nearestPoint`'s tag; the outing widget computes its handle regions from `zoomRect()`. Changing the binners changes no golden; the end-to-end builds are covered by tests that skip while a binner is a stub. `TrackMap::pointAt` and `analysis::trackDistanceOfLapDistance` are not called by the plot (the same formulas; a later refactor may route through them). An AutoscaleCache (04 §8.3) replaces the summary stand-in for both time_distance and scatter.
**User must confirm:** no.

#### D-197 Reports as built: the statistic definitions, the probe and interpolated boundaries, rows divided by headers in any order, the Overlay 0 delta by paired statistics, the StatCache key, the rolling minimum over contiguous laps, the cold report bench on the pool (refines D-027; amends 01 §4.4, 04 §12 items 1-6, §13 items 1-2, outline §2)
**Context.** The M3 contract left `ReportSpec`'s JSON, `StatCache`, `ReportEngine` and `TimeReportEngine` as stubs. AC-M3-03 asks for hand-computed fixtures of every statistic, which needs exact definitions the design does not give: 04 §12 item 3 names the statistics, the 5 s probe and `interpolateBoundaries` but not the mean's weighting, the standard deviation's divisor, what "abs max" reports, where the probe starts or what an invalid interpolated edge gives; item 2 says headers divide the data in any order but not how a lap header divides a section header or what a detail heading ("Driver", "Venue" of the demo, 07 §6.5) does before the detail-group rule exists; 01 §4.2 and the outline route the Δ-versus-Overlay-0 cells through `VarianceTable`; item 1 keys the cache by (channel, version, range, stat), which goes stale when a beacon offset moves a range that keeps its id (D-066) or when a maths output is a new column under the same `ChannelId`; 04 §13 item 2's rolling minimum concatenates all section times in scope, which joins laps that were not driven one after the other; and 07 §12's "cold" is defined for a process reading a file.
**Decision.** (a) Statistics over the valid samples of `[start, end)` in base units: avg the arithmetic mean of the samples; stdDev the population standard deviation (divides by n); min, max and absMax with the time of their first occurrence, absMax being the largest magnitude (≥ 0); delta = max - min; change = end - start. start is the range's first sample when it is valid, else the first valid sample at most `probeSeconds` after the range start (end mirrors it before the range end); with `interpolateBoundaries` the value at exactly the range edge by the channel's `Interp` (store::Resampler's policy, neighbours outside the range count) when that is valid, else the probe. Value statistics convert with the display converter, differences (change, delta, stdDev) with its scale only. `ReportEngine::statistic` gains a trailing `store::Interp interp = Linear` parameter. (b) A header names a range group of the source's layer; each of its enabled ranges (trusted unless `includeUntrusted`) splits a row into the intersection, so `[Corners, Laps]` gives one row per corner per lap; a header that names no group labels the row with the source detail of that name (empty without one) and does not divide; without headers a row is the source range under its own name. Rows are ordered by source (Log Date / Log Time, sources without a date last, then first appearance) and time, then grouped so equal names at each level stay under their first occurrence. (c) The Δ column is Main's statistic minus that of the Overlay 0 row with the same heading path (paired statistics, as the outline's "VarianceTable/paired stats" allows; no `VarianceTable`); it follows its statistic's column; `excludeOverlays` hides the Overlay 0 rows but keeps the pairing; numbered overlays never pair (04 §12 item 5). (d) The cache key's `version` mixes `Column::id()` into `Column::version()`, its clip always carries the row's exact interval, and the probe and boundary options (with the channel's `Interp` when interpolating) enter only the start, end and change keys; a cell's 128-bit key digest is what the cache stores. (e) `cells` takes the cached cells first, then scans the missing (row, channel) pairs in order while prefetching the chunks of the next ones, at most 256 chunks ahead of the scan, through `Column::prefetch` (Background; never blocks; a no-op without a pool), so a pool's idle workers materialise them during the scan without materialising a long log far ahead of it. (f) Time report: the caller expands outings into laps; section columns are the `rowGroup` ranges starting inside a lap, merged into track order; the eclectic total is empty while a section has no time; the rolling-minimum window never spans two laps unless one ends where the next starts (same source and layer), the earliest window winning a tie; bands test `cell <= fastest * (1 + pct / 100)` in list order. (g) `bench/report_bench` measures 07 §12's row on an in-memory outing of 50 laps × 30 channels (RW-2 rates, 49.9 M samples): "cold" is a fresh StatCache and a fresh chunk cache, the report running on a TaskPool worker at Background with the columns on the same pool, as in the application; the one-thread figure is recorded beside it.
**Consequences.** Every statistic has a definition a test can compute by hand (`analysis.report_*`). A detail heading works before the detail-group rule exists and keeps working once the groups exist (they then divide like any group). The cache never serves a value for moved bounds or a replaced column. Rolling minimum can be larger than a reading over a gap would give, by design. On this machine (2026-10-14, Release, 15 workers, three invocations of 7 runs): cold 95-99 ms, one thread 420-432 ms, chunks resident 77-95 ms, cached 1.4-1.5 ms; the one-thread cold figure is dominated by chunk materialisation in `esses_store`.
**User must confirm:** no.

#### D-198 The plots and gauges as built: the gauge bodies and builders, `TextRun::scale` for the numeric gauge's dynamic font, `LayeredComponent`, the scatter's own axis zoom, the export tables, ALT+n on the BandAxis components, the outing graph with the plots work (refines D-027, D-193, D-196; amends 04 §7.2 items 4 and 8, §9, §10.1 items 4, 7-8, 10, §11.1 item 10, §16-17, outline §4)
**Context.** The M3 contract left the gauge bodies and builders (`GaugeGeometry`) and the five drawn components of the plots work (`ScatterPlot`, `Histogram`, `OutingGraph`, `NumericGauge`, `BarGauge`) as stubs. 04 §16-17 give the gauges' content but not their layout; 04 §16 item 1 scales the numeric gauge's point size although the `DrawList` IR carries a style id and no size; the five widgets need the layer pipeline `TimeDistanceGraph` has; 04 §10.1 item 8 zooms the scatter on its own axes without a persisted member for it, and the widget cannot map a pixel to data without the frame plotcore keeps internal; Export Data (04 §7.2 item 8, §10.1 item 10, §11.1 item 10) needs the gathered points and the bin values, which only the worker-side scenes hold; 06 §14.4 lets ALT+n toggle a band of a component's `BandAxis`, which has no per-band visibility in the closed body schemas (D-193 (a)).
**Decision.** (a) **Gauges** (`src/geometry/gauge_geometry.cpp`): one channel (`SlotData::channels[0]`) read at the cursor for Main and for the reference overlay (the second visible slot) by the Values window's rule (x_c = map_Main.xOf(Main.start + cursorT), t_k = map_k.timeAt(x_c)); a missing channel shows "(missing)", a pending value "…" (the widget retries), an invalid one "-". Layout: the caption (Chrome) and a 4 px margin. Numeric: the value in the top 65 % of the content (all of it without a reference), the unit after it in `GaugeUnits`, the reference beneath in its key colour at no more than half the main scale; the font is sized for the widest value the channel can show (its SummaryPyramid extremes at the display decimals, digits as '8'), so it does not jump while the cursor moves, never below 6 pt of the scheme's `gauge` font; without `dynamicFont` the scale is the body font's point size over the scheme's (the body font's family and weight are kept in the body but not applied: the IR names styles only). The first visible colour band holding the value (limits converted to the display unit) colours the text or fills the content. Bar: the scale strip left of a vertical track or under a horizontal one (ticks labelled with `scaleDps` decimals), band lines across the track at each finite band edge or a 4 px band strip beside it (`bandLines` fill), the track in `keyBackground`; the bar from the origin (min, max, zero or `customOrigin`, clamped into the scale) to the clamped value, `bandMode` bar recolouring it; the reference as a 2 px marker line; the value text at the bar end on the background colour. Scale `channel` and `auto` are the Main channel's whole-column SummaryPyramid (D-196 (c)), `manual` the converted limits. Static: background, frame or track, scale and bands (Panel, Axis and Caption hits); Dynamic: the values, the bar and the marker. (b) **`TextRun::scale`** (`draw_list.hpp`): a per-run factor on the style's point size (default 1), written to the DrawList JSON only when it is not 1, so no existing golden changes; `RasterBackend` draws, measures and bounds a scaled run with the style's font scaled (per-thread cache slot keyed by the scale in thousandths); builders measure a scaled run as the style's metrics times the scale. (c) **`LayeredComponent`** (`include/esses/widgets/components/layered_component.hpp`, new public name): the base of the five components with `TimeDistanceGraph`'s pipeline (worker build and raster through `RenderService`, Dynamic and Chrome on the GUI thread, the 100 ms pending stripe, the 30 s release of a hidden component, `renderToImage`, `hitAt` from the Static hits); the subclass supplies its three builders and the change bits that rebuild the Static layer (04 §7.4: the outing graph and the gauges keep Zoom and Offsets on the Dynamic layer; scatter and histogram rebuild on them) and may adjust the inputs before every build. (d) **Scatter**: its own zoom is view state of the widget (not persisted, not undoable): box zoom (double-click, move, click; ESC cancels), the wheel (x0.8 per notch about the pointer) and a double-click on an axis (restore) set manual x/y scales in the axes' display units on the build inputs only; the new `ScatterGeometry::axes(inputs)` (tiles and scales of the frame, no samples) gives the widget its pixel-to-data mapping. A click picks the nearest Main point within 8 logical px and sets the cursor to its tag (D-196 (a)); LEFT/RIGHT (and CTRL) in the Summary context step one (ten) X samples; Copy Equation copies the Main equation of the last delivered Static list, under the menu-only command id `component.copy_equation` (not a keymap row; the shell registers it for the Component menu). (e) **Export Data**: the new `ScatterGeometry::exportSets` and `HistogramGeometry::exportTable` (`ScatterExportSet`, `HistogramExportColumn`, `HistogramExport`) gather exactly as `buildStatic` does, Main first; the scatter writes `set,x,y[,z]` with the set named `<Y channel>@<slot>`, the histogram `bin_lo,bin_hi,<trace@slot>...` (an open band's infinite limit is an empty field); the outing graph and the gauges write their channels' Main samples over the range (`Time [s]` in log time, the other channels at the first channel's sample times by their own interpolation). Ranges as 04 §7.2 item 8 (the current zoom, the Main range padded by 2 s, the whole outing). The work runs on the TaskPool through `RenderService` (a key of its own, Background) while the GUI thread waits in a local event loop that excludes user input: it keeps processing events and never blocks (01 §5.3). (f) **ALT+n** (`component.colour_band [n]`): the gauges toggle `colourBands[n].visible` (one undoable spec edit; a band beyond the list is a handled no-op). Scatter and histogram do not handle it in 0.1: their colour bands are generated from a `BandAxis`, which has no per-band visibility, so the shell's generic behaviour applies until a schema version adds hidden bands. (g) The components answer 06 §14.4's display commands in `handleCommand` (scatter G overlapped → tiled → average, S points → pointsTrend → trend, Y cross → ring → none; histogram G overlapped ↔ tiled, S bar → line → text; K and L the header legends; the outing graph S line ↔ points and L its key labels ↔ none; G is not the outing graph's, its panels follow `edges`); the histogram's wheel zooms the worksheet x0.8 per notch about the cursor (zoom.wheel, Summary) and a click on a bin of a tile makes that tile's trace active. (h) `OutingGraph` moves to `src/components/plots/` with its owner, the plots work; its zoom handles come from `OutingGeometry::zoomRect()` (D-196 (d)) with a 5 logical px grab, a lap cell's double-click zooms the worksheet to that lap, the wheel and the rubber band set the worksheet zoom, a click places the cursor. (i) The scatter and histogram builders pass each channel's own `Interp` to the binners (`ScatterChannel::interp` of x, y and the colour channel; `HistogramInput::colourInterp`); gating keeps `Previous` (D-194 (a)). (j) Pixel goldens: three cases per component in `libs/plotcore/tests/plots_golden_cases.hpp`, rendered through the whole builders with the golden fonts (`tests/golden/images/tst_golden_plots.cpp`); the gauge cases are also DrawList JSON goldens.
**Consequences.** Every drawn M3 component of the plots work renders through the same builders as `esses-render`; the numeric gauge's text fills its rectangle on every backend that honours `TextRun::scale` (any future IR consumer must); scatter zoom is lost when the worksheet is closed (as i2's), and copy/paste keeps only persisted manual scales; ALT+n on BandAxis components waits for a schema change; the shell wires `component.copy_equation` and `component.export_data` (its file dialog writes through `ExportSink`).
**User must confirm:** no.

#### D-199 The report and track components as built: ReportModel's styling roles, the workspace StatCache and track maps reached through RenderService, TrackMapService and TrackMapCommand, generation through esses_qtmodel, the Track Sections pass in SourceLoader, the track view as body edits, the Track Editor without the SourceLoader (refines D-027, D-193, D-195, D-197; amends 04 §12 item 1, §13 item 6, §14 items 1 and 4, 06 §9, §13.5, outline §4)
**Context.** The M3 contract left `ReportModel` and the five classes of `src/components/reports_track/` as stubs, and open issue 4 of the milestone note left the Track Sections groups unbuilt in the application. The contract's `report_model.hpp` promised `Qt::FontRole` and `Qt::BackgroundRole`, whose values (`QFont`, `QColor`) are Qt Gui types D-161 keeps out of `esses_qtmodel`. A component reaches the workspace only through `WorksheetContext`, whose `RenderService` hides the pool and the Dispatcher a `ReportModel` needs, and nothing owns 04 §12 item 1's per-workspace `StatCache` or the `trackmaps/<venue>.track.json` documents that 04 §14 item 1 binds by venue and 06 §13.5 item 2 writes undoably with a backup. The design leaves open which way round "channels in rows" lays the engine's table out, how overlay rows are told apart, what the time report's `project` time format is, how the zoom, pan and fit of 04 §14 item 4 persist without one undo step per wheel notch, how the Track Map dock keeps its own settings (06 §9), and where the generation of 06 §13.5 item 1 ("on a worker through esses_qtmodel") lives; the contract's `TrackEditor` constructor takes a `SourceLoader` it has no use for.
**Decision.** (a) **`ReportModel`** carries its styling as plain values in its own roles (`BackgroundRgbaRole` 0xRRGGBBAA, `ItalicRole`, `BoldRole`, `CellStateRole`, `RowKindRole`); the widgets' private `ReportStyleProxy` turns them into `Qt::BackgroundRole` (the colour at 45 % over the table background) and `Qt::FontRole`. Added members: `setHighlight`, `setTimeFormat`, `setProjectTimeFormat`, `setDistanceService`, `busy`, `deliveries`, `lastError`, `quoteField`, `channelTable`, `timeTable`. `refresh()` coalesces per event-loop turn; the inputs are built on the GUI thread (each channel resolved once per source and layer, maths outputs requested at Background), the engine runs on the pool under `report/<componentId>`, the result comes back through the Dispatcher, and a delivery older than the one shown is dropped. Scopes: selectedRanges = the visible slots with their roles; selectedOutings = the outing around each slot (the first slot's role); allOutings = every outing of every open source, Main unless a selected overlay lies in it. The time report's rows are the laps of the selected ranges, or of their outings. `zoomLinked` clips each range through its own `DomainMapper` (offsets applied; in distance mode a range without a `DistanceChain` is not clipped). Display: `channelsInRows` (the default) is a row per engine column (channel and statistic, then its Main - Overlay 0 column) and a column per heading; `channelsInColumns` is the engine's table as it is; column 0 is the row label (the heading path joined by " / ", " (Overlay 0)" or " (Overlay n)" after an overlay's row); Blank cells are empty, Pending "…", Missing "(missing)", Error "(error)"; the time report adds the Eclectic, Rolling minimum and Total rows after the laps, and `project` reads the project's `lapTimeFormat` handed to `setProjectTimeFormat` (m:ss.fff until then). Export quotes a field holding '"', ',', '%', a tab, CR or LF. (b) **`RenderService`** gains `pool()`, `dispatcher()`, `statCache()` (the workspace's `StatCache`, made on first use and shared by every `ReportModel`) and `setTrackMaps()`/`trackMaps()`; `prepareInputs` fills `ComponentBuildInputs::trackMap` with the map of the Main source's venue. (c) **`TrackMapService`** (`esses_qtmodel`, new public name, one per workspace): a venue's map read from the project's `trackmaps/` on first use and cached (an absent or unreadable file gives null, the latter with a Messages line); `setMap` validates (`TrackMap::validate`), writes the file at once (the previous one copied to `backup/` by `saveWithBackup` when asked: the Track Editor's Accept), and a removal moves the file to `backup/<stem>.<yyyymmdd-hhmmss>.removed.json`; without an open project the maps stay in memory; a project change drops the cache. **`TrackMapCommand`** (`QUndoCommand`, new public name) holds the map before and after and writes one or the other; every Track Editor edit is one such command (a rename is a parent of two). Track-map edits are written when they are made, as the Maths Editor's modules are on close (D-190), not by the session's Save, and undo writes the file back. (d) **Generation** is `TrackMapService::generate(TrackGenerationRequest, context, onGui)` (new names `TrackGenerationRequest`, `TrackGenerationState`, `TrackGenerationStatus`): the GUI thread resolves the lap's channels through `MathsService` (the GPS chain heads of `esses.gps_corrections`, Corr Speed, the lateral channel; logged channels through the source's `ChannelResolver`) and answers Started, Pending (a maths input is being evaluated; the caller retries on `channelReady`/`statusChanged`) or Failed (a missing input named); a worker reads the signals whole (`maths::readWholeSignal`), converts the stored unit through its quantity's base to degrees, m/s or m/s² (a unitless lateral channel is in G), and runs `TrackGenerator` at Background under `geom/track/<source>/<lap>`; the map's name and venue come from the source's Venue detail ("Track" without one) and `generatedFrom` is filled. The Track Editor runs `SectionDetector::defaults` when it accepts: the first acceptance of a venue makes the default sections, a later one keeps the user sections that still fit and regenerates the Default set, as one command. User sections get ids `u<n>` and the set "User" unless named, and the editor refuses a section under 10 m or off the track. Its constructor is `(ProjectSession&, DataSelectionModel&, MathsService&, TrackMapService&, QWidget*)` (the laps come from the selection's sources); Accept writes and leaves the modeless dialog open; the cursor span of "add from cursor span" is passed in by the shell (`setCursorSpan`). (e) **`SourceLoader::setTrackMaps`**: with a `TrackSectionsRule` in the runner and a `DistanceService`, phase 2 runs the `RuleRunner` overload with the source's venue map (null removes stale groups) and the lap indexes ready now (a lap starts at a beacon when an enabled BCN sits at its start or the layer has none), its key gaining `/t<map generation>/i<indexed laps>`; `distanceReady` and `mapChanged` start it again, and the run's Track Sections subtree replaces the current one in the newest snapshot with the same marker set. (f) **`TrackPosition`** keeps its own copy of the layer pipeline (it was written beside `LayeredComponent`, D-198 (c), and may move onto it): Static on Size, Scheme, Selection, Data, Offsets, Scale and a map change, Dynamic on Cursor, Datum, Zoom and Domain. PLUS/MINUS zoom ×1.25 about the centre (clamped to [0.2, 64]), the wheel about the pointer, a drag pans, a double-click fits, while the component is focused (or has the keyboard focus, in the dock); each step changes the body's `scale` at once and the gesture becomes one spec edit 400 ms after it rests ("Zoom track view"), a fit at once; the shell offers `track.zoom_in`/`zoom_out`/`fit` through `handleCommand`; a backgrounded map ignores the press. (g) **`TrackMapWindow`**'s body is dock state: edits apply at once, are not undoable and are not persisted in 0.1. (h) The report tables are two `QTableView`s over one proxy and one selection model (the frozen first column beside the rest), with column widths from the cells' text metrics and the layout settled before a Print to Clipboard, so the same table always renders the same; `TimeReport` takes `component.colour_band [n]` in `handleCommand` (a band beyond the list is a handled no-op) and ALT+n in `keyPressEvent` where no command runs. (i) Goldens: the report tables' pixel goldens are rendered by `QWidget::render` in a widgets test (`tst_reports_track_goldens`) beside exact table goldens (`tests/golden/tables/<type>/<case>.json`: text, tints, italic, bold); the track map's pixel goldens are `OffscreenRenderer`'s and its exact goldens are `DrawList` JSON with `FixedTextMetrics` (`tests/golden/drawlist/track_position/widget_<case>@<dpr>x.json`) from a rectangle map and constant-speed laps, so no transcendental function feeds a coordinate.
**Consequences.** The reports and the track map work in the application once the shell wires `TrackMapService` into `RenderService`, `SourceLoader`, the Track Map dock and the Track Editor (it does, `main_window_m3.cpp`); a time report shows sections as soon as a venue has a map and the lap indexes exist. A track-map edit is on disk at once and survives a crash without the journal, at the price of not being part of Save; `ReportModel`'s styling needs the widget's proxy wherever it is shown. The pixel goldens of this change were rendered on Windows and are provisional (D-127).
**User must confirm:** no.

#### D-200 The properties dialogs and the shell's M3 wiring as built: the component command hook, binders that keep untouched members and validate against the body schema, Apply as one spec edit followed through the canvas, the canvas edited in place, the clipboard payload, the palette's providers and order, animation state without a worksheet, workbook names as file names, the M3 keymap rows and their tests (refines D-027, D-032, D-133, D-193; amends 06 §2.2, §4.1-4.5, §12, §13.6, §14.4, §15, 04 §7.2, outline §4)
**Context.** The M3 contract left `src/properties_shell/` as stubs and gave the properties/shell work the M1 shell files. 06 §14.4 puts many rows in the focused component's key context (the graph's vertical zoom, the track view, the display toggles) but the component code lives in other works' roots and the shell had no way to hand a row to a component. 06 §12 asks for pages covering every body property (AC-M3-08) with inline validation, but not how a page avoids rewriting members it does not show (an absent member means the builder's default; a newer version's members must survive) or how a modeless dialog follows undo and redo. The M1 canvas rebuilt every component on any definition change, which would drop component-local view state (the graph's vertical zoom, the scatter's axis zoom) on every geometry edit or undo. 06 §4.3's "offset by one grid cell" leaves the pasted size open; 06 §15 leaves the palette's order and the recent list open; 06 §2.2 names a reduce-motion setting and a "reduce CPU" option that Qt 6 and the view model's fixed tick do not offer; the session's workbook set uses a workbook's display name as its file name; a native file dialog cannot carry the Open Log Details pane; and the M3 mouse rows have no key for the keymap harness to press.
**Decision.** (a) **`ComponentWidget::handleCommand(id, args)`** (virtual, default false) and `captionBarHeight()`: the shell offers the rows only a component can carry out (`zoom.v_in`/`v_out`/`v_full`, `pan.up`/`down`, `graph.copy_cursor_values`, `track.zoom_in`/`zoom_out`/`fit`, the menu-only `component.copy_equation`, and the programmatic forms of the mouse rows `zoom.rubber_band`, `zoom.full_axis`, `zoom.wheel`, `zoom.outing_rect`, `pan.drag_axis`, `cursor.place`, `offset.drag`) to the focused component; such a row is enabled while the focused component's key context is one of its contexts, and a component that declines it shows "Not available for this component". The worksheet rows of hotkey batch 1 (zoom, pan, cursor, datum) are offered to the focused component first while the keyboard focus is in the canvas (the scatter steps its cursor by Main points), else they run as in M1. The actions' enabled state is refreshed when the canvas focus moves. (b) **Generic body edits** (`component_commands.hpp`, new public names `genericComponentEdit`, `hasGenericComponentEdit`): L, K, G, S, ALT+n, Y, E, SHIFT+E, F3 and M for a component that does not handle the row itself, each one spec edit (one undo step); an absent member counts as the builder's default. (c) **`FieldBinder`**: `store()` writes over a copy of the body; a row whose editor still shows what `load()` put there leaves the member as it was (absent stays absent, a number keeps its exact value), so opening and applying changes nothing and members no row binds survive; `setSchema()` validates a changed row against the body schema's node of its pointer (type, enum, ranges, pattern, lengths, required members) and marks the editor red with the reason. The editors are the new public `field_editors.hpp` (leaf widgets, `UnitValueEdit`, `FieldForm`, `RecordListEditor`, `FixedArrayEdit`, `OptionalEditor`; `jsonptr`, `SchemaTree`). The page header (caption, links, legend) is bound by a second, internal binder, so `PropertiesDialog::binder()` binds exactly the body schema's pointers; the schema is read from `schemas/components/<type>-<version>.schema.json` through `SchemaRegistry::findSchemasDir` (M3 open issue 1 stands). (d) **`PropertiesDialog`**: Apply and OK emit `specEditRequested(spec, "Edit <type> properties")`, one undo step each; Cancel after an Apply restores the spec at open (one more step); the canvas announces every spec it applies (`WorksheetCanvas::componentSpecApplied`, new), which refreshes an open dialog after undo/redo; deleting the component closes it; its geometry is kept per type in layout.json `dialogs."properties.<type>"`. (e) **`WorksheetCanvas`**: a definition change updates the shown components in place (a component whose id remains keeps its widget and local state and receives `applySpec`); selection by click, CTRL+click, a band on empty canvas and `component.select_all`; move (caption, or SHIFT+drag), resize (6 px edge zone; a plain press on a control the component draws there, a scrollbar, a handle, the offset axis or the outing's zoom rectangle, stays the component's), CTRL+drag splitters, ALT+arrow nudges, z order renumbered 0..n-1; each is `layoutEdited(text, mergeKey)` after the definition changed, which the shell pushes as a `WorkbookCommand` merging consecutive geometry edits of one component; the grid snap (ALT turns it off) keeps 4 x 4 cells at least; project.json `ui.layoutLocked` refuses every layout edit. (f) **Clipboard**: the payload of `ComponentClipboard` with the same JSON as text/plain; a pasted rectangle keeps its size in whole cells and its snapped position, moved one cell right and down per paste of the same content into the same worksheet and wrapped inside it; fresh component ids; cut, paste and delete are one undo step each. (g) **`CommandPalette`**: the `>`, `@` and `#` lists come from providers the shell sets; a command runs after the popup closed and the focus returned to where it was when it opened, so it sees the context the key would (the AC-M3-12 harness); prefix matches first, then the recently used (settings key `palette.recent`, 20 ids), then by title; a command with keymap arguments is listed once per row. (h) **Animation**: `AnimationController` keeps its state without a worksheet and moves a running animation to the next one; the end of the outing turns it off; the rate is `options.animation.rate`, set by the new command `data.animation_rate [rate]` behind Data > Animation Rate (one `SetProjectOptionsCommand`). The reduce-motion start and "Reduce CPU during animation" are not implemented (Qt 6 offers no reduce-motion hint; the view model's tick is fixed at 33 ms). AC-M3-10 is measured by the `bench/plot_bench` row `plot.animation_frame_10x`: 60 frames of a x10 animation (`setCursor(..., CursorSource::Animation)` and the graph's Dynamic repaint), a Static build during the run being an error. (i) **`ZoomSummaryBar`**: a `plot::DrawList` from a pure `buildDrawList(Summary, LayoutContext, Scheme)` painted by `RasterBackend` (exact DrawList JSON goldens, provisional PNGs); positions are in the worksheet's domain relative to the Main range start, distance through the Main lap's `DistanceChain`; the menu form of SHIFT+click is `zoom.extend` "Extend Zoom to the Cursor". (j) **`CorrectedSpeedDialog`**: a `FieldBinder` over the module's settings schema plus its enabled flag and `options.lapStretch`, written by Apply as one `SetProjectOptionsCommand`; `documentsChanged` re-registers the module; the per-lap stretch report stays the Lap Editor's. (k) **`LayoutEditor`**: the qtmodel workbook commands (Move To is one undo macro); copies get fresh worksheet and component ids; a workbook name a file name cannot hold is refused, not slugified; the questions go through replaceable `Prompts`. (l) **The shell** (`main_window_m3.cpp`, `main_window_impl.hpp`): `MainWindow` owns the `TrackMapService` (RenderService, SourceLoader, the Track Map dock AutoHide on the right, the Track Editor); `MainWindow::Prompts` (file, folder, text, choose) replace the standard dialogs in tests; the new public `shell_dialogs.hpp` (`SelectWorksheetDialog`, `UnitsListDialog`, `OpenLogDialog`, the last a non-native `QFileDialog` with a Details pane probing through the `ImporterRegistry`); full screen hides the docks and ESC leaves it first. (m) **Keymap**: every M3 row is implemented with its `KEYMAP_TEST` (`tests/ui/keymap/keymap_m3_cases.hpp`); a mouse row's test sends the pointer events to the component through the canvas's application filter at points taken from the component's own layout; a summary row focuses a scatter and a track row a `track_position` showing the track-1 example map; `tools/check_keymap_coverage.py` keeps `CURRENT_MILESTONE = "M2"` (M3 is the milestone in progress) and the project keymap test accepts M3 rows.
**Consequences.** A component work adds keyboard and menu behaviour by answering `handleCommand`, without editing shell files; a properties round trip leaves a body byte-identical unless the user changed a field, so newer or hand-written members survive; component-local view state survives geometry edits and undo/redo; AC-M3-08, AC-M3-10 (2.35 ms per frame, Release), AC-M3-11 and AC-M3-12 have tests; the reduce-motion and reduce-CPU settings, the Track Editor's cursor span in track metres (`TrackEditor::setCursorSpan`) and auto-generation on open remain to wire.
**User must confirm:** no.

#### D-201 The time_distance completion as built: widget-local state and a build report beside the build inputs, override limits on the resolved channel, the datum's key columns in the Static layer, the M3 bands and panels, per-source offset steps and held drags, annotation edits through the selection, the key's label floor, per-case golden provenance (refines D-023, D-027, D-193, D-196; amends 04 §2.2, §8.1-§8.11, §8.13, 06 §14.4, outline §4)
**Context.** 07 §6.1 asks `time_distance` to cover parity-first §5.4 (04 §8). The M1 builders take only `ComponentBuildInputs`, but several §8 behaviours need state the inputs do not carry (the vertical zoom is view state local to the component, §8.3 item 6; the crosshair must line up with the axes the worker drew), data they did not carry (the channel override limits of §8.3 items 2 and 5), and results the widget needs back (band entries for the double-click zoom, measurement positions for the hover line). §2.2 makes Datum a Dynamic-only change while §8.4 adds value@datum and Δ columns that widen the key. 04 §8.14's example has the bands and scrollbars on, which would redraw every M1 golden. §3.3 defines the offset step per slot source, but the view model knew only the Main active channel's period, and the shell's offset rows act on the view model. The marker band's annotation Edit / Remove happen in the widget, which holds neither the session nor the SourceLoader. The design leaves open the status channels without the profiles' status patterns, the labels of overlay range cells aligned with Main's, the variance autoscale, where panel splitters are grabbed, the sign of Δ, and a narrow key elided to nothing by the new columns.
**Decision.** (a) `TimeDistanceBuilder::buildStatic`/`buildDynamic` gain overloads taking `TdLocalState` (per-group vertical zoom as the shown fraction of the scale, `TdGroupZoom`; the scales of the last Static build for the crosshair) and an optional `TdStaticInfo*` (band entries, markers, measurement positions, per-group scales, the scrollbar extent, the number of `VarianceTable`s built); the two-argument forms (OffscreenRenderer, `esses-render`) equal the default state. (b) `ResolvedChannel` gains `limitMin`/`limitMax` (override scale limits in base units); `RenderService::prepareInputs` leaves them unset and the widget fills them from `ProjectSession::overrides()`; Channel scaling takes them per end in place of the autoscale, Auto ignores them, `lines.min`/`max` draw at them. (c) A datum appearing or disappearing is a Static change (the key columns); moving a shown datum stays Dynamic only; Δ = value@main − value@datum, a positive Δ written with "+". (d) Body defaults follow 04 §8.14's example (bands, markers, device events and scrollbars on); the M1 golden cases switch the bands and scrollbars off in their bodies, so their DrawLists and PNGs do not change, except `dark_scheme`, whose datum now adds the key columns: its DrawList golden is regenerated and its ubuntu PNG is marked `linux-stale` (report mode) until replaced from the artefact. (e) Layout: the bands across the trace area's x range under the caption; the status strip (10 px rows) and the variance panel (`heightFrac` of the trace area, only with a visible overlay) above the x axis; the offset axis (2 × 12 px) below it; the horizontal scrollbar at the bottom; overlapped mode draws only the active group's grid and y axis, lists the groups in turn in the key, puts Lo/Hi handles at the right edge and keeps the whole area a Panel of the active group; panel splitters are grabbed left of the trace area (axis strip and key column), so a click in a panel always places the cursor; the vertical scrollbar is the active group's, in its axis strip. (f) Bands: the range band shows the Main range's group (the neighbouring laps when zoomed out) beneath the visible slots' cells (overlays first, Main on top), and slot cells lying mostly under Main's give Main one label "Main | overlay"; sections are the ranges of `bandGroup`, their category the child group holding them (Corners, Straights, else the child's name); markers are the enabled BCN, SOL and EOL, device events (comment, else name) while `showDeviceEvents`, and the Main source's sidecar annotations with a dotted line through the trace area. (g) Status channels resolve among the graph's traces, else in the Main source's layer; without a list the Main layer's Bitfield and Bool channels show (at most eight); a row is filled where its decimated column is non-zero. (h) Variance: one `VarianceTable` per visible overlay against Main (each slot's lap `DistanceIndex`, the chain piece holding its range start), x from the zoom, one column per device pixel, both slots' offsets; autoscale over the tables and 0 with 10 % headroom, manual limits taken as written; a missing index draws no trace for that overlay and the banner. (i) Measurements are read on the worker from the valid raw samples of the visible span (one chunk at a time) with the Static layer; the hover line is widget-local Dynamic. (j) `WorksheetViewModel::setSourceSamplePeriod` records the active channel's period in each visible slot's source (the focused graph reports it) and `offsetRange` steps a range by its own source's period; `beginOffsetDrag`/`endOffsetDrag` hold the 250 ms commit during a sub-axis drag and commit once at the release (one undo command). (k) Annotation Edit / Remove go through `DataSelectionModel::commitSidecar` (the hook `connectSourceLoader` installs: one undoable `SidecarCommand` with its own text); Add stays the shell's `cursor.add_annotation`. (l) `KeyLayout`'s column-aware layout keeps up to 30 % of the clamped column for the label and shrinks the cells below it. (m) `goldens.json` gains a `cases` map naming the provisional cases and where their PNGs came from (`windows`, `linux-stale`); a case not listed takes `generatedOn`; the thirteen M3 time_distance cases are Windows renders until replaced from the ubuntu artefact.
**Consequences.** OffscreenRenderer and the widget draw the same layers except the widget-local vertical zoom (and, without project overrides, the min/max lines); the M1 goldens hold except `dark_scheme`; the shell's offset rows step each range by its own source without knowing the graph; the graph answers the graph rows the shell offers to the focused component (`zoom.v_*`, `pan.up`/`down`, `graph.copy_cursor_values`, the body toggles and cycles, and the programmatic forms of the mouse rows), whose keymap rows the shell can mark implemented; an AutoscaleCache (04 §8.3) still replaces the summary stand-in later.
**User must confirm:** no.

#### D-202 The M3 acceptance as built: the completed circuit workbook, the range key colour of 06 §5.3 item 3 restored in the M3 builders, the F3 bench row, M3 as the keymap checker's milestone with the generic row, O zeroing as one undo step, the values_list goldens, the offscreen demo and the reports' project time format (refines D-198, D-199, D-200, D-201; amends 07 §6.2 AC-M3-05 proof, §6.5)
**Context.** The acceptance pass of M3 (07 §6.2) found what the separate works had left: 07 §6.1 "Data" names the default circuit workbook's overview, braking, corners, reports and track worksheets but not their contents, and the shipped workbook still had M1's two worksheets; 06 §5.3 item 3 gives each range a key colour (Main `cursorMain`, Overlay 0 `overlay0`, Overlay k `overlayPalette[(k - 1) mod 9]`) for the range band, Track Position dots, variance traces and scatter/histogram overlays, but the M3 builders took the slot's trace colour instead, which under the default `overlayStyle` (plain channel colours, D-017) gives every overlay the channel colour, so two overlays' variance traces could not be told apart and a scatter or histogram overlay drew in Main's colour; AC-M3-05 proves "F3 costs 0 ms" with `bench/plot_bench` without saying what is timed; `tools/check_keymap_coverage.py` still named M2 as the current milestone, and the generic row of 06 §14.5 for planned and reserved rows had no test; 04 §3.3 item 2 makes O-off one undo command while the view model commits each range and domain on its own; `values_list` had no goldens although AC-M3-02 covers all ten MVP types; and the demo script of 07 §6.5 had no offscreen form.
**Decision.** (a) **Workbook.** `data/workbooks/circuit-default.workbook.json` keeps worksheet 0 "Overlay" (AC-M1-05) and "Driver" and appends five worksheets: Overview (distance: a graph of Corr Speed, the pedals, gear and engine speed; a `track_position`; numeric gauges of Corr Speed and gear; bar gauges of throttle and brake), Braking (distance: a graph of Corr Speed, brake, throttle, longitudinal G and gear, F3 ready; a brake histogram in 10 % bins; a throttle-vs-brake scatter with a polynomial-3 trend and its equation), Corners (distance: a graph of Corr Speed, steering, lateral G and gear; a lateral-vs-longitudinal G scatter; a `track_position` with section labels), Reports (time: a `channel_report` of Corr Speed, throttle and brake avg/min/max under the headings Venue › Driver › Lap with Δ vs Overlay 0; a `time_report` of the Default track sections with eclectic, rolling minimum and total and bands 0.5/1/2 % shown and 3 % hidden, so ALT+3 colours it) and Track (distance: a `track_position`, an `outing` graph of Corr Speed, a graph of Corr Speed and the pedals). Channel names follow M1's ACC-style names (resolved case-insensitively) plus the neutral `G Force Lat`/`G Force Long`; the new traces of throttle and brake carry `displayUnit` `%`, because a logged percentage is stored in the base unit ratio (D-010) and shows as such without a display unit. (b) **Key colours.** `plot::rangeKeyColour(scheme, slot)` implements 06 §5.3 item 3; the time_distance variance traces, offset sub-axes and range band overlay cells, the Track Position overlay dots, scatter and histogram overlay sets and a gauge's reference value and marker use it; Main keeps its trace colours everywhere except the offset sub-axis (the Main key colour, red). The `DrawList` goldens of eleven cases changed in colour only and their provisional PNGs were re-rendered; the M1 goldens did not change. (c) **AC-M3-05's bench.** `plot.variance_f3_no_overlay` of `esses-plot-bench`: on the AC-M1-02 workload with Main only, the graph's own inputs are built on the pool with `variance.show` off and on alternately, 10 pairs per run; the row is the median of the per-pair differences, limit 1 ms (the timing noise of a 20-trace Static build), and any `VarianceTable` built fails it. (d) **Keymap.** `CURRENT_MILESTONE` is M3 (every M1-M3 row is implemented); `ui.tst_keymap` `plannedAndReservedRows` is 06 §14.5's generic row (each planned or reserved command without an implemented row is registered, disabled, shows "planned for <milestone>" or "reserved" and does nothing); `project.keymap` accepts planned rows of M4, v1.0 or later only. (e) **O off.** The shell's `offset.axis_toggle` wraps the view model's per-range commits in one undo macro "Clear alignment offsets" when a selected range has an offset. (f) **values_list goldens.** `widgets.tst_values_list_goldens`: three cases at DPR 1 and 2 painted by `QWidget::render` as the report tables are, exact table goldens `tests/golden/tables/values_list/<case>.json` with cells also computed by hand, provisional Windows PNGs (`"generatedOn": "windows"`, D-127). (g) **Demo.** `ui.tst_demo_m3` runs 07 §6.5 in the real shell over a log the test writes on the AC-M3-04 circuit's true centreline (an out lap, flying laps of 70, 68, 69 and 67 s, an in lap; one profile per lap fraction). Readings: "ALT+SHIFT+→ ×4 nudges Overlay 1" moves the reference overlay, which is Overlay 0 when one is selected (04 §3.3 item 3); "scope = Laps" is `selectedRanges`; "Export TSV" is the report's whole-table TSV copy (04 §12 item 7); "A → animation at 10×" sets the controller's rate to 10 (the Data menu offers ×0.25 to ×4; ×10 is AC-M3-10's bench rate). (h) **Report time format.** The shell gives every shown `channel_report` and `time_report` the project's `options.lapTimeFormat` (default `m:ss.fff`) as their `project` time format whenever the project options or the shown worksheet change (`ReportModel::setProjectTimeFormat` rebuilds only on a change), closing the open issue of D-199.
**Consequences.** A new project opens with a worksheet for every MVP component; overlays are distinguishable in every M3 component under the shipped schemes; AC-M3-05 has a measured bench row; the lint job checks every M3 row as due; the five worksheets and the new colours change no M1 golden. The M3 PNG goldens stay provisional until the ubuntu artefact replaces them (D-127).
**User must confirm:** no.

#### D-203 The M3 fix pass: the review findings fixed, rejected or deferred (gCorrection's slip reference, the report engine's pairing, trust and units, the histogram's closed last band, the track-sections origin rule for stretched and unfinished laps, loop closure and the GPS seam, idle hidden worksheets, the shell's add, context menus and asynchronous export, heading pickers, rename and move without a rebuild, D-127 (a) amended, the deferrals) (refines D-194, D-195, D-197, D-198, D-199, D-200, D-201, D-202; amends D-127 (a), 04 §7.2 item 8, §8.2 item 4, §12 item 6, 06 §13.5 item 4, §14.4)
**Context.** After the acceptance pass (D-202) three reviews of the M3 branch (an audit against the criteria and
the repository rules, a UI review and an engine review) reported 89 findings. Each was checked against the
design text and the code before anything changed: the ones that held were fixed with a regression test, the
ones that did not were rejected with the reason, and the ones larger than a fix pass are deferred below with
their owner. The letters are cited in the code and the tests. The per-finding record is the fix log of the pass
(build output, not committed); docs/milestones/M3.md §5 and §6 summarise it.
**Decision.** (a) **gCorrection's slip reference** (03 §9.1 item 3). Besides the per-sample test (the wheel
against the previous output plus a dt), a reference integrates the longitudinal acceleration and is pulled
towards the wheel by dt / 1 s per sample; a wheel that leaves the reference by more than the lock threshold also
switches to prediction, from the reference. A lock or a spin that builds over several samples passes the
per-sample test every time and is now caught; an accelerometer offset b moves the reference by at most b x 1 s
(refined by (ai)).
(b) **Report rows and units** (refines D-197). With `includeUntrusted` off an untrusted lap is cut out of every
row that contains it, not only of lap rows (an outing, a detail heading or no heading); a heading named after a
detail group (Events, Vehicles, Sessions) labels the row with that group's detail (Event, Vehicle Id, Session)
and "(no ...)" when the source has none; `start` (`end`) takes the first (last) valid sample only where the
channel covers the range bound, else only inside the probe; `absMax` and `delta` convert the extremes and take
the larger magnitude or the difference afterwards, which is right for an offset or negative-scale unit; each
column has one display unit (the first resolving row's) and every source's cells are converted into it through
the unit registry (`ReportInputs::units`); `a/b/yyyy` dates are day/month unless the second part cannot be a
month, and impossible dates are refused; the time report skips disabled laps and keys sections by name and
abbreviation, so the two sections of one name that the rule emits both show; `StatCache` keeps its index by open
addressing inside the 64 bytes per cell of the budget (`bytes()` counts slots and index). (c) **The Overlay 0
delta pairs rows below the compared ranges**: the pairing key is the part of a row's heading path that lies
inside the compared range (the sections of a lap, the laps of an outing); the range itself and the levels above
it (an outing, a detail label) are left out, because they differ whenever Main and Overlay 0 compare different
laps, outings or drivers. Main and Overlay 0 that are different laps, their normal use, now have delta cells. (d) **The histogram's last regular band is closed** on its
upper limit (`BandAxis` bins stay half-open otherwise), so a sample exactly at `dataMax` is counted.
(e) **Track sections** (refines D-195 (d)). A complete lap (beacon to beacon) whose stretched length is within
5 % of the map's length maps sections proportionally (lap distance = track distance x lap length / map length),
so they stay in place and add up to the lap; a lap that does not end at a beacon (an in lap ending in the pits, a
lap cut short) has no section past its last sample instead of a truncated one with a short time. (f) **Loop
closure and the GPS seam** (refines D-195 (b)). Speed+LatG removes the heading excess over the nearest whole
number of turns always, so a figure-eight circuit (zero turns) is closed too; `W-TRACK-NOT-A-LOOP` is reported
when the corrected ends still lie more than a quarter of the lap apart. The excess stays linear in time: a
lateral-offset model (spread by the integral of dt / v) was measured and was worse on the AC-M3-04 fixture
(4.35 m against 4.11 m RMS), so finding E18 is rejected. A GPS dropout across the lap start or end is bridged
through a virtual point at the seam with the start's tangent. Also: a timestamped x channel of a scatter grids on
the union of the channels' times; the through-zero trend basis is x / |x|max times centred powers, which keeps a
narrow band far from 0 well conditioned; scatter overlays are drawn in the main unit; the variance banner says
"variance compares single laps" when a compared range spans laps. (g) **Animation rates** are x0.25 to x10
everywhere (menu, controller, view model), so the demo's x10 is a menu choice (amends D-200 (h) and the reading of
D-202 (g)). (h) **Inert generic rows declined**: L on the time/distance graph toggles its key column (the body's
`keyMode`) instead of `spec.legend`, which it never draws; G, Y, E and SHIFT+E are the time/distance graph's
only; K is not offered on `track_position` (no colour legend before v1.0); histogram and scatter decline L.
(i) **The scatter's own zoom** is dropped when its channels change, and in the tiled layout only the shared x axis
zooms (each tile keeps its own y scale). (j) **Hidden worksheets idle**: a component of a kept, hidden worksheet
records view changes but builds no Dynamic layer for them and catches up when shown; on a selection or data change
it lets its inputs go, so a closed log is not kept alive by a worksheet visited earlier; a maths output becoming
ready rebuilds only the components that show that channel (`ComponentWidget::onChannelDataChanged`). (k) **An
edit of a component on a hidden worksheet** (its open Properties dialog) is stored in the session as an undoable
edit of that worksheet. (l) **The Properties dialog**: Cancel after Apply undoes only the dialog's own rows (a
move made meanwhile stays); rows the user has not changed follow any outside edit while rows with unapplied edits
keep them; the channel pickers offer every open source's logged channels and maths outputs and follow sources
opened later. (m) **Links** (`links.zoom`, `cursor`, `datum`) are edited and stored but not honoured until M4
(their tooltips say so); the round trip keeps them. (n) **Track Editor**: a default section the user renamed or
moved becomes the user's (kept by a regeneration); a track-map command whose write fails becomes obsolete, so
nothing is left to undo or redo. (o) **Keys a component handles itself** (the time report's ALT+n, the track
view's zoom keys) follow the user's keymap through a keys provider the shell sets. (p) **Import Workbook** gives
an imported workbook a valid, unique name and says so. (q) **Small shell repairs**: the zoom summary bar's ticks
are the laps' starts whatever Main is, its lines scale with the DPR; the Track Map dock follows a scheme change;
Add Annotation from the shell records the focused graph's active channel; the reports' context menus are
created on the heap and shown asynchronously, so a report destroyed while its menu is open takes the menu with it. (r) **Tests drain before a negative
check**: posted deliveries and the pool are drained before "nothing was rebuilt" is asserted, so a slow pool
cannot make it pass vacuously. (s) **Context menus**: a right-click on a time/distance channel label makes it
active and offers the channel menu (scale and offset, zero at cursor, filter, properties, edit maths, status,
hide, remove); elsewhere on a component the Component menu; on the empty canvas Add. (t) **Component > Add**
offers every registered type and places the new component in the largest free rectangle of at least 20 % x 20 %
(else over the whole worksheet, above the others), focused, as one undo step. (u) **Track maps from the shell**: the Track Editor's cursor span is
the datum-to-cursor span of the active worksheet in track metres; a log of a venue without a map generates one on
open when the project option allows (an automatic step, no undo command, announced in Messages).
(v) **Vertical zoom** (ALT+UP/DOWN) is about the active channel's value at the cursor when it is on the scale,
else about the shown span's middle (06 §14.4). (w) **The menu and palette forms of mouse rows** are enabled only
where a component answers them: the wheel on graph, outing, scatter, histogram and track components, the zoom
rectangle on the outing graph only. (x) **Export Data off the GUI thread**: a drawn component hands the shell a
self-contained job (copies of its inputs, never the component); the shell runs it on the pool at Interactive
priority and writes the file when it completes, so nothing waits on the GUI thread and the component may be
deleted meanwhile; the synchronous `exportData` stays for tests and scripting. (y) **Heading pickers**: the
channel report's headings, the time report's rows and the graph's band group are editable combo boxes offering
the active profile's report headings (range groups only where a range group is needed); free text stays
allowed. (z) **Reports on a hidden worksheet** wait for their next show instead of re-running on every change, and
a channel report re-runs on a maths output only when it lists that channel. (aa) **Report column widths** are
measured once per (font, text) and cached across deliveries. (ab) **The outing graph's handle geometry** (its
axis and zoom rectangle) is computed once per inputs revision, zoom and domain, not on every mouse move.
(ac) **Renaming or moving worksheets** keeps their view models (domain, offset axis), contexts and kept
components; only adding or removing one rebuilds the tabs. (ad) **The track fixture family** has no
`fixture.json` (it is not an import family: no input file, no decoding oracle) and `circuit.json` has no
`$schema` (the generator's own description, checked by `gen_track_fixtures.py --check`), so `esses-cli validate
--all` skips it; recorded in `fixtures/README.md`. (ae) **D-127 (a) amended**: `time_distance` has 21 named cases
in M3 (the cap of 12 was M1's), and pixel goldens rendered locally may be committed as provisional ("generatedOn":
"windows", report mode) until the ubuntu artefact replaces them; the `DrawList` JSON goldens stay exact everywhere.
The contributor guides (`CLAUDE.md`, `AGENTS.md`) state the amended rule. (af) **Deferred to M4 or later**: the group-heading behaviours of 04 §8.2 item 4 (drag a heading to
reorder groups, double-click for Group Properties; no `GroupHeading` hit is registered yet); the Track Editor's
speed-class column and section bounds in the Corr Dist unit (06 §13.5 item 4; `TrackSection` has no speed class);
loading track maps on the pool (the file is small and cached after the first read); the legacy CSV dialect for a
component's Export Data (it writes the plain time-and-channels table of D-198 (e); `esses-cli export` writes the
legacy dialect); the channel report's "ignore interpolated data" option; unit combo boxes for display units in
the Properties pages; the 0.5 % pixel bound of AC-M3-02 binds only when the ubuntu artefact replaces the
provisional PNGs (D-127). (ag) **Rejected findings**: the time report's band order (04 §13 item 3 says the bands
apply in list order, which is what is built), the time-linear heading correction (measured, (f)). (ah) **Help >
Keyboard Shortcuts** (06 §2.2, §15 item 5) is the menu command `help.keyboard_shortcuts`, which opens the palette
with the query `?` and widens its key column; like Component > Add (`component.add`) it has no keymap row,
because 06 §14.4 gives it no id and the keymap mirrors that table. (ai) **gCorrection under an accelerometer
offset** (refines (a); found by the re-verification of the pass). The reference of (a) settles b x 1 s from the
wheel, so a constant offset above lockThreshold / 1 s (2 m/s^2 by default) started a prediction with no slip at
all; the per-sample test then compared the wheel with a prediction drifting at b, and the output never came back
(121.7 m/s off after 60 s at b = 2.1 m/s^2, 10 and 100 Hz alike). With any offset, a slip long enough for the
prediction to drift past the lock threshold latched the same way (a simulated 120 s lap with random slips: 3 of
40 runs at 0.5 m/s^2, all 40 at 1 m/s^2). Now: (1) the prediction and the reference integrate the acceleration
less an estimated offset b, which the reference's error updates while the wheel is followed (gain dt / (4 x
(1 s)^2) per m/s, critically damped): a constant offset is learnt without overshoot and moves the reference by
at most 2 b x 1 s / e (0.74 b), so one below about 2.7 m/s^2 never starts a prediction on clean data. The
reference restarts on the wheel whenever the output is back on it, so the offset is learnt only from followed
samples; the hand-computed cases of 03 §9.1 item 3 keep their values (their estimate stays 0). (2) A prediction
lasts at most 3 s: a wheel still outside the band then is taken as right, b gains the prediction's mean drift from
the wheel over those 3 s (what an offset would have caused), and the output blends back over `recoverSeconds`,
the blend's per-sample test starting from the previous wheel sample rather than the output. No offset keeps the
output off the wheel for more than 3 s + `recoverSeconds` at a time, and a constant one does so at most once
(5 m/s^2 from the first sample: at most 2 + 5 x (dt + 3 s) = 17.5 m/s off, on the wheel for good from 4.0 s at
10 Hz). (3) The remaining excursion is inherent: for its first 2 s a constant 5 m/s^2 offset is the same input,
mirrored, as the 5 m/s^2 spin-up that (a) must bridge, so no causal rule stays on the wheel there. A slip longer
than 3 s is not bridged and teaches a wrong offset, which the next time-out corrects. Not chosen: leaving the
prediction when the wheel's rate agrees with the acceleration (it does at the top of every smooth slip and under
a steady slip ratio), and an offset estimated over the whole log (the output at a time would depend on later
samples). Before/after (constant 30 m/s wheel, 60 s): b = 2.1 and 2.5 m/s^2 now output the wheel sample for
sample (121.7 and 147.8 m/s off before); b = 5 m/s^2 is off by at most 16.7 m/s for 3.5 s (299 m/s before).
**Consequences.** Every finding that held has a regression test; the remaining gaps are listed in M3.md §6 with an
owner. No M1 golden changed; the track position goldens changed with (e) and the zoom summary @2x goldens with
(q) (DrawList JSON exact, PNGs provisional).
**User must confirm:** no.

#### D-204 The Track Sections pass keyed by the identity of its lap indexes: a stretch published at the same index count runs it again, the dedupe is against the layer's latest run, only the latest run publishes, a closed source's runs are forgotten (refines D-195, D-199 (e); reads 02 §4.4 rule 13)
**Context.** A review of PR #4 (Codex, P1) found that `SourceLoader`'s phase-2 key recorded only how many lap
indexes a Track Sections run read (`/i<indexed laps>`, D-199 (e)). `LapStretchService` publishes its factors
(`setRangeStretch`) once the laps' indexes exist, and `DistanceService` then hands out copies of the same indexes
with the new stretch: the count does not change. Nothing started phase 2 for that publication (no
`distanceReady` announces a copy, and Corr Speed's version holds), and a run started for another reason would
have been deduplicated against the unstretched run of the same key. The stretch moves the sections of a lap
measured backwards from its end beacon (an out lap), of a lap measured forwards that does not end at a beacon
(an in lap), and of a complete lap whose stretched and unstretched lengths lie on different sides of the map's
5 % tolerance (D-203 (e)); a complete lap within the tolerance either way maps proportionally and keeps them.
Reproduced: with a 2050 m track length on the synthetic oval (laps of 2000 m) the out lap's Turn 1 stayed at
6.06 s, its unstretched time, instead of 6.70 s. Any index replaced at the same count (one rebuilt from a newer
Corr Dist while Corr Speed's version holds) had the same key. The dedupe set also kept every key ever started
(it only grew, also past a source's close), so inputs that came back to an earlier run's (the stretch cleared
again) were never annotated again: the sections stayed stretched.
**Decision.** (a) While a map is bound the run key ends `/t<map generation>/i<indexed laps>-<digest>`; the digest
(xxh3-128, laps in `RangeId` order) covers each indexed lap's id, its index's D-140 key (Corr Dist version,
bounding markers and times, stretch) and whether it starts and ends at a beacon. Equal digests read equal
indexes, so the dedupe still holds for identical inputs, and a replaced index changes the key at any count;
identity by key, not by pointer, so a copy with the same key is the same input. (b) `setRangeStretch` starts
phase 2 for its layer after it publishes new factors (`LapStretchService`'s deferred update among them). (c) The
dedupe is per layer, against the latest run started (running or done) instead of every run ever started: a
request with that run's key returns, any other becomes the latest, and the older runs of the layer are cancelled
as before (02 §4.4 rule 13). (d) Each start has a serial: a finished run that is no longer its layer's latest
publishes nothing (it finished before it saw its cancellation; the newer run publishes), and a latest run that was
cancelled or failed is forgotten, so a later request runs it again. (e) `close` forgets the source's runs.
**Consequences.** The Track Sections groups, and the section bands and time reports read from them, follow the
lap-stretch toggle, the track length and a re-evaluated Corr Dist. Regression tests:
`qtmodel.tst_track_sections_pass` `sectionsFollowTheLapStretch` (a stretch published at the same index count
moves the out lap's Turn 1 to the stretched time, leaves a flying lap's in place, and clearing it brings the
unstretched time back; it fails without (a), without (b) and with the dedupe of every earlier run instead of
(c)) and `sectionsComeBackAfterAReopen` (a guard for (e): a file closed and opened again is annotated). No golden
changes.
**User must confirm:** no.

#### D-205 Track-map file names that keep the venue's identity: a readable slug and a hash of the exact venue, a file read only for the venue it holds, a write refused over another venue's file, the names of earlier builds read and moved (refines D-195 (e), D-199 (c); reads 02 §6.9 `trackmaps/<venue>.track.json`, amends outline §4)
**Context.** A review of PR #4 (Codex, P2) found that `TrackMapFile::fileNameFor` kept only a lower-cased slug of
the venue, so distinct venues shared one file: "Spa / Francorchamps" and "Spa-Francorchamps" both became
`spa-francorchamps.track.json`, every venue without a letter or digit `track.track.json`, and venues that differ
only in ASCII case, spacing or punctuation (on a case-insensitive file system also in non-ASCII case) collided
too. `TrackMapService::setMap` wrote that derived path directly, so saving the second venue overwrote the first
while the cache still showed both maps; `map()` read the file without looking at its `venue` member (only
`venues()` compared it with the name), so after a restart both venues loaded the map written last. Reproduced:
`qtmodel.tst_track_map_service` `venuesOfOneSlugKeepTheirOwnMaps` against the previous code (HEAD 0baa197) left 2
files for its 4 venues. The track-1 document already stores the venue (`TrackMap::toJson` always writes it).
**Decision.** (a) **The name.** `fileNameFor(venue)` is `<slug>-<hash>.track.json`. The slug is the readable part:
ASCII letters and digits lower-cased; every run of other ASCII, C1 controls, Unicode spaces, format characters,
noncharacters or invalid UTF-8 one `-`; other UTF-8 kept; at most 48 bytes (`kMaxSlugBytes`), cut between
characters; `track` when nothing is left. The hash is the first 16 hex digits of xxh3-128 (`esses::hash128`, 02
§1 behaviour 3) of the venue's exact bytes, so venues that differ in any byte get different names, and the
lower-case hex keeps them apart on a case-insensitive file system. A name has at most 76 bytes, is valid UTF-8
(macOS refuses other names), is never a Windows device name (the stem always ends with `-<hash>`) and never ends
with a dot or a space. (b) **The identity.** The file's `venue` member decides whose map it is (track-1 is
unchanged; an absent member reads as ""). `TrackMapFile::loadFor(folder, venue, log)` reads the hashed file, else
the legacy file, and passes over a file whose `venue` is not exactly the venue; a file that cannot be read counts
as the venue's unless it names another venue, so its error reaches Messages as before; NotFound when there is
none. `TrackMapFile::claimFor(folder, venue)` gives the file every write and removal of `setMap` uses, and refuses
(Io, nothing changed) when the hashed name holds another venue's map, so a hash collision or a file copied there
by hand is never overwritten. `setMap` stores a map whose `venue` differs from the venue it is stored under as a
copy bound to that venue, so the file reads back for it; `venues()` lists a venue a file names only when
`loadFor` finds its map, the rule `map()` uses. (c) **Earlier files.** `legacyFileNameFor` is the previous name,
unchanged. `loadFor` still reads it, but only a file that holds exactly the venue (a map of "Spa / Francorchamps"
written by an earlier build is found for that venue and never for "Spa-Francorchamps"); `claimFor` renames it to
the hashed name on that venue's next write or removal, so `setMap`'s backup copy and the removal's
`backup/<stem>.<stamp>.removed.json` work as before, and another venue of the same slug leaves it alone. A
hashed file wins over a legacy file of the same venue; a legacy file that cannot be read is reported and never
moved. New public names: `TrackMapFile::legacyFileNameFor`, `loadFor`, `claimFor`, `kMaxSlugBytes` (outline §4).
**Consequences.** Distinct venues never share a track-map file, no venue shows another venue's map, and no write
replaces one. Maps written by earlier M3 builds keep loading for their own venue and move to the new name when
next edited. The names are longer and cannot be written by hand without the hash (`TrackMapService::pathFor` gives
them). Regression tests (each fails on the previous naming): `project.track map file: the
file name of a venue` (the published xxHash vectors of `libs/base/tests/hash_tests.cpp` pin the hash, so no
build renames another's files), `... distinct venues never share a file name`, `... a map saved for one venue is
never loaded for another`, `... a file of an earlier build is read for its own venue and moves on its next
write`, and `qtmodel.tst_track_map_service` `venuesOfOneSlugKeepTheirOwnMaps` (the review's examples through
`setMap`, a restart and `venues()`, the legacy file, the binding copy and the refused overwrite). Not chosen: a
numeric suffix on a clash (the name would depend on what the folder held when it was written, and the file
would still not say whose map it is) and the hash alone (unreadable names). No golden changes.
**User must confirm:** no.

### A.15 M4 (proposed, 2026-10-18)

#### D-206 The M4 contracts: one shell hook with an extension per package, frozen shared headers with per-package nested state and hook calls, the open-time budget check at publication with Continue and Cancel as a Messages row's two actions, a polled PerformanceMonitor, the re-stat check of every source, the Data window's stale query, DprWatcher and DockHost::windowOf, no new keymap rows, the wave-2 placeholders (refines D-011, D-043, D-193 (f); reads 07 §7.1, 01 §7.2, 02 §1 behaviour 5, 06 §6 and §17; amends 06 §2.2, §11, outline §4)
**Context.** M4 (07 §7) runs as several workflows: wave 1 builds the budget warning, changed-file detection and
reload, the HiDPI work and the M3 follow-ups in parallel, wave 2 packaging, the soak, the Saturday walkthrough and
the user docs. Every one of them touches the shell, and three of them `SourceLoader`; `main_window.cpp`,
`main_window_m3.cpp`, `main_window_impl.hpp` and `source_loader.cpp` would otherwise be edited by several agents
at once. The design text leaves open where the open-time budget check sits and what it compares ("the probe
estimate (raw + pyramids) exceeds the budget", 07 §7.1; "with continue or cancel", 01 §7.2) while a Messages row
carries one action (06 §11); how the Stats reach the GUI when the cache's own hook runs on workers; how a source
whose importer keeps no mapping (the text importers) is checked for a change on disk (02 §1 behaviour 5 names
`MappedFile::changedOnDisk()`); where the stale state lives; how a DPR change reaches the components of a
floating dock (06 §17 items 3-4); and whether the M4 commands without a row in 06 §14.4 get keymap rows.
**Decision.** (a) **One shell hook.** `ShellExtension` (`shell_extension.hpp`): `MainWindow` creates one per M4
package in a fixed order (budget, reload, hidpi, followups, about, docs; `main_window_m4.cpp`) as direct
`QObject` children named `m4.<key>` and calls `buildWidgets(central)`, `registerCommands()` (before the planned
rows), `buildMenus()`, `windowReady()`, `worksheetActivated(i)` and `teardown()` (first thing in the destructor;
the extensions are then deleted, before any widget or model) at fixed points; an extension reaches the window's
private state through `impl()` (`MainWindow` befriends the base) and the window's `hint` and `showEditor`. The
shell files listed in `docs/milestones/M4.md` §3 are frozen for the packages. (b) **Frozen shared headers.**
`source_loader.hpp` gets one marked section per package; each package defines its section's members in its own
file and keeps its private state in a nested struct (`SourceLoader::M4Budget`, `M4Reload`: members of the class,
so they reach its private members without a header edit), created on first use behind a `shared_ptr`. The
contract puts the hook calls into `source_loader.cpp` (`published()`: `budgetHolds` before anything else is done
with the source, `reloadPublished` at the end; `close()`: `budgetForget`, `reloadForget`; `cancelOpens()`:
`budgetDropPending`), and the followups package, which owns that file, keeps them. (c) **The open-time budget
check** happens at publication (step 8, GUI thread), before the source is registered with the maths and before
the summary pass, so no chunk of it is decoded; it compares `formats::estimateResident` of the opened source
(every logged channel at its storage dtype and size, in the chunk layout of `esses_store`: samples, the factor-4
pyramid, a bitmap per chunk as the upper bound, headers) with the cache's budget: `OpenBudgetCheck::exceeds()`
is estimate > budget, and the resident bytes are reported, not added (07 §7.1's comparison). The "probe
estimate" is read as this estimate from the probe's channel sizes and dtypes (`estimateResident(const
ProbeResult&)` gives the same from a probe, for the CLI). The check is off unless the shell sets the gate
(`setBudgetGate(&runtime.cache())`), so headless opens never wait. The budget setting is
`memory.chunkCacheBudgetBytes` (02 §6.9), applied only when the key is present (the runtime's default, or a test
runtime's small budget, stays otherwise) and raised to 1 GiB. (d) **Continue and Cancel** are the two actions of
one Messages row (Budget, Warning): `qt::Message` gains `altAction`/`altActionArgs` (`MessagesModel::
AltActionRole`), run by `MessagesWindow::invokeAltAction`; the commands are `file.continue_open` and
`file.cancel_open` with the pending id, without a keymap row or a menu (06 §11: an action is a command id with
arguments; no modal dialog for a diagnostic). (e) **Stats**: `qt::PerformanceMonitor` polls `ChunkCache::stats()`,
the budget and `TaskPool::stats()` on a GUI timer (1 s by default) instead of a push from the cache, whose hook
runs on workers; the status bar's Performance field is `statusField("performance")`, the Messages dock's Stats
line `MessagesWindow::showPerformance`. (f) **Changed files.** `formats::checkSourceFile` re-stats a source's
path against the size and mtime of its provenance for every importer (the text importers keep no mapping; the
`.ld` provider's own check before each decode batch stays as built); `SourceLoader` holds the stale state
(`checkChangedFiles`, `isStale`, `staleSources`, `staleChanged`), its Import Warning row `open.file_changed`
carries `file.reload_source` with the source id, `reload()` is a close then an open through the pipeline with the
selection restored on publication (`sourceReloaded`), and the Data window's model asks a stale query
(`DataWindowModel::setStaleQuery`, `StaleRole`). (g) **HiDPI.** `DprWatcher` follows the device pixel ratio of
the main window and of every floating dock window and asks the components under it to re-check theirs
(`ComponentWidget::recheckDevicePixelRatio`: the rebuild `QEvent::DevicePixelRatioChange` already queues);
`DockHost::windowOf(id)` names a dock's top-level window. (h) **Keymap.** No keymap row is added:
`file.reload_source` and `help.contents` stay `planned` M4 until their package implements them (each package adds
its `KEYMAP_TEST` entry to its own `tests/ui/keymap/keymap_m4_<package>_cases.hpp`), and the menu or action
commands 06 §14.4 does not list (`help.about`, `file.continue_open`, `file.cancel_open`) get none, as
`help.keyboard_shortcuts` (D-203 (ah)). (i) **Wave-2 placeholders.** `AboutShell` registers `help.about` in the
Help menu and opens `AboutDialog` (06 §2.2 Help > About); `buildInfo()` and `versionReport()` are what
`esses --version` prints (`main.cpp` prints `versionReport()`, whose first line stays "esses <version>");
`DocsShell` will register `help.contents`; portable mode stays `esses::Paths`' `portable.marker`; the top-level
`CMakeLists.txt` adds `tests/soak/` and `tools/packaging/` when they exist. (j) `SourceLoader::summariesReady`
(M3.md §6 item 18) is declared for the followups package. (k) The Qt-free M4 tests of `esses_formats` are one
globbed executable over `tests/m4/**` (`formats_m4.<test>`).
**Consequences.** The packages edit disjoint files (`docs/milestones/M4.md` §3) and replace `M4-STUB`s; nothing of
M4's behaviour exists yet: every hook is a no-op and every stub inert, so the application behaves as at M3 except
that Help has an About entry. Final and tested now (`formats_m4.m4 contract: ...`, `qtmodel.tst_m4_contract`,
`widgets.tst_m4_contract`): `ResidentEstimate`'s sum, the state names, the setting key and floor,
`OpenBudgetCheck::exceeds`, `PerformanceMonitor`, the M4 signals, the alternate action, the stale query, the
extension list, `DockHost::windowOf`, Help > About and `buildInfo()`. A later change of a frozen declaration is a
coordinator edit together with its consumers. Names added: outline §4 "Names added by the M4 contracts". No
golden changes.
**User must confirm:** no.

#### D-207 The memory budget as built: the estimate as the upper bound of every chunk, Continue as the held open's own publication, Cancel releasing only an unshared decode log, the warning row's alternate action on SHIFT and in a row menu, Continue and Cancel enabled while an open waits, the setting floored and given back, the Options field in GB, the Performance field and the Stats line, the resident row as the peak resident set of a fresh process (refines D-011, D-043, D-206 (c)-(e); reads 07 §7.1, §7.2 AC-M4-02, §12, 01 §7.1-§7.2, §8.3, 06 §2.1, §11; amends 06 §11, outline §4)
**Context.** D-206 put the open-time check at publication and made Continue and Cancel a Messages row's two
actions. It left open how a held open is published on Continue without meeting the gate again, what Cancel frees,
how the alternate action is reached (06 §11 gives a row one action, run on double-click or ENTER), what the two
commands do without a pending id (the command palette lists every enabled command), how the budget setting behaves
below its floor, when the key goes away and when the runtime outlives the window, how the Options field shows a
budget the user never set, which units the texts use, and what "resident" means in 07 §12's row "Resident memory,
RW-2 + 20 maths channels <= 1 GB" (the chunk cache's count, the process's private bytes or its resident set) and how
`bench/open_bench` measures it.
**Decision.** (a) **The estimate** counts every chunk as `ChunkBuilder::allocate` lays it out with a validity bitmap,
the upper bound: an AllValid chunk carries none, so once every chunk is resident the estimate exceeds
`ChunkCache::Stats::residentBytes` by exactly the bitmaps of the AllValid chunks, and equals it when every chunk
carries one; an I64 time column counts its samples and header only; a column several channels share counts once
(`Column::id`); the probe estimate adds an I64 time column per timestamped channel. The texts use 1 KB = 2^10, 1 MB =
2^20 and 1 GB = 2^30 bytes: whole KB, one decimal below 10 MB, whole MB, then GB with one decimal. (b) **The gate.**
A held open waits in `SourceLoader::M4Budget` with its snapshot and its Import Log; Continue hands the same snapshot
to `published()`, which lets it through once, so a second open of the file published meanwhile makes it
`open.already_open` (06 §19 item 6), and only then are its Import Log rows posted, its maths registered and its
summary pass started. A held open is not counted by `pendingOpens()`. Cancel and `cancelOpens()` post
`open.budget_cancelled` (Import, Info) and release the registry's decode log only when no open source and no other
waiting open has the same id; closing a source keeps the waiting opens of its file. (c) **The row**: Budget,
Warning, code `budget.open_estimate`, the file as its source, the text "needs about <estimate> of memory once
loaded, above the <budget> memory budget (<resident> in use). Continue to open it anyway, or Cancel."; action
`file.continue_open`, alternate action `file.cancel_open`, both with the pending id. The Messages dock is shown and
the status bar's message area says so. 06 §11 is amended: a row's alternate action is a second link-coloured suffix
run with SHIFT+double-click or SHIFT+ENTER, and a row's context menu lists both actions with their enabled state.
(d) **The commands** "Continue Opening" and "Cancel Opening" (Global, no menu, no keymap row, D-206 (h)) are enabled
while an open waits; with the pending id they answer that open, without one (the palette) the newest open still
waiting; an id that no longer waits leaves the hint "That open is no longer waiting". (e) **The setting** is applied
at `windowReady` and on every `SettingsStore::changed` of its key: an integer or a double above 0, raised to 1 GiB
(above SIZE_MAX: SIZE_MAX); absent or not a positive number, the runtime's budget of `windowReady`
(`BudgetShell::runtimeBudget`) comes back, and `teardown` gives the runtime its own budget back when the setting had
changed it (a runtime can outlive its window, as in the tests). Options > General "Memory budget": GB of 2^30 bytes
with one decimal from 1.0 to 4096.0, showing the applied value, or `ChunkCache::defaultBudgetBytes()` while the key
is absent; it is written, as an integer byte count, only when the user changes it. (f) **Stats**: the
`PerformanceMonitor` samples every second and at `windowReady`, at a change of the setting and at Continue; the
status bar's "performance" field shows `performanceLine` ("Resident 412 MB of 4.0 GB", " (over budget)" above it)
with `performanceDetails` as its tool-tip, and a click shows the Messages dock (06 §2.1); the dock's Stats line above
the list shows `performanceDetails`, hidden until the first snapshot. (g) **The resident row**
`open.resident_rw2_20maths` (07 §12, AC-M4-02): RW-2 as `bench/workloads.md` defines it (the channel set of the maths
readiness set), written once as a `.ld` with an `.ldx` holding a beacon every 90 s through the motec.ld writer; then
each run is a fresh process (the bench started with `--resident-run`): `SourceLoader::open` with the circuit rules,
the built-in maths and the lap indexes, the 20 expressions of the readiness set as one global module, every output
evaluated, every chunk of every logged channel and of every output resident, the pool idle. "Resident" is read as
the strictest figure, the process's peak resident set (Windows: the peak working set, which includes the mapped
file's pages; Linux and macOS: `ru_maxrss`); the row is its median over the runs against 1 GB = 2^30 bytes, with the
chunk cache's resident bytes and the private bytes recorded beside it. A byte row is written under "memory" in the
JSON, not into the "benchmarks" array of timings. The bench reaches the writer through `formats::ImporterRegistry`
as `SourceLoader`'s header exposes it (`open_bench` declares no `esses_formats` dependency in `bench/CMakeLists.txt`).
**Consequences.** AC-M4-02's automated half passes (`ui.tst_budget_warning`, `qtmodel.tst_memory_budget`,
`formats_m4.budget: ...`, `widgets.tst_budget_shell`, `widgets.tst_options_dialog`). On the reference machine of
`bench/workloads.md` (Release, Qt 6.7.2, median of 5) RW-2 + 20 maths channels peak at 863,617,024 bytes resident
(823.6 MiB), the chunk cache holding 552,863,744 bytes (527.3 MiB, 1,328 chunks) and the peak private bytes at
619,896,832 (591.2 MiB). `SessionRestore` (a file of the followups package) does not wait for an open held at the
gate, so a project's restored selection misses a file the user continues later. No golden changes, no keymap rows.
New names: outline §4 "Names added by the M4 budget package".
**User must confirm:** no.

#### D-208 Changed source files as built: E-5's answer on NTFS (the POSIX-semantics, ReplaceFileW and rename-aside writers pass, a classic MoveFileExW replace is refused whatever the share mode, so the sharing policy stays), the stale mark sticky until reload or close, the Messages row added by connectSourceReload, Reload as a close then an open with the selection parked by the close, the command enabled while a source is stale, a fault-injection seam in the .ld provider and ld.io_fault once per source (refines D-206 (f); reads 02 §1 behaviour 5, §8 AC-4a, 06 §5.6, §6, §14.4, E-5; amends 06 §6 "Changed files", outline §4)
**Context.** 02 §1 behaviour 5 and §8 AC-4a expect a writer that replaces a mapped log by rename-over to succeed
while the view keeps the old bytes, and E-5 asks whether it does on NTFS, on older Windows 10 builds and on SMB.
D-206 (f) left open where the stale source's Messages row is made (an Import Log entry carries no action, and
`connectSourceLoader` lives in a file another package owns), how a reload keeps the selection when the close that
starts it drops the source's slots (06 §5.2 closeSource), what "disabled unless the source is stale" means for a
command the Messages row runs with an argument, and how the "a fault inside `guarded()` seals the chunk
`AllInvalid`" half of AC-4a is tested where the system refuses the truncation that would cause a real fault.
**Decision.** (a) **E-5 on NTFS** (measured on Windows 11 build 26300; the same Catch2 cases run on windows-2022):
the sharing policy (`FILE_SHARE_READ | FILE_SHARE_DELETE`, never `FILE_SHARE_WRITE`) stays. A writer that replaces
the file with POSIX semantics (`SetFileInformationByHandle(FileRenameInfoEx, REPLACE_IF_EXISTS |
POSIX_SEMANTICS)`, what `AtomicFile` does), with `ReplaceFileW`, or by renaming the old file aside and moving the
new one in succeeds, and the view keeps the old bytes; every in-place writer that opens the file for writing
(`CREATE_ALWAYS`, `TRUNCATE_EXISTING`, `OPEN_EXISTING`, `OPEN_ALWAYS`, `std::ofstream`) gets
`ERROR_SHARING_VIOLATION`. A classic `MoveFileExW(MOVEFILE_REPLACE_EXISTING)` (and MSVC's `std::filesystem::rename`,
which uses it) is refused with `ERROR_ACCESS_DENIED` while any handle holds the target open, whatever that handle's
share mode (an unmapped `FILE_SHARE_DELETE` reader too), so no share mode admits it: such a writer fails cleanly and
changes nothing, and the user closes the log or the writer retries. The tests pin the writers that must pass and,
for the classic one, the invariant (refused: nothing changed; replaced: the old bytes kept) with its outcome
reported. SMB stays the maintainer's (`docs/milestones/M4.md` §6.2); the same cases run on a share through
`ESSES_TEST_SMB_DIR`. (b) **The stale mark** is the loader's (`checkChangedFiles`, run on application focus):
Changed or Missing marks a source once and stays until the source is reloaded or closed (a file that comes back
unchanged stays stale); it is not taken from the providers' DecodeLog, whose own check before each decode batch is
unchanged. (c) **The Messages row** (Import, Warning, `open.file_changed`, "file changed on disk ...", the action
`file.reload_source` with the source id as 32 hex digits) is appended by the new `qt::connectSourceReload`
(`source_reload.hpp`) on `staleChanged(id, true)`, which the shell calls beside `connectSourceLoader`; a missing
file has its own text. (d) **Reload** is a close then an open (D-206 (f)). `DataSelectionModel::closeSource` parks
what it takes from the selection for the eight newest closes (`kParkedCloses`): the source's slots with their
positions and each range's lap number and class, its own filter, the source that followed it in the Data window,
and the range it moved into Main with the slot that range held; `sourceReloaded(previous, source)` gives it back:
each slot by RangeId, else by lap number and class (06 §5.6), else dropped with a hint, the moved Main back to its
slot, the source before its old successor. Two reloads in flight hand Main over between their parked closes, so
either publication order restores both; `closeAllSources` clears the parking. A reload ends without a publication
when its open fails, is cancelled or finds the file already open (the loader's own `openFailed` and Import Log
codes), and `pendingReloads()` drops it. (e) **`file.reload_source`** ("Reload File", File menu, data_window, no
key) is enabled while any source is stale; with an argument (the Messages row, the Data window's Reload File,
enabled on a stale row) it reloads that source, without one the highlighted source when it is stale, else the first
stale source in Data-window order. The keymap row is implemented. (f) **The fault test.**
`LdChunkProvider::setDecoder` (a private header of `esses_formats`) replaces `decodeConverted` so that the Windows
test raises an in-page error inside `MappedFile::guarded()`; on POSIX the fault is real (the mapped file is
truncated after a file of the same size and time took its name). `ld.io_fault` is logged once per source (the
source's DecodeLog), as behaviour 5 says, no longer once per chunk; the check and the add are one step under a lock,
so channels of a source faulting together on several workers still log it once. (g) **The badge** is an amber disc with an
exclamation mark before the filter icon, with a tool-tip line; the row's text is unchanged.
**Consequences.** The automated half of AC-M4-10 passes on NTFS and in its POSIX form; the SMB case and E-5's
older-Windows-10 question stay on the VM checklist. A writer that replaces files with a classic `MoveFileExW`
cannot replace a log Esses has open (the release note says so). Flipping the keymap row changes two tests outside
this package: `widgets.tst_m4_contract` `plannedM4RowsStayPlanned` (no longer `file.reload_source`) and
`project.keymap: hotkey batch 1 ...` (an implemented row of the milestone in progress, M4). New names: outline §4
"Names added by the M4 reload package". No golden changes.
**User must confirm:** no.
#### D-209 HiDPI as built: per-monitor DPR through a watcher of every top-level window with a re-check after dock moves, crisp axis-aligned strokes on whole device pixels at a fractional DPR drawn by the backend, every layer painted from a whole device pixel, the definitions of crisp and no drift, the offscreen DPR sweep and its goldens (refines D-025, D-206 (g); reads 04 §6.6, 06 §1, §17, 07 §7.1 row 3, §7.2 AC-M4-07; amends 04 §6.6 items 1, 3 and 4, outline §4)
**Context.** 07 §7.1 asks for a sweep at DPR 1, 1.25, 1.5 and 2, mixed-DPI floating docks and a relayout on a
per-monitor DPR change, with the goldens at 1 and 2 staying authoritative (D-025); AC-M4-07 asks that a floating
time_distance dragged between a DPR 1 and a DPR 2 monitor re-render with no blur and no layout drift. As built at M3,
a component rebuilt on Qt's `DevicePixelRatioChange` and `ScreenChangeInternal` widget events (04 §6.6 item 3 names
the `QWindow` signals), but Qt sends no such event to widgets moved into another window (a dock floated or docked
back). At a fractional DPR the builders' widths of logical x DPR (1.25, 1.5 or round(DPR) = 2 px) on pixel centres or
data positions were drawn antialiased over a pixel more than their width (soft grid lines, ticks, cursor and
outlines), and a widget at a fractional device position painted its Dynamic and Chrome lists a fraction of a pixel
off the Static image, which the raster engine blits at the rounded position. "No blur" and "no layout drift" had no
testable definition.
**Decision.** (a) **The watcher.** `DprWatcher` follows top-level windows through an event filter on the top-level
widget (`DevicePixelRatioChange`, `ScreenChangeInternal`, `Show`; no `QWindow` is forced into existence) and runs one
coalesced check per window on the next event-loop turn: every `ComponentWidget` under it re-checks its DPR
(`recheckDevicePixelRatio`: laid out at another DPR than its window's, it queues the `Size | Scheme` rebuild a DPR
event queues, at most once) and every `WorksheetCanvas` applies its 1 % placement again (`refreshGeometry`, new: the
coalesced relayout and a repaint); a changed window DPR is counted and announced (`dprChanged`). `HiDpiShell` watches
the main window from `windowReady` and each dock's window (`DockHost::windowOf`) on every `DockHost::stateChanged`,
and re-checks every watched window one turn after a dock change (the reparent gap). Qt's widget events stay the
first path (04 §6.6 item 3 is read as "on the window's screen or DPR change", which Qt delivers as those events); the
watcher never rebuilds a component Qt already rebuilt. (b) **Crisp strokes at a fractional DPR.** `RasterBackend`
draws a `Polyline` or `Lines` command of a list whose DPR is not whole, wider than 1 px and made only of horizontal
and vertical segments `lround(width)` px wide (at least 1) on whole device pixels: across a segment its centre c
covers the W pixels from floor(c + (1 - W) / 2), along it a free end goes to the pixel edge ceil(e - 1/2), and a
polyline's corners snap both ways and are mitred. The rounding is the backend's, at draw time, not the builders'
(04 §6.6 items 1 and 4 have widths of logical x DPR rounded at emission): the `DrawList` is unchanged, both raster
paths draw the same pixels, slanted strokes, dots and text stay antialiased, and DPR 1 and 2 are drawn exactly as
before, so every golden at 1 and 2 (pixel and `DrawList` JSON) is unchanged and stays authoritative. (c)
**Whole-pixel painting.** `LayerCache::paint` moves the painter by the fraction between its device origin and the
nearest whole device pixel when there is one (a fractional DPR only), so the Dynamic and Chrome lists land on the
pixels of the Static image and a widget shows `composite()` pixel for pixel. (d) **What the tests hold.** *No blur*:
what a widget paints equals its layer composite pixel for pixel, in the widget and in its window at the device
rectangle Qt gives it (no image of another DPR scaled into place); the layers and lists are at the window's DPR and
the Static image is lround(logical x DPR) device pixels; at a fractional DPR the opaque axis-aligned geometry
(clears, scissors, opaque rectangles and axis-aligned quads, solid axis-aligned strokes) covers whole device pixels
(reported, not enforced, at 1 and 2). *No layout drift*: the canvas places a component at the same logical rectangle
at every DPR; its own layout (the time_distance caption, key column, trace area and x axis, every panel scissor, the
table rows and columns) stays within 3 logical px of DPR 1 in the widgets (two roundings and one hinted text line)
and within 2 in the builders with fixed metrics; back at the first DPR the layout and the pixels are identical. (e)
**The offscreen sweep.** Four screens of the offscreen platform's screen configuration at 96, 120, 144 and 192
logical dpi over 96 give DPR 1, 1.25, 1.5 and 2, as Windows reports 100 to 200 % monitors; a window moved between
them changes screen and DPR as a drag does. The offscreen platform keeps a window's native size across the move where
Windows keeps its logical size (`WM_DPICHANGED`), so `ui.tst_dpr_switch` sets the logical size again; a scale
changed in place cannot be simulated there and is a step of the manual check. (f) **AC-M4-07's floating
time_distance.** The shell has no dock that holds a time_distance (worksheets live in the central widget, 06 §1):
`ui.tst_dpr_switch` floats one in a dock of its own on both dock backends, and the manual check
(`docs/milestones/M4.md` §6.3) drags the main window's graph and the floating Track Map dock. (g) **Goldens.** New and
exact on every OS: the builders' `DrawList` JSON of the seven drawn MVP types at DPR 1.25 and 1.5
(`tests/golden/images/dpr_sweep/<type>/<case>@<dpr>x.json`, with 04 §6.6 item 5's time_distance at 1.5); new and
provisional (`"generatedOn": "windows"`, report mode): values_list at DPR 1.5 (`tests/golden/images/dpr_sweep/
values_list/`, its table text exact against the DPR-free table goldens); re-rendered: the geometry-only reference
`geometry_scene@1.5x.png` of `esses_qtrender` (its axis-aligned strokes now on whole pixels).
**Consequences.** AC-M4-07's automated half passes offscreen (`ui.tst_dpr_switch`, `widgets.tst_dpr_sweep`,
`widgets.tst_dpr_watcher`, `widgets.tst_dpr_values_list`, `golden.tst_dpr_goldens`, `qtrender.tst_crisp_strokes`);
the two-monitor drag and a display scale changed in place are the maintainer's (§6.3), and 07 §7.6 lets mixed-DPI
floating docks slip with the limitation documented if they fail there. At DPR 2 the builders' 2 px lines on pixel
centres stay drawn over three device columns (50/100/50 %) as the authoritative goldens show; making them crisp is a
builder change with new DPR 2 goldens (`docs/milestones/M4.md` §10). Tables drawn by Qt (values_list, the reports)
keep Qt's own rendering at a fractional DPR. New names: outline §4 "Names added by the M4 hidpi package".
**User must confirm:** no.
#### D-210 The M3 follow-ups of esses_qtmodel and the Qt-free libraries as built: track-map files on the pool, summariesReady, the channel report's "ignore interpolated data", ReportModel's in-place refresh, the Track Sections pass keyed by its bound map, the missing report and variance tests (refines D-195, D-197, D-199 (c) and (e), D-203 (n), D-204; reads 01 §5.3 rules 3-4, 02 §3.2 rule 10, 04 §12 items 2-3 and 8, §13 item 5; amends 04 §12 items 2 and 8, outline §4)
**Context.** `docs/milestones/M4.md` §5.1 gives the followups package M3.md §6 items 18, 23, 25 and 29-32 for the
Qt-free libraries and `esses_qtmodel` (the widgets follow-ups are a package of their own, D-211). Item 23: a cache
miss of `TrackMapService` read the track file on the GUI thread and every edit wrote it there. Item 18: the
loader's summary pass announced nothing, so a component built before it kept the extents of incomplete
pyramids. Item 25: the channel report had no "ignore interpolated data" switch; the research names the switch and
nothing more. Item 30: `ReportModel` reset the model on every delivery, so a refresh lost the table's selection.
Item 31: a Venue detail edit did not run the Track Sections pass again, and the phase-2 key held a global map
generation rather than which map the source is bound to. Items 29 and 32: tests the M3 engine review asked for.
**Decision.** (a) **Track maps on the pool** (item 23). With a `MathsService` and a `Dispatcher` (the
application's service) every read, write and scan of `trackmaps/` runs on `MathsService::pool()` at Interactive
and none on the GUI thread. `map(venue)` on a cache miss returns null at once and reads the venue's own file
(D-205) on a worker (`isLoading`); when it lands the result is cached, found or not, and `mapChanged(venue)`
fires; a project change drops the reads in flight. `setMap` validates and replaces the cached map and announces
it at once, as before, and queues the write: one write per venue file is in flight, the next one is held and
merged (the newest map, backed up when any merged edit asked), so the file always ends as the last edit left it.
A write that fails is a `logMessage` row, the venue's map is read back from its file, and the `TrackMapCommand`s
whose edits it carried are marked failed and obsolete, so `QUndoStack` drops them at their next undo or redo
without running them: D-203 (n)'s "no step that did not happen stays on the stack", reached when the failure is
known instead of inside `push` (an invalid map is still refused at once). A parent command (the Track Editor's
rename) is not marked; its undo writes both files back. Writes still queued when the service is destroyed are
made by the destructor, on that thread, behind the slot mutex that orders them (SourceLoader's sidecar rule,
D-190). `venues()` answers from the cache and the last scan of the folder, which the first call after a project
change starts on a worker (`venuesChanged`). A map from `generate()` reaches `onGui` only once the venue's own
map is known (read, or set meanwhile), so the shell's auto-generation, which generates while `mapFor()` is null
and writes when it is still null, never writes over a file that was still being read. New members:
`asynchronous()`, `isLoading()`, `mapStamp()` (the identity of the cached map, unique within the service),
`pendingIo()`, the signal `venuesChanged()`. Without a Dispatcher (headless tools, the M3 tests) every operation
stays synchronous. A local event loop around the write (D-198 (e)'s Export Data wait) was rejected: the shell's
auto-generation writes from inside a Dispatcher closure, and a nested loop there breaks the FIFO drain of 01 §5.3
rule 3. (b) **`summariesReady`** (item 18). The summary pass is one `summary/<sourceId>/<first column id>` task
(the column id is process-unique, so the pass of a file closed and opened again never joins the closed source's
cancelled run under the shared `summary/` key; `close()` still cancels by the `summary/<sourceId>` prefix); when it
has run, `summariesReady(id)` is posted, after `sourcePublished`, and emitted only while the source is open with
the columns it summarised. A source a `sourcePublished` listener closed again gets no pass. The components'
rebuild on the signal is the widgets' (their Data path). (c) **`options.ignoreInterpolated`** (item 25,
`ReportOptions::ignoreInterpolated`, default false). Esses' statistics read the channel's own samples (D-197 (a)),
so the one interpolated value a channel report can show is the range-edge value of `interpolateBoundaries`; with
the switch on, `start`, `end` and `change` take the range's samples (the probe still applies) whatever
`interpolateBoundaries` says, and the cells share the StatCache key of `interpolateBoundaries` off (they are
equal). A maths output evaluated above an input's rate holds that input's up-sampled values as its own samples;
0.1 keeps no per-sample provenance, so the switch does not reach them (a v1.0 question). JSON: `channel_report-1`
gains the optional boolean, written by `ReportSpec::toJson` only when true, so every M3 body reads and writes back
unchanged (no schema version); the Properties dialog's Report page binds it ("Ignore interpolated values").
(d) **The Track Sections pass keyed by its bound map** (items 31, 32). While the pass is on, the phase-2 key
holds `/t<setTrackMaps generation>-<mapStamp of the source's venue>` instead of a generation every map change
bumped, so an edited, reloaded or another venue's map changes the key and a change of one venue's map no longer
changes every other source's key; no run starts while the venue's map is still being read (its `mapChanged`
starts the pass, so the groups never drop to "no map" in between); a sidecar edit that changes the Venue detail
starts the pass for every layer of the source. D-204 (d)'s serial guard gets its test without a hook: the test
lets run A finish on the pool while holding the event loop, starts run B, and checks that A's result never
publishes. (e) **`ReportModel`'s in-place refresh** (item 30). A delivery or a display change (highlight, time
format) whose table has the same header labels, row labels and row kinds as the one on display is one
`dataChanged` over the table instead of a reset, so a view's selection and current cell survive; any other table
resets. The widgets' `ReportTable` re-measures its columns on `dataChanged` as it does on a reset. (f) **Tests**
(item 29): `analysis.report_time: TrackSectionsRule output reaches the eclectic without the in lap's cut-off
section` (the time report fed from `RuleRunner::runPhase2` with the track-sections rule: the in lap's section that
would be cut at the lap end is absent, so the eclectic takes its complete section and nothing short, with untrusted
laps included) and `plotcore.variance: the table equals what the traces' DomainMapper::xOf shows (M3 item 29)` (both
domains, two overlay pairs with offsets and stretch, blank columns included; the expectation inverts `xOf` by
bisection, never `DistanceIndex::timeAt`).
**Consequences.** The GUI thread does no track-map I/O in the application; a track-map edit is on disk once the
pool has run it (milliseconds) rather than when `setMap` returns, and its failure is a Messages row and a dropped
undo step instead of a refusal; the Track Editor's tests wait for the files (`widgets.tst_track_editor`). Components
can rebuild their extents when the pyramids exist. The channel report has the switch of the research list. A
report refresh keeps the selection. A Venue detail edit moves the Track Sections to the new venue's map, or
removes them without one. The frozen `source_loader.hpp` comment still describes the M3 key form (`/t<map
generation>`); the coordinator refreshes it. Regression tests: `qtmodel.tst_track_map_io` (5 functions),
`qtmodel.tst_summaries_ready` (3), `qtmodel.tst_track_sections_pass` `aVenueEditRunsThePassForTheNewVenue` and
`aSupersededRunNeverPublishes`, `qtmodel.tst_report_model` `aRefreshOfTheSameShapeKeepsTheSelection`,
`analysis.report_stats: ignoreInterpolated ...`, `analysis.report_cells: ignoreInterpolated ...`,
`analysis.report_spec: options.ignoreInterpolated ...`, the two of (f). No golden changes.
**User must confirm:** no.

#### D-211 The M3 follow-ups of esses_widgets as built: a channel's default display unit (a fraction in %), display units of the scatter and channel report channels, unit pickers in the Properties pages, the Track Editor's speed class and Corr Dist unit, Export Data in the legacy dialect, the time_distance group headings, component links honoured, summariesReady reaching the components (refines D-010, D-177 (e), D-195, D-198 (e), D-200, D-201, D-203 (af) and (m), D-210 (b); reads 03 §9.1 item 6, 04 §2.4, §7.2 item 8, §8.2 item 4, 05 §6 "Writer", 06 §12, §13.5 item 4; amends 04 §7.2 item 8's reading in D-198 (e), outline §4)
**Context.** `docs/milestones/M4.md` §5.1 gives the followups package M3.md §6 items 19, 21, 22, 24, 26 and 27 for
`esses_widgets` (with the body schemas and `esses_plotcore` where they need it; the Qt-free and `esses_qtmodel`
items are D-210). Item 19: samples are stored in the SI base unit of their quantity (D-010), so a logged
percentage stored as a ratio showed as 0-1 wherever no display unit was named, and `scatter-1` and
`channel_report-1` had no per-channel display unit. Item 21: 04 §8.2 item 4's group headings (drag to reorder,
double-click for Group Properties) were not built and no `GroupHeading` hit was registered. Item 22: the Track
Editor had no speed-class column (`TrackSection` and `track-1` had no member) and edited its bounds in metres.
Item 24: a component's Export Data wrote the plain time-and-channels table of D-198 (e), not the legacy dialect of
04 §7.2 item 8 that `esses-cli export --format motec.csv` writes. Item 26: display units were free text in the
Properties pages. Item 27: `links.zoom`, `links.cursor` and `links.datum` were stored and edited but not honoured
(D-203 (m)). D-210 (b) left the components' rebuild on `SourceLoader::summariesReady` (item 18) to the widgets.
**Decision.** (a) **The default display unit** (item 19). Where a component names no display unit (a trace's,
row's, gauge's or channel's `displayUnit`, or one that is not a unit of the channel's quantity), a channel is shown
in its quantity's default display unit, `plot::defaultDisplayUnit` (`esses_plotcore` `display_units.hpp`): the
stored unit, except that a fraction stored in the Ratio base unit `ratio` is shown in `%` (a throttle, brake or
duty reads 0-100). The reference product shows a channel in its logged unit; that is the better default, but it
needs the channel descriptor to keep the file's unit (05 `ChannelInfo::vendorUnit` reaches no model type) and is
left to v1.0; until then only Ratio's base unit is not a reading unit, so only it gets another default (every
other quantity's SI base unit is a real reading unit, and a broader table would change what M1-M3 show).
`RenderService`'s channel resolution applies it, so the graphs, gauges, scatter, histogram, track view, the Values
window and model, the reports (their columns, D-197 (g)) and Export Data all agree; `plot::ResolvedChannel` gains
`displayUnitId` (the unit `toDisplay` converts to). The Channel Editor's Unit cell without an override shows the
same default (amends D-177 (e)'s "the stored unit"), and so do the Maths Editor's channel insertion (`'Duty'
[%]`) and the unit CTRL+SHIFT+O and CTRL+SHIFT+Z write their offset in (the unit of the value on screen, D-186).
The project's channel override display units still reach only the Channel Editor (as in M2-M3). `scatter-1` gains
the optional `xDisplayUnit`, `traces[].displayUnit` and `colourChannel.displayUnit`, `channel_report-1`
`report.channels[].displayUnit` (`analysis::ReportChannel::displayUnit`); all are written only when set, so every
M3 body reads and writes back unchanged (no schema version). The shipped workbook names `G` for the Corners
scatter's lateral and longitudinal G and `km/h` for the Reports worksheet's Corr Speed; M1's Overlay worksheet,
unchanged, now reads its throttle and brake in % (AC-M1-05's render parity holds: both sides resolve alike).
(b) **Unit pickers** (item 26). `FieldKind::Unit` is an editable combo of the unit symbols of the quantity of the
channel its sibling Channel field names (`FieldRow::unitOf`; `setUnitOf`, `linkUnitPickers`, `setUnitChoices`,
`unitChoices` in `field_editors.hpp`): `""` (the default display unit) first, then the quantity's units without the
compat ones (06 §13.1 item 1's rule); it follows a change of the channel field and of the open sources (the
dialog's channel choices); free text stays allowed (an alias). Every `displayUnit` row uses it: the
time_distance and outing traces, the scatter's x, y and colour channels, the channel report's channels, the
values list's rows and the two gauges. `UnitValue` units (limits, bands) stay free text. (c) **The Track Editor**
(item 22). `TrackSection::speedClass` is `""` (none), `slow` or `fast` (`TrackMap::kSpeedClasses`; the classes of
the parity panel's proposal, nothing more being known), `track-1`'s optional `sections[].speedClass` (written only
when set, so earlier maps are written unchanged; `validate()` refuses another value at its pointer); the editor
has a Speed class column and field, `addSection`/`editSection` take it (an edit without one keeps the section's).
The bounds are shown and edited in the Corr Dist display unit, `plot::corrDistDisplayUnit`: the project's Corr
Dist override when it is a length, else following Corr Speed's override display unit (km/h -> km, mph -> mi, 03
§9.1 item 6), else m; the table's headers and cells, the fields' suffix and decimals (enough for 0.1 m) follow,
and change when the overrides change; the editor's API and the file stay in metres. The graphs' distance axis
stays in metres (unchanged). (d) **Export Data in the legacy dialect** (item 24). The time-series exports (the
time_distance and outing graphs, the gauges) write 05 §6's legacy CSV through `formats::makeLegacyCsvWriter`, the
writer of `esses-cli export --format motec.csv`: a source of their own holding the component's Main-slot channels
under the component's names (maths outputs included; repeats left out), the Main source's details with `Export
Range` (the Main range's name) and `Export Worksheet` (the worksheet's name; no workbook is known), the span of the
export range, each channel in its display unit (the writer's `csv.units`), one grid at the highest uniform rate
capped at 1 kHz, Time from 0. The writer writes files only, so the bytes pass through a temporary file (read back
with `readWholeFile` and removed) on the worker the export already runs on (D-203 (x)). This supersedes D-198
(e)'s plain table for those components; the scatter's sets and the histogram's bins (D-198 (e)) and the reports'
tables are derived tables, not channels over time, and stay as they are. No second format is offered (the shell's
export prompts are frozen in M4, D-206). (e) **Group headings** (item 21). A group's heading is its part of the
key column (`TdGroupLayout::key`): the Static build registers it as a `GroupHeading` hit beneath the group's label,
value and measurement hits, so nothing new is drawn and no golden changes; an open-hand pointer shows over it
(hover events, mouse tracking unchanged). Dragging a heading 4 logical px or more shows a dashed line where the
group would go (between the keys, across the key column and the panels; `groupDropIndex`); the release moves it
there, `TimeDistanceGraph::moveGroup`: one undoable spec edit "Move group <name>", the group's traces, its unknown
members (the body's `rest`, which `toJson` merges by index) and its vertical zoom going along; panels laid out by
explicit `edges` keep their heights, stacked again from the first top in the new order; overlapped `lo`/`hi`
regions are kept. A double-click opens Group Properties: the Properties dialog with the group's record selected
on the Channels tab (name, scale, limits, lines, edges, Lo/Hi; `PropertiesDialog::focusRecord`,
`openGroupProperties`). (f) **Links** (item 27, 04 §2.4). `ComponentWidget` keeps the own zoom, cursor and datum
of the parts whose link is off, taken from the worksheet's when the component is created or the link is switched
off, never persisted; `viewState()` is the worksheet's state with them in place (its version the worksheet's
plus the own changes), and it is what `onViewChanged` receives; a worksheet change of an unlinked part is not
delivered; switching a link on again delivers the worksheet's. `domain` and `offsetAxis` stay the worksheet's; on
F9 an own zoom restarts from the worksheet's zoom converted into the new domain (an own zoom has no domain rule of
its own). The outing graph's zoom is always linked (04 §9 item 2). The components' own edits go through
`setViewCursor`, `setViewZoom`, `viewPanBy` and `viewZoomFull` (their mouse: clicks, drags, the wheel, the rubber
band, band and scrollbar double-clicks; the scatter's point stepping): the worksheet's when linked, else the own
state, an own zoom clamped to the Main outing (`mainMapper`). The worksheet keyboard rows the shell offers the
focused component (`cursor.*`, `zoom.in`/`out`/`default_range`/`datum_main`, `pan.*`, `datum.*`) act on its
unlinked parts in `ComponentWidget::handleCommand`'s default, by the WorksheetViewModel's rules on the own state: a
cursor step is one device pixel of the component's width, at least one sample period of the worksheet's active
period; zoom x0.5 / x2 about the own cursor; W `[0, |Main|)`; Z the cursor-datum span; pan 10 % and 90 % of the
span; H centres; SPACE, D and X on the own datum (X only when cursor and datum are both the component's own).
Rows the shell does not offer components (F2 and the like) act on the worksheet only. The values list reads its
values at its own cursor and datum (`ValuesModel::setViewSource`); a zoom-linked channel report clips to its own
zoom when `links.zoom` is off. The Properties dialog's link tooltips say so. (g) **`summariesReady`** (item 18,
D-210 (b)). `FollowupsShell` gives every component of the window, shown or kept hidden, `onDataChanged(source,
0)` when the loader's summary pass of a source has run, so one built while the source's SummaryPyramids were
incomplete rebuilds its extents; the other `FollowupsShell` virtuals stay the base's (nothing else needs shell
wiring).
**Consequences.** New public names: outline §4 "Names added by the M4 followups of esses_widgets". Tests:
`plotcore.followups D-211: ...` (6: the default and named display units against data/units.json, the Corr Dist
unit rule, the scatter members, the report channels' units, the GroupHeading regions), `analysis.followups D-211:
...` (2: the speed class, the report channel's unit), `widgets.tst_followups_units` (the default in the Values
model, the scatter inputs and the channel report; the unit pickers of six types; the Track Editor's speed class and
units; the legacy export's layout and display units), `widgets.tst_followups_view` (the heading drag, Group
Properties, explicit heights and members; the links of a graph, the outing graph and a values list; summariesReady
through the shell); the export expectations of `widgets.tst_gauges`, `tst_outing_graph`, `tst_time_distance_m3`
and `tst_shell_m3` follow (d), and `widgets.tst_channel_editor` and `tst_active_channel` follow (a). No DrawList or
pixel golden changes.
**User must confirm:** no.

#### D-212 Packaging and 0.1 identification as built: the About dialog and `esses --version` with the tag or commit source URL, release builds bound to their tag, single-instance forwarding over the operating system's local IPC with a channel per settings folder, `--scheme` and `--keymap`, the install layout with the notices beside the data, the per-package third-party manifest and CycloneDX SBOM, the Qt licence texts outside `LICENSES/`, the installer's runtime check with app-local runtime for per-user installs, the portable zip's association scripts, the release-tree vendor-mark scan, `package.yml` (refines D-006, D-034, D-055, D-113, D-116, D-206 (i); reads 06 §§2.2, 18-19, 22, 07 §7.1-§7.2 AC-M4-04/-06/-08/-09, §7.5, 08 §§3.10-3.11, 4.1, 5, 7, 9, 10; amends 06 §18 step 1, §19 item 5, §24 item 9, 08 §3.11, §4.1 `package`, §7.1-§7.4, §10, outline §4)
**Context.** Wave 2's packaging package (`docs/milestones/M4.md` §3) turns the contract's About placeholders into the
real dialog and `--version` (AC-M4-06, AC-M4-09), builds 06 §§18-19's last deferrals for `apps/app` (single-instance
forwarding with `--new-instance`, `--scheme`, `--keymap`), ships portable mode, and writes the packages of 07 §7.1
(installer, portable zip, AppImage, DMG, the CLI everywhere and standalone; D-034), their notices, manifest, SBOM and
checksums, the release-tree vendor-mark check (AC-M4-08) and their CI. The design leaves open: the exact source URL
the About dialog shows (the maintainer's §6.5 decision: "the tag URL of the public repository"), how a build knows it
is a release, what `--version` prints beyond the four fields, how D-113's forwarding is carried when the application
must not link Qt Network (01 §2.2 rule 4 lists Core, Gui, Widgets, Svg, PrintSupport, OpenGL for `esses`;
`tools/check_layering.py` enforces it), what `--scheme` and `--keymap` mean, where the Qt licence texts live when
nothing in the tree is under them (`reuse lint` fails on an unused file in `LICENSES/`), where `NOTICES.md` sits in
each package, where the third-party manifest is generated (08 §7.4 names `cmake/notices.cmake` and
`tools/gen_notices.py`, neither of which can see the Qt files a deployment tool copies), what the installer does for a
per-user install on a machine with an old runtime, and the workflow's triggers and runners.
**Decision.** (a) **Build identification.** `buildInfo()` fills `dockLibrary` with the backend in use (`ADS <version>`
from ADS's own `ads_version.h`, included by `about_dialog.cpp` for its version macros only, or `QDockWidget`) and the
new `releaseTag`. `versionReport()` prints `esses <version>` (unchanged first line), then `commit:`, `Qt: <runtime>
(built with <build>)`, `build date:`, `dock library:` and `source:`. `qtLicenceNotice()` begins with 08 §9 item 2's
statement and gives the licence files and the exact Qt source archive (`qtSourceArchiveUrl`, the `official_releases`
path of 08 §9 item 3, for the Qt version run against); `dockLibraryNotice()` does the same for ADS. (b) **The source
URL scheme** (default of the maintainer's §6.5 decision): a release build shows
`https://github.com/mitchdetailed/jubilant-octo-meme/tree/<tag>`, any other build `.../commit/<sha>` (a `-dirty`
suffix dropped), and the repository when the commit is unknown (`sourceUrlFor`, `kRepositoryUrl`). (c) **Release
builds** are configured with `-DESSES_RELEASE_TAG=v<version>` (`tools/packaging/CMakeLists.txt`, which defines it for
`about_dialog.cpp` only); configure fails unless the tag is "v" + the version `cmake/version.cmake` stamped, so a
release build cannot carry another tag and the installer's version (the stamped version) equals the tag (AC-M4-09).
The package workflow sets it from `refs/tags/v*`. (d) **Single-instance forwarding** (D-113) runs over the operating
system's local IPC driven by Qt Core's `QWinEventNotifier`/`QSocketNotifier` instead of `QLocalSocket`: a
single-instance named pipe for local clients only on Windows (client at identification level, both sides check the
other process's user SID, the client grants the server the foreground), a Unix-domain socket in a private directory
(`$XDG_RUNTIME_DIR`, `$TMPDIR`, else `/tmp/esses-<uid>` created 0700; used only when owned by the user and closed to
others; the peer's uid checked; a stale socket file replaced) on POSIX. No thread, no new Qt module. The channel's
name is `esses-<user>-<12 hex digits of SHA-256 of the settings folder>` (06 §18 step 1 said `esses-<user>`): a
portable copy, the installed program and a run with `ESSES_CONFIG_DIR` are separate instances; `ESSES_INSTANCE_NAME`
replaces it for tests. One line of JSON (`protocol` 1, an absolute project, absolute logs, at most 1 MiB, 4096 logs)
answered `ok` or `error`; a refused, silent (20 s) or foreign instance makes the launch start its own window. Only
files are forwarded (D-113's "-w/-l"); a launch without files raises the running window; a forwarded project that is
already open adds the logs; print-only runs neither forward nor listen; `--new-instance` never forwards but listens
when the name is free. The server answers before it opens anything (a project prompt cannot keep the client waiting)
and drops a client that sends nothing complete within 5 s. (e) **`--scheme <name>`** selects a scheme of the scheme
selector by name (any case) or `light`/`dark` for the shipped ones, as the toolbar's selector does (it stays active);
**`--keymap <name|file>`** loads a `keymap-1` file (a path, else `<config>/keymaps/<name>.json`, else
`<data>/keymaps/<name>.json`) over the shipped default for this run only (the binder; settings and `keymap.json`
untouched); problems are Messages warnings (`launch.scheme`, `launch.keymap`). (f) **Portable mode** keeps D-055's
`portable.marker` (the task's example name is not needed: the marker is decided); the zip ships it empty and About
names the settings folder and the portable state. (g) **Install layout** (all install rules in
`tools/packaging/CMakeLists.txt`, components `app` and `cli`): `NOTICES.md`, `THIRD_PARTY_LICENSES.md`, `LICENSE`,
`licenses/` and the user guide (`docs/user/`, where D-215's Help Contents looks) sit beside the `data` folder in every
package (the installation folder on Windows, `share/esses/` on Linux instead of 08 §3.11's `share/esses/licenses/`,
`Esses.app/Contents/Resources/` on macOS), so the About dialog finds NOTICES.md at `<data>/../NOTICES.md`; only ADS's
runtime file (and its SONAME link) is installed, not its headers or CMake package; windeployqt leaves out the PDF
image-format plugin and the TUIO touch, TLS and network-information plugin types, so neither Qt Pdf nor Qt Network is
shipped; the macOS bundle is the `esses` target's own bundle, renamed `Esses.app`; the standalone CLI archives are
flat (`esses-cli`, `data/`, notices). (h) **Manifest and SBOM** are generated per package at package time by
`tools/packaging/gen_third_party.py` from `tools/packaging/third_party.json`, the licence files of the tree and the
files actually staged (the deployed Qt modules and plugins, ICU on Linux, the system libraries linuxdeploy copied with
their Debian copyright files), instead of `cmake/notices.cmake` + `tools/gen_notices.py` at build time; the SBOM is
CycloneDX 1.5 JSON per package (deterministic serial and timestamp), and the source job adds `reuse spdx`'s SPDX
document (08 §7.4 named only the latter). `--check` keeps the list, `NOTICES.md` and `third_party/` in step. (i) **The
Qt licence texts** (LGPL-3.0-only, GPL-3.0-only; the SPDX texts from Qt 6.7.2's `qtbase/LICENSES/`, line ends LF, as
`LICENSES/Apache-2.0.txt` before them) are `tools/packaging/licenses/LICENSE.<id>.txt`, installed as
`licenses/<id>.txt`: in `LICENSES/` `reuse lint` would count them unused (no file of the tree is under them), and
`LICENSE.*` names are licence files to REUSE (08 §10's list of `LICENSES/` amended). (j) **The installer's runtime
check** compares the installed x64 runtime (Major, Minor, Bld) with `VcRequired`, by default the bundled
redistributable's own version: an administrative install runs the bundled `vc_redist.x64.exe` first and refuses
("cannot proceed", exit code 7 silent; nothing copied) when it fails or the runtime is still older; a per-user install
(`PrivilegesRequiredOverridesAllowed`) gets the runtime DLLs beside `esses.exe` (app-local, as the zips; 08 §7.1 had
the redistributable path only). CI builds a second installer with `VcRequired=99.0.0` to exercise both paths. The
`assocld` task is preselected in the wizard unless another program owns `.ld`, and a silent install associates only
with `/MERGETASKS="assocld"`; `.csv` is offered in "Open with" only; App Paths registers `esses`; Windows 10 1809 is
the minimum. (k) **The portable zip** carries the runtime DLLs, `portable.marker` and `associate-ld.cmd` /
`unassociate-ld.cmd` (per-user `.ld` association, 06 §19 item 5's "script"); the CLI zip carries the runtime too. (l)
**Vendor marks of the release tree** (AC-M4-08): `check_vendor_marks.py --tree` scans every name, every text file and
the strings of our programs (only the long marks in binaries; third-party binaries by name), and the source scan
covers the release metadata's file types and `package.yml`; `package.py` runs it on every staged tree; the allow-list
gains the notices' and the user guide's paths in a release tree (class (e)). (m) **The icon** is our own geometric
drawing, written by `tools/packaging/icons/make_icons.py` (PNG, ICO, ICNS, SVG; `--check` compares the committed files
with a fresh render by pixels). (n) **CI**: `.github/workflows/package.yml` (08 §4.1's `release.yml`) runs on tags
`v*`, manual dispatch and pull requests touching `tools/packaging/**`, `apps/app/**` or itself; windows-2022,
ubuntu-24.04 (08 §4.1 said 22.04 for an older glibc; the AppImage targets the Ubuntu 24.04 VM of AC-M4-04 and is built
where ci.yml builds the Linux kit) and macos-14 (a universal binary, ad hoc signed because arm64 runs nothing
unsigned; not a developer signature); smoke tests per package; a draft GitHub release on tags with every artefact and
one `sha256sums.txt` that needs the Windows job only (D-116: Linux and macOS never block it). Signing steps are not
present (U-11 default: unsigned 0.1; no workflow step needs a secret, so forks run it too); actions, linuxdeploy
(dated release tags), Inno Setup (6.4.3 when the image lacks it) and `reuse` are pinned.
**Consequences.** New public names (outline §4 "Names added by the M4 packaging package"): `BuildInfo::releaseTag`,
`kRepositoryUrl`, `dockLibraryNotice`, `sourceUrlFor`, `qtSourceArchiveUrl`, `noticesUrl`, `AboutDialog(const Paths&,
QWidget*)`, `AboutDialog::html`/`noticesLink`/`copyButton`; in `apps/app` (no library): `esses::app::LaunchRequest`,
`CommandLine`, `shouldForward`, `InstanceServer`, `forwardToInstance`, `instanceName`, `instanceAddress`,
`openRequestedFiles`, `applyScheme`, `applyKeymap`, `applyStartOptions`, `bringToFront`; Messages codes
`launch.scheme`, `launch.keymap`; the cache variable `ESSES_RELEASE_TAG`; the install components `app` and `cli`.
`esses` gains `esses::project` in its DEPS (keymap loading). `cmake/version.cmake` (the coordinator's) stamps
`ESSES_GIT_SHA` "unknown" when the build stands exactly on a tag (`git describe` prints no hash there) and reads a
dirty tree at a tag as the prerelease "-dirty"; `smoke.py` requires a real commit, so the tag build's smoke test fails
until `ESSES_GIT_SHA` comes from `git rev-parse --short HEAD` in every case (`docs/milestones/M4.md` §10). The SMB, VM
and old-runtime checks stay the maintainer's (§6.1); the Qt source archive's SHA-256 and its copy as a release asset
are the release step's (§6.5). No golden changes; no keymap row.
**User must confirm:** no (the source-offer wording and the URL scheme are the maintainer's §6.5 decision; (b) is the
default applied until then).

#### D-213 The Saturday soak as built: tests/soak/tst_saturday with its own driver over ten logs written at test time, a Dispatcher thread violation as a failed ESSES_ASSERT or a Qt thread-affinity warning made fatal, pending work and growth judged per cycle, LeakSanitizer suppressions limited to code that never calls back with lsan_check.py counting the leaks that have an esses frame, the soak job on linux-clang-asan with the Qt layers (refines D-031, D-123 (e), D-206 (i); reads 07 §7.1 "Soak", §7.2 AC-M4-03, 08 §4.1, §6.10, 01 §5.3 rules 1, 3-4 and 11, 06 §18; amends 08 §4.1's soak row and §6.10's "parameterised version of the Saturday walkthrough")
**Context.** 07 §7.1 and AC-M4-03 ask for a Qt Test with ten files open under ASan/UBSan, 30 minutes nightly and 2
hours on the release candidate, with no crash, no `Dispatcher` thread violation and no leak whose stack has an
`esses_*` frame, Qt, fontconfig and glib stacks being suppressed by `tests/soak/lsan.supp`. 08 §6.10 describes it as a
parameterised version of the Saturday walkthrough looping over ten files, 08 §4.1 puts the job in `bench.yml` on
ubuntu-24.04 with "linux-gcc + sanitizer". The walkthrough is written at the same time (wave 2) and asserts each 0.1
item once. Nothing defined what a thread violation is, how "a leak counts only if its stack has an esses_* frame"
maps onto LeakSanitizer (which drops a leak when any frame of its stack matches a suppression), or how a soak sees
memory that is still owned at exit and so is no leak for LeakSanitizer. Agents download nothing and this machine has
no Linux, so the sanitizer run itself is CI's.
**Decision.** (a) **Its own driver.** `tests/soak/tst_saturday.cpp` (ctest `soak.tst_saturday`, labels `qt;soak`)
drives the real shell offscreen (a `MainWindow` on a private configuration and a project folder, the shipped
workbook, a runtime as `esses` makes it: default workers and cache budget, the GUI thread NoBlock) through a cycle
of the day (`tests/soak/README.md` "One cycle"): a restart every fourth cycle (save, exit, a new window, the session
restore); ten logs open, one of them through the memory-budget gate answered by Continue or Cancel; the selection
from the Data window's keys and from the model, N, P, Q, J, F4, CTRL+F and a lap-number filter; every worksheet with
the time/distance graph's zoom, pan, cursor and datum keys, F3, M, F9, a wheel step and a click, the reports copied,
the Track Map dock shown and drawn; a global maths module pushed and its channel drawn, the expression changed and
recalculated, a trust edit, the Maths Editor; undo to the cycle's start, redo, a journal entry written by autosave
(every 250 ms here), undo, save; one `.ld` replaced on disk by a rename-over writer (another version or the same
bytes) and reloaded from the Messages row or File > Reload File; three closes or Close All. It does not reuse the
walkthrough (amends 08 §6.10). The ten logs (eight `.ld` + `.ldx` sessions on two venues, 41 channels from 10 to
200 Hz, GPS at 20 Hz, about 1,000 s and 9 MB each; two CSV exports, one per CSV writer, with a lap counter) are
written from closed-form kinematics at test time on a pool of their own, once per build tree
(`<build>/tests/soak/saturday-logs-v<n>-x<scale>`, moved into place when complete) and copied for each run; they
are never committed.
(b) **A `Dispatcher` thread violation** is a failed `ESSES_ASSERT` of the Dispatcher's own check (`drain()` on a
thread other than the one that owns it) or of the GUI thread's NoBlock checks (a Future wait, `Column::require` or a
summary build on the GUI thread; D-123 (e)), or one of Qt's thread-affinity warnings (a QObject parented, timed or
sent events across threads, a timer on a thread Qt did not start such as a TaskPool worker, a pixmap off the GUI
thread, a queued argument type that cannot cross). In the soak the assertion handler and the Qt message handler
print `soak: fatal <class>: <text>` and abort at once, so the failure carries the offending thread's stack (ASan with
`handle_abort=1`); the guard's tests record one violation of each kind in-process and check the fatal path in a child
process. Every other `ESSES_ASSERT` is fatal too. A build without assertions sees only Qt's half; the soak job builds
Debug with `ESSES_FORCE_ASSERTS`.
(c) **Pending work and growth.** Every step ends on the pool settled with every posted result delivered and the
loader idle (`ESSES_SOAK_SETTLE_SECONDS`, 900 s); a step that cannot finish fails at its timeout (the soak's waits
are `QTest::qWaitFor`, not QTRY, which waits three times its timeout before it fails). Once per cycle, in the same
state (ten logs open, worksheet 0 drawn), the soak samples the heap (the ASan allocator, the MSVC debug CRT, glibc's
`mallinfo2`, macOS's malloc zones, else Windows's private bytes), the resident set, the chunk cache, the widgets, the
window's objects and the peaks of queued tasks and Dispatcher closures since the last sample. A series grows when the
smallest of its last third exceeds the largest of its first third, after a warm-up of a fifth of the run and at least
two cycles, by more than max(absolute, relative) slack: heap 48 MiB/10 %, resident set 192 MiB/25 %, chunk cache
64 MiB/10 %, widgets 40/5 %, objects 80/5 %, the peaks 2,000/200 %. Cycles that began with a restart are left out of
the heap, resident and object series (a new window has not yet made its on-demand dialogs). At the end, with every
log closed and the window gone, the chunk cache must hold no byte and no pin. Fewer than warm-up + 3 samples are
reported, not judged.
(d) **Leaks.** `lsan.supp` may name only Qt, fontconfig and glib code that allocates for itself and never calls back
into the application: fontconfig as a module, glib's type, quark and interned-string tables (never its main loop),
Qt's font database and engines, platform plugin and plugin loader. `lsan_check.py --self-test` (ctest
`soak.lsan_check.self_test`, and the job) refuses every other pattern, and any pattern that matches a frame through
which Qt or glib calls the application. `lsan_check.py` judges the run from the sanitizer logs, the Qt Test log and
the exit status: an ASan or UBSan error, a failed or missing Qt Test result, an exit status nothing explains, or a
printed leak with an esses frame (a function in namespace `esses`, a source file under `libs/` or `apps/`, an
unsymbolized frame of an `esses*` executable or library) fails it; a printed leak without one is a warning that names
where it was allocated, to be added to `lsan.supp` under the rule after review (08 §4.1 "counts only if its stack has
an esses_* frame"). Before the soak the job proves the pipeline: an allocation in `esses::soak::leakOnPurpose`, made
inside a Qt timer's callback, must be reported through `lsan.supp` and counted.
(e) **The job.** `bench.yml` job `soak` (the only edit outside it: `workflow_dispatch` gains the input
`soak_minutes`): ubuntu-24.04, the `linux-clang-asan` preset as ci.yml's sanitize job with `-DESSES_BUILD_QT=ON
-DESSES_FORCE_ASSERTS=ON`, Clang 18; Qt 6.12.0 installed by aqtinstall at ci.yml's commit, CMake 3.31.4, Ninja
1.11.1.1 and py7zr 1.1.3 from pip, `actions/checkout@v4.2.2` and `actions/upload-artifact@v4.6.2`; nightly for 30
minutes (`schedule`) and on manual dispatch for `soak_minutes` (120 on the release candidate: the "2 h before each
release tag" is a dispatch on the candidate before the tag, M4.md §6.5); no secret, no pull_request trigger, the
artefact `soak-logs` (the sanitizer logs, the Qt Test log, `saturday-cycles.csv`, `saturday-summary.txt`). The
main run calls only the `saturday` function (the guard's tests make a QObject on a pool worker and kill child
processes on purpose; ci.yml's qt-app runs them). (Amends 08 §4.1's soak row: Clang with ASan + UBSan, not GCC.)
(f) **ctest.** `soak.tst_saturday` stays in the default ctest run and so in qt-app on every OS: 60 s and at least two
cycles, so that every step runs once; ctest gives it 1,800 s and `initMain` raises Qt Test's per-function watchdog
(`QTEST_FUNCTION_TIMEOUT`, 5 minutes by default) above the soak's duration. Longer runs call the executable
(`ESSES_SOAK_SECONDS`, `ESSES_SOAK_LOG_DIR`, `ESSES_SOAK_SCALE`); `ESSES_SOAK_CRT_CHECK` adds the MSVC debug CRT's
heap checks.
**Consequences.** A leak, a thread violation, work that never drains or growth fails the nightly job with the logs
and the per-cycle samples to read. The suppressions start from the families above without a Linux run: the first
nightly runs may list leaks without an esses frame (warnings) to review, and a Qt-internal leak whose stack also
reaches an esses frame counts until its Qt function is added under the rule; the soak's own Qt code runs under ASan
and UBSan for the first time there. The growth rule needs at least six samples: the 60 s ctest run judges no growth
and MSVC Debug, at about 135 s a cycle on the reference machine, makes it about five minutes there. The project's
recent sources hold every log ever opened in the project (06 §18 "reopens the MRU sources"), so a restart reopens
all ten logs, and a project used for a season would reopen every log at its next open: an open issue
(`docs/milestones/M4.md` §10), not changed here. No golden, keymap row or library code changes.
**User must confirm:** no.

#### D-214 The Saturday walkthrough, the CLI golden tests, `esses-cli convert` and the Data finals as built: one test function per 0.1 item in the real shell over data written by the test, item 10's functional half beside the `bench` rows, golden-JSON tests per subcommand with expectations from the corpus and the reference decoder, `convert` with `--format` (alias `--to`), a CLI-side cut of a `.ld` range and the input never overwritten, "final" as the design's checks asserted by the walkthrough (refines D-155, D-181 (e), D-202 (a), D-206 (i), D-209; reads 05 §5, §9.2, 06 §4.3, §14.4, §16, §20 item 3, §22 (M4), 07 §2.2, §7.1 "Data", §7.2 AC-M4-01, §7.5; amends 05 §9.2's `convert` row, 07 §7.2 AC-M4-01's test path)
**Context.** AC-M4-01 maps the twelve items of 07 §2.2 to named tests: items 1-10 in the Qt Test walkthrough
`tests/ui/walkthrough_saturday` with one test function per item (required on Windows and Linux), item 11 as ctest
golden-JSON tests of each `esses-cli` subcommand, item 12 on the VM checklist; 06 §22 (M4) asks that the scripted
walkthrough drive the shell through command ids. 05 §9.2 lists `convert <in> --to <writerId> [-o out] [--rate]
[--channels]` (reader to writer through `ExportView`), never built: D-181 (e) left `motec.ld` output to it, and the
export-only `.ld` writer writes whole layers without resampling (`ld_export.cpp`, M0). 07 §7.1 "Data" asks for the
default circuit workbook, the default schemes and `keymap.json` "final" without saying what final means. Item 10
names the `RasterBackend` budgets, which are absolute limits on the reference machine (07 §12, D-042), not something
an offscreen test may time. The demo of 07 §7.5 had no offscreen form.
**Decision.** (a) **The walkthrough** is `tests/ui/tst_walkthrough_saturday.cpp` (ctest `ui.tst_walkthrough_saturday`:
every Qt Test of `tests/ui` is a `tst_<name>.cpp`, so it is AC-M4-01's `tests/ui/walkthrough_saturday`), one function per item named `itemNN_<what>` so a
failure names the item, plus `demo75_offscreenWithoutTheInstaller`. Its data are written by the test from formulas
(`tests/ui/saturday/saturday_log.hpp`): the day's `.ld` + `.ldx` on the AC-M3-04 synthetic circuit (an out lap,
flying laps of 70, 68 and 68.5 s, an in lap, one profile per lap fraction, the shipped workbook's channel names), a
second logger's legacy CSV (Esses' own writer) and a generic CSV with an unknown unit; every expected value comes from
the formulas, the design or the shipped files read as data (D-041). Each function starts its own runtime, window and
project over a copy of the logs (the sidecar edits of one item never reach another). The helpers are header-only,
because every non-`tst_` `.cpp` under `tests/ui` is compiled into every ui test. The shell is driven through command
ids, keys through the `KeyEventRouter` and the public accessors; a prompt is replaced only where the shell asks a
question (the channel label's scale/offset and filter). No fixed waits: generous `QTRY_*` and the runtime settling
(`runtimeQuiet`, `waitQuiet`). It runs on the configured offscreen screens of D-209 (the windows open on the DPR 1
screen). Where an M1-M3 test covers a behaviour in depth, the item exercises it once end to end through the shell and
`docs/milestones/M4.md` §1 names the deeper test. (b) **Item 10**: the budgets stay the `bench` rows
(`plot.cursor_move`, `plot.zoom_pan_step` on 20 traces; AC-M4-05 on the reference machine); the walkthrough asserts
the functional half: a 20-trace graph built, a cursor move rebuilding the Dynamic layer only, a zoom step rebuilding
the Static layer, and at DPR 2 and back at 1 (the window moved between the configured screens as `ui.tst_dpr_switch`
moves it) a Static image of the logical size times the DPR laid out at that DPR. (c) **Item 11** is
`cli.golden.<command>` for `info`, `channels`, `laps`, `eval`, `convert`, `export`, `validate` and `fixture`
(`apps/cli/tests/cli_golden.py`, label `cli`, `fixtures` too where the generated inputs are needed): the fixtures'
committed `expected.probe.json`/`expected.laps.json`/oracle CSVs, `tools/ld_ref_decode.py` (with 05 §3.4's unit rule
(c) applied to its raw fields), the schema examples' directory layout and `expect.json` pointers, and hand formulas
(`2 v + 1`) are the expectations; `bench` is the benchmark executables' (D-155). (d) **`convert`** as built:
`esses-cli convert <input> --format csv|motec.csv|motec.ld -o <out> [--channels] [--range] [--rate] [--layer]
[--writer-options] [--importer] [--options] [--no-sidecar] [--json]`; `--to` is kept as an alias of `--format`. It
opens the log through the open pipeline with the circuit rules (phase 2 included) and writes logged channels only (by
name, then logged name; the project-aware path with maths stays `export`, D-181). `csv`/`motec.csv` go through the CSV
writers (`--rate` grid, `--range` from time 0). `motec.ld` goes through the export-only writer: a whole `.ld` keeps
every channel's stored bytes (read, write, read bit-exact for samples and validity, 05 §5 item 8); `--range` is cut in
the CLI because the writer writes whole layers: each uniform channel keeps the samples whose time lies in [start,
end), starting at time 0 (a `.ld` channel always starts at 0, so a channel whose first sample in the range lies after
its start moves earlier by less than one of its periods), re-quantised with its stored encoding; the markers in
[start, end] move by -start (the closing beacon of a lap stays with it), SOL/EOL and the ranges are rebuilt by the
reader. `--rate` with `motec.ld` is refused (exit 2: no resampling in 0.1, 05 §5 item 4 comes with the export UI at
v1.0) and a channel the writer cannot hold is skipped with `ld.channel_skipped` and listed as skipped. The output
never replaces the input: an output (or, for `motec.ld`, its `.ldx`) that is the input, the input's `.ldx` or its
sidecar, lexically after normalisation, case-folded on Windows, or the same file by `std::filesystem::equivalent`
(another case on a case-insensitive file system, a hard link), is exit 2 before the log is read. `export --format
motec.ld` stays refused and names `convert`. (e) **"Final" for the Data of 07 §7.1** means checked against the
design and kept checked by the walkthrough: the workbook against 07 §2.1-§2.2 items 2 and 5 (every MVP component
type, the worksheets of D-202 (a)) and 06 §4.3 item 2 (every rectangle on its worksheet's grid, at least 4 x 4 cells,
every type registered) — the Overview gauges sat at 0.775, off the 1 % grid, and now sit at 0.78; the schemes against
06 §16 (all 36 roles and 16 font roles, palettes of at least 10 and exactly 9) and 06 §20 item 3 with §13.9 item 4's
3:1 (every foreground/background pair and every palette colour against its background; the lowest pairs are 3.08:1
and 3.10:1 in the light scheme) — unchanged; their grid lines stay faint by design (§13.9 item 4's flag belongs to the
v1.0 scheme editor, and darker grids would change every golden); `keymap.json` against 06 §14.4 (the file's command
ids are the table's 141, every implemented row registered and bound in its context, planned and reserved rows
registered and disabled) — unchanged by this package (`file.reload_source` is D-208's, `help.contents` D-215's).
(f) The walkthrough keeps the `ui` directory's ctest `TIMEOUT` of 600 s (`esses_add_qt_test_dir`); it runs in about
140 to 190 s in MSVC Debug here, each function in 12 to 25 s.
**Consequences.** A failure of AC-M4-01 names the item. Item 6 checks a channel override in the document and the
Channel Editor: the components and the Values window do not follow overrides in 0.1 (D-211 (a), §10 item 16 of the
milestone note). A project mapping applies at the next open (item 6 reopens the log). The demo's offscreen form
covers `-w -l` through `MainWindow::openProject`, the budget warning, About and the portable layout; the installer and
the association stay on the VM checklist. `convert` of a CSV with a fractional rate to `motec.ld` writes no channel
until the writer resamples (v1.0).
**User must confirm:** no.

#### D-215 The user guide and Help Contents as built: Markdown pages under docs/user/ with index.md as the start page, the installed page looked up beside the data directory then beside the executable, the online page at the build's tag, the online page when the installed one does not open, a Messages warning when nothing opens, CLEANROOM.md completed for 0.1 (refines D-004, D-206 (h)-(i); reads 06 §2.2, §14.4 help.contents, 07 §7.1 "Docs", 08 §7, §9 item 4; amends 06 §14.4's "local HTML or URL")
**Context.** 07 §7.1 asks for a `docs/user/` skeleton (install, open files, selection, graph, maths,
troubleshooting) and a `CLEANROOM.md` complete for every 0.1 behaviour; 06 §2.2 puts Help Contents in the Help menu
and 06 §14.4 gives `help.contents` (F1) "Open the user documentation (local HTML or URL)". D-206 (i) left
`DocsShell`'s lookup, its fallback and the format of the pages open. The packaging package (wave 2, in parallel)
installs the files and needs their expected place; nothing in the build generates HTML; 08 §9 item 4 names
`docs/user/relinking.md` for the LGPL re-linking right.
**Decision.** (a) **Plain Markdown.** The guide is Markdown under `docs/user/`: `index.md` (the start page),
`install.md`, `open-files.md`, `selection.md`, `graph.md`, `maths.md`, `troubleshooting.md` and `relinking.md` (08
§9 item 4), with relative links, readable in any text editor and rendered by the repository host; no HTML is
generated in 0.1 (amends 06 §14.4's "local HTML"). The pages are written in our own words from the design set and
the built application (menu and command titles from `esses --dump-commands`, the Options, Messages and dialog
strings of the source), never from the vendor's help text; the vendor's product is named once, in a nominative
compatibility statement with a non-affiliation notice (`index.md`), and otherwise only through the importer ids.
(b) **The installed start page** is the first regular file of `<data dir>/../docs/user/index.md` and
`<exe dir>/docs/user/index.md` (`DocsShell::installedStartPage`, `kStartPage`): `docs/user/` is installed as a
whole beside the shipped data directory, i.e. `<install dir>\docs\user\` on Windows (installer and portable zip),
`<prefix>/share/esses/docs/user/` in the AppImage and `Esses.app/Contents/Resources/docs/user/` in the bundle
(`docs_shell.hpp`; `docs/milestones/M4.md` §8 "Package docs (wave 2)", for the packaging package). The source tree
has the same layout, so a run with the repository's `data/` finds the repository's pages. (c) **Not installed**
(a build tree, whose data directory is `<build>/bin/data`): the start page in the public repository,
`https://github.com/mitchdetailed/jubilant-octo-meme/blob/<ref>/docs/user/index.md` with `<ref>` = `v<version>`
when the build's version is exactly a tag (`0.1.0`, `0.1.0-m4`) and `main` otherwise (after a tag, dirty, or an
unexpected form; `DocsShell::onlineUrl`). (d) **Opening.** `help.contents` ("Help Contents", Help menu, F1,
Global; its keymap row implemented in M4) hands the installed page's file URL to the shell's URL opener
(`setUrlOpener`; by default `QDesktopServices::openUrl`), then the online URL when there is no installed page or
the opener refuses the file (a system with no program for `.md` files); when neither opens, a Messages warning
(System, source "Help", code `help.not_opened`) names both places, the Messages dock is shown and the status bar
says so. `contentsUrl()` keeps the contract's meaning (the installed page, empty when not installed);
`openContents()` returns the URL that opened. (e) **`CLEANROOM.md`** gains the rows of 2026-10-19 (the key
assignments; the Data window and the selection with its chosen readings; workbooks, worksheets and docks; the
time/distance core and the values; rendering; corrected speed and distance; laps; channels; the command line; the
schemes; the project folder; the runtime; the D-211 follow-ups; the M4 behaviours; the CLI; packaging; the
dictionaries; the guide itself) and a coverage table from the twelve items of 07 §2.2 to the rows, with the
categories of D-128/D-145 and "own design" where no i2-derived behaviour exists.
**Consequences.** F1 works in every build: an installed package opens its local page (if the packaging package
does not install `docs/user/` as in (b), the online page opens instead), a build tree the online one, and the
tests replace the opener (`widgets.tst_docs_shell`; the `help.contents` KEYMAP_TEST, whose fixture's repository
data directory finds the repository's pages). Where no program opens `.md` files the browser shows the online
page. A build after a tag opens `main`, which may describe newer behaviour than the build. The repository URL is a
constant of `docs_shell.hpp` beside the About dialog's `sourceUrl()` (one constant in `cmake/naming.cmake` would
remove the duplicate; the coordinator's). `widgets.tst_m4_contract` `plannedM4RowsStayPlanned` still lists
`help.contents` as planned and must drop it (the coordinator's file). The new prose is a release-checklist item of
`tools/check_text_overlap.py` (08 §5, §8; the maintainer's, with the private reference corpus). No golden changes.
**User must confirm:** no.

#### D-216 The M4 integration as built: the commit stamped at a tag, the soak out of the normal runs with its guard's tests in them, the walkthrough and the CLI golden tests as CI steps of their own, the install rules with and without Qt and the release tree scanned in lint, every M4 keymap row implemented with M4 as the keymap checker's milestone, the demo's command line in a real process, one repository URL, the soak's restart check after a Close All (refines D-206, D-212 (b), D-213 (f), D-214, D-215; reads 07 §7.2, §7.5, 08 §3.10, §4.1, §4.4, §5; amends D-213 (f), 08 §4.1's qt-app and lint rows)
**Context.** Wave 2 of M4 (packaging D-212, soak D-213, walkthrough D-214, docs D-215) was built by four packages in
parallel, each in its own files; the integration makes them one tree and proves M4 locally. The packages left the
coordinator these points (`docs/milestones/M4.md` §10): `cmake/version.cmake` stamped the commit "unknown" when the
checkout stands exactly on a tag (`git describe` prints the tag alone), so a release build's `esses --version` and
About would say "commit: unknown" and the tag build's smoke test (which refuses it) would fail, and it read a
modified tree at a tag (`v0.1.0-dirty`) as the pre-release "-dirty" (item 28); the soak entry ran in the default
ctest run, about 5.5 minutes in MSVC Debug (D-213 (f), item 36), while the integration's brief keeps it out of the
normal runs; the walkthrough ran under the `ui` directory's 600 s on CI runners slower than this machine (item 39);
AC-M4-08 names `lint` as its proof while the release tree was scanned only by `package.yml` and `package.py`;
AC-M4-01 names `ctest -L cli` and the walkthrough as required checks; `widgets.tst_m4_contract` still expected
`help.contents` to be planned (items 22, 43); the repository URL had two constants (item 24); a keymap case file
closed `tst_keymap.cpp`'s anonymous namespace around its includes (item 25); and the demo of 07 §7.5 had no run of
the real `esses -w <project> -l <log>` command line.
**Decision.** (a) **The version stamp.** `cmake/version.cmake` runs `git describe --tags --dirty --always` and `git
rev-parse --short HEAD`; `esses_version_from_git` (`cmake/version_parse.cmake`, no side effects) takes a trailing
"-dirty" off first, parses `vMAJOR.MINOR.PATCH[-pre][-N-gHASH]` into the version (`+N-gHASH`, and `.dirty` for a
modified tree) and takes the commit from `rev-parse` in every case, exact tags included, with "-dirty" for a modified
tree; "unknown" only without git or a commit. A release build at its tag is `0.1.0` with the tag's commit; a
modified tree at the tag is `0.1.0.dirty`, which `ESSES_RELEASE_TAG` refuses (D-212). ctest `lint.version_stamp`
checks 17 hand-written cases (11 of them fail with the old parsing). (b) **ctest and CI.** `soak.tst_saturday` runs
the `saturday` function alone (labels `qt;soak`) and is left out of the normal runs: ci.yml's qt-app and the
integration's full local runs use `ctest -LE soak`; the soak is bench.yml's nightly job (D-213 (e)), and `ctest -L
soak` or the executable locally. The guard's own tests (`growthRule`, `threadViolationMessages`,
`guardRecordsDispatcherViolations`, `violationsAreFatal`) are the new entry `soak.guard` (label `qt`) of the same
executable, in every run on every OS, and `soak.lsan_check.self_test` is labelled `lint` only. `esses_add_qt_test`
gains `ARGS`, and `esses_add_qt_test_entry` registers another entry of an executable (amends D-213 (f)).
`ui.tst_walkthrough_saturday` gets a ctest TIMEOUT of 1,500 s and is a step of its own in qt-app ("Saturday
walkthrough", every matrix row: Windows and Linux on every pull request and push, macOS nightly; left out of the
main test step); core runs every test but the label `cli`, then `ctest -L cli` as its own step ("esses-cli golden
tests", AC-M4-01 item 11); both extra steps run after a successful build even when the step before failed. qt-app's
timeout goes from 45 to 60 minutes (the M4 suites in MSVC Debug). (c) **The release tree in lint.** The top-level
`CMakeLists.txt` adds `tools/packaging/` with and without the Qt layers: a Qt-free tree installs `esses-cli`, the
data, `NOTICES.md`, the licence texts, the user guide and, on Linux, the desktop entry, the MIME type and the icons
(the application's rules apply when its targets exist; the packaging self-tests join the core runs). The lint job
installs its Qt-free build into `build/release-tree` and runs `check_vendor_marks.py --tree` on it (AC-M4-08 in
`lint`; the application's tree with Qt stays `package.py`'s scan on each OS, D-212), and `reuse lint` is pinned to
5.0.2 as in `package.yml`. (d) **Demo 7.5 offscreen** is `ui.tst_walkthrough_saturday`
`demo75_offscreenWithoutTheInstaller` (D-214: `MainWindow::openProject(folder, {log})` in process, the Budget
warning with the estimate, Continue and the Performance field, Help > About with the Qt LGPL notice and the source
URL, the portable layout) plus `app.tst_demo_command_line`: a real `esses --new-instance -w <project> -l <log>`
(a private configuration and channel name) opens the project, then the log, as its settings' recent lists show. The
installer, the association and the USB stick stay on the VM checklist. (e) **The contract as built.**
`widgets.tst_m4_contract` `plannedM4RowsStayPlanned` becomes `everyM4RowIsImplemented`: the shipped keymap's M4
rows are exactly `file.reload_source` and `help.contents`, both implemented, registered by their packages without
the generic row's "planned for M4" note, Help Contents enabled and Reload File disabled while nothing is stale.
`tools/check_keymap_coverage.py`'s current milestone is M4 (every M1-M4 row implemented with its KEYMAP_TEST; v1.0
in progress), and `project.keymap: hotkey batch 1 ...` accepts a planned row only for v1.0 or later. The comments of
the contract tests, the frozen headers and the READMEs describe the members as implemented; no contract stub
marker is left (`grep -rn` of the marker over `libs apps tests tools bench` finds nothing). (f) **Small
cross-package fixes.** `DocsShell::kRepositoryUrl` is the About dialog's `kRepositoryUrl` (one URL in C++);
`tst_keymap.cpp` includes what `keymap_m4_docs_cases.hpp` needs, so the case file no longer closes the anonymous
namespace; the application's tests read a child's `settings.json` through `esses::readWholeFile`
(`app_test_support.hpp` `fileText`), which shares delete access on Windows, where a `QFile` read held the name
against the child's `AtomicFile` rename. (g) **Deferred explicitly** (`docs/milestones/M4.md` §5): the M3 items of
M3.md §6 not taken in M4 (1, 5, 9, 11, 13, 14, 20) stay open there for v1.0 (item 5 waits for the maintainer's real
GPS and IMU lap); the hosted bench baselines at the milestone tag (M3.md §4 item 3) wait for
`tools/bench_compare.py`, which no milestone has written yet (`bench.yml` uploads the results and skips the
comparison), and move to M5 with the v1.0 gates; `tools/check_text_overlap.py` (08 §5, release checklist item 5) is
not written: before the tag the maintainer reviews the new prose against the private reference corpus by hand
(M4.md §6.5). (h) **The soak's restart after a Close All.** A five-minute Clang run of `soak.tst_saturday` (24
cycles) failed at cycle 17 with "restart: Main was not restored": cycle 16 ends with Close All (every fifth cycle
from 1) and cycle 17 restarts (every fourth from 1), which no shorter run had reached; with nothing open Main is the
nil `RangeRef`, which the driver kept as the Main to find again, while the restore, which reopens the project's
previous data (M4.md §10 item 33), sets a Main again. The driver now checks Main after a restart only when there was
one (`DataSelectionModel::hasMain()`); every 20 cycles the nightly and the 2 h runs pass that point (a test fix: no
product behaviour changes).
**Consequences.** A tag build names its commit and passes its smoke test's commit line; a release can only be built
from a clean checkout of its tag. Pull requests no longer run the soak anywhere (the guard's tests still run on all
three OSes); the soak's runs are the nightly job and the maintainer's pre-tag dispatch (M4.md §6.5), so a Windows
or macOS soak is a local run only. A failing walkthrough or CLI golden test shows as a step of its own. Core
configures gain the five packaging self-tests. The keymap checker refuses a planned M1-M4 row. The changed CI steps
run first in CI (none of them can run here); M4.md §6 lists what the maintainer still runs. Three MVP rows of 07
§12 have no benchmark (full relayout, the Values window's refresh of 250 channels, startup to an empty workspace;
M4.md §10 item 46): AC-M4-05 needs them measured or moved under 07 §1.5 by the maintainer.
**User must confirm:** no.

#### D-217 The M4 fix pass: PR #5's first CI failures fixed at the root, Reload that keeps the source until its file is open again, decode-time diagnostics in Messages, the activation check on the pool, forwarded requests at the top of the event loop and only after their answer, a salted per-user channel, the release stamped from its own tag and development builds marked, convert's companion, cut details, empty output and duplicate channels, the hosted bench without the reference gates, the three missing MVP bench rows, installer upgrades and uninstalls that keep what is not theirs, and the deferrals (refines D-208, D-209, D-212, D-214, D-215, D-216; reads 02 §1 behaviour 5, 05 §5, §9.2, 06 §6, §17, §18-19, 07 §1.5, §7.2, §12, 08 §3.10, §4, §7; amends D-208's "Reload as a close then an open", D-212 (b)'s channel name, D-216 (a)'s version of a build without a version tag)
**Context.** PR #5 (M4 waves 1 and 2, head 32b7795) failed its first CI run: `lint` at clang-format (three lines that
19.1.5 lays out differently from the 18.1.7 the packages used locally; every later lint step skipped), both required
`qt-app` rows in `ui.tst_dpr_switch` (SIGSEGV on ubuntu, 0xc0000005 on windows-2022, Qt 6.12), and every `package`
job (Windows at the Qt install, Linux and macOS at the vendor-mark scan of the Release tree). The milestone audit and
two reviews (`build/m4-review/*.json`, 44 findings) added: Reload lost a source whose file was missing or did not
reopen; decode-time faults reached no one; the activation check stat'ed every file on the GUI thread; forwarded
launches ran inside modal prompts; `convert` kept another file's `.ldx`, copied stale details into a cut, refused
`run.csv -> run.ld` and wrote empty `.ld` files; a second `v*` tag on the release commit could change the release
build's version; the hosted bench alarm failed on the reference limits; three MVP rows of 07 §12 had no benchmark;
and smaller points in the installer, the association script, Help Contents, the badge, the reports and the docs.
**Decision.** (a) **The main window's teardown.** `~MainWindow` deleted every direct child one by one while
QMainWindow's layout lived; that layout keeps raw pointers to the `QDockWidget` frames and to the tab bars of tabbed
docks, and a dock newer than its group's tab bar (added later into a tabbed area, or floated and docked back) met the
deleted tab bar when it was removed (Qt 6.12 crashes there; 6.7.2 happens not to touch it). Now only the docks' contents
(which reference the models) go there; the frames and the tab bars go in `~QWidget`, after the layout, as in any
QMainWindow (`widgets.tst_main_window` `dockFramesGoAfterTheMainWindowLayout`). (b) **The formatter.** The three
declarations are written `struct stat st = {};`, which 18.1.7, 19.1.5 and 22.1.3 format alike; AGENTS.md names the local
formatters (Qt Creator's 18.1.7 and Visual Studio 2026's 22.1.3) and the rule: CI's 19.1.5 is the reference, and a line
the two local ones format differently is rewritten into a form both accept. (c) **Vendor marks in optimised binaries.**
GCC and Apple Clang at -O2 store the allow-listed literal `motec.csv` as the 8-byte immediate `motec.cs`, an opcode,
then `v`; `check_vendor_marks.py --tree` now also blanks a prefix of an allow-listed literal that is a whole number of
8-byte words and is cut off there (a cut of another length and a bare mark are still reported; self-test cases). (d)
**The Qt install.** With `--external 7z`, aqtinstall's 4 default workers ran a 7-Zip process per archive into the kit's
folder, and they raced to create it. The Windows installs of `package.yml` and `ci.yml`'s `qt-app` now give aqtinstall
one worker (`.github/aqt-settings.ini` through `AQT_CONFIG`, read on top of aqtinstall's own settings; checked against
aqtinstall 3.3.0's code here), so the archives are extracted one after another; a failed first install still gets a
second try over what it left (`continue-on-error`). Neither has run in CI yet: the next package run is the evidence.
Every action of `ci.yml` and `bench.yml` is pinned as `package.yml`'s are, and `package.yml` runs on every pull request
(what is deployed depends on more than the packaging, and its Release-tree scan proves (c) on what ships). (e)
**Reload** (amends D-208): `SourceLoader::reload` keeps the stale source until its file is open again. It is refused
while the file cannot be stat'ed (Missing) and while the source's due sidecar edits cannot be written (the document then
stays due); otherwise the sidecar is flushed and the path opened beside the stale source. When the reopen publishes, the
stale source is closed (its selection parked) and `sourceReloaded` hands the selection on; a reopen with the same
content id replaces it in `published()` (its decode log kept). A reopen that fails, is cancelled or is held and
cancelled (Mapping panel, budget gate) leaves everything as it was; the shell says why a reload was refused. (f)
**Decode-time diagnostics.** The loader reads every open source's `DecodeLog` on a 1 s GUI timer: new entries reach the
Messages dock through `importLog`, and `ld.io_fault` (Missing) or `ld.source_changed`/`text.source_changed` (Changed)
marks the source stale at once, so a source a dropped share sealed is offered Reload; `ld.source_changed` is logged once
per source, as `ld.io_fault`. (g) **The activation check on the pool.** The shell's `Qt::ApplicationActive` runs
`SourceLoader::checkChangedFilesInBackground()`: one TaskPool task re-stats every open source (one pass at a time), the
result comes back through the Dispatcher, and the Messages row reads the state the check recorded (`staleState`) instead
of stat-ing again; `checkChangedFiles()` stays the synchronous pass for tools and tests. (h) **Forwarded launches.**
`InstanceServer` emits a request only once its "ok" reached the client (a launch that gave up waiting opens its own
window, and the files never open twice), and a launch tries a second forward only when the first found no instance. The
application applies forwarded requests through `app::RequestQueue`: only with no modal widget or popup open and the
event loop at its top level, one at a time. A forwarded project switch the user cancels opens none of its logs into the
project that stays (`MainWindow::projectOpenCancelled`). The channel (amends D-212 (b)): the Windows pipe gets a
protected DACL for this user alone, the name mixes in a random salt kept in the settings folder (`instance.salt`,
written once by an atomic rename), and a launch that would forward but ends without the channel says so in Messages
(`launch.instance`); `--scheme`/`--keymap` given to a launch that forwards are reported on the terminal. On POSIX,
connects are non-blocking and bounded by the caller's deadline. (i) **The version stamp** (amends D-216 (a)). With
`ESSES_RELEASE_TAG`, the stamp describes that very tag when HEAD carries it (`git describe --tags --dirty --exact-match
--match <tag>`), so another `v*` tag on the release commit (the milestone tag) cannot change the release's version; a
build without a version tag (a shallow clone, a non-version tag) is `<project version>+g<hash>` (`.dirty` when
modified), never the bare release version; pre-release tags may hold hyphens. (j) **convert.** The motec.ld writer
removes a `.ldx` already beside its output when it writes none; a `.ld` cut carries its own details (Log Time and, past
midnight, Log Date moved to the cut's time 0; Start Lap the first flying lap of the cut) and does not reuse the input's
verbatim header; only a `.ld` input's companion is protected; a conversion that writes no channel leaves no file and
exits 1; a channel named twice is a usage error. The cut stays the half-open `[start, end)` of D-214 (d): its closing
beacon is the new end of log, so the circuit rule calls that single lap the In lap, and a slower channel has no sample
after its last one in the range (05 §5: no resampling in 0.1). (k) **Benchmarks.** `bench/CMakeLists.txt` gains
`ESSES_BENCH_GATE` (ON); the hosted preset sets it OFF, so the hosted alarm records results instead of failing on the
reference limits, and its comparison and upload always run. The three missing MVP rows of 07 §12 are measured:
`plot.relayout_resize_20tr` (`plot_bench`, the 20-trace graph resized between 4,200 x 1,000 and 3,000 x 800, each step
from its leading edge to the repaint), `values.refresh_250ch` (`plot_bench`, the Values window over 250 channels, 60
cursor moves to the repaint) and `app.startup_empty_warm` (`open_bench`, `esses --new-instance` to its
`ESSES_STARTUP_PROBE` ready line); the §1.5 option M4.md offered for them is withdrawn (07 §1.5 never moves a budget row
but for a machine cause). The Values row failed at first (2.6 ms): `ValuesModel` now reads the view once per refresh and
computes a row when a view asks for its cells, the window sizes its value columns to the rows on screen, and its cells
are drawn by one positioned `drawText` each over the style's item panel (the style's per-cell text layout was most of a
repaint), with selection, focus, alignment, elision and the pending colour unchanged. (l) **Packaging.** The installer's
`[InstallDelete]` removes before `[Files]` what it and earlier packages put into `{app}` (runtime DLLs, Qt modules and
plugin folders); an upgrade no longer takes the previous tasks (`UsePreviousTasks=no`), and the uninstaller removes
`.ld`'s default only while it is `Esses.LogFile`. `associate-ld.cmd` passes the folder's path to `reg` through delayed
expansion. The macOS Info.plist names `LSMinimumSystemVersion` (the deployment target, 14.0 in `package.yml`) and
exports the log type; `smoke.py dmg` checks both on the built bundle (`--macos-minimum`, the job's
`MACOS_DEPLOYMENT_TARGET`). `smoke.py windows-portable` compares `%APPDATA%\esses` file by file and runs the scripts
from a folder whose name holds `&` and `%`. (m) **Smaller fixes.** Moving a time_distance group, its undo and its redo
carry the group's vertical zoom, and a group edited in place keeps it (`applySpec`: a group still in its place keeps its
zoom, the others are found by their name and channels, and those left over on both sides are paired in order when there
are as many of each, so adding or removing a channel or renaming a group does not drop it); the Track Editor keeps a
bound whose field still shows it (the last section's end) and keeps edited bounds on the track; Help Contents goes
online at once when no program opens `.md` files (Windows: the association); the Data window's badge and funnel are
drawn at each DPR and cached; a channel report keys its channels by display unit too; File > Export Data holds the
component by QPointer across its prompts. (n) **Deferred** (`docs/milestones/M4.md` §10): the macOS nightly's
`qtrender.tst_static_strips` at DPR 1.5 and 2 (red since M1: text painted on a pool thread differs from the GUI thread's
under CoreText; needs a Mac or CI iterations; macOS is "builds green, not supported", and the walkthrough step still
runs after it); **crisp 2 px lines at DPR 2** (the builders centre them on pixel centres, so they spread over three
device columns, D-209 (d)'s "no blur" admits it): centring even widths on pixel edges changes every DPR 2 golden and
needs the ubuntu artefact, so the maintainer decides before the tag whether 0.1 ships the soft lines with a line in the
release note or waits for that change (the user-confirm part of this ADR); a same-size replacement that keeps the mtime
(02 §1's `changedOnDisk()` is a re-stat of size and mtime by design; a content or identity check is a design change for
v1.0); the binary-distribution notices of Qt's bundled third-party code in `THIRD_PARTY_LICENSES.md` (they need the Qt
6.12.0 sources the maintainer attaches at the release, M4.md §6.0 step 8).
**Consequences.** PR #5's four CI failures are fixed at their cause: (a) and (c) with tests that fail without the fix,
(b) in a form both local clang-format releases accept, (d) by construction (one aqtinstall worker). None has had a CI
run since: the first runs of the new and changed steps are CI's (M4.md §8 "Fix pass (wave 3)"). A reload never loses a
source, its selection or its edits; for a moment during a reload both files are open. Decode faults and changes made
while Esses stays active show at once. A forwarded request waits while a prompt or menu is open (at most until it
closes); a second Esses launched while the first is hung waits once, not twice. A build from a shallow clone reports
`+g<hash>`; the release build's version comes from its tag whatever other tags the commit carries. `esses-cli convert`
changes behaviour for empty outputs (exit 1) and duplicate channels (exit 2). The hosted bench job is green unless a run
fails to run; its regression signal waits for `tools/bench_compare.py` (M5). AC-M4-05 has a benchmark for every MVP row;
their verdict is the reference machine's (§6.4). Package runs cost every pull request about 12 minutes per OS. The
settings folder gains `instance.salt`. A local soak of fewer than about 16 cycles can judge the widget series grown from
the Maths Editor's first use in a restart epoch, not a leak (M4.md §10 item 57); the nightly's 30 minutes are not
affected.
**User must confirm:** yes: (n)'s crisp 2 px lines at DPR 2 (ship 0.1 with them soft and say so, or wait for the
builders' change and new DPR 2 goldens); the rest no.
