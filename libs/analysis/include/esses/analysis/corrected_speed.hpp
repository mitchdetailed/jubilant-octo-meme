// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// CorrectedSpeed and CorrectedDistance: the algorithms behind the built-in maths module
// esses.corrected_speed_distance (03 section 9.1 items 1-7, D-016; esses_maths wraps them, analysis never
// depends on maths). M1 implemented `fastestWheel` with the clamps and the trapezoid integral;
// `alternateList` and `gCorrection` arrived in M3 with the dialog (07 section 6.1, D-084). Acceptance
// AC-M1-11: fastestWheel equals a hand-computed reference on a synthetic four-wheel fixture (one wheel
// locking, one clamp-invalidated span); Corr Dist equals the trapezoid integral within 1e-9 relative and
// holds through invalid speed samples.
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

// CorrectedSpeed::alternateList's result: the chosen signal (clamped) and its index in the candidate list.
struct AlternateListChoice {
    UniformSignal speedMps;
    std::size_t index = 0;
};

struct CorrectedSpeedResult {
    UniformSignal speedMps;
    std::vector<std::string> inputs;      // channel names used, in order (Channel Status "Resolved to")
    std::vector<std::string> diagnostics; // "W-CSD-FALLBACK", ...
};

class CorrectedSpeed {
public:
    // Resolves and reads the input channels named by `settings` in `layer` (worker only: pins through
    // Column::require), converts them to base units and computes Corr Speed. No usable speed channel (or, for
    // gCorrection, either of its two channels unusable): NotFound with context.detail "E-CSD-NO-SPEED" (the
    // graph then shows the "no distance data" banner, D-016).
    static Result<CorrectedSpeedResult> compute(const CorrectedSpeedSettings& settings,
                                                const model::DataLayer& layer,
                                                const units::UnitRegistry& units, const CancelToken& cancel);
    // compute() with an optional registry: null converts nothing and takes every speed channel as m/s and the
    // longitudinal-G channel as G (tools that open a log without a unit registry, where channels carry no
    // unit).
    //
    // Channel lookup: the layer's canonical name (case-folded), then its logged name; a channel with status
    // Error, a timestamped channel and (with a registry) a channel whose unit is not a speed (for the
    // longitudinal G: not an acceleration) are unusable. A speed channel without a unit is taken as m/s, a
    // longitudinal-G channel without a unit as G. fastestWheel with any listed channel unusable falls back to
    // alternateList ("W-CSD-FALLBACK" in diagnostics); alternateList reads the list in order up to its first
    // usable channel and hands the candidates to alternateList(); gCorrection reads its speed and
    // longitudinal-G channels and calls gCorrection(), without a fallback (the module's inputs are those two
    // channels). Samples are read at Background priority.
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

    // M3 (07 section 6.1, shipped with the CorrectedSpeedDialog; src/speed/), called by compute().
    //
    // Item 2, alternateList: the first engaged candidate (the settings' list in order, nullopt = the channel
    // is missing or unusable in the layer), used as-is, then the clamp. NotFound with context.detail
    // "E-CSD-NO-SPEED" when no candidate is engaged.
    static Result<AlternateListChoice> alternateList(std::span<const std::optional<UniformSignal>> candidates,
                                                     const SpeedClamp& clamp);
    // Item 3, gCorrection, as refined by D-203 (a) and (ai). Stateful, F64, on the wheel speed's grid (the
    // longitudinal acceleration resampled Linear onto it; m/s^2, i.e. the G channel already converted), then
    // the clamp. Invalid for a signal without a rate. Not bit-identical to other tools (outline section 9
    // risk). With a = a_long[i] - b (b the estimated accelerometer offset, below) and T = lockThreshold:
    //  - Follow (the output is the wheel speed). A slip starts when the wheel leaves v_pred +- T, v_pred =
    //    v_out[i-1] + a * dt (item 3's per-sample test: a sudden lock or spin), or leaves r +- T, where the
    //    slip reference r integrates a and is pulled towards the wheel by dt / 1 s per sample (a slip that
    //    builds over several samples); the prediction then starts at v_pred, or at r when only r saw it.
    //    While following, the reference error r - v_wheel also updates b by dt / 4 s^2 per m/s (critically
    //    damped), so a constant offset is learnt, and one below about 1.36 T / 1 s (2.7 m/s^2 at the default
    //    threshold) never starts a prediction on clean data.
    //  - Predict: v_out = v_out[i-1] + a * dt while the wheel stays outside v_pred +- T. Back inside: the
    //    blend starts at v_pred and decays the difference linearly, v_out = v_wheel + (v_pred[r] -
    //    v_wheel[r]) * (1 - (t - t_r) / recoverSeconds) until t - t_r reaches recoverSeconds
    //    (recoverSeconds <= 0: straight back to the wheel); leaving the band during the blend predicts
    //    again. A prediction lasts at most 3 s: a wheel still outside the band then is taken as right, b
    //    gains the prediction's mean drift from the wheel over those 3 s (what an offset would have
    //    caused), and the blend back starts there, its per-sample test from the previous wheel sample rather
    //    than the output. So no offset keeps the output off the wheel for more than 3 s + recoverSeconds at
    //    a time, and a constant one at most once.
    //  - The reference restarts on the wheel whenever the output is back on it (Follow resumes), so the
    //    offset is learnt only from samples where the wheel is followed.
    // Fixed here (03 section 9.1 leaves them open): the first valid wheel sample, a sample where the
    // acceleration is invalid or outside its samples, and the sample after an invalid wheel sample output
    // the wheel speed (nothing to predict from); an invalid wheel sample gives an invalid output.
    static Result<UniformSignal> gCorrection(const UniformSignal& wheelSpeedMps,
                                             const UniformSignal& longAccelMps2,
                                             const GCorrectionSettings& settings, const SpeedClamp& clamp);
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
