# synthetic-ldx-companion

synthetic: an .ldx companion with markers of every class, an unknown marker class, details overriding the .ld (Driver) and unknown elements at several levels.

- Exercises: unknown classes stay opaque (ldx.unknown_marker_class), unknown elements are kept (ldx.unknown_element); companion details win over the .ld (D-015); marker times keep fractions of a microsecond.
- Written by `esses-gen-fixtures --family synthetic-ldx-companion` through the `motec.ld` writer (dialect option `pc-layout`); licence CC0-1.0 (our own output).
- Byte edits after the writer: the .ldx is hand-written (unknown elements cannot come from a DataSource); device version pinned.
- The `.ldx` companion is hand-written by the generator.
- `input.ld` and `input.ldx` are not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
