# synthetic-multirate

synthetic: 14 channels at rates from 1 Hz to 1 kHz (including 3, 7, 33 and 333 Hz, whose periods are not whole microseconds) with different lengths.

- Exercises: timeAt(i) = floor((2 i 10^6 + rate) / (2 rate)); the layer lasts as long as its longest channel.
- Written by `esses-gen-fixtures --family synthetic-multirate` through the `motec.ld` writer (dialect option `pc-layout`); licence CC0-1.0 (our own output).
- Byte edits after the writer: device version pinned to 100 at 0x052.
- `input.ld` is not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
