# synthetic-beacons-reset

synthetic: BCN beacons with a RESET marker (group "") inside the second flying lap, then an out lap, two flying laps and an in lap.

- Exercises: AC-M1-07 / 02 section 4.4 rules 11-13: BCN->RESET is a partial lap (Lap 2 (partial), untrusted, number 2 not consumed), RESET->BCN an out lap, the next flying lap is Lap 2; the RESET starts stint 2 and ordinary beacon intervals never split a stint.
- Written by `esses-gen-fixtures --family synthetic-beacons-reset` through the `motec.ld` writer (dialect option `pc-layout`); licence CC0-1.0 (our own output).
- Byte edits after the writer: device version pinned to 100 at 0x052.
- `input.ld` and `input.ldx` are not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens), except the `Outings:Laps` group of `expected.laps.json`, which the generator writes from a hand-checked lap table (AC-M1-07): SOL-B1 out, B1-B2 Lap 1, B2-R1 partial P2 (not consumed), R1-B3 out, B3-B4 Lap 2, B4-B5 Lap 3, B5-EOL in (4); stint 1 up to the RESET at 45 s, stint 2 from R1-B3; the fastest trusted flying lap is Lap 2 (19 s).
