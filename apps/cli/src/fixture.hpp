// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// esses-cli fixture add | verify | diff (05 section 9.1-9.2, 08 section 6.7; AC-M0-01..03).
//
// fixture add <file> --format <id> --oracle ref-decode|<dir> --out <dir>
//   Writes the fixture directory: input.<ext> and its companions (input.ldx), fixture.json (fixture-1),
//   expected.probe.json (= esses-cli info --json with the fixture's import options), expected.markers.json
//   (markers-1) and expected.laps.json (laps-1) from the open pipeline without units or sidecar, and
//   expected.channels/<NNN>_<name>.csv from the ORACLE, never from the reader (D-041): `ref-decode` runs
//   python -I tools/ld_ref_decode.py --dump-channel ... --dump-format csv --shift-order <the fixture's
//   order>; a directory supplies the CSVs of another oracle (a pinned third-party decode).
//
// fixture verify [<dir>...] [--manifest <file>...] [--update]
//   For every directory holding a fixture.json (searched recursively): fixture.json against fixture-1, the
//   input's and companions' size and sha256, expected.probe.json byte-identical to the probe, the dialect,
//   expected.markers.json and expected.laps.json byte-identical (when present), and every expected channel
//   against the reader's decode (expect_channels.hpp). A private fixture without its input is reported
//   "skipped (private)" (08 section 4.5); a public one fails. --update rewrites the probe, markers and laps
//   expectations, the hashes and the dialect from the current reader (never the channel values).
//   A manifest lists fixture directories whose input lives elsewhere (the user's real files):
//   {"fixtures": [{"dir": "<path, relative to the manifest>", "input": "<path of the .ld>"}]}.
#pragma once

#include "app_context.hpp"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace esses::cli {

struct FixtureAddOptions {
    std::filesystem::path input;
    std::string format;        // importer id, e.g. motec.ld (required)
    std::string oracle;        // "ref-decode" or a directory of expected CSVs
    std::filesystem::path out; // the fixture directory
    std::string name;          // default: the directory name
    std::string source;        // provenance prose
    std::string provenance;    // fixture-1 provenance class ("" = not stated)
    std::string licence;       // SPDX expression; default LicenseRef-unknown
    bool isPrivate = false;
    std::string permission;
    std::string notes;
    std::vector<std::string> channels; // --channels: the oracle dumps only these
    std::optional<std::string>
        optionsJson;        // --options; default for motec.ld: {"ld":{"shiftOrder":"afterMul"}}
    std::string validity;   // oracle.validity; empty: finite-only for --oracle ref-decode, else strict
    std::string oracleKind; // for a directory oracle (default public-project)
    std::string oracleRef;  // for a directory oracle
    std::string python;     // interpreter for ref-decode
    std::optional<std::filesystem::path> refDecode; // tools/ld_ref_decode.py
    bool quiet = false;
    bool json = false; // the summary as canonical JSON
};

struct FixtureVerifyOptions {
    std::vector<std::filesystem::path> roots;
    std::vector<std::filesystem::path> manifests;
    bool ifPresent = false; // a missing manifest is "skipped (private)", not an error
    bool update = false;
    bool json = false;
    bool verbose = false;
};

// fixture diff <dir>... [--channel <name>] (05 section 9.2): verify's checks explained in full: per channel
// the samples compared, how many differ, the largest ulp distance and the first differing sample; the JSON
// Pointer paths where expected.probe.json, expected.markers.json and expected.laps.json differ (missing
// markers and laps included). Exit 1 when a fixture differs.
struct FixtureDiffOptions {
    std::vector<std::filesystem::path> dirs;
    std::string channel; // only this channel (reader or oracle name, case-insensitive)
    bool json = false;
};

int runFixtureAdd(AppContext& ctx, const FixtureAddOptions& options, std::ostream& out, std::ostream& err);
int runFixtureVerify(AppContext& ctx, const FixtureVerifyOptions& options, std::ostream& out,
                     std::ostream& err);
int runFixtureDiff(AppContext& ctx, const FixtureDiffOptions& options, std::ostream& out, std::ostream& err);

// The default Python interpreter for ref-decode: <prefix>_PYTHON, else python (Windows) / python3.
std::string defaultPython();
// tools/ld_ref_decode.py: <prefix>_REF_DECODE, else below the source tree of this build.
std::filesystem::path defaultRefDecode();

} // namespace esses::cli
