# synthetic-sim-mi2

synthetic: a PC-software layout carrying the public motec-i2 writer's header defaults (u32 0x00010064 at 0x05A, u16 0 at 0x040).

- Exercises: parameterised from docs/formats/ld.md Producer fingerprints; dialect pc-layout,sim-exporter.
- Written by `esses-gen-fixtures --family synthetic-sim-mi2` through the `motec.ld` writer (dialect option `pc-layout`); licence CC0-1.0 (our own output).
- Byte edits after the writer: u16 at 0x040 set to 0 and u32 at 0x05A to 0x00010064 after the writer; device version pinned.
- `input.ld` is not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
