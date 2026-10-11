# synthetic-header-only

synthetic: a header and detail records with no channel list (meta and data pointers at the end of the file).

- Exercises: a valid empty source with Warning ld.header_only; the oracle reports ld.meta_ptr_out_of_range and decodes no channel.
- Written by `esses-gen-fixtures --family synthetic-header-only` through the `motec.ld` writer (dialect option `pc-layout`); licence CC0-1.0 (our own output).
- Byte edits after the writer: device version pinned to 100 at 0x052.
- `input.ld` is not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
