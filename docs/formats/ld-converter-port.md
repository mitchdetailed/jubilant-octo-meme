# `.ld` reader and fixture writer: port specification from the author's converter

**Status:** draft for phase B (M0 week 3), written 2026-10-10. Specification only: nothing under `libs/` is created by this document.
**Owner document:** 05_IMPORTERS_AND_PLUGINS.md §3–§5 and §9. The design wins where this document only restates it; §8 lists where this document proposes to change it.
**Importer id:** `motec.ld` (nominative use, D-003/D-125 class (a)). Esses identifiers derived from this document never contain the vendor name; converter type names such as `MotecMetadata` appear here only as references to the converter's source.

## 0. Purpose, sources and provenance

The user's instruction (2026-10-10): the user's own Log File Converter already writes `.ld` files that i2 opens, so the Esses reader should be the inverse of that writer. This document turns the converter's writer and reader into a specification for the Esses reader (`motec.ld`) and fixture writer. It records every quirk and magic number with line references, maps the converter's types onto the Esses types, plans the Qt-free port, and lists what still has to be settled on real files.

**Converter sources read in full.** Paths are relative to the converter tree: the Qt C++ edition 9.1.0.23 (`src/details.h:6`) and the Python reference edition it was ported from.

| Ref | File | Role |
|---|---|---|
| `LP.h`, `LP` | `src/parsers/LdParser.h`, `src/parsers/LdParser.cpp` | C++ reader |
| `LW.h`, `LW` | `src/formats/LdWriter.h`, `src/formats/LdWriter.cpp` | C++ writer |
| `CD.h` | `src/data/ChannelData.h` | `EnumEntry`, `GapSegment`, `LogEvent`, `TypeInfo`, `ChannelData` |
| `TS` | `src/data/TimeSeriesData.h/.cpp` | gap detection (`detectAndClearGaps`) |
| `CM.h` | `src/data/ChannelMapEntry.h` | per-channel overrides (not part of the file format) |
| `CU` | `src/conversion/ConversionUtils.h/.cpp` | `strToByteLen`, `checkNameLength`, decimal statistics, type analysis, upsample byte map, pro-code (excluded, §1.13) |
| `CWP` | `src/conversion/ConversionWorkerPipelines.cpp` | beacon channel construction (`buildBeaconFromTimes`, XRK lap path) |
| `py.LP` | `parsers/ld_parser.py` (Python reference edition) | original reader |
| `py.CW` | `conversion_worker.py`, `write_ld_file` (lines 70–677) | original writer |
| `notes` | `reference files/Motec LD Format descriptor v5.txt` | the author's layout notes |

**Corroboration used for this document.**

1. **Public notes.** I read Friss/i3rs `docs/ld-file-format.md` (MIT) on 2026-10-10. §6 lists where it differs. gotzl/ldparser (GPL-3) was not consulted, and nothing from it is reproduced here.
2. **User-owned files.** A throw-away probe script read these files byte by byte on 2026-10-10. The probe is not part of the repository. The files:
   - 8 from M1-series ECU loggers: 1 to 601 channels; three parts of one session, two further sessions, and three short captures (two of them deliberate event-code captures).
   - 3 from a C125 display logger, 1 channel each.
   - 9 written by various converter versions, device type `MTCA2LD`.

   No ACC, ADL-family or third-party sim-exporter file is on the development machine. The i2 install's `Samples` folder holds only the Conversion Wizard samples (CSV-type, relevant to `motec.csv` in M2) and no `.ld` files. The facts these files establish are numbered **E-1 … E-29** in §6.2 and cited throughout.

**Provenance of the converter's format knowledge.** The author must confirm this (§7):

- The author derived the layout by analysing log files: hex inspection of files written by loggers and by i2, and of the converter's own output after opening it in i2. No MoTeC software was disassembled or decompiled.
- The converter tree has no `LICENSE` file. Its `.ld` code has no reference to gotzl/ldparser or any other GPL source.
- The relayed user request establishes authorship ("my logfileconverter"). The MIT grant for `LdWriter`/`LdParser`/`ld_parser.py`/`write_ld_file` comes from the orchestrating task text, not from the user's own words. The author must confirm it before any line is ported (§7, §8 item 1).

---

## 1. Binary layout as the converter writes and reads it

All integers are little-endian. "Pad" means zero bytes as the converter writes them; several are not zero on real files, as noted. Offsets are absolute file offsets unless a block-relative `+0x…` is given.

### 1.1 File map

The converter writes the same layout that M1-series loggers produce: a fixed header record followed by four fixed-size detail records. That is why the first free byte is 0x3448 (E-12).

| Region | Converter (`LW` 509–822) | Real files |
|---|---|---|
| `0x0000–0x06E1` header record (1,762 B) | `buildHeaderStruct` (`LW` 440–504) | same size on M1 and C125 |
| `0x06E2–0x1335` event record (3,156 B) | written as the tail of the 4,918-byte header struct (`LW` 493–499) | M1: at 0x6E2; C125: elsewhere (pointer at 0x24), same size |
| `0x1336–0x1F53` venue record (3,102 B) | patched in by `setStr`/`setU32Scaled` (`LW` 740–743) | same size on M1 and C125 |
| `0x1F54–0x2C47` vehicle record (3,316 B) | `LW` 745–766 | same size on M1 and C125 |
| `0x2C48–0x3447` weather record (2,048 B) | `LW` 768–780 | M1: present |
| `0x3448…` enumeration definitions (optional) | `buildEnumDefinitionsBlock` (`LW` 334–386), only when enumerations exist | M1: log-event block, an unknown block, then enumeration definitions, then channels |
| `meta_ptr…` channel records, each followed by its gap block and aux block | `LW` 595–669 | M1: same chaining; C125: channel list near the end of the file |
| `data_ptr…` sample arrays, contiguous, in channel order, unaligned | `LW` 617–618 | same |

The converter pads the 4,918-byte header struct with zeros to 0x3448 (`LW` 693–696, `LW.h` 182–183). It then writes the optional enumeration block, the channel records and the data (`LW` 814–818). Older converter versions put the channel list at 0x1336 (legacy layout, §2 Q-W30).

### 1.2 Header record (`0x000–0x6E1`)

| Off | Type | Converter writes (`LW`) | Converter reads (`LP`) | Meaning / real-file notes |
|---|---|---|---|---|
| 0x000 | u8 + 7 pad | `0x40` (448–449) | not checked | magic: u32 `0x00000040` on every file (E-1) |
| 0x004 | (pad) | 0 | — | C125: non-zero pointer to an unknown block containing the string `default` (E-29); the notes call it "pointer to where the Config is" |
| 0x008 | u32 | `metaStartPtr` (450) | list start (106) | `meta_ptr`, first channel record |
| 0x00C | u32 | `metaEndPtr` (451) = first data byte | fallback list start (107–108); wrong, because this is the data pointer | `data_ptr` |
| 0x010 | 20 pad | (452) | — | |
| 0x024 | u32 | `0x06E2` (443, 453) | event name pointer (641) | `event_ptr`; C125 points late in the file |
| 0x028 | 24 pad | (454) | — | C125: non-zero u32 at 0x02C (unknown) |
| 0x040 | u16 | `2` (455) | **must equal 2** or the file is rejected (74) | M1 and converter: 2; C125: **0** (E-2) |
| 0x042 | u32 | written as u16 `16960` + u16 `15` (456–457) = 1,000,000 | — | 1,000,000 on every file (E-2); plausibly ticks per second, matching the µs event times (hypothesis) |
| 0x046 | u16 + 2 pad | `meta.serial` cast to u16 (458–459) | u32 (576) | device serial; u32 on real files (E-3) |
| 0x04A | char[8] | `meta.model`, default `MTCA2LD` (460, `LW.h` 82) | char[8] (580) | device type (`M1`, `C125`, …) |
| 0x052 | u16 | `100` (461) | — | device version × 100 (M1: 100; C125: 640) |
| 0x054 | u16 | `0x80` (462) | — | M1: 0x80; C125: 0 |
| 0x056 | u16 | channel count incl. beacon (463) | — | **u16** channel count (E-5) |
| 0x058 | u16 | channel count again (464) | — | M1: count again; C125: 0 |
| 0x05A | u16 | `1` (465) | — | M1: 10 or 1000; C125: 1; notes say 0xC8 |
| 0x05C | u16 | `1` (466) | — | M1: 1 or 10 |
| 0x05E | char[10] + 22 pad | `logDate`, `DD/MM/YYYY` (467–468) | split on `/` into day/month/year (583–589) | date |
| 0x07E | char[8] + 24 pad | `logTime` (469–470) | converted to 12-hour + AM/PM (592–617) | time `HH:MM:SS` (24 h on real files) |
| 0x09E | char[63] + NUL | driver (471–472) | reads 32 (620) | |
| 0x0DE | char[63] + NUL | vehicle id (473–474) | reads 32 (623) | |
| 0x11E | char[63] + NUL | engine id (475–476) | reads 32 (626) | |
| 0x15E | char[63] + NUL | venue (477–478) | reads 32 (629) | |
| 0x19E | 1,088 pad | (479) | — | |
| 0x5DE | u32 | pro-logging code (481–483) | — | see §1.13; excluded from the port |
| 0x5E2 | 2 pad | (484) | — | |
| 0x5E4 | char[63] + NUL | session (485–486) | reads 32 (632) | |
| 0x624 | char[63] + NUL | short comment (487–488) | reads 64 (635) | |
| 0x664 | 48 pad | (489) | — | contains the pointer slots of §1.4 |
| 0x694 | char[31] + NUL | team (490–491) | reads 32 (638) | |
| 0x6B4 | 46 pad | (492) | — | |

### 1.3 Detail records (event, venue, vehicle, weather)

**Event record** (3,156 B) at `event_ptr`:

