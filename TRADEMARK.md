# Trademarks and names

## The Esses name

"Esses" is the project's working name (D-003). There is no logo yet. No trademark search has been made or
scheduled (decided 2026-10-10), and the name may change before the first public release. To make a change
cheap, the name appears as an identifier only in the prefix table `cmake/naming.cmake` and in documentation;
a rename is a scripted search-and-replace driven by that table.

Until a policy for the final name exists: forks and redistributions of modified versions should use a
different name so users can tell them apart from the official distribution.

## Third-party marks

"MoTeC" and "i2" are trademarks of their owner. This project uses them only nominatively, to describe file
formats it can read or write (for example "reads MoTeC .ld log files"). The project is not affiliated with,
sponsored by or endorsed by MoTeC.

The product's own names, identifiers, file extensions, schema ids and user-interface strings never contain
those marks. The only exceptions are the allow-listed nominative uses of D-125: importer ids such as
`motec.ld`, file-format labels in import and export dialogs, the format-name constant that the CSV writer
must emit, the `x-i2` round-trip key, and prose that describes provenance. A CI check
(`tools/check_vendor_marks.py`) enforces this.
