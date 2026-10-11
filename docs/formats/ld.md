# `.ld` and `.ldx` format notes (draft)

Status: draft from spike S-1 (M0 week 1, 2026-10-10). Owner of the format decisions: `05_IMPORTERS_AND_PLUGINS.md` §3–§5. This page restates the public layout notes in our own words and tables, adds what S-1 observed on the user's own files, and lists every warning code the reference decoder emits. Since the S-1 reconciliation (2026-10-10) it also records the layout facts of the port specification that 05 §3 adopted, marked **[port]**; the reference decoder does not use them (D-041). Nothing here is copied from vendor software, vendor files or the GPL-3 project; no struct format strings are reproduced.

Esses reads MoTeC `.ld` log files and their `.ldx` companions. The importer id is `motec.ld`.

## Sources

| Key | Source | Licence | Used for |
|---|---|---|---|
| [i3rs] | Friss/i3rs, `docs/ld-file-format.md` and `crates/i3rs-core/src/{ld_parser.rs, ldx_parser.rs}` — <https://github.com/Friss/i3rs> | MIT | primary layout notes, 212-byte records, enum tables, u32 block pointers, scaling order |
| [mi2] | afonso360/motec-i2, `src/{reader.rs, structs.rs, writer.rs}` — <https://github.com/afonso360/motec-i2> | MIT | header field order (session at 0x5E4), dtype pairs, writer constants |
| [tobi] | tobi/motorsport-telemetry-rs, `crates/motec-telemetry/src/{lib.rs, ldx.rs}` — <https://github.com/tobi/motorsport-telemetry-rs> | MIT | bounds and zero-factor handling, `.ldx` marker time unit, writer defaults |
| [gotzl] | gotzl/ldparser, `ldparser.py` — <https://github.com/gotzl/ldparser> | GPL-3 | read for understanding only; nothing reproduced |
| [S-1] | `docs/spikes/S-1.md`: 87 `.ld` files (81 distinct) and 42 `.ldx` files on the user's machine, decoded by `tools/ld_ref_decode.py` | user-owned data, never committed | observations marked **S-1** |
| [port] | `docs/formats/ld-converter-port.md`: specification of the `.ld` reader and writer of the user's Log File Converter, with facts E-1 to E-29 checked on user-owned files | user-contributed own work, licence and lineage to be confirmed (D-144); facts only, no code | record tail, gap, aux, enumeration and log-event blocks, detail-record sizes, unit placement; marked **[port]** |

## Conventions

- All integers little-endian. Offsets are absolute file offsets unless a table says "within the block/record".
- Strings are fixed-width fields, NUL-padded and bounded by the field width (a full field has no NUL). Decode (05 §3, amended 2026-10-10): cut at the first NUL, decode as UTF-8 when the bytes are valid UTF-8 and as cp1252 otherwise, right-trim spaces; the Esses reader records the encoding used (`ld.string_encoding`). The reference decoder decodes cp1252 and flags bytes ≥ 0x80 (`ld.string_non_ascii`), so the two can differ on non-ASCII text. **S-1**: some writers leave non-zero bytes after the NUL (stale buffer); one file had a non-ASCII byte in a channel name. **[port]**: the user's converter writes UTF-8. A full field (no NUL) whose only UTF-8 fault is an incomplete sequence in its last 1–3 bytes was cut by a byte-truncating writer (port specification Q-W19/Q-W20); the Esses reader decodes it as UTF-8 without the partial tail when the field shows itself as UTF-8 (a multi-byte sequence before the tail, or a tail of 2–3 bytes). A lone lead byte after pure ASCII stays cp1252 (an accented letter at the end of a full cp1252 name).
- Pointers are u32 file offsets; 0 means "absent".

## File header (0x000–0x6E1, 1,762 bytes)