| +Off | Abs (converter) | Type | Field | Converter |
|---|---|---|---|---|
| +0x000 | 0x06E2 | char[64] | event name | `LW` 493–494; `LP` 641–645 reads it through pointer 0x24, else at absolute 0x6E2 |
| +0x040 | 0x0722 | char[64] | session (second copy) | `LW` 495–496; not read |
| +0x080 | 0x0762 | char[1024] | long comment | `LW` 497–498; `LP` 648 (absolute) |
| +0x480 | 0x0B62 | **u32** | `venue_ptr` | always `0x1336` (`LW` 782–783); `LP` 651 reads the venue name through it |
| +0x484 | 0x0B66 | u32 | `weather_ptr` | `0x2C48` only when any weather field is set (`LW` 796–805); never read |
| +0x488 | | pad to record end | | |

**Venue record** (3,102 B) at `venue_ptr`:

| +Off | Abs | Type | Field | Converter |
|---|---|---|---|---|
| +0x000 | 0x1336 | char[64] (63 + NUL) | venue name (`venueArea`, else `venue`) | `LW` 740–741; `LP` 651–656 reads 66 bytes |
| +0x042 | 0x1378 | u32 | venue length in mm | `LW` 742; `LP` 659–666 reads it at the **absolute** offset |
| +0x44A | 0x1780 | u32 | `vehicle_ptr` | `0x1F54` only when a vehicle field (or the venue category) is set (`LW` 785–794) |
| +0x44E | 0x1784 | char[32] (31 + NUL) | venue category | `LW` 743; `LP` 669 (absolute) |

**Vehicle record** (3,316 B) at `vehicle_ptr`:

| +Off | Abs | Type | Field | Converter (`LW`) |
|---|---|---|---|---|
| +0x000 | 0x1F54 | char[64] | vehicle id | 745 |
| +0x040 | 0x1F94 | char[64] | vehicle description | 746; `LP` 672 (absolute) |
| +0x080 | 0x1FD4 | char[64] | engine id | 747 |
| +0x0C0 | 0x2014 | u16 | weight, kg | 748 |
| +0x0C2 | 0x2016 | u16 | fuel tank capacity, L × 10 | 749 |
| +0x0C4 | 0x2018 | char[32] | vehicle type | 750 |
| +0x0E4 | 0x2038 | char[32] | drive type | 751 |
| +0x104 | 0x2058 | u16 | diff ratio × 1000 | 752 |
| +0x106 … +0x118 | 0x205A … 0x206C | 10 × u16 | gear 1 … 10 ratio × 1000 | 753–762 |
| +0x11A | 0x206E | u16 | track width, mm | 763 |
| +0x11C | 0x2070 | u16 | wheelbase, mm | 764 |
| +0x120 | 0x2074 | char[1024] | vehicle comment | 765 |
| +0x524 | 0x2478 | char[32] | vehicle number | 766; `LP` 676 (absolute) |

`setU16Scaled`/`setU32Scaled` (`LW` 713–738) write the numeric fields: parse the text, multiply by the scale, `llround`, clamp to `[0, 65535]` or `[0, 2³²−1]`. Unparseable text leaves the field 0.

**Weather record** (2,048 B) at `weather_ptr` (`LW` 768–780). The writer declares 16-byte value fields and 16-byte unit fields but places them 8 bytes apart, so each unit field is effectively 8 bytes (Q-W27):

| +Off | Abs | Field | Effective size |
|---|---|---|---|
| +0x00 | 0x2C48 | sky | 64 |
| +0x40 / +0x50 | 0x2C88 / 0x2C98 | air temperature / unit | 16 / 8 |
| +0x58 / +0x68 | 0x2CA0 / 0x2CB0 | track temperature / unit | 16 / 8 |
| +0x70 / +0x80 | 0x2CB8 / 0x2CC8 | barometric pressure / unit | 16 / 8 |
| +0x88 / +0x98 | 0x2CD0 / 0x2CE0 | relative humidity / unit | 16 / 8 |
| +0xA0 / +0xB0 | 0x2CE8 / 0x2CF8 | wind speed / unit | 16 / 8 |
| +0xB8 | 0x2D00 | wind direction | 64 |
| +0xF8 | 0x2D40 | weather comment | 1024 |

### 1.4 Pointer slots inside the header padding

| Off | Type | Converter | Real files |
|---|---|---|---|
| 0x687 | u8 | 0 | M1: 0x15; C125: 0x22 (notes: "0x26 ???"); unknown, preserve |
| 0x688 | u32 | never written; `LP` 98–103 reads it as the log-event block pointer | M1: log-event block (§1.5); C125: 0, or a block whose first u32 is not 1 |
| 0x68C | u32 | never written or read | M1 and C125: non-zero pointer to an unknown block (notes: "Enumeration Prefix … not used"); preserve |
| 0x690 | u32 | `0x3448` when enumerations exist (`LW` 698–700); `LP` 91–96 parses it | M1: enumeration definition block |

### 1.5 Log-event block (at u32@0x688)

`LP` 354–395 reads this block; the converter never writes it.

| +Off | Type | Field |
|---|---|---|
| +0x00 | u32 | block type, must be 1 |
| +0x04 | u32 | record count |
| +0x08 | u32 | record array pointer (M1: block + 0x2C) |
| +0x0C | 32 B | reserved (zero on M1) |

Each record is 20 bytes: `+0x00 u16 code`, `+0x02 f64 time in µs from log start` (fractional on M1, E-26), `+0x0A 10 B reserved` (zero).

- The converter converts the time to seconds (`LP` 380).
- It stable-sorts by time with NaN last (`LP` 388–393), because stored order groups events by restart. Observed: code 7 stored before code 6, which has an earlier time.
- Code meanings: 0 firmware started, 1 firmware stopped, 2 power cycle, 3 logging started, 4 logging stopped, 5 discontinuity. The converter's label table (`CD.h` 50–62) was taken from i2's display on the author's install. i2 shows one fixed fallback label for every code ≥ 6.
- Codes 0–52 are observed (E-26).

### 1.6 Enumeration definition block (at u32@0x690)

Written by `LW` 334–386. The Python writer (`py.CW` 84–110) never sets the extended flag. Read by `LP` 201–265.

```
u32 1                      block marker
u32 enumCount
repeat enumCount:
  u32 2                    enumerator marker
  u16 valueCount
  u16 2                    "size of index" (the notes once say 0x20; every real file has 2, E-18)
  u16 enumIndex            referenced by the aux block's type-5 argument
  repeat valueCount:
    u32 3                  value marker (the notes' "30 00 00 00" is a transcription slip)
    u16 extended           1 when a severity block follows
    u16 itemSize           = labelBytes + 3 (2 for the value, 1 for the NUL)
    i16 value              0xFFFF/0xFFFE read as -1/-2 naturally
    char label[itemSize-3]
    u8  0
    if extended:           10 bytes
      u32 6
      u16 0
      u16 2
      u16 severity         0 normal, 1 info, 2 warning, 3 fault (CD.h 11; the notes call it a colour: default/blue/yellow/red)
```

Writer specifics:

- One enumerator per channel name, in `QMap` key order, so **sorted by channel name**. The aux block's index is that sorted position (`LW` 546, 569).
- Values outside int16 are dropped with a warning (`LW` 348–353).
- Severity is clamped to 0..3 (`LW` 381).

Reader specifics: any marker mismatch aborts and keeps what was already parsed (`LP` 217–242); `itemSize < 3` also aborts (`LP` 237).

### 1.7 Channel record (124 bytes = 0x7C)

| +Off | Type | Converter writes (`LW` 392–431 unless noted) | Converter reads (`LP` 111–196) | Real files |
|---|---|---|---|---|
| 0x00 | u32 | prev record (0 for first) | read, unused | |
| 0x04 | u32 | next record (0 for last) | followed | M1 ends with an empty record (E-23) |
| 0x08 | u32 | data pointer | `ldDataPtr` | |
| 0x0C | u32 | sample count | `sampleCount` | |
| 0x10 | u16 | `20101 + n` (`LW` 646) | not read | M1: starts at 20101 with holes; C125: 8091 (E-25) |
| 0x12 | u16 | type code (§1.10) | `ldTypeEnum` | |
| 0x14 | u16 | bytes per sample | `byteLength` | |
| 0x16 | u16 | rate, Hz | `freq` | 0 on the empty terminator record |
| 0x18 | i16 | add (shift), always 0 (`LW` 657) | `ldAdd` | 0 on every file seen |
| 0x1A | i16 | mul, always 1 | `ldMul` | 1, except the terminator (0) |
| 0x1C | i16 | div (scale), always 1 | `ldDiv` | M1 int32: 10000 or 1000 |
| 0x1E | i16 | exp (decimal places), `dpsShift` | `ldExp` | M1: 0–5 |
| 0x20 | char[32] | `name.left(32)` NUL-padded, no NUL when full (`LW` 658, `CU` 174–188) | `shortName` (159); the converter calls the 32-byte name "short" | |
| 0x40 | char[20] | unit (`LW` 415) | last non-empty NUL-separated part (162–164) | C125: short name char[8] at 0x40 + unit char[12] at 0x48; M1: unit at 0x40, 0x48 empty (E-19) |
| 0x54 | i32 | max of valid samples, stored units (`LW` 416) | — | M1, C125: raw max of the data (E-21) |
| 0x58 | i32 | min of valid samples, stored units (`LW` 417) | — | raw min |
| 0x5C | f32 | display min (`LW` 418–420) | — | M1 Gear: 10.0 (E-22) |
| 0x60 | f32 | display max (`LW` 421) | — | M1 Gear: −10.0; i2 orders the pair itself; 0/0 means no range |
| 0x64 | u8 | dps (channel override, else mode of the data, `LW` 647) | `dps` (167) | display decimals; differs from 0x1E on many M1 channels (E-20) |
| 0x65 | u8 | interpolation byte (`LW` 649–652; Bool forced to 1) | `upsampleType` (170) | M1: 1 or 3; C125: 0 |
| 0x66 | 14 pad | (`LW` 424) | — | C125: a non-zero u32 at 0x6C |
| 0x70 | (inside the pad) | 0 | `ldPtr70` declared, never filled | 0 on every file; the Python reader calls it `ptr_70` |
| 0x74 | u32 | gap block pointer or 0 (`LW` 631–637) | `ldPtr74` → `parseGaps` (173, 190–191) | M1: always record + 0x7C when present |
| 0x78 | u32 | aux block pointer or 0 (`LW` 639–644) | `ldPtr78` → `readPtr78Metadata` (174, 178–185) | after the gap block when both exist |

