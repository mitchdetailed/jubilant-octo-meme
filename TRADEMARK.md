# Trademarks and names

## The Esses name

"Esses" is the project's working name (D-003). It has no logo beyond the application icon in
`tools/packaging/icons/`. No trademark search has been made or scheduled (decided 2026-10-10), and the name may
still change. The name is used as a prefix throughout the code (the C++ namespace `esses`, the `esses_` targets,
the `ESSES_` options); every spelling of it is collected in the prefix table `cmake/naming.cmake`, so a rename can
be a scripted search-and-replace driven by that table (the script, `tools/rename.py`, is planned).

Until a policy for the final name exists: forks and redistributions of modified versions should use a
different name so users can tell them apart from the official distribution.

## Third-party marks

"MoTeC" and "i2" are trademarks of their owner. This project uses them only nominatively: to describe the file
formats it can read or write (for example "reads MoTeC .ld log files") and the maths syntax it is compatible with.
The project is not affiliated with, sponsored by or endorsed by MoTeC.

The product's own names, identifiers, file extensions, schema ids and user-interface strings never contain
those marks. The only exceptions are the allow-listed nominative uses of `docs/design/00_OVERVIEW.md` section 8
rule 2 (D-125, D-222): importer ids such as `motec.ld` with their dictionary and profile file names, file-format
labels in import and export dialogs, the format-name constant that the CSV writer must emit, the `x-i2`
round-trip key (`xI2` in code), and prose that describes provenance or compatibility (`README.md`, this file,
`CLEANROOM.md`, `NOTICES.md`, `docs/`, the fixtures' READMEs and the `source` fields of `fixture.json`). A CI
check (`tools/check_vendor_marks.py`) enforces this.
