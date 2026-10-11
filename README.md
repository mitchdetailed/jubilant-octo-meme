# Esses

Esses (working name) is a free, MIT-licensed desktop application for analysing motorsport data logs: time and
distance plots, laps and ranges, maths channels and reports. It reads MoTeC `.ld`/`.ldx` log files and, later,
other formats through importers and plugins. It is a clean-room project: see `CLEANROOM.md`.

Status: M0 complete locally (see `docs/milestones/M0.md`); CI-only checks pending the first push. Only the
Qt-free core libraries and `esses-cli` exist yet.

- Design documents: `docs/design/` (start with `README.md` and `00_OVERVIEW.md`).
- Milestone and format notes: `docs/milestones/M0.md` (M0 status, evidence and deviations), `docs/spikes/S-1.md`
  (the `.ld` spike), `docs/formats/ld.md` (the `.ld`/`.ldx` format as implemented) and
  `docs/formats/ld-converter-port.md` (the converter port specification).
- Building: `tools/dev/README.md`; contributing: `CONTRIBUTING.md`.
- Licence: MIT (`LICENSE`); third-party notices: `NOTICES.md`; governance: `GOVERNANCE.md`;
  names and marks: `TRADEMARK.md`.

Esses is not affiliated with or endorsed by MoTeC.
