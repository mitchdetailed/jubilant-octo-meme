# synthetic-nonfinite

synthetic: float channels holding NaN, +-infinity, signed zeros and subnormals.

- Exercises: a NaN sample is invalid (S-1 D13); infinities are values; the oracle marks every non-finite value invalid in its CSV, which fixture verify reads as 'not finite'.
- Written by `esses-gen-fixtures --family synthetic-nonfinite` through the `motec.ld` writer (dialect option `pc-layout`); licence CC0-1.0 (our own output).
- Byte edits after the writer: device version pinned to 100 at 0x052.
- `input.ld` is not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
