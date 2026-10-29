# Governance

## Maintainer and decisions

Esses has one maintainer, the project's founder, who owns the repository and makes the final decision on
every change. Decisions with lasting consequences are recorded as numbered decisions (`D-nnn`): today in
`docs/design/DECISIONS.md`, later as one ADR per file in `docs/adr/`. A change that contradicts a recorded
decision first amends the decision.

## Licence policy

- The official distribution contains only MIT-licensed code of this project. Third-party components keep their
  own licences and are listed in `NOTICES.md`; GPL-only libraries are never linked, and LGPL components (Qt,
  Qt Advanced Docking System) are linked dynamically. Qt is unmodified. The vendored Qt Advanced Docking System
  is changed only to fix a defect upstream has not fixed: each change is listed with its date, reason and upstream
  status in `third_party/qtads/UPSTREAM.md` and marked in the changed file (LGPL-2.1 section 2).
- **No relicensing without every contributor's consent.** The project will not change the licence of
  contributed code unless every contributor whose work is affected agrees in writing.
- **No CLA.** Contributors keep their copyright. Sign-off under the Developer Certificate of Origin (`DCO`) is
  suspended while the maintainer is the only contributor and is required again before outside contributions are
  accepted (D-160); see `CONTRIBUTING.md`.

## Delegation

When a second regular contributor exists (planned from M6), an `OWNERS` file in a format directory
(for example `libs/formats/src/<format>/`) names the people who maintain it; a pull request touching that
directory needs the approval of one of them. The maintainer can always approve.

## Conflict resolution

Disagreements are discussed in the pull request or issue, with reference to the design documents and the
acceptance criteria. If no consensus forms, the maintainer decides and records the reasoning in the
decision log. Conduct problems are handled privately by the maintainer.

## Project name

"Esses" is the working name, held by the maintainer for the project. No trademark search is scheduled and the
name may still change (D-003); every spelling of the codename is collected in one prefix table,
`cmake/naming.cmake`, so a rename can be a scripted search-and-replace driven by it. `TRADEMARK.md` states how the
project uses names and marks.
