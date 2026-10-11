# Clean-room record

This file is the written answer to "where did this come from?" for every format and every behaviour of Esses
that relates to MoTeC i2 or its file formats (D-004, D-128; 08_REPO_AND_BUILD.md section 11).

## Rules

1. No disassembly or decompilation of MoTeC binaries or files, ever.
2. i2 behaviour comes only from: the help pages as displayed by the shipped help viewer, shipped XML or
   configuration files, SDK samples, public sources, and black-box observation on the user's licensed install.
3. No MoTeC code, art, icons, help text, sample files, templates, schemes or trademarks are committed or
   shipped. Vendor marks appear only nominatively (`TRADEMARK.md`).
4. AI-provenance rule (from `CONTRIBUTING.md`): never paste i2 help text, i2 shipped files, vendor XML or GPL
   source into a prompt that generates committed code, documentation, keymap `doc` strings or tooltips;
   describe behaviour in your own words first.
5. The clean-room working material (`research/`, `help_text/`, `conv_samples/`, vendor files, the design
   panel's background notes) is kept outside the repository; `.gitignore` and the `lint` job enforce it.
6. GPL reference projects are read for understanding only; nothing is ported from them.
7. Ported MIT/BSD code keeps its upstream copyright line and is listed under "Ported code" below.
8. The `.ld` "pro logging" field (the u32 at header offset 0x5DE) is never computed, derived, validated or
   written with a non-zero value; the reader records only whether it is zero, the writer always writes 0,
   and no Esses file records a value or how one is derived. Working around a vendor licence check is out of
   scope (D-143).
9. Code from the user's Log File Converter is not pasted or ported verbatim until the user confirms in
   writing that it is their own MIT work, not derived from gotzl/ldparser or another GPL source (D-144). Until
   then the `.ld` reader and writer are new code written from `docs/formats/ld-converter-port.md`,
   `docs/formats/ld.md` and the public MIT notes, and `tools/ld_ref_decode.py` is never changed to agree
   with them (D-041).

## Field of use and the user's answers (2026-10-10)

- **i2 field of use (D-038).** The i2 licence permits use only with data from MoTeC logging devices or
  MoTeC-licensed software. i2 is therefore run only on files from MoTeC logging devices, and never on files
  Esses wrote; exports are verified by re-import into Esses and by the public MIT decoders instead.
- **ACC files.** The user answered on 2026-10-10 that ACC (Assetto Corsa Competizione) exports are not
  MoTeC-licensed, so i2 is never run on ACC data, not even as an oracle. ACC exports are ordinary third-party
  fixtures and stay private until the game's terms have been reviewed (see the third table).
- **D-005 answers (Q-03), all "yes" on 2026-10-10.** (1) i2 CSV/report exports of the user's MoTeC-device logs
  may serve as private test oracles, kept in the private fixture checkout. (2) The research-phase use of files
  written by the user's i2 install (below) stands. (3) i2's behaviour may be observed on the user's licensed
  install, black-box only, never by disassembly; whether such observation counts as reverse engineering under
  the EULA is the user's own legal check, which is not recorded here as performed.
- **Vendor sample files (D-117, Q-06).** The importer samples shipped with the user's i2 Pro install may be used
  for local/private testing, as test inputs and oracles. They are never committed or redistributed.
  The two sample `.ldx` files under `C:\MoTeC\Logged Data\Samples\` and the four T2 test-model `.ld` files
  under `Documents\MoTeC\T2\Test Model\` count as vendor sample files (D-146); nothing derived from their
  content leaves `fixtures/private`.

## Provenance entries

Categories (D-128 as amended by D-145): help page; public release notes; vendor-shipped template; file written by
the user's install (with the D-005 (2) consent date); observed on a licensed install, D-005 (3); user-contributed
own work; user-owned device logs; and, as rows below already use them, public project (with its licence),
public standard, own design and rule.

| Area | Behaviour | Category | Source | Date |
|---|---|---|---|---|
| Research | Configuration-model research (workspaces, maths, scheme and settings files) | File written by the user's install; D-005 (2) consent 2026-10-10 | The user's own i2-written files; notes in `background/research/config_model.md` (private, never in the repository) | 2026-10-09 |
| Process | i2 exports as private oracles for MoTeC-device logs only | Observed on a licensed install, D-005 (1)/(3) | The user's answers, Q-03 | 2026-10-10 |
| Process | i2 never run on ACC data or Esses output | Field-of-use rule, D-038 | The installed EULA; the user's answer on ACC | 2026-10-10 |
| Process | Vendor sample files used locally only | Vendor-shipped files, D-117, D-146 | The user's answer, Q-06; the T2 test models and the two sample `.ldx` files count as samples (D-146) | 2026-10-10 |
| Process | `.ld` pro-logging field excluded: never computed, derived, validated or written non-zero; the reader records only zero or non-zero; no value or derivation recorded anywhere | Rule, D-143 | The converter's routine for it (port specification §1.13, Q-W36) is not ported or restated | 2026-10-10 |
| Process | Reuse of the user's converter code on hold until the user confirms it is their own MIT work with no GPL lineage | Rule, D-144 | `docs/formats/ld-converter-port.md` §0 and §8 item 1; Q-15 | 2026-10-10 |
| Process | Vendor-software CSV exports used as local decode oracles in spike S-1: five distinct exports the user made earlier, three of them of one C125 logger file (6,720 cells) and two of converter outputs; 53,431 cells agree within print precision; never committed | Observed on a licensed install, D-005 (1), D-038 | `docs/spikes/S-1.md` §8. The two exports of converter output are not MoTeC-device data, so their use as oracles lies outside D-005 (1) and awaits the user's confirmation; no S-1 conclusion rests on them alone | 2026-10-10 |
| Formats | `.ld` reader and `.ld` part of the fixture writer (`motec.ld`) | User-contributed own work (D-142, D-145), written as new code (D-144); checked against public layout notes and `tools/ld_ref_decode.py` | Written from `docs/formats/ld-converter-port.md` (the specification of the user's Log File Converter `.ld` reader and writer), `docs/formats/ld.md` and the public MIT sources (i3rs `docs/ld-file-format.md`, motec-i2, motorsport-telemetry-rs); no converter code pasted while D-144 holds; gotzl/ldparser never a source. Landed in M0 phase B as importer `motec.ld` 0.1.0: `libs/formats/src/ld/{ld_file,ld_decode,ld_strings,ld_codes,ld_chunk_provider,ld_writer}.cpp` and `libs/formats/src/importer/{ld_importer,ld_details,ld_export}.cpp`, new code checked against `tools/ld_ref_decode.py` (D-041) on the 49 synthetic families and the user's 87 files; the M0 review fixes of 2026-10-11 (`docs/milestones/M0.md`) are new code from the same sources | 2026-10-11 |
| Formats | `.ld` layouts beyond the public notes: header slots 0x040, 0x054, 0x11E and 0x687 to 0x694; event, venue, vehicle and weather record sizes and fields; record tail (raw max/min, display range, display decimals, interpolation byte); gap block (validity `[entry, exit)`); aux block (long name, enumeration reference); enumeration block with severities; log-event block; scaling as implemented; beacon-channel encoding | User-contributed own work, D-145 (licence and lineage to be confirmed, D-144) | The user's Log File Converter (Qt C++ 9.1.0.23 `LdWriter`/`LdParser`, Python `ld_parser.py`/`write_ld_file`, the author's layout notes v5), restated as facts in `docs/formats/ld-converter-port.md` §1 to §2; no converter code copied. Per that specification the author derived the layout by analysing log files, with no disassembly or decompilation of any MoTeC software; the author is to confirm this | 2026-10-10 |
| Formats | `.ld` layout facts confirmed on user-owned files: u16 channel count, u32 detail pointers, fixed detail-record sizes, variable per-record extension data (gap and aux blocks, offsets at record 0x74/0x78), M1 terminator record, text at header 0x11E, counter semantics, `dtype_a` 0x06 integers, F64 only on GPS, `DD/MM/YYYY` dates, gap semantics `[entry, exit)`, unit placement, display-decimals byte, log-event block | User-owned device logs, D-145 | Spike S-1 (`tools/ld_ref_decode.py` over 87 files, `docs/spikes/S-1.md` §3) and the port specification's byte inspection of the user's M1 and C125 logger files and converter outputs (facts E-1 to E-29); files private, never committed; no vendor software inspected | 2026-10-10 |
| Formats | Logger device-configuration region (between the header and the event record of logger files) | User-owned device logs, D-145 | Recorded by extent only; never parsed, copied or committed (spike S-1) | 2026-10-10 |
| Formats | `.ld` cross-check of the converter's layout against the public notes; differences in `docs/formats/ld-converter-port.md` §6.1 | Public project, MIT | Friss/i3rs `docs/ld-file-format.md` | 2026-10-10 |
| Formats | Meaning of `.ld` device-event codes 0 to 5 | Observed on a licensed install, D-005 (3) | The converter author's observation of the analysis software's event display (port specification §1.5). Until 2026-10-11 the Esses labels repeated the converter's wording (which follows that display) up to letter case; they are now Esses' own: Firmware start, Firmware stop, Power cycled, Logging on, Logging off, Log discontinuity (`deviceEventLabel`, `model::deviceEventName`) | 2026-10-11 |
| Formats | `.ldx` structure | User files; public projects | `libs/formats/src/ld/ldx.cpp` (parser and writer over pugixml) and `libs/formats/src/importer/ldx_companion.cpp` (merge into esses_model), new code written from `docs/formats/ld.md` §`.ldx` companion: the element and attribute structure from i3rs (`ldx_parser.rs`), motorsport-telemetry-rs (`ldx.rs`) and motec-i2 (MIT); marker `Time` in microseconds per [tobi]; the 42 `.ldx` files of the user's machine (S-1: details only, no markers). sim-to-motec and SARUSySLab consulted, unlicensed, nothing copied. Fixtures: `synthetic-ldx-companion`, `synthetic-acc-dialect` | 2026-10-11 |
| Formats | `.ld` shift-versus-multiplier order: `afterMul` (D-037 as amended; public projects disagree) | User-owned device logs, D-145 | Physical evidence on user-owned logger data, spike S-1 (`docs/spikes/S-1.md` §7): `20251009-1552000.ld` (C185) channel `Gbox Oil Temp` and, corroborating, `20260420-0120200.ld` (C127) channel `Ambient Air Temp`, both `shift 32, mul 9, scale 5, dec 1` (a °C to °F conversion that only afterMul expresses). A vendor-software CSV export of either file would add confirmation and is recorded here when made. No internals inspected. Fixtures: `synthetic-shift-mul-after`, `synthetic-shift-mul-before` (public); `s1-27-20251009-1552000`, `s1-30-20260420-0120200` (private) | 2026-10-10 |
| Formats | `.ld` dialect heuristics (`m1`, `c-series`, `pc-layout`, `converter-legacy`, `sim-exporter`, `pro-logging`) and the C-series record rules | User-owned device logs and tool outputs, D-145; public projects (MIT) for the writer defaults | Rules, status and fixture ids in `docs/formats/ld.md` "Dialect heuristics". Settled by real files: `m1` (private `s1-62-…`, `s1-70-…`), `c-series` and its record rules keyed on the logger layout (private `s1-08-…` with `s1-09-…_pro`, `s1-28-…_pro`), `pc-layout` (private `s1-27-…`, `s1-83-…`), `converter-legacy` (private `s1-24-example-1`, `s1-72-testdrive`). **Unsettled** (synthetic only): `sim-exporter` from the [gotzl] and [mi2] writer defaults (`synthetic-sim-gotzl`, `synthetic-sim-mi2`, `synthetic-acc-dialect`); since 2026-10-11 the [mi2] u32 at 0x05A counts only with the writer's 0 at 0x040 outside the logger layout, because PC-software files carry it too. `pro-logging` is a recorded flag, not a producer claim (D-143) | 2026-10-11 |
| Units | Unit table and alias spellings (`data/units.json`, `esses_units`) | Own table (D-141); public standard | Quantities, dimensions and factors from the SI Brochure (9th edition) and NIST SP 811 (2008, appendix B), plus standard automotive units (km/h, mph, rpm, psi, bar, lambda); sized by need. Alias spellings from the design set (02 §2.3: `kPa a`, `psi g`, `dKPA`, `P A/F`, `1000rpm`, `%Trim`, `dBTDC`, `LA`, `øC`), from the unit fields of the user-owned S-1 corpus (device logs and converter outputs, D-145; private, never committed) and common usage. i2 `units.xml` was not consulted for this implementation and no table was copied; legacy-only units are defined from their physical meaning and flagged `"compat": "legacy"` | 2026-10-11 |
| Public projects | GeekyDeaks/sim-to-motec | Public project, unlicensed | Read only, unlicensed, no code or template copied (D-040) | 2026-10-09 |
| Public projects | gotzl/ldparser | Public project, GPL-3.0 | Read for understanding only; nothing ported | 2026-10-09 |
| Tools | `tools/ld_ref_decode.py`, the independent `.ld` reference decoder (D-041) | Public project, MIT | Written 2026-10-10 from public layout notes: Friss/i3rs `docs/ld-file-format.md` and `crates/i3rs-core` (MIT, primary), afonso360/motec-i2 (MIT), tobi/motorsport-telemetry-rs `motec-telemetry` (MIT). gotzl/ldparser (GPL-3) read for understanding only; no code and no struct format strings reproduced. Written before and independently of `esses_formats` and of the user's converter; never changed to agree with the reader | 2026-10-10 |
| Tools | Spike S-1 comparison with the user's converter (`tools/spikes/s1/compare_converter.py`) | User-contributed own work, D-145 | The converter's Python `.ld` parser was imported through its API and only its outputs compared; no converter code was read for or copied into `tools/` by the spike. The port specification was written separately from a reading of the converter's `.ld` sources (D-142) and is the only channel by which converter knowledge reaches Esses code (D-144) | 2026-10-10 |
| `esses_base` | `Rate` arithmetic, `TaskPool`, `Result`, `Hash128`, `MappedFile`, `Paths` | Own design from the design set (D-008, D-129, D-031, D-123, 01 section 6) | No i2-derived behaviour | 2026-10-10 |
| `esses_base` | Case-folding and compatibility tables in `caseFold` | Public standard | Unicode Character Database (`CaseFolding.txt`, `UnicodeData.txt`) character properties; no file copied | 2026-10-10 |
| `esses_base` | XXH3-128 test vectors | Public project, BSD-2-Clause | Empty-input value from the xxHash sanity tests; short-input values re-derived independently from the published algorithm | 2026-10-10 |

## Ported code

No file is ported from the user's converter while D-144 holds: the `.ld` reader and writer are new code. The two
rows below become real only if the user confirms the converter's licence and lineage and its code is then reused
verbatim; each ported file then gets its own row (`docs/formats/ld-converter-port.md` §7 lists the candidates).

| File | Upstream project | URL | Commit or date | Licence | Upstream copyright line |
|---|---|---|---|---|---|
| (on hold, D-144) `libs/formats/src/ld/...` reader | The user's Log File Converter, `src/parsers/LdParser.cpp` (mirrors the earlier Python `parsers/ld_parser.py`) | private | to be recorded | MIT as stated in the task text, not yet confirmed by the user (D-142, D-144) | to be recorded |
| (on hold, D-144) `libs/formats/src/ld/...` writer | The user's Log File Converter, `src/formats/LdWriter.cpp` (mirrors the Python `ConversionWorker.write_ld_file`) | private | to be recorded | MIT as stated in the task text, not yet confirmed by the user (D-142, D-144) | to be recorded |

## Third-party file licences reviewed for fixtures

| Files | Producer | Conclusion | Date |
|---|---|---|---|
| ACC telemetry exports (`.ld`/`.ldx`) | Kunos Simulazioni (game) | Not reviewed yet. Private only (`fixtures/private`); public CI uses the `synthetic-acc-dialect` family from our own writer (D-127). Not MoTeC-licensed (user, 2026-10-10), so never processed by i2. No ACC file was found on the development machine (spike S-1) | 2026-10-10 |
| sim-to-motec output | GeekyDeaks/sim-to-motec | No licence: private only (D-040) | 2026-10-09 |
| i2 Pro importer samples; the two sample `.ldx` files under `C:\MoTeC\Logged Data\Samples\`; the four T2 test-model `.ld` files under `Documents\MoTeC\T2\Test Model\` | MoTeC (vendor-produced) | Local/private testing only, never committed; nothing derived from their content leaves `fixtures/private` (D-117, D-146). The install's `Samples` folder holds only Conversion Wizard samples, no `.ld` files | 2026-10-10 |
| `.ld` files written by the user's Log File Converter | The user (own tool) | User-owned files, private and never committed (Q-04). Valid round-trip inputs for the `pc-layout` dialect; never oracles for dialect claims about logger files (D-041) | 2026-10-10 |
