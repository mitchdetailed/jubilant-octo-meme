# synthetic-count-mismatch

synthetic: the header channel count at 0x056 says one channel more than the list holds.

- Exercises: ld.count_mismatch; the reader trusts the walk, never the count (05 section 14 item 5).
- Written by `esses-gen-fixtures --family synthetic-count-mismatch` through the `motec.ld` writer (dialect option `pc-layout`); licence CC0-1.0 (our own output).
- Byte edits after the writer: u16 at 0x056 (the channel count) incremented by one after the writer; device version pinned.
- `input.ld` is not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
