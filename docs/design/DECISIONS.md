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

Addendum A (after the open questions) adds D-037–D-152: decisions proposed by the section writers of documents 00–08, by the 2026-10-10 review (A.1–A.8) and by the final cross-document revision (A.9), plus implementation decisions taken once M0 started (A.10: the user's instructions, and the reconciliation with spike S-1 and the converter port specification) and the departures of M0 as built from the design (A.11), with their own index. Where an addendum entry amends an ADR above, the ADR carries a one-line "Amended by" note.

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

**User must confirm:** no.

---

## D-020 Maths language and operator precedence

**Context.** Research: keep the expression language 100 % source-compatible (`'Name'[unit]`, same ~60 functions, operator word aliases, `"Range:Path"` literals, comments, int-vs-decimal window semantics, `invalid()`). Open questions: precedence/associativity of `^` and unary minus; channel vs function case sensitivity (help: not case sensitive; release notes: functions case sensitive). Contradiction (all three scorecards): `^` tighter than unary minus so `-2^2 = -4` (core, eco) vs unary minus tighter so `-2^2 = 4` (parity, perf); core-first lumps `& | && ||` at one level.

**Decision.** Pratt parser; precedence highest → lowest: postfix call and `[unit]` → `^` (right-associative) → unary `- + ! ~ NOT` → `* /` → `+ -` → `< <= > >= LT LTE GT GTE` → `== != EQ NEQ` → `&` → `|` → `&& AND` → `|| OR`. Hence `-2^2 = -4` (mathematical convention; Python/Excel-formula differ and i2 is undocumented). A compat switch `maths.compat.unaryMinusAbovePower` (per project, default off) flips the two rows; the Maths editor hover help states the rule. Tokens: `'Channel Name'[unit]` (`[]`/omitted = as-is; NFKC case-fold), `"text"`, `"Outings:Laps"` range literals, integer vs decimal literals as distinct AST nodes, scientific notation, `//` and `/* */` comments, arity overloading. Channel and function names are case-insensitive; a mismatched-case function name produces a style warning. Bitwise `& | ~` require integer operands; `bit_xor()` for xor. Every ambiguous semantic (window edge rules, `derivative` default, `pulse_width`, `bit_decode`, `integrate_dist` arities, running min/max names, table out-of-range) is listed in `docs/maths-compat.md` with the chosen reading, a `*.case.json` test and, where allowed (D-005), an oracle from the user's i2 exports.

**Consequences.** Users paste i2 expressions unchanged; the only divergence risk is `^`/unary minus and it is switchable.

**User must confirm:** **yes** — verify `-2^2` on the user's i2 install if possible (D-005 (3), D-038 (c)); permitted since the user's yes to Q-03 (3) on 2026-10-10 and scheduled for M2. Until observed the default stands, recorded "chosen, not observed".

---

## D-021 Maths type system and dimension checking

**Context.** Contradiction (solo-dev 15, power-user 10, architect 11): warnings only (core, parity, perf) vs ecosystem-first's "`+ -` require equal dimensions … violations are diagnostics, never silent" that may reject i2-accepted expressions. Ecosystem-first's `LapScalar` lowering to a Hold series answers ungated `stat_*`.

**Decision.** `Type { Kind {Scalar, Series, LapScalar, Bool, Text, RangeRef, Invalid}; UnitId unit; Dimension dim; std::optional<Rate> rate; bool integer; }`. The checker infers bottom-up (`m/s × s → m`; relational/boolean yield `Bool` 0/1 series; `choose` unifies branches; text only in `==`/`!=`; bitwise require integer). **Dimension or unit mismatches are warnings** shown live under the editor and in Channel Status; they never block evaluation (i2 only syntax-checks, and users' existing maths rely on "numbers are numbers once annotated"). The declared result unit is stamped on the output; display unit is separate. `LapScalar` values (ungated `stat_*`, numeric details per range) lower to a `Previous`-interpolated series over the ranges. Plugin functions declare unit rules in their manifest so they participate in inference.

**Consequences.** No rejected i2 expressions; a strict mode (errors) is a later option behind a project flag.

**User must confirm:** no.

---

## D-022 Maths evaluation, caching and invalidation

**Context.** Research: block-wise vectorised evaluator over SoA buffers, stateful intrinsics, DAG with version-based invalidation, lazy cached evaluation, thread pool. Contradictions: block size 4,096 (core, perf) vs 64 k (parity, eco; 512 KB `double` operands spill L2); `double` blocks (parity) vs F32 with F64 accumulators; zero-phase IIR cannot run in a one-pass halo scheme (architect on core-first). Parity-first's `SelectionDependent` cache key for ungated `stat_*`/`variance_*`; performance-first's kernel classes; i2 order-of-calculation rules.

**Decision.** The typed AST compiles to a `Plan` of `Kernel`s over **4,096-sample blocks** (`BlockCtx { first, n, t0, rate, ranges, mainDist, refDist, temps }`). Kernel classes: Elementwise (fused subtree, `chunkIndependent`, halo 0), Windowed (`smooth`, `derivative`, Filter item; halo = n/2; odd windows centred, even biased one earlier, seconds → odd count at the result rate), IIR (`filter_lp/hp`, `filter_cheby_*`; forward orders chunk-independent with state carry, negative orders = forward+backward which **forces whole-extent evaluation**), Stateful (`integrate`, `integrate_dist`, `stat_*`, running min/max, `flip_flop`, `edge_delay`, `time_valid`, `time_shift`, `pulse_width`, `previous_sample`; sequential, whole extent, one worker), Range (`range_is`, `range_change`), Variance (D-023), Table (2D/3D, Linear/Next/Previous, out-of-range clamp with an "invalid" option). Precision: F32 buffers by default, F64 for integrators, distance and any F64 input; per-module F64 option (i2's "maths precision"). Inputs are `ResampleView`s at the result rate (explicit Hz or Auto = highest input rate) using each channel's `Interp`. `PlanProvider : IChunkProvider` writes results as chunks into the shared `ChunkCache` (D-011) so maths outputs are plotted, pyramided and evicted like logged data. `MathGraph` (one per `(DataSource, layer)`): node version = hash(definition hash, enabled, upstream versions, source version); cache key `(ChannelKey, version[, selectionKey])` where `selectionKey` is added for `SelectionDependent` nodes (ungated `stat_*`, `variance_*`); editing an item invalidates only its cone. Resolution follows i2: local (sidecar) maths before global, modules in project order, first definition in a module wins, self-maths (Filter/Scale&Offset with output name = input) shadows the channel for consumers but reads the unshadowed input; cycles → status `Error: cycle A → B → A`. Every maths channel starts **Pending** and is evaluated on demand (component, report, Values row, export); Values-window scrolling queues at `Background` priority; the GUI thread never evaluates. Per-module detail-based enable condition (`"condition": "'Vehicle Id' == \"GT3-07\""`) is honoured, including for constants.

**Consequences.** Chunk-granular reuse, L1-resident blocks, correct zero-phase filters, and the Channel Status window has a precise state for every item.

**Amended by (A.9, proposed):** D-139 (0.1 whole-node staging) and D-140 (node versions hash only what a node reads).

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

**User must confirm:** no (converter licence/provenance is an open question, Q-15; since D-144 it also gates any verbatim reuse of the converter's `.ld` code in M0).

---

## D-029 Text engine, profiles and dictionaries

**Context.** Research: a generic CSV/TSV engine with declarative profiles (encoding, delimiter, decimal comma, preamble regexes, header/units rows, `Name (unit)`/`Name[unit]`, time column, sentinels, duplicate-name policy, sparse cells → timestamped) makes dozens of ECU/app dialects data. Contradiction (solo-dev 17, architect 26): `fast-cpp-csv-parser` (core, parity, eco) vs own column-oriented SIMD engine with DCF (perf). MoTeC CSV dialect: 11 quoted metadata lines, blank rows, names row, units row, single rate; Pi versioned ASCII has per-channel blocks.

**Decision.** Own **light** two-pass text engine (pass 1: line index + preamble/header/units detection against the profile; pass 2: per-column decode of 65,536-row slabs into chunks) using `fast_float` for number conversion, no SIMD, no DCF in the MVP (added only on a measured complaint, D-011). Header-only CSV libraries are not used because preambles, per-channel blocks (Pi), sparse cells and ragged rows are outside their model. Profiles are JSON (`schemas/importer-profile-1`): `sniff {preambleRegex, extensions, confidence}`, `encoding`, `delimiter`, `decimal`, `quote`, `preamble {lines, capture → details}`, `header {namesRow, unitsRow, unitInName}`, `time {column, unit}`, `sentinels`, `sparse: timestamped | hold`, `duplicateNames`, `columns {vendor → {name, quantity, dtype, interp}}`, `lapSource {column, kind: lap_counter_edge | beacon_event | lap_table}` (ecosystem-first graft). Shipped in MVP: `motec.csv` (details from the preamble, lap times to markers), `generic.csv`. v1.0: Pi, EFI, RacePak ASCII, VBO, TrackAddict, RaceChrono, RaceBox, MSL, RaceCapture, Haltech/Link/MaxxECU/Syvecs exports (each = profile + fixture).

**Consequences.** Adding a CSV dialect is a data PR with one fixture; the engine is ~1,500 lines; the chunk provider path means a later DCF cache needs no importer change.

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
