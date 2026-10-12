# synthetic-acc-dialect

synthetic: the sim-exporter dialect with ACC-style channel names and units, an .ldx with BCN beacons for an out lap, 12 flying laps and an in lap, and a one-hour session with 1 kHz channels.

- Exercises: 08 section 6.7 / D-127: public stand-in for a real ACC export (which stays private); the oracle compares GEAR and FUEL (channelsCompared), the 1 kHz channels are covered by the dtype families.
- Written by `esses-gen-fixtures --family synthetic-acc-dialect` through the `motec.ld` writer (dialect option `sim-exporter`); licence CC0-1.0 (our own output).
- `input.ld` and `input.ldx` are not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens), except the `Outings:Laps` group of `expected.laps.json`, which the generator writes from a hand-checked lap table (AC-M1-07): out lap, flying laps 1 to 12 (Start Lap absent: numbering from 1), in lap 13; one stint.
