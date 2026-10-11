# synthetic-c-series

synthetic: the c-series logger layout (device type C125, a 64 KiB configuration region before the event record, short names at 0x40 and units at 0x48).

- Exercises: unit rule (b)/(c): a unit in the short-name field with no unit is the unit; otherwise the short name stays a short name.
- Written by `esses-gen-fixtures --family synthetic-c-series` through the `motec.ld` writer (dialect option `c-series`); licence CC0-1.0 (our own output).
- `input.ld` is not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
