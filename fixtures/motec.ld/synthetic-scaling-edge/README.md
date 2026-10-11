# synthetic-scaling-edge

synthetic: scale 0 and mul 0 (each read as 1), a negative scale, dec 5 and dec 7 (F64 storage).

- Exercises: ld.zero_scale, ld.zero_mul (Warning), ld.negative_scaling; dec >= 5 keeps F64.
- Written by `esses-gen-fixtures --family synthetic-scaling-edge` through the `motec.ld` writer (dialect option `pc-layout`); licence CC0-1.0 (our own output).
- Byte edits after the writer: device version pinned to 100 at 0x052.
- `input.ld` is not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