| Offset | Type | Field | Sources | S-1 observations |
|---|---|---|---|---|
| 0x000 | u32 | magic, 0x40 | [i3rs] [mi2] [tobi] | 0x40 in all 87 files |
| 0x004 | u32 | unknown | [mi2] writes 0 | 0 everywhere |
| 0x008 | u32 | `meta_ptr`, first channel record | all | one converter test file has `meta_ptr == data_ptr == size` (no channels) |
| 0x00C | u32 | `data_ptr`, first sample array | all | equals the lowest channel data pointer in every multi-channel file |
| 0x010 | 20 B | unknown | [mi2] | zero everywhere |
| 0x024 | u32 | `event_ptr` | all | device files place the event block after a large device-configuration region (≈1.4 MB on C125, ≈3 MB on M1); PC-software-written files put it directly after the header (0x6E2) |
| 0x028 | 24 B | unknown | [mi2] | device files: a non-zero u32 at 0x02C (an offset into the file) |
| 0x040 | 3 × u16 | unknown constants | [i3rs] "statics"; [mi2] writes 0, 0x4240, 0x000F | first u16 is 0 on C125/C127 logger files, 2 on M1 logger files and on PC-software-written files; then 0x4240, 0x000F everywhere, which is one u32 of 1,000,000 (**[port]**: the header reads u16 + u32 here, 05 §3.2) |
| 0x046 | u32 | device serial | all | |
| 0x04A | 8 B | device type | all | `C125`, `C127`, `C185`, `M1` on logger files; `MTCA2LD` on PC-software-written files |
| 0x052 | u16 | device version | all | |
| 0x054 | u16 | unknown | [mi2] writes 0x0080 | 0 on files as downloaded; 0x0080 on PC-software-written files and on header-modified copies |
| 0x056 | 2 × u16 | channel count, then the count again or 0 (05 §3.2) | all; [i3rs] "unreliable" | **read it as two u16**: the low u16 equals the walked count (counting an M1 terminator record) in all 86 files with channels; the high u16 is 0 on C125/C127 logger files and repeats the count on M1 logger files and on every PC-software file |
| 0x05A | u32 | unknown | [mi2] writes 0x00010064 | varies (0x00010001, 0x000A000A, 0x000101F4, …) |
| 0x05E | 16 B | date `DD/MM/YYYY` | all | parsed in all files |
| 0x06E | 16 B | unknown | | zero |
| 0x07E | 16 B | time `HH:MM:SS` | all | |
| 0x08E | 16 B | unknown | | zero |
| 0x09E | 64 B | driver | all | |
| 0x0DE | 64 B | vehicle id | all | |
| 0x11E | 64 B | unknown in the public notes | | **S-1**: a text field; holds the "Engine ID" detail text on logger and PC files |
| 0x15E | 64 B | venue | all | |
| 0x19E | 64 B | unknown | | zero |
| 0x1DE | 1024 B | unknown | | **S-1**: free text on one logger-derived file (configuration notes); zero elsewhere |
| 0x5DE | u32 | "pro logging" | [i3rs] [mi2] [gotzl] | non-zero in 84 of 87 files. The Esses reader records only zero or non-zero; the field is never computed, derived, validated or interpreted, and the Esses writer always writes 0 (D-143) |
| 0x5E2 | u16 | unknown | [mi2] | zero |
| 0x5E4 | 64 B | session | [mi2] | holds the session text |
| 0x624 | 64 B | short comment | all | |
| 0x664 | 126 B | unknown | | **S-1**: non-zero on most files: further u32 offsets into the file and a short text field near 0x694. **[port]**: u8 at 0x687 (unknown), u32 pointers at 0x688 (log-event block), 0x68C (unknown block) and 0x690 (enumeration block), team text of 32 B at 0x694 (05 §3.2, §3.7, §3.9) |

## Event, venue and vehicle blocks

| Block | At | Layout (offsets within the block) | Notes |
|---|---|---|---|
| Event | `event_ptr` | name 64 B @0x000; session 64 B @0x040; long comment 1024 B @0x080; venue pointer @0x480; **[port]**: weather pointer u32 @0x484 | **[port]**: 3,156 B |
| Venue | venue pointer | name 64 B @0x000; vehicle pointer @0x44A; **[port]**: length u32 mm @0x042, category 32 B @0x44E | **[port]**: 3,102 B; other bytes unknown |
| Vehicle | vehicle pointer | id 64 B @0x000; [i3rs]: weight u32 @0x0C0, type 32 B @0x0C4, comment 32 B @0x0E4. **[port]**: description 64 B @0x040, engine id 64 B @0x080, weight u16 kg @0x0C0, fuel tank u16 L × 10 @0x0C2, type 32 B @0x0C4, drive type 32 B @0x0E4, diff ratio, gear ratios, track and wheelbase as u16 @0x104 to @0x11C, comment 1024 B @0x120, number 32 B @0x524 | **[port]**: 3,316 B. 05 §3.3 follows [port]: on an M1 file 0x0C0/0x0C2 hold u16 279 and u16 76, which a u32 reading cannot explain |
| Weather | weather pointer | **[port]**: sky 64 B @0x000, five value/unit pairs (16 B + 8 B) from @0x040, wind direction 64 B @0x0B8, comment 1024 B @0x0F8 | **[port]**: 2,048 B; present on M1 files |

Pointer width: [gotzl] and [mi2] read the venue and vehicle pointers as u16, [i3rs] as u32. **S-1 settles this as u32**: on every logger file the event block sits above 1.4 MB, so the venue and vehicle pointers exceed 16 bits (code `ld.ptr_width_ambiguous` fired on 35 files), and the u32 reading lands on the venue and vehicle text in all of them. PC-software files often have no venue/vehicle blocks (pointer 0; `ld.block_out_of_range`, Info).

## Channel records (linked list)

The list starts at `meta_ptr` and follows `next_ptr` until 0. Base record, 124 bytes ([mi2] [tobi] [gotzl]):

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0x00 | u32 | `prev_ptr` | 0 on the first record; consistency only |
| 0x04 | u32 | `next_ptr` | 0 ends the list |
| 0x08 | u32 | `data_ptr` | sample array |
| 0x0C | u32 | sample count | samples, not bytes |
| 0x10 | u16 | "counter" | [gotzl] writes 0x2EE1 + index. **S-1**: logger files carry what look like channel identifiers here (e.g. 23003, 1450, 9737); PC-software files carry 20101 + index (0x4E85 + i). No file in the corpus uses 0x2EE1 + i |
| 0x12 | u16 | `dtype_a` | type class |
| 0x14 | u16 | `dtype` | byte width |
| 0x16 | u16 | rate | Hz, integer |
| 0x18 | i16 | shift | scaling |
| 0x1A | i16 | mul | scaling |
| 0x1C | i16 | scale | scaling, 0 means 1 |
| 0x1E | i16 | dec | decimal places |
| 0x20 | 32 B | name | not always NUL-terminated |
| 0x40 | 8 B | short name | logger and M1 files often put the **unit** here and leave the unit field empty; where the unit sits per producer: 05 §3.4 unit rule (**[port]** R5) |
| 0x48 | 12 B | unit | |
| 0x54 | 40 B | record tail | not decoded by the reference decoder. **[port]**: i32 raw maximum @0x54, i32 raw minimum @0x58, f32 display range @0x5C/@0x60, u8 display decimals @0x64, u8 interpolation @0x65, u32 gap block pointer @0x74, u32 aux block pointer @0x78 (05 §3.4) |

