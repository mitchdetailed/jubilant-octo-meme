# synthetic-device-events

synthetic: a log-event block (header pointer 0x688) with codes 0 to 5, 17 and 52, two events at the same time and a restart stored out of time order.

- Exercises: device events are markers of class DeviceEvent in group DeviceEvents, ordered by (time, stored order); labels for codes 0 to 5, 'Event <code>' otherwise.
- Written by `esses-gen-fixtures --family synthetic-device-events` through the `motec.ld` writer (dialect option `pc-layout`); licence CC0-1.0 (our own output).
- Byte edits after the writer: device version pinned to 100 at 0x052.
- `input.ld` is not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` pins the sha256).
- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader. `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output (regression goldens).
