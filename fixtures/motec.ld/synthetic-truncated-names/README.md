# synthetic-truncated-names

synthetic: names that fill the 32-byte field exactly (no NUL), as sim exporters cut them, in ASCII, cp1252 and UTF-8.

- Exercises: ld.name_unterminated; strings decode as UTF-8 when valid, else cp1252 (ld.string_encoding).
- Written by `esses-gen-fixtures --family synthetic-truncated-names` through the `motec.ld` writer (dialect option `pc-layout`); licence CC0-1.0 (our own output).
- Byte edits after the writer: device version pinned to 100 at 0x052.
- `input.ld` is not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
