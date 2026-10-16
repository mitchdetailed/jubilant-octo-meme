# generic.csv / semicolon-decimal-comma

Self-authored (CC0-1.0), written by `tools/gen_text_fixtures.py`. A logger-style export the generic profile reads
with every `auto` rule: a two-line free-text preamble and a blank line, then a names row with units in brackets
(`Speed (km/h)`), ';' as the delimiter and ',' as the decimal point, LF line ends, 50 Hz (inferred from the time
column), an integer column and a text column (`Mode`, an enumeration in first-seen order).

Expected channel values: `expected.channels/` from the generator (oracle kind `generator`). `expected.probe.json`
is `esses-cli info --json` output reviewed against those facts (dialect `semicolon,decimal-comma`).
