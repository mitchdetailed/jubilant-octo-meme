# Fixture corpus

Layout and `fixture-1` are defined in 05 §9.1 (08 §6.7, D-033, D-040, D-041, D-127).

```
fixtures/motec.ld/synthetic-*/      public, CC0-1.0, written by esses-gen-fixtures (tools/gen_ld_fixtures.cpp)
  fixture.json                      fixture-1: input sha256 and size, provenance, licence, import options,
                                    dialect, oracle (ref-decode = tools/ld_ref_decode.py) and tolerances
  expected.probe.json               esses-cli info --json (probe-1)
  expected.markers.json             markers-1, after the .ldx merge, before the sidecar
  expected.laps.json                esses-cli laps --json --no-sidecar (laps-1)
  expected.channels/<NNN>_<name>.csv  the oracle's decoded values (never the Esses reader's, D-041)
  README.md                         what the family exercises, byte edits after the writer
  input.ld, input.ldx               generated, not committed (git-ignored); fixture.json pins their sha256
fixtures/motec.csv/<name>/          public, CC0-1.0, self-authored text inputs (M2, AC-M2-13), written by
fixtures/generic.csv/<name>/        tools/gen_text_fixtures.py: input.csv|.txt committed (small), fixture.json
                                    (oracle kind generator), expected.channels from the generator's own values,
                                    expected.probe/markers/laps.json from esses-cli, reviewed against the facts
                                    the generator states (README.md per family)
fixtures/track/synthetic-circuit/   public, CC0-1.0, written by tools/gen_track_fixtures.py (M3, AC-M3-04): a
                                    made-up circuit with its true centreline, inner and outer lines and corner
                                    apexes, and simulated GPS and IMU laps (README.md there); checked by
                                    fixtures.track_generator_current (--check), read by the analysis track tests
fixtures/private/                   never committed: the user's files (spike S-1 inventory in s1/) and the
                                    private manifest motec.ld/manifest.json built from it
```

Commands (from the repository root, with a built tree in `build/<dir>`):

```
build/<dir>/bin/esses-gen-fixtures --inputs-only                    # the inputs, into fixtures/motec.ld
build/<dir>/bin/esses-cli fixture verify fixtures/                  # AC-M0-01..03
build/<dir>/bin/esses-gen-fixtures                                  # regenerate inputs AND expectations
python -I tools/gen_text_fixtures.py --root .                       # the text families (--check: compare)
python -I tools/build_private_fixtures.py --root . --cli build/<dir>/bin/esses-cli   # private manifest
build/<dir>/bin/esses-cli fixture verify --manifest fixtures/private/motec.ld/manifest.json
```

ctest generates the inputs into the build tree and verifies through a manifest (`fixtures.generate_inputs`,
`fixtures.verify_public`), so concurrent builds never write here; `fixtures.verify_private` runs when the
private manifest exists and reports "skipped (private)" otherwise.

The 52 synthetic families cover every `(dtype_a, dtype)` pair of dtype_a 0x00 and 0x03 to 0x08 at widths 1, 2,
4 and 8 (`synthetic-dtype-<aa>-<w>`), multi-rate channels, `shift != 0` with `mul != 1` in both orders, scaling
edge cases, aux-chained records (long names; enumeration references, the 212-byte record + aux case), gap blocks,
32-byte names, a wrong header channel count, device events, a header-only file, a zero-rate record, a truncated
data array, non-finite floats, an `.ldx` with unknown elements, the m1, c-series, converter-legacy and
pc-layout dialects, the public sim writers' header fingerprints (`synthetic-sim-gotzl`, `synthetic-sim-mi2`)
`synthetic-acc-dialect` (sim-exporter, ACC-style names, BCN laps, one hour at 1 kHz) and the beacon families of
AC-M1-07: `synthetic-beacons-numbering` (Start Lap 7, SPLTBCN, IGRDBCN, a device event inside a lap),
`synthetic-beacons-reset` (a RESET inside a flying lap: partial lap, out lap, stint 2) and
`synthetic-beacons-pit` (four wheel speeds and a lap counter: a wait in the pit box, a 10 s stop, a 20 s pit
stop, a counter reset, a locking wheel, front wheels below zero while stationary).

For every family with beacons (those three, `synthetic-acc-dialect` and `synthetic-ldx-companion`) the
`Outings:Laps` group of `expected.laps.json` comes from a hand-checked lap table in the generator
(`Family::expectedLaps`, reasoned from the beacon-pair rule of 02 §4.5 and D-075/D-132), never from the code
under test; `fixtures.verify_public` and `fixtures.laps_golden` compare `esses-cli` against it.

The four text families (M2, AC-M2-13; `docs/formats/text.md`): `motec.csv/dialect-basic` (the legacy export
skeleton: quoted key/value preamble, names and units rows, CRLF, 20.48 Hz from the details, `-###`/`+###`
sentinels, held empty cells, a `Lap` counter), `generic.csv/semicolon-decimal-comma` (free-text preamble, `;` and
decimal comma, units in names, a text column), `generic.csv/sparse-gps` (a units row, 10 Hz GPS columns in a
100 Hz table: sparse timestamped channels with their own times, a lap counter) and `generic.csv/timestamped-tab`
(tab-separated epoch milliseconds with jitter, a pause, a repeated and an earlier row). `fixtures.verify_public`
checks them with the `.ld` families; `fixtures.text_generator_current` checks that the committed files are what
the generator writes.
