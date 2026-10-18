# Text importers as implemented (M2)

This page records how `esses_formats` reads and writes delimited text in M2: the `TextEngine` and its
profiles (05 §7), the two shipped profiles `motec.csv` and `generic.csv` (05 §6, §7.3), dictionaries and unit
maps in the open pipeline (05 §2 step 4, §8), the CSV writers (05 §6 "Writer") and every Import Log code the
text engine emits. The design is 05 §6–§8 and D-028/D-029; where this page differs, the difference is listed
in "Departures" and in `docs/design/DECISIONS.md` (D-167, D-168, D-190).

Sources: `libs/formats/src/text/` (`text_scan`, `text_analyse` = pass 1, `text_decode` = pass 2,
`text_engine` = the importer, reader and slab decoder, `text_profile`, `dictionary`, `unit_mapping_profile`,
`csv_writers`) and the M2 part of `libs/formats/src/registry.cpp` (the pipeline).

## Shipped profiles

| File | Importer id | Sniff | Notes |
|---|---|---|---|
| `data/profiles-text/motec.csv.json` | `motec.csv` | the first-line format name (95); `.csv` alone 20 | quoted key/value preamble (`keyValueColumns` `[0,1,2]`, `[4,5,6]`), names and units rows, the rate from the `Sample Rate` detail, sparse `hold`, `Corr Dist` stored F64, lap source `lap_table` (placeholder, D-106) falling back to a `Lap` counter |
| `data/profiles-text/generic.csv.json` | `text.profile:generic.csv` | `.csv .txt .tsv .asc .log` (30); delimited text under another extension (10) | every `auto`/`@auto` rule, sparse `timestamped`, a lap counter column found by name |

`ImporterRegistry::withBuiltins` registers both (after `motec.ld`) together with their dictionaries
`data/dictionaries/motec.csv.json` and `generic.csv.json`; the build embeds the four files
(`builtin_text_profiles.hpp`), so the CLI, the fuzz targets and the tests need no data directory. User profiles
are added with `ImporterRegistry::addProfile` (an id that collides with a registered importer is refused, and
only the built-in id `motec.csv` may be named as `importerId`).

## Pass 1 (open and probe)

1. **Encoding.** BOM (UTF-8, UTF-16LE/BE), else valid UTF-8 over the first 64 KiB (a sequence cut at the end of
   that window is tolerated when its continuation bytes are), else cp1252. UTF-16 files are transcoded to UTF-8
   in memory at open (from a guarded copy of the file; an odd last byte is dropped with a `text.encoding`
   Warning, "odd byte count"); cp1252 files are read in place and their strings transcoded. A string that is not
   valid UTF-8 in a UTF-8 file falls back to cp1252 for that string, as the `.ld` strings do.