Interpolation byte values (`CU` 896–926, notes line 107): 0 closest, 1 previous, 2 next, 3 linear, 4 auto, 5 manual.

Each record is followed immediately by its gap block (if any), then its aux block (if any), then the next record (`LW` 663–666, `py.CW` 476–489). This chaining is what public notes describe as a "212-byte M1 entry" (§6.1 item 12).

### 1.8 Gap (validity) block (at record u32@0x74)

Written by `LW` 248–273 (`py.CW` 512–517), read by `LP` 420–465.

| +Off | Type | Field |
|---|---|---|
| +0x00 | u16 | 0 |
| +0x02 | u16 | channel rate in Hz (`LW` 266); E-16 confirms this on every M1 channel |
| +0x04 | u32 | number of 6-byte events, 2 per gap (`LW` 261) |
| +0x08 | u32 | event array pointer = block + 44 (patched at `LW` 635–636) |
| +0x0C | 32 B | zero |
| +0x2C | 6 B × count | `u16 kind` (0 = invalid from here, 1 = valid again from here) + `u32 sample index` |

Semantics:

- Each pair `(0, s), (1, e)` marks samples `[s, e)` invalid. In `TS` 182–224, `s` is the first NaN, `e` is the first valid sample after the run, and `e = n` for a run that reaches the end.
- E-16 confirms the half-open reading on a real M1 file: the exit index is the first logged sample after a restart.
- E-16 also shows that the stored values inside a gap are stale held values. Validity must come from this block, not from the samples. The converter zero-fills gap samples before packing (`TS` 212–213).
- The Python writer used `(s + 1, last NaN)` instead (`py.CW` 139–146). That disagrees with both the C++ writer and the M1 evidence.

### 1.9 Aux block: enumeration reference and long name (at record u32@0x78)

Written by `LW` 286–327 and patched at `LW` 639–644 (`py.CW` 165–201, 519–536). Read by `LP` 279–329.

| +Off | Type | Field |
|---|---|---|
| +0x00 | u16 | 0 |
| +0x02 | u32 | content pointer = block + 70 (E-17: every real file) |
| +0x06 | 64 B | zero |
| +0x46 | u32 | 1 |
| +0x4A | u32 | argument count (1 or 2) |
| +0x4E | args | in order: enumeration reference, then long name |

Arguments:

- **Type 5, enumeration reference** (10 bytes): `u32 5`, `u16 0`, `u16 2` (size of the index), `u16 enumIndex`.
- **Type 4, long name:** `u32 4`, `u16 0`, `u16 len` (UTF-8 bytes + 1), `char[len]` including the NUL.
  - The converter then writes **one extra 0 byte** (`LW` 314, `py.CW` 196), and its reader skips it (`LP` 316). Real M1 files have no such byte (E-17).
  - The long name is the channel name with `_` replaced by a space (`LW` 307). The 32-byte name field keeps the underscores.

The converter writes a long name when the name has more than 32 characters (`CU` 172; `LW` 567). It writes the enumeration reference when the enumeration map has an entry under the channel name (`LW` 568–569).

### 1.10 Data region and type codes

The samples of channel *k* are `count × bytesPerSample` contiguous bytes at its data pointer. Channels follow each other in record order with no alignment (`LW` 617–618).

| Type code @0x12 | Bytes @0x14 | Converter writes | `LP` decodes | `py.LP` decodes (by size) | Seen on real files |
|---|---|---|---|---|---|
| 0 | 2 | Bool, int16 0/1 (`CU` 125–130), only when the unit is empty (`LW` 182–185) | int16 scaled | int16 | converter outputs |
| 3 | 2 | int16, × 10^exp with exp 0..2 | int16 scaled | int16 | M1, converter |
| 5 | 4 | int32, exp 0 or fixed-point exp 4..9 | int32 scaled | int32 | M1, C125, converter |
| 7 | 4 | float32 | float32, **no scaling** | float32, no scaling | M1, converter |
| 7 | 2 | never | read as 4-byte floats at wrong offsets (Q-R14) | int16 (wrong) | not seen (public notes: float16) |
| 8 | 8 | never | dropped silently | dropped | one file whose producer is unclear (E-23); public notes: float64 GPS |
| 6 | 4 | never | dropped | int32 | M1 lap-beacon-style channel at 5 Hz (E-23) |
| 0 | 0 | never | dropped | dropped | M1 empty terminator record, rate 0, count 0 |
| any | 1 | never | dropped | int8 | not seen |

### 1.11 Scaling as the converter implements it

Writer (`LW` 95–241): `add = 0`, `mul = 1` and `div = 1` always (`LW` 657).

| Path | Condition | Stored value | exp @0x1E |
|---|---|---|---|
| int32 fixed point | channel dps in 4..9 and every value fits int32 (`LW` 111–153) | `round((v + valueOffset) × 10^dps)` computed in double | dps |
| float32 | default (`CU` 104) | `float(round((v + valueOffset) × 10^dps) / 10^dps)` when dps ≥ 0 (`LW` 155–166) | 0 |
| bool / int16 | `analyzeDataStructure` (`CU` 101–166) | `lround(v × 10^exp)` (`LW` 224–230) | 0, 1 or 2 |
| int32 | max decimals 0, range beyond int16 | `lround(v)` (`LW` 231–237) | 0 |

Non-finite samples are written as 0 (`LW` 223, 227, 234).

Type analysis (`CU` 32–166):

- Decimal places 0..3 per sample, found by `fmod` with a 1e-5 tolerance, on up to 1,000 sampled values.
- Bool when max decimals is 0 and the range is within [0, 1].
- int16 when the range fits ±32767, scaled by 10 or 100 for 1 or 2 decimals.
- int32 for larger integers.
- float32 otherwise.

Reader (`LP` 474–534; `py.LP` 423–461):

```
float32 (type 7):           physical = raw                       (no scaling at all)
int16/bool/int32 (0, 3, 5): physical = raw × (mul / div) × 10^(−exp) + add
                            C++: div == 0 → factor 1.0 (mul is dropped too)    (LP 500–502, 517–519)
                            Python: div == 0 → div = 1 (mul kept)              (py.LP 222, 427)
```

- **Order of `add` and `mul`.** The reader applies `add` after `mul`, which is the i3rs order (D-037 `afterMul`). The writer always writes `add = 0`, so the converter's own files cannot settle the order. No user-owned file has `add ≠ 0` (E-24).
- **M1 evidence.** On M1 int32 channels, the formula with `div` 10000 and `exp` 1..5 gives physically plausible values (E-24). That confirms both `div` and `exp` apply.

### 1.12 Beacon channel and lap data

The converter writes no `.ldx`. When the source has lap or beacon times, it appends one extra channel (`LW` 47–83, 672–686), built by `CWP` 100–144 and `CWP` 1728–1760.

**Record:**

- Counter 110, type 3, 2 bytes, **1 Hz**; add 0, mul 1, div 1, exp 0.
- Name `Beacon (Internal)` in 28 bytes + 4 pad; a 12-byte empty unit at 0x40 + 8 pad.
- i32 1 and 1 at 0x54/0x58. The Python writer put the beacon max/min there instead (`py.CW` 558–573).
- f32 min/max of the beacon words at 0x5C/0x60; dps 0; interpolation byte 0.
- Always last in the list, and counted in 0x56/0x58.

**Data:**

- int16 per second, 0 when idle. For lap *i* at time *t* seconds, three consecutive words: `0xC000 + i`, `0x8000`, `0x4000 + frac`.
- The running counter starts at −1, so the first word lands at index `floor(t) + 1`, and the array ends one sample longer than the session (`CWP` 104–134).
- `frac` has two definitions:
  - MoTeC-CSV path: `(t − floor t) × 32768` (`CWP` 116).
  - XRK path: `(t − floor t) × 1000`, in ms (`CWP` 1747).
  - The notes (lines 157–161) describe ms with an overrun rule.
- The MoTeC-CSV path multiplies times by the channel rate before using them as 1 Hz indices (`CWP` 1574–1576). That is only right when the rate is 1 Hz.

The reader does not decode this channel: `LP` reads it as an ordinary int16 channel.

### 1.13 Pro-logging field at 0x5DE (excluded)

The converter computes a u32 for 0x5DE from the device type and serial (`CU` 196–250, called at `LW` 481–483). The value acts as a licensing token for i2.

- **It is not ported.** Its derivation is not restated in any Esses document, and Esses never computes or validates it.
- **Reader:** records only whether the field is zero (dialect flag `pro-logging`, 05 §3.8).
- **Writer:** writes 0, for fixtures and export alike. It preserves a read file's value byte-for-byte only on a verbatim header round-trip of that same file (05 §5 item 8).
- **Observed values:** every user-owned logger file and every converter output has a non-zero value. Per the user (2026-10-10), ACC files are not licensed by the vendor, so ACC output is expected to carry zero there.

---

## 2. Quirks, magic numbers and heuristics

Ids: **Q-W** writer, **Q-R** reader, **Q-N** notes and Python edition. The "Esses" column says what the port does.

