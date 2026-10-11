# synthetic-zero-rate

synthetic: the middle of three records has rate 0 but samples.

- Exercises: Esses does not make it a channel (ld.zero_rate); the oracle decodes it untimed, and fixture verify reports it as excluded by the reader.
- Written by `esses-gen-fixtures --family synthetic-zero-rate` through the `motec.ld` writer (dialect option `pc-layout`); licence CC0-1.0 (our own output).
- Byte edits after the writer: u16 rate at record + 0x16 of the second record set to 0 after the writer; device version pinned.
- `input.ld` is not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
