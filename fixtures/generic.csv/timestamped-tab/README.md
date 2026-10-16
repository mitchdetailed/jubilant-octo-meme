# generic.csv / timestamped-tab

Self-authored (CC0-1.0), written by `tools/gen_text_fixtures.py`. A tab-delimited table with a units row whose
`Timestamp` column holds Unix epoch milliseconds with jitter (18 to 23 ms steps) and a 1.5 s pause, so no rate
fits and the channels are timestamped (05 section 7.1). One row repeats the previous time (it replaces it: last wins) and one
row goes back in time (it is dropped). The epoch unit sets the layer's UTC anchor; times count from the first row.

Expected channel values: `expected.channels/` from the generator (oracle kind `generator`), applying the time rule
to the printed rows. `expected.probe.json` is `esses-cli info --json` output reviewed against those facts.
