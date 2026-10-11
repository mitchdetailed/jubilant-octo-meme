# synthetic-gap-blocks

synthetic: gap blocks (invalid sample ranges [entry, exit)) at the start, middle and end of float and integer channels, adjacent ranges, and a channel invalid throughout.

- Exercises: float gaps are stored as NaN, integer gaps hold the previous valid value (05 section 5 item 5); the expected validity of integer gaps comes from the generator's declaration (oracle.validity strict).
- Written by `esses-gen-fixtures --family synthetic-gap-blocks` through the `motec.ld` writer (dialect option `pc-layout`); licence CC0-1.0 (our own output).
- Byte edits after the writer: device version pinned to 100 at 0x052.
- `input.ld` is not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader; the valid column of integer gap ranges comes from the generator's declaration. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
