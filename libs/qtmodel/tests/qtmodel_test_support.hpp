// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Shared helpers of the esses_qtmodel Qt Tests: the shipped unit table, synthetic circuit sources built in
// memory (an out lap, flying laps, an in lap; beacons in group "Beacons"; wheel speeds whose integral is
// exactly the track length per lap) with their laps from the circuit rules, the RangeRefs of their ranges,
// and the same source written as a .ld + .ldx pair for the open pipeline.
#pragma once

#include <esses/analysis/rule_runner.hpp>
#include <esses/model/range.hpp>
#include <esses/model/source.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/units/unit_registry.hpp>

#include <QString>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace esses::qt::test {

std::filesystem::path repoRoot();
std::shared_ptr<const units::UnitRegistry> shippedUnits();

struct SyntheticOptions {
    std::string path = "synthetic.ld"; // prov.path; the SourceId is derived from it
    // Lap durations in seconds: the first is the out lap, the last the in lap, the rest flying laps.
    std::vector<double> lapSeconds{40.0, 50.0, 48.0, 49.0, 30.0};
    double trackLengthM = 2000.0;
    unsigned rateHz = 50;
    bool wheelSpeeds = true; // "Wheel Speed FL" / "Wheel Speed FR" (m/s); false: no distance data
    bool throttle = true;    // "Throttle" (%), a logged channel for the item models
    // M2 additions: a per-lap factor on the wheel speeds (lap k measures trackLengthM * scale[k]; missing
    // entries are 1), "GPS Latitude" / "GPS Longitude" (deg, the first second of fixes at (0, 0)), the logged
    // spelling of "Throttle" when a dictionary or mapping renamed it, and a "Venue Length" detail (m).
    std::vector<double> lapLengthScale;
    bool gps = false;
    std::string throttleLoggedName;
    std::optional<double> venueLengthM;
};

// The source with its phase-1 ranges ("Outings", "Outings:Laps") from RuleRunner::circuitDefault().
std::shared_ptr<const model::DataSource> makeSource(const SyntheticOptions& options,
                                                    store::ChunkCache& cache);

// The RangeRef of range `index` (time order) of `groupPath` in the "Normal" layer.
model::RangeRef rangeRef(const model::DataSource& src, std::size_t index,
                         const std::string& groupPath = "Outings:Laps");
const model::Range& rangeAt(const model::DataSource& src, std::size_t index,
                            const std::string& groupPath = "Outings:Laps");
std::size_t rangeCount(const model::DataSource& src, const std::string& groupPath = "Outings:Laps");

// Writes `src` as <dir>/<name>.ld (+ .ldx with the beacons) through the motec.ld writer; returns the .ld
// path.
std::filesystem::path writeLd(const model::DataSource& src, const std::filesystem::path& dir,
                              const std::string& name);

// A fresh empty directory under the test binary directory.
std::filesystem::path scratchDir(const std::string& name);

// A copy of `src` whose bytes are overwritten (0xA5) when its last owner lets go of it, in storage that is
// freed only at the end of the process: a read of its id through a reference that outlived the source (a
// use after free) then sees the overwritten bytes instead of the id, in any build and without a fault.
std::shared_ptr<const model::DataSource> quarantinedCopy(const model::DataSource& src);

QString qs(const std::string& s);

} // namespace esses::qt::test
