# synthetic-shift-mul-before

synthetic: channels with shift != 0 and mul != 1 (including the deg C -> deg F logger case) read with ld.shiftOrder beforeMul.

- Exercises: the same input bytes as synthetic-shift-mul-after; expectations from the oracle's beforeMul dump; afterMul is the default (D-037).
- Written by `esses-gen-fixtures --family synthetic-shift-mul-before` through the `motec.ld` writer (dialect option `pc-layout`); licence CC0-1.0 (our own output).
- Byte edits after the writer: device version pinned to 100 at 0x052.
- `input.ld` is not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
