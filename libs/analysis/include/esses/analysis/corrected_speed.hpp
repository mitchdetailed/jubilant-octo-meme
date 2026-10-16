// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// CorrectedSpeed and CorrectedDistance: the algorithms behind the built-in maths module
// esses.corrected_speed_distance (03 section 9.1 items 1-7, D-016; esses_maths wraps them, analysis never
// depends on maths). M1 implements `fastestWheel` with the clamps and the trapezoid integral; `alternateList`
// and `gCorrection` arrive in M3 with the dialog (07 section 4.1). Acceptance AC-M1-11: fastestWheel equals a
// hand-computed reference on a synthetic four-wheel fixture (one wheel locking, one clamp-invalidated span);
// Corr Dist equals the trapezoid integral within 1e-9 relative and holds through invalid speed samples.
//
// Values are SI base units (m/s, m). The 0.1 maths path evaluates whole extents (03 section 6.0), so the
// algorithms work on whole-extent UniformSignal buffers; esses_maths seals them into ChunkCache chunks.
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/json_doc.hpp>
#include <esses/base/rate.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/opaque_json.hpp> // compatibility: model::OpaqueJson names JsonDoc
#include <esses/store/column.hpp>
#include <esses/store/series.hpp>
#include <esses/units/unit_registry.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace esses::analysis {

enum class SpeedMethod : std::uint8_t { FastestWheel, AlternateList, GCorrection };
enum class DistanceSource : std::uint8_t { Integrate, Logged };
enum class Integration : std::uint8_t { Trapezoid, Rectangle };

struct SpeedClamp { // 03 section 9.1 item 4; base units
    std::optional<double> minMps = 0.0;
    bool minInvalidate = false;
    std::optional<double> maxMps; // "max": null
    bool maxInvalidate = false;
};

struct GCorrectionSettings { // M3 (03 section 9.1 item 3)
    std::string speedChannel = "Wheel Speed FL";
    std::string longGChannel = "G Force Long";
    double lockThresholdMps = 2.0;
    double recoverSeconds = 0.5;
};

// The module's "settings" object (03 section 9.1; project.json
// builtinMaths["esses.corrected_speed_distance"]).
struct CorrectedSpeedSettings {
    SpeedMethod method = SpeedMethod::FastestWheel;
    std::vector<std::string> fastestWheelChannels{"Wheel Speed FL", "Wheel Speed FR"};
    // 03 section 9.1's list plus "Speed" last (M1 integration: the sim-exporter dialect logs its road speed
    // as SPEED, which no other entry names; lookups are case-insensitive).
    std::vector<std::string> alternateListChannels{
        "Ground Speed", "Wheel Speed FL", "Wheel Speed FR", "Wheel Speed Front",
        "Wheel Speed",  "Wheel Speed RL", "Wheel Speed RR", "Wheel Speed Rear",
        "Drive Speed",  "GPS Speed",      "Speed"};
    GCorrectionSettings gCorrection;
    SpeedClamp clamp;
    DistanceSource distanceSource = DistanceSource::Integrate;
    std::string loggedDistanceChannel = "Lap Distance";
    Integration integration = Integration::Trapezoid;
    std::string speedOutput = "Corr Speed";
    std::string distanceOutput = "Corr Dist";

    // Parses the settings object (schema builtin-esses.corrected_speed_distance-1; every member optional, an
    // absent member keeps the default above, "min": null / "max": null remove that limit); {"v", "unit"}
    // values are converted to base units through `units` (a unit it lacks falls back to a fixed table of
    // speed units). Invalid with context.detail (a JSON pointer) on a value the settings schema rejects
    // (fastestWheel takes exactly two channels, alternateList at least one, names are not blank).
    static Result<CorrectedSpeedSettings> fromJson(const JsonDoc& settings, const units::UnitRegistry& units);
};

// A whole-extent uniform signal in base units.
struct UniformSignal {
    Rate rate;
    Micros t0 = 0;
    std::vector<double> values;
    std::vector<std::uint8_t> valid; // one flag per sample, 1 = valid; empty = every sample valid

    std::size_t size() const noexcept { return values.size(); }
    bool validAt(std::size_t i) const noexcept { return valid.empty() || valid[i] != 0; }
};