**S-1 findings on the record layout:**

1. Records are **not** a fixed size. On PC-software and M1 files the distance between consecutive records varies from 124 to about 2,500 bytes (`ld.entry_stride_irregular`, 31 files): each record may be followed by per-channel extension data. Two u32 values in the 40-byte tail (at 0x74 and 0x78 within the record) hold offsets to that extension data when present (often `record + 0x7C`). Its content (validity/gap ranges and enum or label data, judging from the bytes) is undecoded by the reference decoder; the reader must walk by `next_ptr` only and never assume a stride. **[port]** decodes it: the pointer at 0x74 names a gap block (invalid sample ranges `[entry, exit)`, which the Esses reader turns into validity) and the pointer at 0x78 an aux block (long name and enumeration reference); layouts in 05 §3.4.
2. [i3rs]'s 212-byte record (enum type u16 @0xD0, enum id u16 @0xD2) is one case of this. S-1 saw exactly-212-byte gaps only in some PC-software files; the reference decoder reads the two enum fields only when the gap is exactly 212 bytes. **[port]**: such a gap is a record followed by an aux block that holds only an enumeration reference (124 + 88 bytes); i3rs's two fields are that reference's index size (always 2) and table index.
3. Two floats at 0x5C and 0x60 of the tail look like a display range (e.g. 0/100, −10/10) on PC-software and M1 files; not interpreted by the reference decoder. **[port]** confirms the display range (05 §3.4).
4. M1 files end the list with an all-zero record (empty name, count 0, rate 0, dtype 0/0), as [i3rs] says. The reference decoder drops it with `ld.terminator_entry`; the header count includes it.
5. The [i3rs] enum-table signature scan (u16 count 1–500, u16 type 2, u16 id, entries starting `03 00`) finds tables in 17 files, M1 and PC-software alike (up to 405 per file). Linking a table to a channel needs the extension data of point 1, so the reference decoder does not link it. **[port]**: the tables also sit in the enumeration block named by the header pointer at 0x690, with a severity per value, and a channel links to one through its aux block (05 §3.9).

**Gap blocks (Esses reader).** Each `(0, s), (1, e)` pair marks samples `[s, e)` invalid (port specification §1.8, fact E-16). A reversed pair is read as `[exit, entry)` and logged `ld.gap_unsorted`; it is never dropped. The converter's Python writer wrote `(first invalid + 1, last invalid)` instead: one invalid sample gives a reversed pair `(s + 1, s)`, two give an empty pair `(s + 1, s + 1)`, and every normal pair misses its first and last invalid sample. The reader reports that shape (`ld.gap_convention_shifted`, Info) and reads the pairs as written; the importer option `{"ld": {"gapConvention": "shifted"}}` reads every pair as `[entry − 1, exit + 1)`. On `Reference Haltech CSV exported.ld` (private fixture `s1-65-reference-haltech-csv-exported`: 6,470 normal pairs, 24 reversed, 88 empty) every one of the 182,914 samples the shifted reading marks invalid is zero-filled, while the reading as written (reversed pairs included) leaves 13,116 of them valid. A gap block is parsed once however many records name it (`ld.gap_block_shared`), and per file the events parsed and the intervals materialised are each bounded by `size / 6`; a block beyond that budget is ignored with `ld.gap_block_malformed`. Aux blocks are read up to 16 arguments and at most `size` bytes per file (`ld.aux_out_of_range`). No file of the corpus shares a gap or aux block between records.

## Data types

| `dtype_a` | `dtype` | Decoded as | Sources | S-1 (files containing the pair) |
|---|---|---|---|---|
| 0x00, 0x03, 0x05 | 2 | i16 | all | 0x03/2 in 77, 0x00/2 in 34 |
| 0x00, 0x03, 0x05 | 4 | i32 | all | 0x05/4 in 42 |
| 0x07 | 4 | f32 | all | 47 |
| 0x07 | 2 | f16 | [i3rs] [gotzl]; [mi2] unimplemented; [tobi] rejects | not seen |
| 0x08 | 8 | f64 | [i3rs] [gotzl] [tobi] | 7 files, only GPS channels (latitude/longitude on M1; latitude and speed in one converter test file) |
| 0x06 | 4 | integer | [i3rs] includes 0x06 as integer; design notes say "state" | 9 files (M1): GPS date as DDMMYY, satellite count, warning counters: plain integers. The reference decoder decodes them signed with `ld.dtype_guessed`; 05 §3.5 now treats 0x06/4 as a plain signed integer without a guess flag |
| 0x04 | 1/2/4/8 | unsigned integer (guess, `ld.dtype_guessed`; Bitfield kind) | design notes | not seen. u8/u16 read 255/65535; the Esses reader stores u32 as F64 (exact), so 0xFFFFFFFF reads 4,294,967,295, not −1 |
| 0x00 | 0 | none | | only in the M1 terminator record |
| other | any | raw signed integer of that width, `ld.unknown_dtype` | | none outside the terminator |

