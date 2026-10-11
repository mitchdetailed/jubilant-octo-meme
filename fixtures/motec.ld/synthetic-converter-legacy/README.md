# synthetic-converter-legacy

synthetic: the legacy converter layout (channel list at 0x1336, extended records, no venue or vehicle records).

- Exercises: Info ld.legacy_layout; no long-name recovery.
- Written by `esses-gen-fixtures --family synthetic-converter-legacy` through the `motec.ld` writer (dialect option `converter-legacy`); licence CC0-1.0 (our own output).
- Byte edits after the writer: device version pinned to 100 at 0x052.
- `input.ld` is not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
