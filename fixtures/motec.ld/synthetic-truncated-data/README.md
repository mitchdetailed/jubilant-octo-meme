# synthetic-truncated-data

synthetic: the file ends half way through the last sample array.

- Exercises: ld.data_out_of_range: the count is clamped to the samples present (reader and oracle).
- Written by `esses-gen-fixtures --family synthetic-truncated-data` through the `motec.ld` writer (dialect option `pc-layout`); licence CC0-1.0 (our own output).
- Byte edits after the writer: file cut after half of the last sample array plus one byte; device version pinned.
- `input.ld` is not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
