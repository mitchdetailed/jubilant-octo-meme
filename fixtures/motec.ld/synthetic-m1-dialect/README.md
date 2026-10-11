# synthetic-m1-dialect

synthetic: the m1 dialect (device type M1, a terminator record ending the list, units at 0x40), with a GPS date in dtype_a 0x06, an F64 latitude, an enumeration and a deg C -> deg F channel.

- Exercises: the terminator record is not a channel (ld.terminator_entry); 0x06/4 is a plain integer.
- Written by `esses-gen-fixtures --family synthetic-m1-dialect` through the `motec.ld` writer (dialect option `m1`); licence CC0-1.0 (our own output).
- `input.ld` is not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