struct CorrectedSpeedResult {
    UniformSignal speedMps;
    std::vector<std::string> inputs;      // channel names used, in order (Channel Status "Resolved to")
    std::vector<std::string> diagnostics; // "W-CSD-FALLBACK", ...
};

class CorrectedSpeed {
public:
    // Resolves and reads the input channels named by `settings` in `layer` (worker only: pins through
    // Column::require), converts them to m/s and computes Corr Speed. No usable speed channel: NotFound with
    // context.detail "E-CSD-NO-SPEED" (the graph then shows the "no distance data" banner, D-016).
    static Result<CorrectedSpeedResult> compute(const CorrectedSpeedSettings& settings,
                                                const model::DataLayer& layer,
                                                const units::UnitRegistry& units, const CancelToken& cancel);
    // compute() with an optional registry: null converts nothing and takes every channel as m/s (tools that
    // open a log without a unit registry, where channels carry no unit).
    //
    // Channel lookup: the layer's canonical name (case-folded), then its logged name; a channel with status
    // Error, a timestamped channel and (with a registry) a channel whose unit is not a speed are unusable.
    // fastestWheel with any listed channel unusable falls back to alternateList ("W-CSD-FALLBACK" in
    // diagnostics); alternateList takes its first usable channel as-is; gCorrection is Unsupported until M3
    // (D-044). Samples are read at Background priority.
    static Result<CorrectedSpeedResult> compute(const CorrectedSpeedSettings& settings,
                                                const model::DataLayer& layer,
                                                const units::UnitRegistry* units, const CancelToken& cancel);
    // Item 1: per sample max(a, b) after resampling both Linear to the higher rate; one invalid -> the other;
    // both invalid -> invalid. Then the clamp. The output grid is the higher-rate input's (a's on a tie:
    // rate and t0) and runs to the later of the two last samples; outside an input's [first, last] sample
    // times that input is invalid (store::Resampler's Linear policy). Invalid for an input without a rate.
    static Result<UniformSignal> fastestWheel(const UniformSignal& a, const UniformSignal& b,
                                              const SpeedClamp& clamp);
    // fastestWheel over any number of wheels (the settings' channel list), one resampling to the highest
    // rate: per sample the maximum of the valid inputs. Invalid for an empty list or an input without a rate.
    static Result<UniformSignal> fastestWheel(std::span<const UniformSignal> wheels, const SpeedClamp& clamp);
    // Item 4: below min -> min or invalid (minInvalidate); above max -> max or invalid (maxInvalidate).
    // Invalidated samples become NaN with valid[i] == 0.
    static void applyClamp(UniformSignal& speedMps, const SpeedClamp& clamp);
};

class CorrectedDistance {
public:
    // Item 6, "integrate": cumulative integral of v dt over the extent in metres (F64); invalid speed samples
    // contribute 0 (the distance holds). The result has the speed's rate and t0 and no invalid samples.
    // Trapezoid: d[0] = 0, d[i] = d[i-1] + (v[i-1] + v[i]) / 2 * dt when both samples are valid, else
    // d[i-1]. Rectangle (maths integrate()'s rule): d[i] = d[i-1] + v[i] * dt when v[i] is valid. dt is the
    // exact period den / num seconds. Invalid for a signal without a rate.
    static Result<UniformSignal> integrate(const UniformSignal& speedMps,
                                           Integration method = Integration::Trapezoid);
    // Item 6, "logged": a monotonic cumulative distance from a resetting lap-distance channel (metres):
    // wherever d[i] < d[i-1] - max(10 m, d[i-1] / 2) the previous value is added to a running offset (d[i-1]
    // is the previous valid sample). Invalid samples hold the previous output (leading ones take the first
    // valid output). NotFound when no sample is valid.
    static Result<UniformSignal> fromLogged(const UniformSignal& lapDistanceM);
};

// A UniformSeries over an in-memory F64 copy of the signal (invalid samples invalid), for consumers that read
// store series: RuleRunner::runPhase2's stationary speed in tools without the maths engine, tests. The column
// is cached in options.cache (ChunkCache::installed() when null).
store::UniformSeries toUniformSeries(const UniformSignal& signal, const store::ColumnOptions& options = {},
                                     std::string name = "analysis:signal");

} // namespace esses::analysis
