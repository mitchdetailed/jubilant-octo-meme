# synthetic-dtype-04-8

synthetic: the (dtype_a, dtype) pair (0x04, 8) with identity and non-identity scaling, written by esses-gen-fixtures through the motec.ld writer.

- Exercises: integer pair: the signed extremes of the width (width 8: +-(2^53 - 1)) and a ramp; the scaled channel has mul 3, scale 4, dec 1; dtype_a 0x04 is decoded unsigned by Esses and signed by the oracle, so the values keep the high bit clear (unsettled: no real file has 0x04, docs/formats/ld.md).
- Written by `esses-gen-fixtures --family synthetic-dtype-04-8` through the `motec.ld` writer (dialect option `pc-layout`); licence CC0-1.0 (our own output).
- Byte edits after the writer: device version pinned to 100 at 0x052.
- `input.ld` is not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