| Id | Where | Quirk / magic number | Esses |
|---|---|---|---|
| Q-W01 | `LW.h` 182, `LW` 520–525, `py.CW` 82 | `0x3448` "base meta start". It is simply the end of the four fixed detail records (§1.1) | Writer: emit the records contiguously; the constant falls out. Reader: never assume it |
| Q-W02 | `LW.h` 183, `LW` 502 | `0x1336` = 4,918-byte header struct = header + event record | same |
| Q-W03 | `LW` 448–457 | header bytes 0x40..0x45 written as u16 2, u16 16960, u16 15 (= u16 2 + u32 1,000,000) | Writer: u16 per dialect (2; 0 for the C-series family) + u32 1,000,000 |
| Q-W04 | `LW` 458 | serial truncated to u16 (Python raises for serials above 65535, `py.CW` 606) | Writer and reader: u32 |
| Q-W05 | `LW.h` 81–82 | defaults serial 50196, device type `MTCA2LD` | Writer: own neutral constants, kept beside `cmake/naming.cmake`'s codename so a rename touches one place (the codename may change, user 2026-10-10) |
| Q-W06 | `LW` 461–466 | 0x52 = 100, 0x54 = 0x80, 0x5A = 1, 0x5C = 1 | Writer: dialect table; reader: preserve in the raw header |
| Q-W07 | `LW` 463–464 | channel count written twice as u16, beacon included | Reader: u16@0x56 is the count; 0x58 is informational |
| Q-W08 | `LW` 467–470 | 10-byte date, 8-byte time; `MotecMetadata::logTime` may hold `HH:MM:SS AM` (`LW.h` 80), which the 8-byte field truncates | Writer: 24 h `HH:MM:SS` only |
| Q-W09 | `LW` 471–498 | strings 63 bytes + NUL in 64-byte slots (Python: 32 bytes + 32 pad) | Writer: full slot minus NUL |
| Q-W10 | `LW` 485, 495 | session written twice (0x5E4 and event +0x40) | Writer: both. Reader: event copy wins when non-empty |
| Q-W11 | `LW` 475 | engine id written at 0x11E and again at vehicle +0x80 | same as Q-W10 |
| Q-W12 | `LW` 743, 785–792 | venue category lives in the venue record, but setting it also forces the vehicle pointer | Writer: set each pointer for its own record |
| Q-W13 | `LW` 782–783 | venue pointer always written, even when no venue field is set | harmless; Writer: always write all three pointers, like M1 |
| Q-W14 | `LW` 698–700 | enumeration pointer at 0x690; block at the old 0x3448 | Writer: after the weather record |
| Q-W15 | `LW` 546, 569 | enumeration index = position of the channel name in a sorted map | Reader: use the index, never the order |
| Q-W16 | `LW` 366, 376–382 | extended flag = severity ≠ 0; severity block `06 00 00 00 00 00 02 00 tt 00` | port as is |
| Q-W17 | `LW` 646 | counter 20101 + n | Writer: same; reader: `vendorId` only |
| Q-W18 | `LW` 657 | add 0, mul 1, div 1 always | Writer: from `sourceEncoding` when the channel came from `.ld` (05 §5 item 2), else the same |
| Q-W19 | `LW` 658, `CU` 172–188 | name cut by characters (`left(32)`), then by UTF-8 bytes; a long name is written only when the name has more than 32 **characters**. A ≤ 32-character name with multibyte characters loses its tail with no long-name block | Writer: long name whenever the encoded name exceeds 32 bytes |
| Q-W20 | `CU` 174–188 | fixed fields are NUL-padded to the exact width, so a full field has no NUL; truncation chops UTF-16 code units and can split a surrogate pair | Reader: bound every string by its field; writer: truncate on code-point boundaries |
| Q-W21 | `LW` 307 | long name has `_` → space; the 32-byte name keeps `_` | Reader: `loggedName` = long name when present |
| Q-W22 | `LW` 313–314 | extra 0 byte after the long name | Reader: tolerate 0 or 1 (§3 R7); writer: no pad, like M1 |
| Q-W23 | `LW` 266 | gap header u16 = channel rate | Reader: Info `ld.gap_rate_mismatch` when it differs |
| Q-W24 | `LW` 261 | gap count = events, not gaps | port as is |
| Q-W25 | `LW` 418–421 | display min/max f32; 0/0 = auto-scale; slot order not significant | Reader: `HasDisplayRange` when not 0/0; lo = min, hi = max of the pair |
| Q-W26 | `LW` 650–652 | Bool channels forced to interpolation byte 1 | Reader: Bool → `Interp::Previous` regardless |
| Q-W27 | `LW` 769–778 | weather unit fields declared 16 bytes but overwritten after 8 | Reader: read units as 8 bytes; writer: 8 |
| Q-W28 | `LW` 182–185 | Bool type 0 is replaced by int16 type 3 when the channel has a unit | Reader: type 0 → `SampleKind::Bool` |
| Q-W29 | `LW` 95–153, `CD.h` 129–132 | `valueOffset` keeps GPS precision through float32; fixed-point int32 exp up to 9 | Reader: exp ≥ 5 → F64 (05 §3.5) |
| Q-W30 | legacy converter files (E-28) | channel list at 0x1336; 132-byte records whose aux pointer sits at +0x80 and points 8 bytes before the real block; slots 0x74/0x78 zero | Reader: no recovery (i2 sees no long name either); Info `ld.legacy_layout` when the stride exceeds the declared blocks |
| Q-W31 | `LW` 808–819 | file written in place, not atomically | Writer: `<path>.tmp` + rename (05 §1.3 rule 10) |
| Q-W32 | `LW` 159–166 | float32 samples pre-rounded to the dps | Writer: no pre-rounding (round trip must be bit-exact) |
| Q-W33 | `CU` 40–79 | decimal-mode tie broken by `QHash` iteration order (not deterministic) | Writer: not ported (§5) or ties → lower dps |
| Q-W34 | `CWP` 104–134 | beacon words at `floor(t) + 1`; array one sample long | Reader: see §3 R9 (post-M0) |
| Q-W35 | `CWP` 116 vs 1747 | beacon fraction ×32768 in one pipeline, ×1000 in another | resolve on a real file (§6.3) |
| Q-W36 | `CU` 196–250 | pro-logging code | excluded (§1.13) |
| Q-R01 | `LP` 74 | rejects a file unless u16@0x40 == 2; C125 files have 0 | Sniff on u32@0 == 0x40 plus structure (05 §3.1) |
| Q-R02 | `LP` 106–108 | falls back to 0x0C (data pointer) as list start | no fallback; `ld.bad_meta_ptr` error |
| Q-R03 | `LP` 111 | walk capped at 2,000 entries; no cycle detection (`py.LP` 91 had one) | bound `size / 124`, visited set, `ld.list_cycle` |
| Q-R04 | `LP` 111, 134, 178, 204, 284… | 32-bit `ptr + n` checks wrap near 2³² (only `parseLogEvents` widens, `LP` 357–372) | every check in 64-bit (05 §3.4) |
| Q-R05 | `LP` 159 | 32-byte name split at the first NUL after `trimmed()` | bound by field, strip trailing NUL/space only |
| Q-R06 | `LP` 162–164, `py.LP` 134–146 | unit = last non-empty NUL-separated part of 20 bytes: returns the short name as unit when a C-series channel has a short name and no unit | §3 R5 rule |
| Q-R07 | `LP` 167 | dps byte read but dec places unused for dps | `dps` = byte@0x64 (§3 R5) |
| Q-R08 | `LP` 178, 284 | long name needs `ptr78 + 70 < size` and `ptr78 + 78 ≤ size` | 64-bit bounds, `ld.aux_out_of_range` |
| Q-R09 | `LP` 303–326 | unknown argument types are not skipped, so the loop reads misaligned (Python breaks, `py.LP` 259–261) | stop at an unknown type, Info `ld.aux_unknown_arg` |
| Q-R10 | `LP` 314 | `len − 1` assumes a NUL; `len == 0` underflows | require `len ≥ 1`, strip trailing NULs |
| Q-R11 | `LP` 442–462 | unpaired entry at end of array dropped (gap to end of data lost); exit without entry ignored | open entry → invalid to end of data; Info `ld.gap_unpaired` |
| Q-R12 | `CD.h` 22 | `GapSegment` comment says "inclusive on both ends"; the code is half-open | half-open `[s, e)` |
| Q-R13 | `LP` 480 | `int` byte count overflows past 2 GiB | 64-bit |
| Q-R14 | `LP` 490–497 | type 7 always read as 4-byte floats, even when bytes = 2 | dispatch on `(type, bytes)` |
| Q-R15 | `LP` 490 | float32 never scaled, even when div/exp/mul/add are not identity | scale every dtype unless identity (i3rs scales all) |
| Q-R16 | `LP` 500–502 | `div == 0` drops `mul` | `div == 0` → 1, keep `mul` (05 §3.6) |
| Q-R17 | `LP` 474–534 | whole-channel decode to `QVector<float>`; int32 fixed-point GPS loses precision | chunked `readBlock` into F64/F32/I32 (§3 R8) |
| Q-R18 | `LP` 380, 388–393 | events converted to seconds and re-sorted | keep µs (`llround`), order by `(t, ordinal)` |
| Q-R19 | `LP` 592–617 | time turned into 12 h + AM/PM | keep 24 h string; parse to UTC anchor |
| Q-R20 | `LP` 620–676 | details read at absolute offsets (0x1378, 0x1784, 0x1F94, 0x2478) instead of through pointers: wrong for C125, whose records sit elsewhere | follow `event_ptr → venue_ptr → vehicle_ptr`, `weather_ptr` |
| Q-R21 | `LP` 620–638 | driver/vehicle/venue/session read with 32-byte limits although the slots are 64 | read the full slot |
| Q-R22 | `LP` 97, 186 | enumerations attached by id map; a missing id is ignored silently | Info `ld.enum_ref_missing` |
| Q-R23 | `LP` 363, `CD.h` 37–41 | event block type must be 1; otherwise ignored (correct for C125's different block) | same, Info `ld.event_block_unknown` |
| Q-N01 | `py.LP` 107, 148–174 | reads a 232-byte "body"; long name guessed from body offset 160; "segment table" at 0xA8 | not ported |
| Q-N02 | `py.LP` 270 | gap block accepted only when it starts `00 00 01 00`, which is true only for 1 Hz channels | not ported |
| Q-N03 | `py.LP` 305–347 | 12-byte legacy segment table | not ported |
| Q-N04 | `py.LP` 350–387 | hard-coded enum "discovery" by searching for test strings | never ported |
| Q-N05 | `py.CW` 128, 461 | missing rate defaults to 1000 Hz | not ported |
| Q-N06 | `py.CW` 492, 506 | interpolation byte hard-coded 3; no display range | superseded by `LW` |
| Q-N07 | notes 2–64 | header table: 0x1780 called "pointer to session name???" (it is the vehicle pointer); 0x0B66 "unused details" (it is the weather pointer); 0x0687 "0x26 ???" | superseded by §1 |
| Q-N08 | notes 93–110 | channel name "char[28]" (only the beacon record uses 28); dps "max 23"; gap padding "char[20]" (20 hex = 32) | superseded |
| Q-N09 | notes 170–208 | aux argument markers recorded as `40/50/10 00 00 00` and enum index size as `20 00` in early dumps | superseded: 4/5 and 2 (E-17, E-18) |
| Q-N10 | notes 157–161 | beacon encoding (1 Hz, `0xC000+lap`, `0x8000`, `0x4000+ms`) | §1.12, verify (§6.3) |

---

## 3. Inverse mapping: writer step → reader step

"Have" = what `LdParser` already does; "Missing" = what the Esses reader adds. R-steps are in reader order; probe is O(header) (05 §1.3 rule 2): steps R1–R7 and R10 run in `probe`, R8 in `readBlock`.

| Writer step (`LW`) | Reader step | Have (`LP`) | Missing / change |
|---|---|---|---|
| W1 header struct (440–504) | **R1** header: magic u32@0 == 0x40; `meta_ptr`, `data_ptr`, `event_ptr`; u16@0x40, u32@0x42; serial u32; device type; version; u16 count@0x56 (+u16@0x58); date, time; 64-byte strings; pro-logging zero/non-zero | partial (`parseHeaderMetadata` 547–679); wrong magic test (Q-R01); 32-byte limits (Q-R21) | correct magic and bounds; u16 count compared with the walked count (`ld.count_mismatch`, Warning; the empty terminator counts on M1); raw header kept for round trip; details as typed `Detail`s; UTC anchor from date + time (05 §3.7) |
| W2 detail records (740–805) | **R2** follow `event_ptr` → event record; `venue_ptr` (+0x480, u32) → venue; `vehicle_ptr` (venue +0x44A, u32) → vehicle; `weather_ptr` (event +0x484, u32) → weather. Each pointer: 0 → absent; out of range → `ld.block_out_of_range` (Info) | event name via pointer, venue name via pointer; everything else absolute (Q-R20); no vehicle/weather numbers | all four records through pointers; numeric decoding (u16 kg, u16 L×10, u16 ×1000 ratios, u16 mm, u32 mm venue length; 0 → absent) |
| W3 pad to 0x3448 (693–696) | — | — | nothing; never assume 0x3448 |
| W4 enumeration block + pointer 0x690 (334–386, 698–700) | **R4** enumeration definitions | complete (201–265) | 64-bit bounds; counts capped by remaining bytes; severity kept; per-table errors keep earlier tables (`ld.enum_block_malformed`); public scan (05 §3.9) only as a fallback when 0x690 is 0 and a channel references an index |
| W5 channel records, linked (595–669) | **R5** walk from `meta_ptr`; per record: rate, count, type pair, scaling, name, unit/short name, raw max/min, display range, dps byte, interpolation byte, gap and aux pointers | walk (111–196) with Q-R02/03/04 | cycle and bound checks; `prev` check (Info); skip records with rate 0 **and** count 0 (`ld.terminator_entry`, Info) and other zero-rate records (`ld.zero_rate`); **unit rule:** (a) the string at 0x40 has no NUL within its first 8 bytes → unit = 20-byte string, short name = "" (converter long unit); (b) else u = str(0x48, 12) non-empty → short name = str(0x40, 8), unit = u (C-series); (c) else unit = str(0x40, 8), flag `UnitFromShortName` + `ld.unit_from_short_name` (M1, converter, sims). **dps:** byte@0x64 when non-zero or when dec places = 0, else `max(0, dec)` (`ld.dps_from_dec`, Info). **interp:** 1 → Previous, 2 → Next, 3 → Linear, 0 → Closest when the device writes the byte (M1/converter), else kind default; 4/5 → kind default (Info) |
| W6 gap block (248–273, 631–637) | **R6** gap block → sorted, merged invalid intervals `[s, e)` per channel, clamped to `[0, count)` | pairs only (420–465) | Q-R11 (open entry → to end); unsorted/overlapping intervals merged (`ld.gap_unsorted`, Info); index beyond count clamped (`ld.gap_out_of_range`); rate check (Q-W23). Intervals feed `readBlock`'s `validBits` (and NaN for float dtypes, 05 §1.3 rule 7) |
| W7 aux block (286–327, 639–644) | **R7** aux: content pointer, argument list; type 5 → enum index; type 4 → long name (len ≥ 1, strip trailing NULs; next argument at `pos + len`, or `pos + len + 1` when that is where a valid type sits, the converter dialect) | (279–329) with Q-R08/09/10 | bounds, unknown-type stop, both padding forms, long name → `loggedName`, 32-byte name → raw provenance |
| W8 data packing (95–241, 617–618) | **R8** decode `[first, first+count)` of one channel: `(type, bytes)` dispatch, scaling per §1.11 with `ld.shiftOrder`, output dtype per 05 §3.5, validity per R6 | whole-array float decode for (0/3/5, 2/4) and (7, 4) (474–534) | f16 (7,2), f64 (8,8), raw unsigned for (4/6, 2/4) as `Integer` + `DtypeGuessed`, unknown pairs (`ld.unknown_dtype`); scaling on all dtypes; 64-bit offsets; `data_ptr + count × bytes > size` → clamp + `ld.data_out_of_range`; chunk-granular, page-fault-guarded (05 §3.11) |
| W9 beacon channel (47–83, 672–686) | **R9** exposed as an ordinary int16 channel in M0; a later `lapSource: "ld.beacon-words"` option may turn the `0xC000/0x8000/0x4000` triplets into `BCN` markers with `prov = Derived` (05 §15 item 5) | reads it as int16 | M0: nothing beyond Info `ld.beacon_channel` naming it |
| — (never written) | **R10** log-event block via 0x688 → `DeviceEvent` markers | complete (354–395) | µs kept; marker mapping (§4); type ≠ 1 → ignore + Info; records beyond file → `ld.event_block_malformed` |
| — | **R11** unknown blocks (0x004, 0x02C, 0x68C, record +0x6C) | ignored | kept only in the raw header/record bytes for round trip |
| W10 pro-logging code (481–483) | **R12** zero/non-zero → dialect flag | not read | done; never validated (§1.13) |
| — (converter writes no `.ldx`) | `.ldx` companion | none | 05 §4 unchanged; nothing to port |

**Items named in the task, by status:**

| Item | Converter | Esses |
|---|---|---|
| M1 "212-byte" entries | handled implicitly, because the reader follows the 0x74/0x78 pointers | no stride dialect: §6.1 item 12, §8 item 4 |
| Sim-exporter dialects | none, and the magic test may reject them (Q-R01) | 05 §3.8 `sim-exporter` heuristics, plus the R5 unit rule; no sample available locally (§6.3) |
| float16 | missing and mis-decoded (Q-R14) | R8 |
| `num_channs` mismatch | ignored (walks the list), not reported | R1 Warning; header read as u16 (E-5) |
| `.ldx` | none | 05 §4/§5 |
| Validity | gap blocks parsed into `GapSegment`, but `extractData` ignores them | R6 → bitmap |
| Device events | parsed, sorted | R10 → markers |

---

## 4. Type mapping: converter → Esses

| Converter (file:line) | Field | Esses type (design §) | Esses field / rule |
|---|---|---|---|
| `ChannelData` (`CD.h` 94–153) | `name` (long name, else 32-byte name) | `ChannelInfo` (05 §1.2), `ChannelDescriptor` (02 §4.1) | `loggedName`; `name` after the dictionary/resolver |
| | `shortName` (actually the 32-byte name) | `ChannelInfo` | raw 32-byte name kept in `LdChannelEncoding.rawName` (below); `ChannelInfo::shortName` = the 8-byte C-series short name per R5, else "" |
| | `unit` | `ChannelInfo::vendorUnit` | R5 rule; base-unit conversion at decode (D-010) |
| | `freq`, `sampleCount` | `ChannelInfo::timing`, `sampleCount` | `Rate{freq, 1}`, `t0Us = 0` |
| | `data`, `timestamps` | `IReader::readBlock` → `Block` (05 §1.3) | no whole-channel vector; `.ld` has no timestamps |
| | `gaps` (`GapSegment`, `CD.h` 24–28) | `Block::validBits`, `Chunk::Validity` (02 §3.1) | `[start, end)` cleared; chunk `Bitmap` when it intersects a gap, `AllInvalid` when covered, else `AllValid`; F32/F64 also NaN |
| | `enums`, `enumSetId` | `ChannelInfo::enumTable`, `EnumTableInfo` (05 §1.2) → `EnumTable` (02 §4.1) | id = enumerator index; `SampleKind::Enum`, `Dtype::I32`, `Interp::Previous` |
| | `ldMul/ldDiv/ldAdd/ldExp`, `typeInfo` | `ChannelInfo::vendorDtype`, `dtype`, `kind` + `LdChannelEncoding` | `vendorDtype` "i16", "i32", "f16", "f32", "f64", "u16", "u32"; dtype/kind per 05 §3.5 |
| | `dps` | `ChannelInfo::dps` → `ChannelDescriptor::dps` | R5 dps rule |
| | `valueOffset` | — | writer-side only |
| | `minOverride/maxOverride` | `ChannelInfo::flags` `HasDisplayRange` + `LdChannelEncoding.displayRange` | display range in vendor units → base units at resolve |
| | `upsampleType` | `ChannelDescriptor::interp` | R5 interp rule |
| | `ldDataPtr`, `ldPtr74`, `ldPtr78` (`ldPtr70`, `ldPtr132` unused) | `LdChannelEncoding` | reader-private |
| `GapSegment` (`CD.h` 24–28) | start, end | validity intervals (reader-private `std::vector<std::pair<uint64_t, uint64_t>>` per channel) | half-open; built in probe, used by `readBlock` |
| `EnumEntry` (`CD.h` 13–17) | `label`, `type` | `EnumTableInfo.entries[{value, label}]` | severity has no slot in `EnumTable` (02 §4.1); proposed `std::uint8_t severity` per entry (§8 item 9); until then preserved in `LdEnumTableRaw` for the writer |
| `LogEvent` (`CD.h` 37–41) | `code`, `time` | `MarkerInfo` (05 §1.2) → `Marker` (02 §4.3) | `cls = DeviceEvent`, `group = "DeviceEvents"`, `tUs = llround(time_us)`, `name` = Esses' own label for the code (codes ≥ 6 → "Event <code>"), `comment = ""`, `flags = code`, `ordinal` per `(class, group, tUs)` in stored order; `prov = Logged` |
| `logEventLabel` (`CD.h` 50–62) | i2 display strings | — | not copied; Esses words its own labels (observed UI text, D-005 (3)) |
| `MotecMetadata` (`LW.h` 28–83) | event, venue, driver, team, engineId, vehicleId, shortComment, longComment, session | `Detail` (02 §4.2), `ProbeResult::details` | `Event`, `Venue`, `Driver`, `Team`, `Engine Id`, `Vehicle Id`, `Short Comment`, `Long Comment`, `Session`; `prov = Logged` |
| | venueArea (venue record name) | `Detail` | `Venue` when the header venue is empty, else a vendor detail `Venue Name` |
| | venueCategory, venueLengthMm | `Detail` | `Venue Category`; `Venue Length` `Quantityd{mm → m}` |
| | vehicleDescription, vehicleNumber, vehicleType, driveType, vehicleComment | `Detail` | `Vehicle Desc`, `Vehicle Number`, `Vehicle Type`, `Vehicle Drive Type`, `Vehicle Comment` |
| | vehicleWeightKg, fuelTankCapacityL, diffRatio, gear 1–10, trackWidthMm, wheelBaseLengthMm | `Detail` (`Quantityd`) | `Vehicle Weight` (kg), `Fuel Tank Capacity` (raw/10 L), `Diff Ratio`, `Gear 1`…`Gear 10` (raw/1000), `Vehicle Track`, `Vehicle Wheelbase` (mm); raw 0 → detail absent |
| | weather fields | `Detail` | `Sky`, `Air Temp`, `Track Temp`, `Baro Pressure`, `Rel Humidity`, `Wind Speed` (value + unit text → `Quantityd` when the unit resolves, else text), `Wind Direction`, `Weather Comment` |
| | logDate, logTime | `Detail` (`DateTime`) + `LayerInfo::utc` | `Log Date`, `Log Time`; anchor flagged `utcAssumedLocal` |
| | serial, model, (version) | `Detail` | `Device Serial`, `Device Type`, `Device Version` |
| `BeaconConfig` (`LW.h` 85–90) | data, min, max | — (M0) | channel exposed; future `lapSource` (R9) |
| `TimeSeriesData` (`TS`) | channel list | `ProbeResult::layers[0]` (`"Normal"`) + `IReader` | channel index = list order (terminator skipped); duplicate names per 05 §3.4 |
| `ChannelMapEntry` (`CM.h`) | output name, unit, freq, dps, min/max, upsample, enums, omit | `MappingRow`/`UnitMappingProfile` (05 §8, M2), project overrides | not a reader concern |
| (sidecar) | — | `SidecarDocument` (02 §5) | the reader writes nothing there; user edits of `.ld` details become sidecar details with `prov = User` and `original` = the logged value (02 §5.3 rule 2) |
| (whole file) | — | `ProbeResult` (05 §1.2) | `importerId "motec.ld"`; `dialect` flags (proposed set, §8 item 4): `m1`, `c-series`, `converter`, `converter-legacy`, `sim-exporter`, `pro-logging`; `warnings[]` = R1–R12 Import Log entries |

Proposed reader-private record (in `libs/formats/src/ld/`, also the writer's re-quantisation input, 05 §5 item 2):

```cpp
struct LdChannelEncoding {                 // one per channel record; 124 raw bytes kept for verbatim round trip
  std::uint64_t recordOffset, dataOffset, sampleCount;
  std::uint16_t counter, typeCode, bytesPerSample, rateHz;
  std::int16_t  shift, mul, scale, dec;    // @0x18..0x1E
  std::int32_t  rawMax, rawMin;            // @0x54, @0x58
  float         rangeA, rangeB;            // @0x5C, @0x60 (display range, either order)
  std::uint8_t  dpsByte, interpByte;       // @0x64, @0x65
  std::uint32_t gapPtr, auxPtr;            // @0x74, @0x78
  std::array<std::byte, 124> raw;          // verbatim record
  std::string   rawName;                   // 32-byte field as stored
};
```

---

## 5. Port plan for `libs/formats` (phase B)

The rules for every file:

- **Qt-free.** The `ESSES_QT_FREE` target property applies.
- **No vendor token in identifiers.** The namespace is `esses::formats::ld`.
- **Mechanical replacements:**
  - `QByteArray` + `readU16/readU32/readI16/readF64` (`LP` 12–46) → `std::span<const std::byte>` from `ByteSource`/`MappedFile` + `esses::base::ByteReader` (LE).
  - `QString::fromUtf8(b.mid(...))` → `ld_strings` field reader. The design specifies cp1252; the converter writes UTF-8, so §8 item 10 proposes UTF-8 when valid, else cp1252, with Info `ld.string_encoding`.
  - `QVector`/`QMap`/`QVariantMap` → `std::vector`, sorted `std::vector<std::pair<>>`, `std::vector<Detail>`.
  - `QFile` → `MappedFile` (read); `std::ofstream` + tmp/rename (write).
  - `qWarning` → `ImportLog`/`WriteReport::notes`.
  - `qBound` → `std::clamp`; `Q_ASSERT` → `assert`.
- **JSON seam.** No JSON library is available in this phase. `ProbeResult::toJson`/`fromJson` and `fixture.json` handling stay behind a declared seam (`// JSON: phase B`). The `.ld` code itself needs no JSON. *As built:* the user approved the downloads on 2026-10-10, nlohmann/json 3.11.3 is vendored under `third_party/` (D-147), and `probe_json` and `fixture.json` use it.

| File (new) | Ports / replaces | Content | Est. lines |
|---|---|---|---|
| `include/esses/formats/ld/ld_layout.h` | `LP.h` 9–45, `LW.h` 10–26, notes | offset constants, record sizes (0x6E2, 0xC54, 0xC1E, 0xCF4, 0x800, 124, 44, 70, 20), type codes, gap kinds, argument types; no logic | 180 |
| `src/ld/ld_strings.{h,cpp}` | `CU` 174–188 (`strToByteLen`), `LP` 555–573 | `readField(span, off, width)`, `writeField(...)` on code-point boundaries, UTF-8/cp1252 transcoding, trim rules | 150 |
| `src/ld/ld_header.{h,cpp}` | `LP` 547–679 (inverted), `LW` 440–504, 702–805 | R1, R2: header + four detail records → `std::vector<Detail>`, dialect flags, raw header bytes; and the inverse `buildHeaderRegion(details, dialect)` for the writer | 520 |
| `src/ld/ld_channels.{h,cpp}` | `LP` 85–199, 279–329; `LW` 286–327, 392–431 | R5, R7: list walk, record decode, unit/dps/interp rules, aux block both ways, `LdChannelEncoding` | 560 |
| `src/ld/ld_enums.{h,cpp}` | `LP` 201–265; `LW` 334–386 | R4 both ways; public-notes scan fallback (05 §3.9) | 220 |
| `src/ld/ld_validity.{h,cpp}` | `LP` 420–465; `LW` 248–273; `TS` 182–224 | R6: gap block ↔ sorted intervals; `fillValidBits(intervals, first, count, span<uint8_t>)`; `intervalsFromInvalid(...)` for the writer | 200 |
| `src/ld/ld_events.{h,cpp}` | `LP` 354–395; `CD.h` 37–62 | R10: log-event block ↔ `MarkerInfo`; writer emits it for `DeviceEvent` markers (fixtures) | 140 |
| `src/ld/ld_decode.{h,c,cpp}` | `LP` 474–534, `py.LP` 393–465 | R8 kernels as plain C routines for `MappedFile::guarded()`: i16/i32/u16/u32/f16/f32/f64 × scaling (`afterMul`/`beforeMul`) × output F32/F64/I32; NaN/bitmap application | 380 |
| `src/ld/ld_importer.{h,cpp}` | `LP` 68–78 | `LdImporter` (`IImporter`: sniff/probe/open) + `LdReader` (`IReader`: `readBlock`, `markers`, `enumTable`, `warnings`, `chunkIndependent() = true`) | 420 |
| `src/ld/ld_chunk_provider.{h,cpp}` | — (05 §3.11) | `LdChunkProvider` over `MappedFile` (needs `esses_store`) | 180 |
| `src/ld/ld_writer.{h,cpp}` | `LW` 509–822, `LW` 47–83 (beacon, optional) | `LdWriter : IWriter`: layout planning (two passes like `LW` 527–587), records, gap/aux/enum/event blocks, data, dialect options (`m1`, `c-series`, `converter`, `sim-exporter`, `converter-legacy` for fixtures only), tmp + rename | 760 |
| `src/ld/ld_encoding_auto.{h,cpp}` (optional, M1+) | `CU` 26–166 | converter's compact type analysis as writer option `ld.encoding = "compact"` with deterministic ties; M0 uses 05 §5 item 2 rules instead | 160 |
| `tests/ld_*_test.cpp` (Catch2 3.7.1 amalgamated, vendored under `third_party/catch2v3`, D-148; the phase-A 2.13.10 single header is retired) | — | per-file unit tests: header/detail round trip, unit-rule table, gap edge cases (open entry, overlap, out of range), aux padding forms, enum severity, event ordering, every dtype pair × scaling × shift order, hostile-pointer table (wraps, cycles) | 1,400 |

**Totals.** About 3,700 lines of production code (≈ 2,800 reader + shared, ≈ 900 writer) and about 1,400 lines of tests. That is consistent with M0 week 3 in 07 §3.2 only if the `.ldx` parser and `SidecarDocument` are not also due that week (§8 item 12).

**Not ported:**

- the pro-code (`CU` 196–250)
- `py.LP` heuristics (Q-N01–Q-N05)
- `TimeSeriesData::resampleZOH`/`detectFrequency` (writer resampling follows 05 §5 item 4)
- `ChannelMapEntry`
- the Qt worker plumbing

**Order:**

1. `ld_layout`, `ld_strings`.
2. `ld_header` + `ld_channels` + `ld_enums` + `ld_events` (probe complete).
3. `ld_validity` + `ld_decode` + `ld_importer` (decode).
4. `ld_writer` (fixtures).
5. `ld_chunk_provider` once `esses_store` lands.

**Oracle independence (D-041).** `tools/ld_ref_decode.py` must be written from the public notes plus §1's facts **without reading the Esses code**. For converter-only structures (gap blocks, aux chain, enumeration block, events), the public notes say nothing, so a dialect claim about them is settled only by a real-file oracle. The private M1 files with gap tables, events and enumerations qualify (E-15–E-18, E-26), and so would an i2 CSV export of the same files (D-005 (1)).

---

## 6. Differences: converter vs public notes vs user files

### 6.1 Converter (code and notes) vs Friss/i3rs `docs/ld-file-format.md` (MIT)

| # | Topic | Converter | i3rs | User files | Phase B |
|---|---|---|---|---|---|
| 1 | 0x40–0x45 | u16 2 + u32 1,000,000 | 3 × u16 "static" | u16 2 (M1) / 0 (C125) + u32 1,000,000 | read as u16 + u32; preserve |
| 2 | serial 0x46 | u16 written, u32 read | u32 | fits u16 so far | u32 |
| 3 | 0x54 / 0x56 / 0x58 | u16 0x80; u16 count; u16 count | u16 unknown; **u32** count, "unreliable" | M1: count, count; C125: count, 0 | **u16** count; the "unreliability" is the u32 reading (E-5) |
| 4 | 0x5A–0x5D | u16 1, u16 1 | unknown | M1 varies | preserve |
| 5 | date/time | 10 + 22 / 8 + 24 bytes | 16 + 16 each | same strings | read 16, write 10/8 |
| 6 | 0x687–0x693 | event ptr, unknown ptr, enum ptr, team at 0x694 | not described (header padding) | all present on M1 | implement R4, R10; preserve 0x687/0x68C |
| 7 | event record | name, session, comment, u32 venue ptr; **u32 weather ptr at +0x484** | u32 venue ptr; no weather | u32 confirmed (C125 value > 0xFFFF) | u32 everywhere; correct 05 §3.3 |
| 8 | venue record | u32 length mm at +0x42; category at +0x44E | unknown bytes | C125 +0x42 = 10000 | as converter |
| 9 | vehicle record | u16 weight + u16 fuel×10 at +0xC0; drive type at +0xE4; ratios, mm, comment at +0x120, number at +0x524 | u32 weight; comment at +0xE4 | M1: +0xC0 = 279, +0xC2 = 76 (u32 reading meaningless) | as converter |
| 10 | channel +0x40 | char[20] unit | char[8] short name (unit on M1) + char[12] unit | C125 8+12; M1 unit at 0x40; converter spills past 0x48 | R5 three-way rule |
| 11 | +0x54–+0x77 | raw max/min, display range, dps, interp, gap ptr, (aux ptr at 0x78) | zero padding | populated on M1/C125 | decode |
| 12 | entry size | 124 + gap block + aux block, pointer-chained | 212 bytes on M1, enum type/id at 0xD0/0xD2 | strides 180–310; i3rs's 0xD0/0xD2 is the type-5 argument of an aux block that directly follows the record (124 + 70 + 18) | follow pointers; drop the stride dialect |
| 13 | enumeration tables | pointer at 0x690; full grammar incl. severity block | heuristic scan for `count, 02 00, id`; "4 unknown bytes" and "06 00 separators" | pointer valid on every M1 file | pointer first, scan as fallback; i3rs's unknowns are the `u32 3` tail + `extended`, and the severity block |
| 14 | scaling | `raw × mul/div × 10^−exp + add` (floats unscaled) | `(raw/scale) × 10^−dec × mul + shift` (all types) | div/exp confirmed; no `shift ≠ 0` | both orders agree with the non-GPL sources (`afterMul`); D-037 default `beforeMul` stands until a real file settles it (§8 item 6) |
| 15 | dtypes | 0/3/5/7 only | adds (7,2) f16, (8,8) f64, (6,4) unknown | (6,4) = 5 Hz beacon-tick channel on M1; (0,0) terminator | per 05 §3.5 + terminator rule |
| 16 | validity | gap blocks | not described | present on 87/88 and 310/310 M1 channels | R6 (changes 05 §3.11 "no validity") |
| 17 | device events | 0x688 block | not described | present on M1 | R10 |
| 18 | dps | byte at 0x64 | dec places | differ on 34/89 and 384/601 M1 channels | R5 dps rule (changes 05 §3.6) |
| 19 | long-name padding | +1 byte | n/a | none on M1 | tolerate both |
| 20 | terminator record | none written | "next = 0, n = 0, usually empty name" | M1 has one (rate 0) | skip with Info |
| 21 | header length | writes all records to 0x3448 | 0x6E2 | M1 identical to converter | writer emits all five records |
| 22 | strings | UTF-8 | single-byte | ASCII on every file | §8 item 10 |
| 23 | magic | u16@0x40 == 2 | u32@0 == 0x40 | C125 fails the converter test | i3rs test + structure |

### 6.2 Facts established on user-owned files (2026-10-10)

E-1 u32@0 = 0x40 on all 20 files. E-2 u16@0x40 = 2 (M1, converter) or 0 (C125); u32@0x42 = 1,000,000 on all. E-3 serial u32, values < 65536 so far. E-4 version M1 100, C125 640. E-5 u16@0x56 = count; u16@0x58 = count (M1, converter) or 0 (C125). E-6 0x5A/0x5C vary on M1 (10/1, 1000/1, 10/10), 1/1 elsewhere (a legacy converter file has 200/20). E-7 0x687 = 0x15 (M1), 0x22 (C125), 0 (converter). E-8 0x688 → type-1 event block on M1; 0 or a non-type-1 block on C125; 0 on converter files. E-9 0x68C non-zero on M1 and C125 (block begins u16 1 on M1). E-10 0x690 → enumeration block on M1 (133–227 enumerators); converter files at 0x3448 or 0x1336 (legacy). E-11 venue pointer u32 (C125 value 0x00159096); weather pointer at event +0x484 (M1 0x2C48). E-12 detail records are fixed-size and contiguous on both C125 and M1: event 0xC54, venue 0xC1E, vehicle 0xCF4; weather 0x800 on M1. E-13 vehicle +0xC0/+0xC2 = u16 279 / u16 76 on an M1 file. E-14 venue +0x42 = 10000 on C125 defaults. E-15 M1 record strides 180–310; gap block always at record + 0x7C; gap before aux. E-16 gap header u16 = channel rate on every channel (1,179 of 1,179 gap blocks); event array at block + 44; reserved bytes zero; exit index = first logged sample after a restart; stale values inside gaps. E-17 aux content pointer = block + 70 on every block (695 of 695); argument orders (5), (4), (5,4); no padding after the long name (528/528). E-18 enumerator "size of index" and type-5 size field = 2. E-19 unit placement: M1 at 0x40 (0x48 empty); C125 short name at 0x40 + unit at 0x48; converter 20-byte unit spilling past 0x48. E-20 dps byte ≠ dec places on many M1 channels; C125 dps byte 0 with dec 2. E-21 +0x54/+0x58 = raw max/min of the logged data; never-logged channels carry 1,000,000 / 1,000,000,000. E-22 +0x5C/+0x60 mostly 0/0; M1 Gear 10/−10. E-23 type pairs: (7,4), (3,2), (5,4), (6,4), (0,0) terminator, (0,2) converter Bool, (8,8) in one file whose header names the converter device (producer unclear); no (7,2). E-24 M1 int32 channels use div 10000/1000 with exp 1–5, mul 1, shift 0, and decode to plausible engineering values; no file has shift ≠ 0. E-25 counters: M1 from 20101 with holes, C125 8091, converter 20101 + n. E-26 events: records at block + 0x2C; f64 µs with fractions; stored order not time-sorted; codes 0–52 seen; reserved bytes zero. E-27 event block location varies (0x3448, 0x3848, late in a continuation file). E-28 legacy converter layout (Q-W30). E-29 C125: unknown pointers at 0x004 and 0x02C, record +0x6C non-zero, channel list after the detail records near the end of the file.

### 6.3 Must be resolved empirically in phase B (spike S-1)

1. **Shift-vs-mul order:** still needs a real logger file with `shift ≠ 0 ∧ mul ≠ 1` (none among the 11 real logger files; D-037, Q-06).
2. **Sim-exporter dialect:** no ACC or third-party sim file is on the machine. Check u16@0x40, pro-logging = 0 (expected, user 2026-10-10), unit placement, type pairs, `num_channs`, missing detail records.
3. **float16 (7,2) and float64 (8,8):** whether they occur on logger files, and the producer of the (8,8) file.
4. **(6,4) beacon-tick channel:** encoding (5 Hz on M1) and whether it can feed laps without `.ldx`.
5. **Interpolation byte 0:** meaning on C-series files (unset vs closest); byte 4/5 handling.
6. **R5 unit rule case (c):** on C-series channels that have a short name but no unit.
7. **dps rule:** on C-series (byte 0, dec 2).
8. **Unknown blocks:** what 0x004, 0x02C, 0x68C, 0x687, 0x5A/0x5C and record +0x6C hold (preserve meanwhile).
9. **Beacon channel:** fraction units (×32768 vs ms) and the `floor(t) + 1` index, against i2's lap display of a converter file. This needs i2 observation (D-005 (3)) and is allowed only on MoTeC-device data; converter output is not, so use the notes and a real logger file with a beacon channel if one exists.
10. **Event codes 6–52:** meanings. Codes 0–52 occur in the user's two capture files, but only 0–5 have converter labels.
11. **Raw max/min sentinel pair (1e6, 1e9):** meaning.
12. **String encoding:** of non-ASCII details and names in files written by loggers and by i2.
13. **Legacy converter layouts** among the S-1 inputs: count them and decide whether `converter-legacy` needs more than an Info note.
14. **`num_channs` on files with a beacon channel and on sim exports.**

---

## 7. Proposed `CLEANROOM.md` rows

**Main table** (columns per 08 §11 and D-128). Two categories outside D-128's five are needed and are proposed in §8 item 2: *user-contributed own work* and *user-owned device logs*.

| Area | Behaviour | Category | Source | Date |
|---|---|---|---|---|
| `motec.ld` reader/writer | Header, detail-record, channel-record, gap-block, aux-block (long name, enumeration reference), enumeration-definition and log-event layouts; scaling as implemented; beacon-channel encoding | user-contributed own work | The author's Log File Converter (Qt C++ 9.1.0.23 `LdWriter`/`LdParser`, Python `ld_parser.py`/`write_ld_file`, the author's "LD Format descriptor v5" notes), contributed by its author under MIT (licence to be confirmed in writing). Knowledge derived by the author from analysing log files; no disassembly or decompilation of any MoTeC software | 2026-10-10 |
| `motec.ld` reader | Confirmation of layout facts E-1…E-29 (u16 channel count, u32 detail pointers, fixed record sizes, gap semantics `[entry, exit)`, aux-block chaining, dps byte, unit placement, event block) | user-owned device logs | Byte inspection of the user's own M1 and C125 logger files and converter outputs (private, never committed) | 2026-10-10 |
| `motec.ld` reader | Cross-check against public notes; differences recorded in `docs/formats/ld-converter-port.md` §6 | public project | Friss/i3rs `docs/ld-file-format.md` (MIT) | 2026-10-10 |
| `motec.ld` device events | Meaning of event codes 0–5 | observed on a licensed install, D-005 (3) | The converter author's observation of i2's display (`ChannelData.h` 50–62); Esses uses its own label wording, no i2 text copied | 2026-10-10 |
| `motec.ld` pro-logging field | **Excluded:** the converter's computation of the u32 at 0x5DE is not ported or documented; Esses never computes or validates it, writes 0, and records only zero/non-zero | rule | `ConversionUtils.cpp` 196–250 (not ported) | 2026-10-10 |
| `motec.ld` scaling order | `afterMul` (converter and i3rs) vs `beforeMul` (design default): unsettled, no real file with `shift ≠ 0` among the 11 logger files inspected | user-owned device logs | as row 2 | 2026-10-10 |

**Ported-from table.** Each row is added when its file is ported. The upstream copyright line comes from the author.

| File | Upstream project | URL | Commit | Licence | Upstream copyright line |
|---|---|---|---|---|---|
| `libs/formats/src/ld/ld_header.cpp` | Log File Converter (author's own) | private repository | revision of the 9.1.0.23 tree, to be recorded | MIT (to be confirmed) | to be supplied by the author |
| `libs/formats/src/ld/ld_channels.cpp` | same | same | same | same | same |
| `libs/formats/src/ld/ld_enums.cpp` | same | same | same | same | same |
| `libs/formats/src/ld/ld_validity.cpp` | same | same | same | same | same |
| `libs/formats/src/ld/ld_events.cpp` | same | same | same | same | same |
| `libs/formats/src/ld/ld_writer.cpp` | same | same | same | same | same |
| `libs/formats/src/ld/ld_encoding_auto.cpp` (optional) | same (`ConversionUtils.cpp` 26–166) | same | same | same | same |

**Third-party files and permissions table:**

| Item | Conclusion | Date |
|---|---|---|
| ACC `.ld`/`.ldx` | Per the user, not licensed by the vendor: i2 is never run on ACC files (D-038 (a)); real ACC files stay in `fixtures/private`; the public `synthetic-acc-dialect` family covers the dialect | 2026-10-10 |
| i2 install sample files | The user permits their use for testing: local oracles only, never committed (D-117, Q-06). The install holds only the Conversion Wizard samples (M2, `motec.csv`) | 2026-10-10 |
| User's converter outputs | User-owned files written by the user's own tool. Not oracles for dialect claims about logger files (D-041), but valid round-trip inputs for the `converter` dialect | 2026-10-10 |

---

## 8. Proposed amendments and open issues

1. **D-028 / Q-15:** the `.ld` part of the converter is used as a primary source in M0, ahead of the post-v1.0 harvest. This is the user's instruction of 2026-10-10. The author must confirm in writing (a converter `LICENSE` or a statement recorded in `CLEANROOM.md`) that `LdWriter`, `LdParser`, `ld_parser.py`, `write_ld_file` and the notes are their own work, MIT-licensed, with no GPL lineage. Until then, port nothing verbatim; implement from §1 in new code.
2. **D-128:** add the `CLEANROOM.md` categories *user-contributed own work* and *user-owned device logs*.
3. **05 §3.2–§3.3:**
   - u16 channel count at 0x56;
   - 0x40 = u16 + u32;
   - u32 venue/vehicle pointers (i3rs agrees, and C125 proves it);
   - fixed record sizes (event 3,156 B, venue 3,102 B, vehicle 3,316 B, weather 2,048 B);
   - weather pointer at event +0x484;
   - vehicle layout per §1.3;
   - pointer slots 0x688/0x68C/0x690.
4. **05 §3.4/§3.8/§3.9:**
   - Replace the `m1-212` stride dialect with pointer-chained gap and aux blocks for every file.
   - Add the R5 unit rule.
   - Enumeration tables via the 0x690 pointer, with the scan as a fallback.
   - Dialect flags `m1`, `c-series`, `converter`, `converter-legacy`, `sim-exporter`, `pro-logging`.
   - Retire the "212-byte entries" synthetic family in favour of "enum + long name + gap aux chains" (07 §3.1, AC-M0-01 wording).
5. **05 §3.5/§3.6:**
   - Display dps from byte 0x64 (R5).
   - Scale float dtypes too, unless the scaling is identity.
   - `div == 0` keeps `mul`.
   - (0,0) terminator record.
6. **D-037:** the two non-GPL sources (converter and i3rs) both use `afterMul`. Keep the default `beforeMul` as decided until a real file settles it, but record the disagreement. The user may prefer to flip the default now, because the only `beforeMul` source is the GPL project that 05 §3 says is superseded.
7. **05 §3.7, §3.11, §5 item 5:**
   - Validity comes from gap blocks: `Bitmap` chunks rather than "the format has no validity".
   - The `.ld` carries device events (0x688 → `DeviceEvent` markers).
   - The writer emits gap blocks, so integer encodings no longer lose validity (`ld.validity_lost` is retired).
8. **02 §4.1 / 05 §5 item 2:** `ChannelDescriptor` has no `sourceEncoding` member although 05 §5 relies on one, and 05 §3.2's `provenance.rawHeader` has no field in 02's `Provenance_Source`. Proposal: both live reader-side (`LdChannelEncoding` and a raw header/detail-record blob) and travel to the writer through `ExportView::source`, plus an opaque per-source `std::any`/`std::shared_ptr<const void>` "importer extension" on `DataSource`.
9. **02 §4.1 `EnumTable`:** add a per-entry `severity` (0 normal … 3 fault), so enumeration severities round-trip and can colour values.
10. **String encoding:** the design says cp1252; the converter writes UTF-8. Proposal: the reader decodes valid UTF-8 as UTF-8, else cp1252 (Info `ld.string_encoding`), until S-1 shows what loggers and i2 write. The writer writes cp1252 when representable, else UTF-8 with a note.
11. **New Import Log codes** to document in `docs/formats/ld.md` (AC-M0-12):
    - `ld.bad_meta_ptr`, `ld.terminator_entry`, `ld.dps_from_dec`, `ld.unit_from_short_name`
    - `ld.aux_out_of_range`, `ld.aux_unknown_arg`, `ld.enum_ref_missing`, `ld.enum_block_malformed`
    - `ld.gap_unpaired`, `ld.gap_unsorted`, `ld.gap_out_of_range`, `ld.gap_rate_mismatch`
    - `ld.event_block_unknown`, `ld.event_block_malformed`
    - `ld.beacon_channel`, `ld.legacy_layout`, `ld.string_encoding`
12. **Schedule:** about 3,700 production lines plus 1,400 test lines for `.ld` alone is heavy for M0 week 3 alongside `.ldx` and `SidecarDocument`. Expect the writer's dialect options and `ld_encoding_auto` to slip to week 4 or M1.
13. **Naming:** the user said a trademark search is not needed and the name may change (2026-10-10). The writer's device-type constant and every other codename-bearing literal therefore sit next to `cmake/naming.cmake` (08 §1) so `tools/rename.py` covers them.
