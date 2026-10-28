# Security policy

## Reporting a vulnerability

Please report security problems privately through the repository host's private vulnerability reporting
(GitHub: "Report a vulnerability" on the Security tab) instead of a public issue. You will get an
acknowledgement within a week. A dedicated contact address will be listed here before the first public
release.

## Supported versions

The latest release receives security fixes. 0.1.0 is the first release.

## Threat model in brief

Log files, sidecars and project documents are untrusted input: importers are fuzzed (`tests/fuzz/`) and every
JSON document is validated against its schema. Plugins (from v1.0) and C++ extensions are trusted code that
runs in-process. Lua scripts (from v1.1) run in a sandbox (D-030).
