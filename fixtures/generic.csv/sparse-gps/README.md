# generic.csv / sparse-gps

Self-authored (CC0-1.0), written by `tools/gen_text_fixtures.py`. A 100 Hz table with a units row in which the GPS
columns (`GPS Lat`, `GPS Lon`, 10 Hz) are filled on every tenth row only. Under the generic profile's sparse
"timestamped" rule a column with empty cells keeps only its own samples, each with the time of its row, in its
own time column; the dense columns stay uniform at 100 Hz. The coordinates have seven decimals (stored F64). A
`Lap` counter gives a beacon at each change.

Expected channel values: `expected.channels/` from the generator (oracle kind `generator`), with the times of the
GPS samples. `expected.probe.json` is `esses-cli info --json` output reviewed against those facts.
