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
fixtures/private/                   never committed: the user's files (spike S-1 inventory in s1/) and the
                                    private manifest motec.ld/manifest.json built from it
```

Commands (from the repository root, with a built tree in `build/<dir>`):

```
build/<dir>/bin/esses-gen-fixtures --inputs-only                    # the inputs, into fixtures/motec.ld
build/<dir>/bin/esses-cli fixture verify fixtures/                  # AC-M0-01..03
build/<dir>/bin/esses-gen-fixtures                                  # regenerate inputs AND expectations
python -I tools/build_private_fixtures.py --root . --cli build/<dir>/bin/esses-cli   # private manifest
build/<dir>/bin/esses-cli fixture verify --manifest fixtures/private/motec.ld/manifest.json
```

ctest generates the inputs into the build tree and verifies through a manifest (`fixtures.generate_inputs`,
`fixtures.verify_public`), so concurrent builds never write here; `fixtures.verify_private` runs when the
private manifest exists and reports "skipped (private)" otherwise.

The 49 synthetic families cover every `(dtype_a, dtype)` pair of dtype_a 0x00 and 0x03 to 0x08 at widths 1, 2,
4 and 8 (`synthetic-dtype-<aa>-<w>`), multi-rate channels, `shift != 0` with `mul != 1` in both orders, scaling
edge cases, aux-chained records (long names; enumeration references, the 212-byte record + aux case), gap blocks,
32-byte names, a wrong header channel count, device events, a header-only file, a zero-rate record, a truncated
data array, non-finite floats, an `.ldx` with unknown elements, the m1, c-series, converter-legacy and
pc-layout dialects, the public sim writers' header fingerprints (`synthetic-sim-gotzl`, `synthetic-sim-mi2`)
and `synthetic-acc-dialect` (sim-exporter, ACC-style names, BCN laps, one hour at 1 kHz).
