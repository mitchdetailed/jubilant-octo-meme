# esses-cli

Layer 3, Qt-free (08 §3.5). The command line of 05 §9.2 in its M0 scope (07 §3.1): inspect logs, validate
documents and maintain the fixture corpus. `src/` is the internal library `esses_cli_core` (shared with
`esses-gen-fixtures` in `tools/` and the unit tests); `src/main.cpp` is the executable.

| Command | What it does |
|---|---|
| `info <file> [--json] [--importer id] [--options json] [--timing]` | sniff + probe; `--json` prints `probe_json` (`probe-1`). With `--options` the probe comes from the reader `open()` returns (options such as `ld.shiftOrder` change it), otherwise from `IImporter::probe` (O(header)) |
| `channels <file> [--json] [--no-sidecar]` | the channel table after the open pipeline: name, unit (the quantity's base unit), vendor unit, rate, dtype, kind, samples |
| `laps <file> [--json] [--no-sidecar]` | enabled markers (device events, `.ldx` markers, synthesised SOL/EOL) and the range-group tree of the circuit rules (`data/rules/circuit.whole-outing` and `circuit.beacon-pair` through `analysis::RuleRunner`, else the same rules built in code): outings, beacon-pair laps with classes, numbers and trust, and the phase-2 `stint`/`pitStop` annotations with Corr Speed from the built-in algorithm's default settings (AC-M1-07); `--json` prints `laps-1` |
| `validate <paths...> [--schema file] [--json] [--verbose]` | every JSON document against the schema its `$schema` names (`urn:esses:<name>-<major>`, `esses:`, https form); `draft-07` documents must load as schemas; directories are walked for `*.json` and files without `$schema` are skipped there |
| `fixture add <file> --format motec.ld --oracle ref-decode\|<dir> --out <dir> --licence <spdx> [--provenance <class>] [--private] [--validity strict\|finite-only] [--json] [...]` | writes a fixture directory (05 §9.1): `input.<ext>` and companions, `fixture.json`, `expected.probe.json`, `expected.markers.json`, `expected.laps.json` and `expected.channels/<NNN>_<name>.csv` **from the oracle** (`python -I tools/ld_ref_decode.py --dump-channel`), never from the reader (D-041). A public fixture needs `--licence` with a redistributable licence (checked before anything is written); `oracle.validity` defaults to `finite-only` for `--oracle ref-decode`, because the reference decoder does not decode gap blocks |
| `fixture verify [<dir>...] [--manifest file] [--if-present] [--update] [--json]` | re-probes, re-opens and re-decodes every fixture under the given directories (default `fixtures/`) and compares: input and companion sha256, `expected.probe.json`, the dialect, `expected.markers.json` and `expected.laps.json` byte for byte, every expected channel sample (timestamps exact; integers exact; F32/F64 within the fixture's ulp tolerance; validity per `oracle.validity`). A private fixture without its input is `SKIP (private)`; a public one fails. A manifest (`{"fixtures": [{"dir", "input"}]}`) points fixture directories at inputs elsewhere (the user's files, or inputs generated into a build tree). `--update` rewrites the probe/markers/laps goldens, the hashes and the dialect, never the channel values |

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
- `fixture diff`, `convert`, `export`, `units scan` and `validate --all` are later milestones; `validate`
  takes directories instead of `--all`. There is no `bench` subcommand: the measured paths need Qt Gui, so the
  budget rows come from `esses-plot-bench` and `esses-open-bench` under `bench/` (D-155, proposed; 07 §4.5).

Tests: `esses_cli_tests` (helpers), `cli.validate.*` (exit codes, every shipped JSON), `cli.demo_script` (the
synthetic part of the 07 §3.6 demo), and the corpus tests of `tools/CMakeLists.txt` (`fixtures.*` including
`fixtures.laps_golden`, AC-M1-07, `lint.check_importlog_codes`).
