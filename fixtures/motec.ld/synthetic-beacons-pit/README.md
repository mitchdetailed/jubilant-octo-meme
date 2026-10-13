# synthetic-beacons-pit

synthetic: four wheel speeds and a lap counter with BCN beacons; a 20 s wait in the pit box before the out lap, a 10 s stop in Lap 1, a 20 s pit stop in Lap 2, a counter reset in Lap 3, a locking front-left wheel and front wheels reading below zero while stationary.

- Exercises: AC-M1-07 / D-075, D-132: >= 15 s below 1 km/h of Corr Speed (fastest of the front wheels, clamped at 0) inside a lap sets only pitStop (class, number and trust unchanged); after a pit stop the next lap starts a new stint unless the car had not moved yet; a lap counter decrease starts a stint; beacon intervals never do.
- Written by `esses-gen-fixtures --family synthetic-beacons-pit` through the `motec.ld` writer (dialect option `pc-layout`); licence CC0-1.0 (our own output).
- Byte edits after the writer: device version pinned to 100 at 0x052.
- `input.ld` and `input.ldx` are not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens), except the `Outings:Laps` group of `expected.laps.json`, which the generator writes from a hand-checked lap table (AC-M1-07): out lap (pitStop: 20 s in the pit box, still stint 1), Lap 1 (10 s stop: no pitStop), Lap 2 (pitStop, trusted, still flying), Lap 3 stint 2, Lap 4 stint 3 (counter reset at 215 s), in lap 5 (10 s stop: no pitStop).