2. **Lines.** `\n` ends a line (a `\r` before it is dropped); a file without any `\n` in its first 64 KiB uses
   `\r` (old Mac files). A line of spaces, tabs and `\r` only is blank. Quoted fields do not span lines: a line
   of the head (the first 4,096 lines or 1 MiB) that ends inside an open quoted field is reported
   (`text.quoted_newline`, the first such line and their count), since its row was split there. The head ends
   before a line longer than 1 MiB; a file whose first non-blank line is that long is not a table (Format, "line
   N exceeds the maximum line length"). Cells are split up to what a row can use: 65,537 in the header search
   (a names row of more than 65,536 cells is Format), the header's count plus two after it, in both passes.
3. **Delimiter** (`auto`): over the last (up to) 50 non-blank lines of the first 4,096 lines, the first of tab,
   `;`, `,`, `|`, space whose count outside quotes is the same and non-zero on every line; else the most
   frequent on the first of those lines; else `,`.
4. **Preamble.** `{lines: n}`, `{untilBlank: true}` (the first blank line ends it), `{untilRegex}` (the matching
   line is the names row) or `@auto`: the names row is the first line with at least two fields and at most
   half of them numbers, followed by a line at least half numeric, or by a units-like line (no number, every
   cell at most 12 characters) and then such a line. Captures: with `keyValueColumns` the `capture` map goes
   from the file's key to a detail id or an `@input`; without, from a detail id or `@input` to a regular
   expression whose first group is the value, applied line by line (first match per capture). Details are
   text, or a number with its unit when a key/value line gives a unit and the value parses; empty values make
   no detail. Unmatched lines go to `unmatchedTo` when it is set. A file whose every line is in the head and
   that has no numeric line is a header without data under `@auto`: its last line with at least two mostly
   non-numeric fields, one of them the time column, is the names row (or the line above it, with this one as the
   units row, when this one is units-like and the profile looks for a units row); it opens with no samples
   (`text.no_data_rows`).
5. **Header.** Rows count among non-blank lines after the preamble (`skipBlank`). `unitsRow` is a row, `null`
   or `@auto` (the row after the names when it is units-like); `unitInName` takes the unit from the name when
   the units row has none and removes it from the name. A trailing delimiter (an empty last header cell, or
   `trailingDelimiter: true`) is dropped from every row; empty names are dropped (`ignoreEmptyNames`) or named
   `Column <n>`; duplicates get ` (2)`, ` (3)`, ... (`duplicateNames: suffix`) or fail (`error`).
6. **Time column.** By name (exact, then case-folded), or `@auto`: the first name matching
   `^(time|t|elapsed.*|secs?|timestamp)$` (case-insensitive), else the first numeric column that never
   decreases and increases at least once over the first 20 data rows. The time column is the time base, not a
   channel. Unit `@auto`: the column's own unit (`s`, `sec`, `secs`, `ms`, `us`), with epoch values recognised
   by magnitude (first value above 10^12 in ms, above 10^9 in s), else `hh:mm:ss[.fff]` text, epoch by magnitude,
   whole numbers above 10^4 as ms, else s. Origin: `first` (the first row is time 0; the default for epoch and
   clock units, which also set the layer's UTC anchor for epoch units) or `zero` (times as written).
7. **Rows and samples.** One memchr pass counts the data rows (non-blank lines after the header), records where
   every 65,536th row starts and keeps the first 256 rows and every 1,000th (and the last) as samples.
8. **Decimal point** (`auto`): `,` when the delimiter is not `,` and, among the number-like tokens of the first
   50 sampled rows that carry a separator, at least 90 % carry exactly one `,` (whole numbers say nothing).
9. **Columns.** From the sampled rows (sentinels and empty cells skipped): more than half not numeric: `Enum`
   (I32 ids of the labels in first-seen order over the whole file); all whole numbers: `Integer` (F32, F64 above
   2^24); else `Real` (F64 when a value has more than 7 significant digits, else F32). `dps` is the most
   fraction digits seen (at most 6). The profile's `columns` override kind, dtype, dps, interpolation,
   sentinels and the unit; its `name` and `quantity` are the column hint of pipeline step 4a.
10. **Rate.** A fixed `{num, den}`; `fromDetail:@x` (the decimal text of the `@x` input as an exact rational:
    `20.480` gives `{512, 25}`); or `infer`: the candidates of 05 §7.1 (the rationals with denominator at most
    1,000 within 0.1 % of `1/step`, smallest denominator first), each checked against the sampled rows; the
    first that fits wins. The check: `|t_i − (t0 + i/rate)| ≤ 0.6/rate` with `t0` from the first sampled row.
    A violation makes the file timestamped (`text.time_rate_mismatch` when the profile stated the rate). No
    time column and no rate: one sample per second (`text.no_time_column`).
11. **Full scan**, only when needed (timestamped time, sparse `timestamped`, an `Enum` column or a lap source
    that reads rows): enumeration labels, the time rule of timestamped sets (below), each sparse column's own
    chunk index, lap-counter beacons.
12. **Hold carry-in** (sparse `hold`): for every chunk start after the first, the last non-empty cell of each
    column before it, found by scanning back to the previous chunk start (and inherited from there when the
    column stayed empty). A sentinel carries an invalid value.

A file on disk is read through its mapping in guarded windows (`GuardedLines`: 4 MiB copies through
`MappedFile::guarded()`; the hold carry-ins scan back through 64 KiB copies), so a file truncated under the
mapping is Io, never a crash (D-190); a line longer than 16 MiB is read as its first 16 MiB in both passes.
Pass 1 stops at its 65,536-row checkpoints when the open is cancelled (`ImportOptions::cancel`).

### Row sets and the time rule

The dense set holds every column without an own index. With a uniform time column every data row is one
sample. With a timestamped one, a row is a sample when its time parses and is later than the previous sample's:
a row with the same time as the pending sample replaces it (last wins, 02 §3.3 rule 12) and an earlier row is
dropped; the dense set's channels share one I64 time column. Under sparse `timestamped` a column with an
**empty** cell keeps only the rows where its cell is not empty, under the same rule, with its own time column
(a **missing** cell, in a row shorter than the header, is an invalid sample instead, in both passes). Each set
records where
every chunk of 65,536 samples starts (offset, line, the previous sample's time, the carried values), so any
chunk decodes alone and re-decodes identically.

## Pass 2 (chunks)

`decodeChunk` sweeps one chunk of one row set for every requested column: quotes and doubled quotes, trimming,
the decimal point through fast_float (a leading `+` accepted, `inf`/`nan` not numbers), sentinels (exact text
after trimming) invalid, empty cells held (`hold`), labels to enumeration ids, the conversion to the base unit
of step 4 in F64 before the one narrowing. Invalid samples are NaN with a cleared bit (floats) or a cleared bit
(I32). The **slab decoder** shared by all providers of a source decodes a chunk of a set for every column at
once: the requesting column's chunk goes to its builder, the chunks of columns whose threads are already waiting
for the same sweep are handed to them, and the rest are sealed into the ChunkCache with `insertGroup` (through
`store::WeakColumn`, so the decoder keeps no column alive). A slab is parsed once however many of its columns
are drawn. Readers are chunk-independent. Before each chunk of a file on disk the decoder checks that the file
has not changed (size and time against the open): a changed file fails the chunk with Io and
`text.source_changed`, and the provider seals it invalid (`open.read_error`); its bytes are read through the
same guarded windows as pass 1.

## Pipeline step 4 (all importers)

- **4a** the reader's column hint (the profile's `columns`), then the importer's dictionary entry
  (`ImportOptions::applyDictionary`): name (an analysis name; `loggedName` stays), short name, the unit spelling
  looked up, the quantity, kind, interpolation and storage dtype.
- **4b** the unit map of the importer (`ImportOptions::unitMap`, else `OpenContext::unitMaps[<stem>]`), most
  specific rule first, then the unit registry's spelling match; samples are converted to the quantity's base
  unit at decode. A rule's `analysisName` becomes a load-time Mapping.
- **4c** `OpenContext::mappings` and those analysis names through `model::applyMappings`.
- **Unresolved** = a vendor unit nothing knows, or a quantity without a unit. With a `MappingHook`, task A ends
  with a `PendingResolution` (rows with up to five decoded sample values); `resume` runs task B with the
  `MappingDecision` (its unit map, its mappings; whatever stays unresolved opens unitless). Without a hook the
  channel opens unitless with the status text `Unit unknown: "<unit>"` and `open.unit_unknown`;
  `strictUnits` fails with NotFound. A channel with neither unit nor quantity opens unitless and is not
  unresolved.

## Dialect flags

`probe_json.dialect` of the text importers, in this order: the delimiter when it is not a comma (`tab`,
`semicolon`, `pipe`, `space`), `decimal-comma`, `timestamped` (the time column is not uniform).

## Import Log codes

Pass 1 (in `probe_json.warnings`):

| Code | Level | Meaning (`ctx.detail`) |
|---|---|---|
| `text.encoding` | Info | the file is not plain UTF-8: `utf-8-bom`, `utf-16le`, `utf-16be` (transcoded at open) or `cp1252`; as a Warning, `odd byte count`: a UTF-16 file's lone last byte was dropped |
| `text.auto_delimiter` | Info | the delimiter `auto` chose: `comma`, `semicolon`, `tab`, `pipe`, `space` |
| `text.auto_decimal` | Info | the decimal point `auto` chose |
| `text.auto_preamble` | Info | the number of non-blank preamble lines the `@auto` rule found |
| `text.auto_units_row` | Info | `yes` or `no` for `unitsRow: "@auto"` |
| `text.auto_time_column` | Info | the time column `@auto` chose, or `none` |
| `text.auto_time_unit` | Info | `s`, `ms`, `us`, `hhmmss`, `epoch_s` or `epoch_ms` |
| `text.rate_inferred` | Info | the rate inferred from the time column |
| `text.rate_from_detail` | Info | the rate read from the details (`fromDetail`) |
| `text.rate_default` | Warning | too few rows to infer a rate: one sample per second |
| `text.no_time_column` | Warning | no time column and no rate: one sample per second |
| `text.time_rate_mismatch` | Warning | the time column does not follow the profile's rate: the channels are timestamped |
| `text.timestamped` | Info | the time column is not uniform: the channels are timestamped |
| `text.time_not_monotonic` | Warning | rows whose time goes back were dropped (count) |
| `text.time_duplicate` | Info | rows with the pending sample's time replaced it (count) |
| `text.time_invalid` | Warning | rows without a valid time were skipped in a timestamped file (count) |
| `text.duplicate_name` | Info | a duplicate column name got a suffix (the new name) |
| `text.empty_name` | Info | a column without a name was dropped |
| `text.sparse_column` | Info | a column with empty cells keeps its own samples and times (sparse `timestamped`) |
| `text.text_column` | Info | a column of text becomes an enumeration |
| `text.enum_overflow` | Warning | a text column has more than 65,536 distinct values; the rest are invalid |
| `text.no_data_rows` | Warning | the file has a header but no data rows |
| `text.quoted_newline` | Warning | a line of the head ends inside a quoted field (its row was split there; `ctx.line` of the first, `repeats`) |
| `text.lap_source` | Info | the lap source used (`lap_counter_edge:<column>`, `beacon_event`) |
| `text.lap_source_absent` | Info | the profile's lap source (and its fallbacks) are not in this file |
| `text.lap_table_unverified` | Warning | the file has the details key of a `lap_table` source; lap tables are not read until verified (D-106) |
| `text.capture_unparsed` | Warning | the input the profile reads the rate from is missing or unreadable |

Pass 2 (the source's `DecodeLog`, coalesced per code and channel):

| Code | Level | Meaning |
|---|---|---|
| `text.ragged_row` | Warning | rows with another number of cells than the header (`ctx.line` of the first, `repeats`); missing cells are invalid |
| `text.parse_error` | Warning | cells that are neither a number nor a sentinel (the first, cut to 32 bytes); invalid |
| `text.source_changed` | Warning | the file changed on disk after it was opened (the chunk fails and is sealed invalid), or has fewer rows than then (the missing samples are invalid) |

Pipeline (M2): `open.unitmap_load_failed` (Warning) — the unit map named by `ImportOptions::unitMap` could not
be read; the source opens without it.

The application's Mapping-panel flow (`esses_qtmodel` `SourceLoader`, 06 §13.8): `open.mapping_resolved` (Info) —
task B was submitted with the panel's decision (the count of unresolved channels; "opened anyway" when they stay
unitless); `open.mapping_cancelled` (Info) — the pending open was dropped by Cancel, or when the project
changed or the files were closed; `open.cancelled` (Info) — an open in flight was cancelled for the same
reasons (its result is dropped); `sidecar.write_failed` (Warning) — the sidecar written 500 ms after an edit, or
when its source closed, could not be written (D-190).

Writer notes: `csv.unit_not_applied` (a display unit of another quantity; base units written),
`csv.channel_missing` (a channel of the export view is not in the layer).

## Writers

`motec.csv` writes the legacy skeleton of 05 §6 (eleven key/value lines in the observed order with values from
the details, two blank lines, names, units, two blank lines, data; every field quoted, CRLF, `.` decimal
point; `""` for invalid samples). `csv` writes names, units and data, LF, fields quoted only when needed. Both
resample every channel onto one grid (`ExportView::resampleRate`, by default the highest uniform rate of the
view capped at 1 kHz) with the channel's interpolation (`store::Resampler`); Time counts from 0 at the range
start (three decimals when the period is a whole number of milliseconds, else six). Values are in the stored
base unit unless `{"csv": {"units": {"<channel>": "<unit>"}}}` names a display unit.

## Departures from 05 §6–§8 (D-167, D-168, D-190)

- The `blocks` layout (sectioned files, D-102) is parsed and validated but not read: opening such a profile's
  file fails with Unsupported until the Pi profile arrives (v1.0).
- The sniff regex sees the first 16 lines within the first 4 KiB of the file and `^` anchors at every line
  start on every standard library (std::regex has no portable multi-line flag); capture, `untilRegex`,
  `unitInName` and dictionary `nameRegex` patterns see one line or one name, each cut to 4 KiB. A pattern with
  a repeated group that repeats inside, unless each alternative holds a character that must occur exactly
  once (`(a+)+` and `(\w+\s?)*` are refused, `(\.[a-z]+)*` is not), is refused in a profile or dictionary
  (libstdc++ and libc++ bound neither its time nor its stack); dictionary patterns are compiled once (D-190).
- `@auto` names row: the following data line must be at least half numeric (05 §7.3: 80 %), so tables with a
  text column in three are found.
- `decimal: auto` counts only tokens with a separator (05 §7.2 counts every numeric token, so whole-number
  columns would hide a decimal comma).
- The rate search checks each candidate rational against the time column (05 §7.1 takes the smallest
  denominator only, which picks 41/2 for 20.48 Hz).
- An unresolved unit is a vendor unit nothing knows, or a quantity without a unit; a column with neither opens
  unitless without the Mapping panel (05 §8.2: "complete" when every channel has a quantity and unit).
- The slab share key `slab/<sourceId>/<k>` is the slab decoder's in-flight table, not a TaskPool key; sparse
  columns decode per column (their chunks do not align with the dense slabs).
- Column classification samples the first 256 rows and every 1,000th (05 §7.1: the first 256 values), so a
  column that is zero at standstill is not taken for an integer column.
- `capture` maps the file's key to the detail id with `keyValueColumns` and the detail id to a regular
  expression without (05 §7.2 writes one direction; its two examples use both).
- Text files are read through the page-fault guard of the `.ld` reader by copying windows (D-190 withdraws
  D-167's departure); the registry's sniff still reads the first 64 KiB of the mapping in place.
- Quoted fields holding a line break (RFC 4180) are not joined into one row: the line ends the row and
  `text.quoted_newline` says where (D-190).

Provenance: written from 05 §6–§8 and the research summaries (layout facts of the legacy CSV export); no
vendor file content is reproduced. The fixtures under `fixtures/motec.csv/` and `fixtures/generic.csv/` are
self-authored by `tools/gen_text_fixtures.py` (CC0-1.0).
