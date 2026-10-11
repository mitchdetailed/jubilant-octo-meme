# synthetic-aux-enum-212

synthetic: channels whose aux block holds only an enumeration reference, so each record plus its aux block is exactly 212 bytes (the i3rs 'M1 record' case), and an enumeration block with severities and the values -1 and -2.

- Exercises: the oracle reads the 212-byte record's enum fields and scans for the tables; Esses links them through the aux block (05 section 3.9).
- Written by `esses-gen-fixtures --family synthetic-aux-enum-212` through the `motec.ld` writer (dialect option `pc-layout`); licence CC0-1.0 (our own output).
- Byte edits after the writer: device version pinned to 100 at 0x052.
- `input.ld` is not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
