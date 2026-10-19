// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// esses-cli convert (05 section 9.2 "reader -> writer through ExportView", D-181 (e), D-214): opens a log
// through the open pipeline (sniff, probe, the .ldx merge, the sidecar unless --no-sidecar, the circuit
// rules with phase 2) and writes its logged channels through one of the built-in writers, without a project
// and without maths (that is `export`).
//   esses-cli convert <input> --format csv|motec.csv|motec.ld [--channels "A,B"] [--range "Outings:Laps/3"]
//                     [--rate <Hz>|<num>/<den>] [--layer Normal] [--writer-options <json>] [--json] -o <out>
// --to is an alias of --format (05 section 9.2 names the option so). A name is the logged channel by name,
// then by logged name; without --channels every channel of the layer. csv and motec.csv go through the CSV
// writers (05 section 6 "Writer": one grid, --rate or the highest rate up to 1 kHz; --range writes that range
// with time from 0 at its start). motec.ld goes through the export-only .ld writer (05 section 5): a whole
// .ld input is written verbatim channel by channel (read -> write -> read is bit-exact for samples and
// validity, item 8); with --range every channel keeps the samples whose time lies in the range, each from
// time 0 (a .ld channel always starts at 0, so a channel whose first sample in the range is later than its
// start moves earlier by less than one of its sample periods) and is re-quantised with its stored encoding;
// the beacons and device events inside the range move with it. The .ld writer does not resample in 0.1
// (05 section 5 item 4 arrives with the export UI at v1.0): --rate is refused for motec.ld, and a channel the
// format cannot hold (timestamped, a rate that is not a whole number of Hz, text) is skipped with
// ld.channel_skipped. A cut also carries its own details (D-217): Log Time (and Log Date past midnight) moved
// to the cut's time 0, and Start Lap the number of the first flying lap that starts in the cut; the input's
// verbatim .ld header is not reused for a cut. The output never replaces the input: an output (or, for
// motec.ld, its .ldx companion) that is the input, the input's .ldx companion (a .ld input's) or the input's
// sidecar is refused before anything is read. A motec.ld output leaves no other .ldx beside it, and a
// conversion that writes no channel leaves no file and exits 1 (D-217); a channel named twice in --channels
// is a usage error, found once the input is open (names resolve to channels there) and before anything is
// written.
// Exit status: 0 written; 1 an unreadable input, an unknown channel or range, a write error, no channel
// written; 2 usage errors (unknown format, no -o, the input as output, --rate with motec.ld, malformed
// options, a channel named twice).
#pragma once

#include "app_context.hpp"

#include <esses/base/time.hpp>
#include <esses/model/source.hpp>

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace esses::cli {

struct ConvertOptions {
    std::filesystem::path input;
    std::string format;                // csv | motec.csv | motec.ld
    std::vector<std::string> channels; // empty: every channel of the layer
    std::string range;                 // "Outings:Laps/3"
    std::string layer = "Normal";
    std::string rate;          // csv grid only; "" = the writer's default
    std::string writerOptions; // WriterOptions::options as JSON, e.g. {"ld":{"dialect":"m1"}}
    std::optional<std::filesystem::path> output;
    std::string importerId;
    std::string optionsJson;
    bool noSidecar = false;
    bool json = false; // a canonical JSON report on stdout
};

// True when writing `output` (and, for a .ld output, its .ldx companion) would replace `input`, the input's
// .ldx companion or its sidecar (<input>.sidecar.json): lexically after normalisation, case-insensitively on
// Windows, or because the files are the same file on disk.
bool convertWouldOverwriteInput(const std::filesystem::path& input, const std::filesystem::path& output,
                                bool ldOutput);

// The details of a .ld cut of `layer` that starts at `start` (D-217): Log Time + start (HH:MM[:SS]) and Log
// Date one day on for each midnight passed (DD/MM/YYYY, the .ld header's order); a Log Time that does not
// parse goes, and so does a Log Date that would have to move but does not parse. Start Lap: the number of the
// first flying lap of `layer` that starts at or after `start` (set, or added when it is not 1); details and
// baseDetails alike.
void cutDetails(model::DataSource& source, const model::DataLayer& layer, Micros start);

int runConvert(AppContext& ctx, const ConvertOptions& options, std::ostream& out, std::ostream& err);

} // namespace esses::cli
