# esses-cli

Layer 3, Qt-free (08 §3.5). The command line of 05 §9.2 in its M0 scope (07 §3.1): inspect logs, validate
documents and maintain the fixture corpus; from M2 also the maths commands of 03 §13 (`eval`, `maths check`,
`maths functions`), the project-aware `export` and `fixture diff` (05 §9.2, 07 §5.1, D-181); from M4 `convert` (05 §9.2,
D-214). `src/` is the internal library `esses_cli_core` (shared with
`esses-gen-fixtures` in `tools/` and the unit tests); `src/main.cpp` is the executable.

| Command | What it does |
|---|---|
| `info <file> [--json] [--importer id] [--options json] [--timing]` | sniff + probe; `--json` prints `probe_json` (`probe-1`). With `--options` the probe comes from the reader `open()` returns (options such as `ld.shiftOrder` change it), otherwise from `IImporter::probe` (O(header)) |
| `channels <file> [--json] [--no-sidecar]` | the channel table after the open pipeline: name, unit (the quantity's base unit), vendor unit, rate, dtype, kind, samples |
| `laps <file> [--json] [--no-sidecar]` | enabled markers (device events, `.ldx` markers, synthesised SOL/EOL) and the range-group tree of the circuit rules (`data/rules/circuit.whole-outing` and `circuit.beacon-pair` through `analysis::RuleRunner`, else the same rules built in code): outings, beacon-pair laps with classes, numbers and trust, and the phase-2 `stint`/`pitStop` annotations with Corr Speed from the built-in algorithm's default settings (AC-M1-07); `--json` prints `laps-1` |
| `validate [<paths...>] [--all [--root dir]] [--exclude dir]... [--schema file] [--json] [--verbose]` | every JSON document against the schema its `$schema` names (`urn:esses:<name>-<major>`, `esses:`, https form), after steps 1-2 of the loader (02 §6.6 rules 2 and 5, M2): a major newer than this build's schemas is refused at `/$schema`, an older minor is migrated through `esses_project`'s `Migrations` first and each step is printed (`--json`: `log`), a newer minor is validated as it is with a note (a major or `schemaVersion` beyond the integer range is read as newer than any, so that document is invalid or noted and the rest of the batch goes on); every error is printed with its JSON pointer; `draft-07` documents must load as schemas; directories are walked for `*.json` and files without `$schema` are skipped there. `--all` validates `data/`, `fixtures/`, `tests/maths/` and `schemas/*.schema.json` under `--root` (default `.`; 08 §5) without the private checkouts (`fixtures/private`, `tests/maths/user_modules`, `tests/maths/oracle`); `--exclude` leaves a directory out of every walk |
| `eval <expression> <logs...> [--layer L] [--rate auto\|<Hz>\|<num>/<den>] [--unit u] [--range "Outings:Laps/3"] [--compat unaryMinusAbovePower=1\|alias:<spelling>=<function>] [--precision single\|double] [--json\|--csv] [-o file] [--timing]` | parses, type-checks and compiles the expression against each log's channels (by name, then logged name), numeric and text details (03 §13, AC-M2-14), evaluates it over the layer extent at the plan's rate (auto: the highest input rate; no series input: the layer's highest logged rate; a `--rate` is exact, D-008: each of `<Hz>`, `<num>` and `<den>` is an unsigned decimal with an optional fraction and exponent, converted exactly and reduced, so `20.5` and `20.5/1` are 41/2 Hz and `33.333` is 33333/1000; a sign, a zero, a non-number or a num or den beyond 32 bits once reduced is exit 2 with the reason) on the calling thread, and prints `t_us value valid` rows (text), `t_us,value,valid` (`--csv`) or canonical JSON with `canonical` (the expression's canonical form), `diagnostics` and per log `rate`, `hz`, `t0Us`, `firstIndex`, `count`, `validCount`, `unit`, `dtype`, `diagnostics` and `values` (`null` for an invalid sample). `--unit` declares the result unit and reports the values in it; without it the values are the expression's numbers, labelled with the inferred unit. An F32 result prints as it is stored. Diagnostics go to stderr as `CODE level begin-end: message (hint)`. Exit 1 when the expression or a log fails |
| `maths check <modules or dirs...> [--log file] [--layer L] [--compat ...] [--json]` | loads maths-1 modules (`*.maths.json` under a directory) into one `MathGraph` in the order given (the project order) and reports each item's node (AC-M2-01, D-188): resolution and shadowing as the application does them, cycles (`E-CYCLE`), the synthesised filter and scale_offset items, compile and table errors and the module `condition`; with `--log` against the log's channels, details and the built-in modules the application registers, else without a source (references `I-UNRESOLVED`; an item that reads one is checked, not compiled). Per item `Ok`, `Warning`, `Error` or `Disabled` with its diagnostics (`W-DUP-IGNORED` on a second item of the same name), and the module `condition` as a row of its own (the type check, plus the graph's `E-CONDITION` when it refuses the condition; `Disabled` in a disabled module); a summary with counts per item status, per code and of parse errors (condition rows included), and `moduleErrors`: modules that do not load, enabled modules whose condition is in error, built-in modules that do not register. Exit 1 when an item is in error or `moduleErrors` is not 0. `tools/maths_corpus_check.py` reduces the report to counts |
| `maths functions [--json]` | the manifests of the function registry (03 §10.1) |
| `fixture add <file> --format motec.ld --oracle ref-decode\|<dir> --out <dir> --licence <spdx> [--provenance <class>] [--private] [--validity strict\|finite-only] [--json] [...]` | writes a fixture directory (05 §9.1): `input.<ext>` and companions, `fixture.json`, `expected.probe.json`, `expected.markers.json`, `expected.laps.json` and `expected.channels/<NNN>_<name>.csv` **from the oracle** (`python -I tools/ld_ref_decode.py --dump-channel`), never from the reader (D-041). A public fixture needs `--licence` with a redistributable licence (checked before anything is written); `oracle.validity` defaults to `finite-only` for `--oracle ref-decode`, because the reference decoder does not decode gap blocks |
| `export [<project>] <log> -o <file> [--channels a,b] [--range "Outings:Laps/3"] [--format csv\|motec.csv] [--rate Hz\|num/den] [--units "<channel>=<unit>,..."] [--layer L] [--importer id] [--options json] [--no-sidecar] [--json]` | opens the log as the application does for the project (its folder or `project.json`; without one, the built-in modules with their defaults): the project's mappings and unit maps at open, its maths modules in `mathsOrder`, the sidecar's local maths, the corrected speed and distance module with the project's settings and enable flag, the GPS corrections where both GPS inputs are logged, aliases and `options.maths.compat`. A name is the maths output when the graph has one (the head of its chain), else the logged channel (name, then logged name); an unknown name is exit 1; without `--channels` every logged channel no maths output shadows, then every maths output that evaluates. Units: a maths item's display unit, else its result unit, then the project's `overrides.json`, then `--units`, else the stored base unit. `--range` writes that range from time 0 and is the Main range of the selection-dependent maths. The maths evaluate on a `TaskPool` of the process (the CLI waits, 03 §7.4); the CSV is the plain (`csv`) or legacy (`motec.csv`) writer of 05 §6 on one grid (`--rate`, exact as for `eval` and at most 1 MHz, else the highest rate up to 1 kHz) |
| `convert <input> --format csv\|motec.csv\|motec.ld -o <out> [--channels a,b] [--range "Outings:Laps/3"] [--rate Hz\|num/den] [--layer L] [--writer-options json] [--importer id] [--options json] [--no-sidecar] [--json]` | reader to writer without a project or maths (05 §9.2; `--to` is an alias of `--format`): the log's logged channels (by name, then logged name; all of the layer without `--channels`) through the plain or legacy CSV writer (one grid: `--rate`, else the highest rate up to 1 kHz; `--range` from time 0 at its start) or the export-only `.ld` writer: a whole `.ld` is written channel by channel from its stored bytes (read, write, read is bit-exact for samples and validity, 05 §5 item 8); with `--range` each channel keeps the samples whose time lies in the range, from time 0, re-quantised with its stored encoding, and the beacons and device events inside it move with it. The `.ld` writer does not resample in 0.1: `--rate` with `motec.ld` is exit 2, and a channel it cannot hold (timestamped, a rate that is not a whole number of Hz, text) is skipped with `ld.channel_skipped` (listed under `skipped` of `--json`). The output never replaces the input: an output (or a `.ld` output's `.ldx`) that is the input, its `.ldx` or its sidecar (also through another spelling, a case difference where the file system ignores case, or a hard link) is exit 2 before the log is read. `--json`: `input`, `output`, `format`, `files`, `bytes`, `channels` (written), `skipped`, `range`, `notes` |
| `fixture verify [<dir>...] [--manifest file] [--if-present] [--update] [--json]` | re-probes, re-opens and re-decodes every fixture under the given directories (default `fixtures/`) and compares: input and companion sha256, `expected.probe.json`, the dialect, `expected.markers.json` and `expected.laps.json` byte for byte, every expected channel sample (timestamps exact; integers exact; F32/F64 within the fixture's ulp tolerance; validity per `oracle.validity`) (M2: timestamped channels compare their own time column). A private fixture without its input is `SKIP (private)`; a public one fails. A manifest (`{"fixtures": [{"dir", "input"}]}`) points fixture directories at inputs elsewhere (the user's files, or inputs generated into a build tree). `--update` rewrites the probe/markers/laps goldens, the hashes and the dialect, never the channel values |
| `fixture diff [<dir>...] [--channel name] [--json]` | `fixture verify`'s checks explained: per channel the samples compared, how many differ, the largest F32/F64 ulp distance and the first differing sample with both values; the JSON Pointer of every difference (at most 25 per file) in `expected.probe.json`, `expected.markers.json` and `expected.laps.json`, which shows missing markers and laps; `--channel` limits the report to one channel (reader or oracle name). Exit 1 when a fixture differs |

Every command that opens a log (`channels`, `laps`, `fixture add|verify`) runs the open pipeline with the circuit
rules as step 6 (`OpenContext::rangeRebuild`); `laps` and `fixture add|verify` then run phase 2 (`runRangePhase2`),
so `expected.laps.json` equals `laps --json --no-sidecar`.

Global options: `--data-dir` (default `<exe dir>/data`, where the build copies `data/units.json` and
`schemas/`), `--units`, `--schemas`, `--no-units`. Every `--json` output is canonical JSON (02 §6.5 rule 1,
through `esses_model`'s writer). Exit status: 0 success, 1 a check failed, 2 usage or environment error.

## Expected channel files

`expected.channels/<NNN>_<name>.csv`: `NNN` is the channel's position in the oracle's record walk, `<name>` the
oracle's 32-byte-field name, file-name encoded (`%XX` for path separators, reserved characters, `%` and
non-ASCII bytes). Header `index,timeUs,value,valid`; rows ascend and always include the last sample (the
reader's count is checked as last index + 1), so windows of a long channel are allowed (the private manifest
uses them). `value` is the oracle's F64 (Python repr), `valid` its finiteness; for synthetic families the
generator marks its declared integer gap ranges invalid (`oracle.validity` `strict`); for the user's files the
oracle cannot see gap blocks (`finite-only`: samples the reader marks invalid are counted, not compared). An
oracle channel the reader does not make a channel (`ld.zero_rate`, `ld.unknown_dtype`) is reported as excluded.

## Deviations from 05 §9 (M0)

- Expected channel files are plain `.csv` named by oracle index and name, not `<channel>.csv.gz`: no zlib is
  vendored, and the index keeps duplicate and truncated names unambiguous.
- `fixture-1` gained two optional members: `provenance` (synthetic, user-device-log, user-tool-output,
  third-party, vendor-sample) and `oracle.validity` (strict, finite-only).
- New schema `laps-1` (05 §15 item 15) for `laps --json` and `expected.laps.json`; `channels --json` has no
  schema yet.
- `fixture diff` and `export` arrived in M2, `convert` in M4; `units scan` is a later milestone; `validate --all` exists since M2
  (the `lint` job runs it). There is no `bench` subcommand: the measured paths need Qt Gui, so the
  budget rows come from `esses-plot-bench` and `esses-open-bench` under `bench/` (D-155, proposed; 07 §4.5).

Tests: `esses_cli_tests` (helpers; M2: `validate_tests.cpp` for migrations, the newer major, pointers, `--all`
and `--exclude`, a major or `schemaVersion` beyond the integer range; `document_roundtrip_tests.cpp` for the `unitmap-1` part of AC-M2-07), `cli.validate.*` (exit
codes, every shipped JSON, every fixture expectation, a broken file failing with its pointer, a migrated
`project-1`, a newer major; AC-M2-08), `cli.demo_script` (the
synthetic part of the 07 §3.6 demo), and the corpus tests of `tools/CMakeLists.txt` (`fixtures.*` including
`fixtures.laps_golden`, AC-M1-07, `lint.check_importlog_codes`). The maths commands (M2): `maths_cli_tests.cpp` (compat switches, the CLI's
source scope, diagnostic JSON, `eval` refusals, `maths check` statuses and counts, a module condition in error counted whatever the module holds), `cli_tests.cpp` (exact `--rate` parsing) and the ctest entries
`cli.eval.golden_gear` (an integer channel of the generated `synthetic-acc-dialect` input against the oracle's
values, D-041), `cli.eval.json_speed_lap` (`--json` rate, unit, dtype and values of one lap of the 1 kHz
speed), `cli.eval.json_rate_exact` (`--rate 20.5/1` evaluates at 41/2 Hz), `cli.eval.syntax_error` and `cli.eval.syntax_error_code`; `cli.export.project` (`export` with the project of
`tests/data/export-project` over the generated `synthetic-acc-dialect` input; `tests/export_check.py` checks the CSV
against relations that do not come from the code: a mapping applied at open, `'SPEED'[km/h] * 2` written in its
declared km/h, a selection-dependent `stat_max` equal to the lap's maximum, an override unit, the time grid, exit 1
for an unknown channel) and `cli.fixture_diff.explains` (an altered copy of `fixtures/generic.csv/sparse-gps`).

## Deviations from 03 §13 (M2)

- `eval` evaluates on the calling thread, not on the TaskPool, and resolves logged channels (by name, then logged
  name) and the source's details only: maths outputs (other items, `Corr Speed`/`Corr Dist`) need `MathGraph::define`
  (D-166, proposed). `--main`/`--ref` (the selection of the variance functions) are not offered yet.
- `eval --json` gives the values as one array (`null` for an invalid sample) with `rate`, `t0Us` and
  `firstIndex`, from which each sample's time follows, rather than `{t_us, value, valid}` objects; `--csv` and the
  text output print the rows. An F32 result prints as the shortest decimal that reads back to the stored F32 value.
- AC-M2-14's command evaluates `'Ground Speed' * 2` over `synthetic-acc-dialect`, whose speed channel is logged as
  `SPEED`: the criterion reads `'SPEED'` (D-184, proposed); the ctest uses `'SPEED'[m/s] * 2`. With a project,
  `export` resolves `'Ground Speed'` through the project's mapping or alias.
- `export` writes CSV only (`--format csv|motec.csv`); `motec.ld` output waits for `convert` (05 §9.2 makes `export`
  its alias). See D-181 for the name, unit and selection rules.

## M4: `convert` and the golden-JSON tests of item 11 (D-214)

- `convert` takes `--format` (05 §9.2 spells it `--to <writerId>`, kept as an alias) and writes logged channels only:
  the project-aware path with maths stays `export` (D-181). `--range` on `motec.ld` cuts the layer in the CLI
  because the export-only writer writes whole layers (`ld_export.cpp`, M0); resampling for `.ld` (05 §5 item 4) waits
  for the export UI of v1.0. `--writer-options` passes `WriterOptions::options` (e.g. `{"ld":{"dialect":"m1"}}`).
- `export --format motec.ld` stays refused (exit 2), now naming `convert`.
- Tests (label `cli`; 07 §2.2 item 11, AC-M4-01): `cli.golden.info`, `.channels`, `.laps`, `.eval`, `.convert`,
  `.export`, `.validate` and `.fixture`, each `tests/cli_golden.py <command>`: `info --json` equals the fixtures'
  `expected.probe.json` and agrees with `tools/ld_ref_decode.py`; `channels --json` against the reference decoder (the
  unit rule (c) of 05 §3.4 applied to its raw fields) and a text family's generator files; `laps --json` equals
  `expected.laps.json`; `eval --json` is 2 v + 1 of the oracle at the channel's rate; `convert` round-trips six
  families `.ld` -> `motec.ld` -> `fixture add --oracle ref-decode` (the oracle CSVs byte for byte; for the strict
  gap family the values, and the reader's validity through `eval`), `fixture verify` on the converted file, markers
  unchanged, a lap cut by `--range` to exactly its oracle samples, csv and motec.csv on the lap's grid with the
  motec.csv read back, and the refusals; `export --json` without a project; `validate --json --schema` over every
  `schemas/examples/<schema>` (statuses from the directory layout, error pointers from `expect.json`);
  `fixture verify --json` over a manifest and `fixture add` reproducing the committed oracle. The `bench` row of item
  11 is the benchmark executables' (D-155: `ctest -L bench`). Unit tests: `tests/convert_tests.cpp` (the overwrite
  rule, the usage errors before the log is read).
