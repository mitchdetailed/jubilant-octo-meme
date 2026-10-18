# motec.csv / dialect-basic

Self-authored (CC0-1.0), written by `tools/gen_text_fixtures.py`; every value comes from a closed-form function of
the row. It reproduces the layout facts of the legacy CSV export (05 section 6): eleven quoted key/value metadata
lines with a second pair in columns 5-7, two blank lines, a names row and a units row, two blank lines, data;
every field double-quoted, CRLF, '.' decimal point, one sample rate (20.48 Hz, stated in the details and printed
with three decimals in the time column).

Exercised: details from the key/value columns (empty values are no detail), the rate from the `Sample Rate`
detail ({512, 25}) validated against the printed times, `Corr Dist` stored F64 by the profile's column override,
`-###` sentinels (invalid samples), empty cells held at the previous value (sparse "hold"), integer columns,
the `Lap` counter as the profile's fallback lap source (a beacon at each change).

Expected channel values: `expected.channels/` from the generator (oracle kind `generator`). `expected.probe.json`
is `esses-cli info --json` output reviewed against those facts.
