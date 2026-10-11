# synthetic-dtype-08-8

synthetic: the (dtype_a, dtype) pair (0x08, 8) with identity and non-identity scaling, written by esses-gen-fixtures through the motec.ld writer.

- Exercises: float pair: extremes, subnormals and signed zero; the scaled channel has scale 2 (ld.float_scaling_nonidentity).
- Written by `esses-gen-fixtures --family synthetic-dtype-08-8` through the `motec.ld` writer (dialect option `pc-layout`); licence CC0-1.0 (our own output).
- Byte edits after the writer: device version pinned to 100 at 0x052.
- `input.ld` is not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
