# synthetic-dtype-08-4

synthetic: the (dtype_a, dtype) pair (0x08, 4) with identity and non-identity scaling, written by esses-gen-fixtures through the motec.ld writer.

- Exercises: integer pair: the signed extremes of the width (width 8: +-(2^53 - 1)) and a ramp; the scaled channel has mul 3, scale 4, dec 1.
- Written by `esses-gen-fixtures --family synthetic-dtype-08-4` through the `motec.ld` writer (dialect option `pc-layout`); licence CC0-1.0 (our own output).
- Byte edits after the writer: device version pinned to 100 at 0x052.
- `input.ld` is not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
