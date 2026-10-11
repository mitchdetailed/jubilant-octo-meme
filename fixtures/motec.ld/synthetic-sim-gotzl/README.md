# synthetic-sim-gotzl

synthetic: the sim-exporter dialect with the public gotzl-style writer's header defaults (device ADL, serial 0x1F44, version 420, 0xADB0 at 0x054, counters 0x2EE1 + index).

- Exercises: parameterised from docs/formats/ld.md Producer fingerprints; no code or template of any exporter was used (D-040).
- Written by `esses-gen-fixtures --family synthetic-sim-gotzl` through the `motec.ld` writer (dialect option `sim-exporter`); licence CC0-1.0 (our own output).
- `input.ld` is not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
