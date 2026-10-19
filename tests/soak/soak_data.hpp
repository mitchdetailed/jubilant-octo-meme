// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The soak's logs (07 section 2.1 "the day's .ld files with their .ldx and a CSV export from another logger";
// D-213), written at test time into a temporary folder and never committed: eight sessions of a circuit day
// as .ld + .ldx and two CSV exports, ten files in all.
//   * The .ld sessions run on two venues (closed analytic circuits of about 3.3 and 2.6 km), each with an out
//     lap, flying laps and an in lap; the beacons are in the .ldx (lap boundaries), the details (Venue,
//     Driver, Session, Vehicle Id, Short Comment) in the header. 41 channels in five rate classes, as a club
//     car logs them: wheel speeds 100 Hz, dampers 200 Hz, ride heights 100 Hz, pedals, steering, G, engine,
//     gear, speed and brake pressures 50 Hz, GPS 20 Hz (F64), temperatures, pressures, voltage, lambda and
//     fuel 10 Hz. Speed follows L/T (1 + 0.3 sin 2 pi u + 0.08 sin (6 pi u + phi)) in lap fraction u, so
//     every lap integrates to the circuit length; GPS, steering and the G forces come from the same path.
//     About 1,000 s and 9 MB per session at scale 1.
//   * The CSV exports: 16 channels at 20 Hz over about 1,000 s, one written by the legacy CSV writer and one
//   by
//     the plain CSV writer (about 3 MB each), opened by their importers.
//   * Two sessions have a second version with one more flying lap (`variant`), for the reload rotation.
// Expectations in the soak never come from these values: it checks that the workflow keeps working (sources,
// laps, states), not numbers.
#pragma once

#include <esses/base/result.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace esses::soak {

struct LogFile {
    std::filesystem::path path;    // the file the shell opens (.ld or .csv)
    std::filesystem::path variant; // the same session with one more flying lap (.ld + .ldx), or empty
    std::string kind;              // "ld", "legacy-csv", "csv"
    std::uintmax_t bytes = 0;      // on disk, the .ldx included
    std::size_t laps = 0;          // out lap + flying laps + in lap of the .ld; 0 for a CSV export
};

// The generator's version: a change to the logs bumps it, so a build tree's cached set is written again.
inline constexpr int kDataVersion = 1;

// Writes the ten logs under `dir` (created), on a pool of its own, and `dir`/manifest.txt last; `scale`
// stretches every session's length (1 = about 1,000 s). Run it where the calling thread may block (writing
// reads every chunk), before the soak's runtime exists.
Result<std::vector<LogFile>> writeSaturdayLogs(const std::filesystem::path& dir, double scale);
// The logs of a complete folder (its manifest names this generator version and scale).
Result<std::vector<LogFile>> readSaturdayLogs(const std::filesystem::path& dir, double scale);
// The logs for one run: written once per build tree under `cacheRoot` (saturday-logs-v<version>-x<scale>,
// moved into place when complete, so concurrent runs never see half a set) and copied into `workDir`, which
// the run may change (the reload rotation replaces files there).
Result<std::vector<LogFile>> saturdayLogs(const std::filesystem::path& cacheRoot,
                                          const std::filesystem::path& workDir, double scale);

} // namespace esses::soak
