# synthetic-beacons-numbering

synthetic: BCN beacons for an out lap, three flying laps and an in lap, numbered from the Start Lap detail 7, with an SPLTBCN, an IGRDBCN and a device event inside laps.

- Exercises: AC-M1-07 / 02 section 4.5: flying laps are numbered from Start Lap (7, 8, 9), the out lap is 0 and the in lap takes the next number (10); SPLTBCN and IGRDBCN (ignoreClasses) and the device event (group DeviceEvents) create no lap; in and out laps are untrusted.
- Written by `esses-gen-fixtures --family synthetic-beacons-numbering` through the `motec.ld` writer (dialect option `pc-layout`); licence CC0-1.0 (our own output).
- Byte edits after the writer: device version pinned to 100 at 0x052.
- `input.ld` and `input.ldx` are not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens), except the `Outings:Laps` group of `expected.laps.json`, which the generator writes from a hand-checked lap table (AC-M1-07): SOL-B1 out (0), B1-B2, B2-B3, B3-B4 flying 7, 8, 9 (Start Lap 7), B4-EOL in (10); S1 (SPLTBCN), I1 (IGRDBCN) and the device event at 40 s are skipped; one stint.
