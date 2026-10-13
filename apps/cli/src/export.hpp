// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// esses-cli export (05 section 9.2, 07 section 5.1 "project-aware CSV with maths", D-090): opens a log the
// way the application does for a project and writes the chosen channels, logged and maths alike, through a
// CSV writer.
//   esses-cli export [<project>] <log> [--channels "A,B"] [--range "Outings:Laps/3"] [--format csv|motec.csv]
//                    [--rate <Hz>] [--units "A=km/h,B=deg C"] [--layer Normal] -o <file.csv>
// With a project (its folder or project.json): the project's mappings and unit maps apply at open (05 section
// 2 step 4), its maths modules in project order, the sidecar's local maths, the corrected speed and distance
// module with the project's settings and enable flag, the GPS corrections where both GPS inputs are logged,
// aliases for name resolution, options.maths.compat, and the display units of channels/overrides.json.
// Without one: the built-in modules with their defaults over the log. A name is the maths output when one has
// it (the head of its shadowing chain, as every consumer reads it, 03 section 4.3), else the logged channel
// (by name, then by logged name). --range writes that range only (time from 0 at its start) and is the Main
// range of the maths' selection-dependent nodes (stat_* without reset, 03 section 7.6). Without --channels:
// every logged channel, then every maths output that evaluates. The evaluation runs on a pool of this
// process; the calling thread waits for it (03 section 7.4: the CLI may wait). Exit status: 0 written; 1 an
// unknown channel, a maths output that cannot be evaluated or a write error; 2 usage or environment errors.
#pragma once

#include "app_context.hpp"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace esses::cli {

struct ExportOptions {
    std::vector<std::filesystem::path> inputs; // [project] log
    std::vector<std::string> channels;         // empty: every logged channel, then every maths output
    std::string range;                         // "Outings:Laps/3"
    std::string layer = "Normal";
    std::string format = "csv";     // csv | motec.csv
    std::string rate;               // "" = the writer's default grid
    std::vector<std::string> units; // "Channel=unit" display units (over the project's overrides)
    std::optional<std::filesystem::path> output;
    std::string importerId;
    std::string optionsJson;
    bool noSidecar = false;
    bool json = false; // a canonical JSON report on stdout
};

int runExport(AppContext& ctx, const ExportOptions& options, std::ostream& out, std::ostream& err);

} // namespace esses::cli
