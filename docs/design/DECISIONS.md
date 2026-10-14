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

Addendum A (after the open questions) adds D-037–D-192: decisions proposed by the section writers of documents 00–08, by the 2026-10-10 review (A.1–A.8) and by the final cross-document revision (A.9), plus implementation decisions taken once M0 started (A.10: the user's instructions, and the reconciliation with spike S-1 and the converter port specification) the departures of M0 as built from the design (A.11), those of M1 as built (A.12, proposed) and those of M2 (A.13, proposed), with their own index. Where an addendum entry amends an ADR above, the ADR carries a one-line "Amended by" note.

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
| D-184 | AC-M2-14 reads the fixture's own channel name (amends 07 §5.2 AC-M2-14) | **yes** | M2 integration, AC-M2-14 |
| D-185 | Maths engine corrections of the M2 review: result rate through statistics, integer and Bool dtypes, non-finite results, literal factors, table axis units, the power switch's hash, synthesised self references, filter windows, stepping constants, recursive `previous_sample`, the math window in the selection key, variance across sources, timestamped rates (refines D-021, D-022, D-164, D-170, D-171; amends 03 §3.2, §4.4, §5.2, §5.4, §5.6, §6.4–6.6, §7.2, §7.6, §8) | no | M2 review, AC-M2-02 |
| D-186 | `scale_offset` is computed in the unit of its offset (amends 03 §4.4) | **yes** | M2 review |
| D-187 | The journal as a per-session snapshot: write-ahead save, autosave beside a deferred recovery, Recover adopts, case-only renames (amends D-172 (h)–(i); 02 §6.8) | no | M2 review, AC-M2-06 |
| D-188 | `esses-cli maths check` reports MathGraph's nodes (refines D-166; 03 §13, AC-M2-01) | no | M2 review, AC-M2-01 |
| D-189 | Seven keymap rows of milestone M2 move to M3; the workbook rows are built (amends 06 §14.1's milestone column, D-178 (e)) | no | M2 review |
| D-190 | The rest of the M2 review: recoverable saves, bounded and guarded text reads, coalesced sidecar writes, cancellable opens, the Maths Editor's partial save (amends D-167, D-172, D-177 (a), D-187; 02 §4 sidecar rule 4, §6.8; 05 §7.1) | no | M2 review, AC-M2-06, AC-M2-11, AC-M2-13 |
| D-191 | The 0.1 evaluator streams a plan in pieces and seals a node's chunks on idle workers (refines D-139; 03 §6.0; AC-M2-10; supersedes the proposal of D-183) | no | AC-M2-10 |
| D-192 | Public README is a work-in-progress notice; TRADEMARK.md withheld until a working model (amends D-004; AC-M0-10) | no (user request 2026-10-13) | Maintainer |

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
*Amended 2026-10-11 (user statement): "the python program is now deprecated and left behind for the qtc++ version; the qt C++ has the conversion mappings from specific formats to .csv or .ld". The converter's Python version is therefore not used as a source or a reference. The Qt C++ tree (`~\Documents\AntiGravity Projects\V8 of log file converter\QT C++ Version`), including its per-format JSON mapping files such as `haltech_mappings.json`, `MaxxECU_mapping.json`, `emtron_mappings.json`, `Emublog_mapping.json`, `cosworth_mappings.json`, `Syvecs_SD_scaling.json`, `iracing_ibt.json` and `windarab_txt.json`, is the reference for the later input-format phase (the harvest of D-028 and D-107, and the text profiles of D-029). The statement does not say that the converter is the user's own work or that it is licensed MIT, so (a) to (f) still apply: nothing from the Qt C++ tree, its mapping files included, is reused verbatim until the user confirms authorship and licence (Q-15, still open), and its format knowledge enters Esses only as restated facts recorded in `CLEANROOM.md` as user-contributed own work (D-145). (a) still names the Python predecessors, because the C++ `.ld` files say they mirror them.*
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
**User must confirm:** yes (it changes an acceptance criterion).

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
**User must confirm:** yes (it changes the computation 03 §4.4 specifies).

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
