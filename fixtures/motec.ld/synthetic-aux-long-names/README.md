# synthetic-aux-long-names

synthetic: channel names longer than the 32-byte field (an aux long name each), one cut inside a multi-byte UTF-8 sequence, and a long name followed by an enumeration reference.

- Exercises: aux blocks after the records (record + aux, never a fixed stride); names are read from the aux block, the 32-byte field holds the cut prefix.
- Written by `esses-gen-fixtures --family synthetic-aux-long-names` through the `motec.ld` writer (dialect option `pc-layout`); licence CC0-1.0 (our own output).
- Byte edits after the writer: device version pinned to 100 at 0x052.
- `input.ld` is not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