**Storage dtype of the Esses reader** (05 §3.5, amended 2026-10-11 after the M0 review): an identity-scaled integer of up to 32 bits is stored I32, except u32, which is stored F64 like the 8-byte integers (F64 holds both exactly up to 2^53). A scaled integer is stored F64 when F32 would lose raw steps: when its effective number of decimals `dec + floor(log10 |scale|)` is 5 or more (lat/lon-like values; M1 files carry decimals in `scale = 10000` with `dec` 1–4, fact E-24), or when the record's raw range (i32 at 0x54/0x58) of a 32- or 64-bit integer exceeds 2^24; F32 otherwise. On the user's files every one of the 161 scaled channels (7 files) whose raw integers exceed 2^24 is now F64; 171 channels of 6 private fixtures changed from F32 to F64.

## Scaling

For a raw integer `r` with fields shift, mul, scale (0 → 1), dec:

- `base = r / scale × 10^−dec`
- **beforeMul** ([gotzl], also what [tobi] computes): `value = (base + shift) × mul`
- **afterMul** ([i3rs]): `value = base × mul + shift`

The two agree when `shift == 0` or `mul == 1`. [mi2] refuses `shift ≠ 0`. Zero `mul` is treated as 1 by [tobi]; zero `scale` as 1 by all. [tobi] skips scaling for float types, while [gotzl] and [i3rs] apply it; 05 §3.5 scales every dtype unless the scaling is identity. No float channel in the corpus has non-identity scaling.

**S-1 evidence: afterMul.** Two logger channels in the corpus have `shift ≠ 0 ∧ mul ≠ 1`, both `shift 32, mul 9, scale 5, dec 1`, unit °F, raw counts in 0.1 °C:

- `20251009-1552000.ld` (logger type C185, data re-written by PC software; channel `Gbox Oil Temp`): afterMul starts at 75.2 °F (24.0 °C), equal to the coolant (24.0 °C) and inlet air (25.3 °C) temperatures at the start of the same cold start, and ends at 168.8 °F (76 °C). beforeMul gives 331.2 °F (166 °C) at the cold start, which is not physical.
- `20260420-0120200.ld` (logger type C127, file as downloaded; channel `Ambient Air Temp`): raw is 0 throughout (sensor absent); afterMul gives 32 °F (0 °C), beforeMul 288 °F.

In both files the field set is the textbook °C→°F conversion `F = C × 9/5 + 32`, which only afterMul expresses. The user's converter also decodes both channels with afterMul (`docs/spikes/S-1.md`). A vendor-software CSV export of either file would confirm it (D-038 (a) allows that); none exists on the machine yet. On this physical evidence D-037 was amended on 2026-10-10 and now defaults to `afterMul`; the export would add confirmation and is recorded in `CLEANROOM.md` when made. The reference decoder keeps reporting both orders.

The reference decoder computes each value as the exact rational result rounded once to F64, so the oracle does not depend on an implementation's operation order: integer raws use integer arithmetic and one correctly rounded division.

## Time base

Uniform per channel: sample `i` is at `i / rate` seconds. In µs, `timeAt(i) = floor((2·i·10⁶ + rate) / (2·rate))` (02 §1 behaviour 1). Rates seen in S-1: 1–10,000 Hz (1, 2, 5, 9, 10, 19, 20, 25, 30, 40, 47, 50, 60, 61, 94, 100, 200, 250, 500, 1000, 2000, 5000, 10000). Rate 0 occurs only on the M1 terminator.

## Producer fingerprints (header evidence)

| Class | Evidence | S-1 files |
|---|---|---|
| Logger file as downloaded | event block after a device-configuration region (`event_ptr` > 0x10000); u16@0x054 = 0; logger device type (u16@0x040 is 0 on C125/C127, 2 on M1) | 19 (C125, C127, M1) |
| Logger file with rewritten header fields | same layout, but u16@0x054 = 0x0080 and the u32 at 0x5DE changed; some copies also replace the device type, serial and version. Where the original is on disk the copy is byte-identical outside 0x046–0x055 and 0x5DE–0x5E1, so samples are unaffected | 16 (the `_Pro` copies and one M1 `_03` file, whose original is not on disk) |
| PC-software layout, logger identity kept | event block at 0x6E2; u16@0x040 = 2; u16@0x054 = 0x0080; logger device type | 8 (C125, C185, M1) |
| PC-software layout, generic identity | as above with device type `MTCA2LD`, serial 50196, version 100 | 43: vendor T2 test models, and the user's converter outputs (the converter writes the same header constants, so header evidence cannot separate it from vendor PC software) |
| Public sim/converter writers | [gotzl] writer defaults (serial 0x1F44, `ADL`, version 420, u16@0x054 = 0xADB0) or [mi2] writer defaults (u32@0x05A = 0x00010064 **with** u16@0x040 = 0 outside the logger layout) | none on the machine; no simulator-native export was found. u32@0x05A = 0x00010064 alone is **not** a fingerprint: the vendor PC software and the converter write it too (the C185 file `20251009-1552000.ld`, private fixture `s1-27-20251009-1552000`, carries it with u16@0x040 = 2) |

No fingerprint uses the value of the u32 at 0x5DE, and this page lists none: the Esses reader records only whether it is zero (D-143). 05 §3.8 maps these classes onto the dialect flags `m1`, `c-series`, `pc-layout` and `sim-exporter`.

### Dialect heuristics: rule, status and the fixtures behind them

AC-M0-05: a claim is **settled** only when a real file or a third-party decoder backs it; a claim that only synthetic fixtures exercise is **unsettled**. Fixture ids name `fixtures/motec.ld/<id>` (public, synthetic) and `fixtures/private/motec.ld/<id>` (the user's files; private, never committed). Every heuristic decision also leaves an `ImportLog` code.

| Flag or rule | Esses reader rule | Status | Fixtures |
|---|---|---|---|
| `m1` | device type `M1` | settled (real M1 logger files and their PC re-saves) | `synthetic-m1-dialect`; private `s1-33-…`, `s1-62-m1-event-codes-0-15`, `s1-70-original-m1-file-unaltered`, `s1-80-…`, `s1-82-…` |
| `c-series` (label) | device type `C<digits>` and the logger layout (u16@0x040 = 0, event record beyond 0x10000) | settled (C125/C127 logger files) | `synthetic-c-series`; private `s1-00-20250531-047140000` … `s1-16-…` |
| C-series record rules (unit rule (b)/(c) with the registry check, interpolation byte 0 = unset) | the logger layout alone, whatever the device type (header-modified copies keep the logger's records) | settled (the `_Pro` copies against their originals; a PC-software re-save, private `s1-22-testing2334tr2t3`, has an empty unit field where rule (a) used to read `Ct1`/`Ct2` as units) | `synthetic-c-series`; private `s1-08-…` with `s1-09-…_pro`, `s1-28-20251108-2137903_pro`, `s1-22-testing2334tr2t3` |
| `pc-layout` | event record at 0x6E2 and u16@0x054 = 0x0080 | settled (PC-software files, vendor T2 test models) | `synthetic-multirate` and most families; private `s1-27-20251009-1552000`, `s1-83-t2-1khz-test` … `s1-86-…` |
| `converter-legacy` | list at 0x1336, records longer than 124 bytes with neither gap nor aux pointer | settled (converter outputs) | `synthetic-converter-legacy`; private `s1-24-example-1`, `s1-72-testdrive` |
| `sim-exporter` | [gotzl] or [mi2] writer defaults (table above) | **unsettled**: no real or third-party file; synthetic only | `synthetic-sim-gotzl`, `synthetic-sim-mi2`, `synthetic-acc-dialect` |
| `pro-logging` | u32@0x5DE non-zero (recorded only, D-143) | not a producer claim | most private fixtures |
| shift order `afterMul` (default, D-037 as amended) | `value = raw / scale × 10^−dec × mul + shift`; option `ld.shiftOrder` | settled by real logger data (§Scaling) | `synthetic-shift-mul-after`, `synthetic-shift-mul-before`; private `s1-27-20251009-1552000` (`Gbox Oil Temp`), `s1-30-20260420-0120200` (`Ambient Air Temp`) |
| gap pairs `[entry, exit)` | port specification §1.8, fact E-16; option `ld.gapConvention` for the Python writer's shifted pairs | settled for M1 logger files and the C++ converter; the shifted convention is settled on `s1-65-reference-haltech-csv-exported` | `synthetic-gap-blocks`; private `s1-62-…`, `s1-65-…` |

## `.ldx` companion

Same stem, extension `.ldx` (case-insensitive). XML:

- Root `LDXFile` with attributes `Locale`, `DefaultLocale`, `Version` ("1.6").
- `Layers/Layer/MarkerBlock/MarkerGroup(Name, Index)/Marker(Version, ClassName, Name, Flags, Time)`; `Time` is cumulative microseconds with two decimals ([tobi] reads µs; 05 §4).
- `Layers/Details/String(Id, Value)`, e.g. `Total Laps`, `Fastest Time` (`m:ss.mmm`, minutes not wrapped at 60, e.g. `1456:09.100`), `Fastest Lap`. [tobi] also writes `DateTime` details; [i3rs] reads `MarkerPair(Time, EndTime)` elements.

**S-1**: all 42 `.ldx` files on the machine (including the two vendor sample `.ldx` files) contain only the three `Details` strings and **no markers**, even where `Total Laps` is 75. Marker parsing is covered by the synthetic self-test only.

## Warning codes of `tools/ld_ref_decode.py`

`python -I tools/ld_ref_decode.py --list-codes` prints this table from the code. "S-1 files" counts the corpus files (of 87) that emitted the code.

| Code | Severity | Meaning | S-1 files |
|---|---|---|---|
| `ld.too_small` | error | file shorter than the 1,762-byte header | 0 |
| `ld.magic_unexpected` | warning | u32 at 0x00 is not 0x40 | 0 |
| `ld.meta_ptr_out_of_range` | error | header `meta_ptr` outside the file | 1 |
| `ld.data_ptr_out_of_range` | warning | header `data_ptr` outside the file | 1 |
| `ld.data_ptr_mismatch` | info | header `data_ptr` differs from the lowest channel data pointer | 0 |
| `ld.block_out_of_range` | info | event/venue/vehicle pointer is 0 or the block does not fit | 34 |
| `ld.ptr_width_ambiguous` | info | venue/vehicle pointer needs more than 16 bits; read as u32 | 35 |
| `ld.count_mismatch` | warning | header channel count differs from the walk | 0 |
| `ld.count_upper_bits` | info | channel count read as u32 has non-zero upper 16 bits; the lower 16 bits match | 58 |
| `ld.list_cycle` | error | channel list revisits an offset; walk aborted | 0 |
| `ld.list_backwards` | info | next pointer lower than the current record | 0 |
| `ld.list_prev_mismatch` | info | prev pointer does not name the previous record | 0 |
| `ld.entry_out_of_range` | error | channel record extends past the end of the file | 0 |
| `ld.entry_stride` | info | most common record distance is not 124 bytes | 21 |
| `ld.entry_stride_irregular` | info | records not evenly spaced (extension data); walk by next pointer only | 31 |
| `ld.terminator_entry` | info | trailing all-zero record treated as the list terminator | 9 |
| `ld.data_out_of_range` | warning | sample array runs past the end of the file; count clamped | 0 |
| `ld.data_overlap` | warning | sample arrays overlap each other or the channel records | 0 |
| `ld.zero_rate` | warning | channel rate 0 | 0 |
| `ld.empty_channel` | info | channel has zero samples | 0 |
| `ld.unknown_dtype` | warning | unrecognised `(dtype_a, dtype)`; decoded as raw signed integer if the width allows | 0 |
| `ld.dtype_guessed` | info | pair decoded by a guess the sources do not agree on (0x06, 0x04, width 1, 0x07/8, 0x08/2·4) | 9 |
| `ld.zero_scale` | info | scale 0 treated as 1 | 0 |
| `ld.zero_mul` | warning | mul 0 treated as 1 | 0 |
| `ld.negative_scaling` | info | scale or mul negative | 0 |
| `ld.shift_nonzero` | info | channel has shift ≠ 0 | 3 |
| `ld.shift_mul_order` | warning | shift ≠ 0 and mul ≠ 1: both orders reported | 3 |
| `ld.float_scaling_nonidentity` | warning | float channel with non-identity scaling | 0 |
| `ld.nonfinite_samples` | info | float channel contains NaN or Inf | 4 |
| `ld.duplicate_name` | info | two channels share a name | 17 |
| `ld.name_unterminated` | info | 32-byte name without NUL (possible truncation) | 24 |
| `ld.name_continuation_candidate` | info | [i3rs] heuristic: short name may continue a truncated name (S-1: unreliable, see below) | 20 |
| `ld.unit_from_short_name_candidate` | info | unit empty while short name set | 66 |
| `ld.string_non_ascii` | info | string bytes ≥ 0x80 decoded as cp1252 | 1 |
| `ld.string_trailing_bytes` | info | non-zero bytes after a string's NUL | 0 |
| `ld.counter_sequence` | info | counters are not 0x2EE1 + index | 86 |
| `ld.date_unparsable` | info | header date/time not parseable | 0 |
| `ld.enum_tables` | info | enum tables found by the [i3rs] signature scan | 17 |
| `ld.enum_scan_failed` | info | a 212-byte record names an enum table the scan did not find | 1 |
| `ld.pro_logging_nonzero` | info | u32 at 0x5DE non-zero (recorded, not interpreted) | 84 |
| `ld.stats_sampled` | info | statistics on a strided subsample | 0 |
| `ldx.missing` | info | no `.ldx` beside the `.ld` | 47 |
| `ldx.parse_error` | warning | `.ldx` not well-formed | 0 |
| `ldx.unknown_element` | info | element outside the known structure (kept) | 0 |
| `ldx.unknown_marker_class` | info | marker class outside SOL EOL BCN SPLTBCN IGRDBCN RESET STGSTRT STGEND | 0 |
| `ldx.marker_time_unparsable` | warning | marker `Time` missing or not finite | 0 |
| `ldx.marker_beyond_end` | info | marker after the end of the longest channel | 0 |
| `ldx.marker_unsorted` | info | markers not in ascending time order | 0 |

The Esses reader (`esses_formats`, M0) will emit its own `ImportLog` codes (05 §3); AC-M0-12's check (`tools/check_importlog_codes.py`) applies to those. The codes 05 §3 names beyond this table (for example `ld.header_only`, `ld.string_encoding`, `ld.gap_*`, `ld.aux_*`, `ld.enum_block_malformed`, `ld.enum_ref_missing`, `ld.event_block_*`, `ld.dps_from_dec`, `ld.unit_from_short_name`, `ld.beacon_channel`, `ld.legacy_layout`) are documented here together with the reader. Where the names coincide (`ld.list_cycle`, `ld.count_mismatch`, `ld.data_out_of_range`, `ld.zero_rate`, `ld.unknown_dtype`, `ld.duplicate_name`, `ld.shift_nonzero`, `ld.shift_mul_order`, `ld.block_out_of_range`, `ld.enum_scan_failed`, `ldx.parse_error`, `ldx.unknown_marker_class`) the meaning is the same.

Name heuristics: on an M1 file the [i3rs] continuation rule would turn `Fuel Film Cylinder 1 Secondary F` + short name `ul` into `…Secondary Ful`, but `ul` is the unit (µL). Both candidates are therefore only reported, never applied.

## Import Log codes of the Esses reader

The `motec.ld` reader and writer (`esses_formats`) report every heuristic with one of these codes (05 §1.3 rule 9); the table is the code table of `libs/formats/src/ld/ld_codes.cpp`, which `lint.check_ld_codes` keeps in step with the reference decoder's names. "Shared" marks a code the reference decoder also emits with the same meaning. AC-M0-12's `tools/check_importlog_codes.py` fails when the corpus (the public synthetic families and, when present, the user's files under `fixtures/private`) emits a code this page does not list. Writer notes (`ld.channel_skipped`, `ld.detail_skipped`, `ld.truncated`, `ld.value_clamped`) appear in a `WriteReport`, never in a probe.

| Code | Level | Shared | Meaning |
|---|---|---|---|
| `ld.too_small` | Error | yes | file shorter than the 1,762-byte header; not read |
| `ld.magic_unexpected` | Warning | yes | u32 at 0x00 is not 0x40 |
| `ld.header_only` | Warning |  | meta_ptr and data_ptr both equal the file size: a valid source without channels |
| `ld.meta_ptr_out_of_range` | Error | yes | header meta_ptr outside the file; no channel list read |
| `ld.data_ptr_out_of_range` | Warning | yes | header data_ptr outside the file (informational field) |
| `ld.data_ptr_mismatch` | Info | yes | header data_ptr differs from the lowest channel data pointer |
| `ld.block_out_of_range` | Info | yes | event, venue, vehicle or weather record pointer does not fit in the file; record skipped |
| `ld.date_unparsable` | Info | yes | header date/time not DD/MM/YYYY HH:MM[:SS]; no UTC anchor |
| `ld.pro_logging_nonzero` | Info | yes | u32 at 0x5DE is non-zero (recorded as the pro-logging dialect flag, never interpreted) |
| `ld.string_encoding` | Info |  | text encoding used for the file's strings: ascii, utf-8, cp1252 or mixed (writer: UTF-8 used) |
| `ld.string_trailing_bytes` | Info | yes | non-zero bytes after the NUL terminator of a string field |
| `ld.legacy_layout` | Info |  | legacy converter layout (list at 0x1336, records longer than 124 bytes, no gap/aux pointers); extension data not recovered |
| `ld.count_mismatch` | Warning | yes | header channel count (u16 at 0x56) differs from the walk |
| `ld.list_cycle` | Error | yes | channel list revisits an offset; walk aborted |
| `ld.list_backwards` | Info | yes | next pointer is lower than the current record |
| `ld.list_prev_mismatch` | Info | yes | prev pointer does not name the previous record |
| `ld.entry_out_of_range` | Error | yes | channel record outside the file or inside the header; walk stopped |
| `ld.terminator_entry` | Info | yes | all-zero last record (rate 0, count 0, type pair 0/0, empty name) treated as the list terminator; not a channel |
| `ld.zero_rate` | Warning | yes | channel rate 0 (any record other than the terminator); it cannot be timed and is skipped |
| `ld.empty_channel` | Info | yes | channel has zero samples |
| `ld.data_out_of_range` | Warning | yes | sample array runs past the end of the file; count clamped |
| `ld.unknown_dtype` | Warning | yes | unrecognised (dtype_a, dtype) pair; decoded as a raw signed integer of that width, or skipped when the width is not 1, 2, 4 or 8 |
| `ld.dtype_guessed` | Info | yes | (dtype_a, dtype) pair decoded by a guess (0x04 unsigned, width 1, 0x07/8, 0x08/2|4, 0x06/2) |
| `ld.zero_scale` | Info | yes | scale is 0; treated as 1 |
| `ld.zero_mul` | Warning | yes | mul is 0; treated as 1 |
| `ld.negative_scaling` | Info | yes | scale or mul is negative |
| `ld.shift_nonzero` | Info | yes | a channel has shift != 0 (once per file) |
| `ld.shift_mul_order` | Info | yes | shift != 0 and mul != 1: the two shift orders differ; ctx.detail names the order used |
| `ld.float_scaling_nonidentity` | Warning | yes | float channel with non-identity scaling; scaling applied |
| `ld.duplicate_name` | Info | yes | two channels share a name; the later one gets a ' (n)' suffix |
| `ld.name_unterminated` | Info | yes | 32-byte name field has no NUL (possible truncation) |
| `ld.unit_from_short_name` | Info |  | unit taken from the 8-byte field at 0x40 (unit rule (c)); flag UnitFromShortName |
| `ld.unit_from_short_name_candidate` | Info | yes | C-series channel with a short name and no unit; the short name does not resolve as a unit and stays the short name |
| `ld.dps_from_dec` | Info |  | display decimals byte is 0; dps taken from dec_places |
| `ld.interp_default` | Info |  | interpolation byte not 0-3 (or 0 in the C-series logger record layout); the kind default applies |
| `ld.beacon_channel` | Info |  | converter beacon channel (1 Hz int16 lap words) exposed as an ordinary channel |
| `ld.gap_block_malformed` | Warning |  | gap block or its event array outside the file, or an event kind other than 0/1 (events that fit are kept); or the per-file gap budget (each event of the file once) is spent and the block is ignored |
| `ld.gap_unpaired` | Info |  | gap entry without exit (invalid to the end of the data), or exit/entry out of turn |
| `ld.gap_unsorted` | Info |  | gap intervals unsorted, overlapping or reversed (a reversed pair is read as [exit, entry)); merged |
| `ld.gap_out_of_range` | Info |  | gap index beyond the sample count; clamped |
| `ld.gap_rate_mismatch` | Info |  | gap block rate differs from the channel rate |
| `ld.gap_block_shared` | Info |  | several channel records name the same gap block; it is parsed once |
| `ld.gap_convention_shifted` | Info |  | gap pairs in the shifted convention (entry = first invalid + 1, exit = last invalid): detected and read as written, or read as [entry - 1, exit + 1) with the option ld.gapConvention "shifted" |
| `ld.aux_out_of_range` | Info |  | aux block or one of its arguments outside the file, more than 16 arguments (the first 16 read), or the per-file aux budget (the file size) spent |
| `ld.aux_unknown_arg` | Info |  | aux block argument of unknown type; scan stopped |
| `ld.enum_block_malformed` | Warning |  | enumeration block marker mismatch, item size below 3 or data beyond the file; earlier tables kept |
| `ld.enum_ref_missing` | Info |  | channel references an enumeration table that does not exist; channel stays Integer |
| `ld.enum_scan_failed` | Info | yes | no enumeration block pointer and the fallback scan did not find a referenced table |
| `ld.enum_index_duplicate` | Info |  | two enumeration tables share an index; the first one is used |
| `ld.enum_ref_ignored` | Info |  | a scaled, float or wide-integer channel references an enumeration table; labels not applied (ctx.detail: scaled, float or wide integer) |
| `ld.event_block_unknown` | Info |  | device-event block type is not 1; block ignored |
| `ld.event_block_malformed` | Warning |  | device-event records beyond the file or with a non-finite time; records that fit are kept |
| `ld.source_changed` | Warning |  | the .ld changed on disk after open; chunk not decoded |
| `ld.io_fault` | Warning |  | page fault while decoding (truncated file, lost share); chunk sealed AllInvalid |
| `ld.truncated` | Info |  | writer: string cut at its field width on a code-point boundary |
| `ld.value_clamped` | Warning |  | writer: a raw value does not fit the channel's stored width; clamped |
| `ld.channel_skipped` | Warning |  | export: a channel the .ld cannot hold (timestamped, a rate that is not a whole number of Hz up to 65535, or text) is not written |
| `ld.detail_skipped` | Info |  | export: a detail whose value does not fit its .ld header or record field is not written there |
| `ldx.parse_error` | Warning | yes | .ldx is not well-formed XML; the .ld still opens |
| `ldx.unknown_element` | Info | yes | .ldx element outside the known structure (kept verbatim) |
| `ldx.unknown_attribute` | Info |  | .ldx attribute outside the known structure (kept) |
| `ldx.unknown_marker_class` | Info | yes | marker ClassName outside SOL EOL BCN SPLTBCN IGRDBCN RESET STGSTRT STGEND (kept verbatim, not a marker) |
| `ldx.marker_time_unparsable` | Warning | yes | marker Time missing or not a finite number (kept verbatim, not a marker) |
| `ldx.marker_beyond_end` | Info | yes | marker Time is after the end of the longest channel |
| `ldx.marker_unsorted` | Info | yes | markers are not in ascending Time order in file order (checked within each marker group) |

## Still to verify (05 §3.10)

| # | Item | Status after S-1 |
|---|---|---|
| 1 | Magic 0x40 | confirmed on 87 files |
| 2 | Width of venue/vehicle pointers | **u32** (logger files need it) |
| 3 | Shift-vs-mul order | **afterMul** by physical evidence on two logger channels; D-037 flipped the default on 2026-10-10; a vendor CSV export of one of them would add confirmation |
| 4 | 212-byte M1 record | records carry variable-size extension data; 212 is one size; **[port]** decodes the content as gap and aux blocks (05 §3.4), the reference decoder does not |
| 5 | Enum-table scan | the signature scan works (17 files); **[port]**: linkage through the aux block, tables through the 0x690 enumeration block (05 §3.9) |
| 6 | `dtype_a` 0x04/0x06 | 0x06/4 holds plain integers (dates, counts); 0x04 not seen |
| 7 | Counter semantics | not 0x2EE1 + i anywhere; logger ids on logger files, 20101 + i on PC-software files |
| 8 | "Pro logging" u32; F64 outside GPS | recorded as zero or non-zero only, never computed or written (D-143); F64 only on GPS channels |
| 9 | Date locale | `DD/MM/YYYY` in every `.ld` header (the vendor CSV export writes `M/D/YYYY`, so the CSV reader must not assume the `.ld` order) |
| 10 | Header-only file | `meta_ptr == data_ptr == size` on one converter test output: 05 §3.4 treats it as a valid empty source with Warning `ld.header_only`; the reference decoder reports `ld.meta_ptr_out_of_range` |
