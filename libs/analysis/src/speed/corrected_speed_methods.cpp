// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// CorrectedSpeed::alternateList and CorrectedSpeed::gCorrection (03 section 9.1 items 2-3, D-084; 07 section
// 6.1, shipped with the CorrectedSpeedDialog). compute() in corrected_speed.cpp calls both.

#include <esses/analysis/corrected_speed.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace esses::analysis {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// The time constant of gCorrection's slip reference (D-203 (a), (ai)): it integrates the longitudinal
// acceleration less the estimated accelerometer offset and is pulled towards the wheel by dt /
// kReferenceSeconds per sample, so a wheel that leaves the car progressively (a lock building over several
// samples, a driven wheel spinning up) parts from it. While the wheel is followed the same error also
// estimates the offset (gain dt / (4 kReferenceSeconds^2): critically damped), so a constant offset b moves
// the reference by at most 2 b kReferenceSeconds / e (0.74 b) and then not at all.
constexpr double kReferenceSeconds = 1.0;
// The longest prediction (D-203 (ai)): a wheel that is still outside the band this long after the prediction
// started is taken as right again, and the prediction's drift from it over that time as accelerometer offset.
constexpr Micros kMaxPredictMicros = 3'000'000;

// The value of `s` at t with the Linear policy (store::Resampler rule 13): the sample at an exact hit, else
// the interpolation of the bracketing samples; false when either is invalid or t lies outside the samples.
bool linearAt(const UniformSignal& s, Micros t, double& out) noexcept {
    const auto n = static_cast<SampleIdx>(s.size());
    if (n == 0 || !s.rate.valid()) {
        return false;
    }
    const SampleIdx j = s.rate.indexAt(t, s.t0);
    if (j < 0 || j >= n) {
        return false;
    }
    const auto uj = static_cast<std::size_t>(j);
    const Micros tj = s.rate.timeAt(j, s.t0);
    if (tj == t) {
        out = s.values[uj];
        return s.validAt(uj) && std::isfinite(out);
    }
    if (j + 1 >= n || !s.validAt(uj) || !s.validAt(uj + 1)) {
        return false;
    }
    const Micros tk = s.rate.timeAt(j + 1, s.t0);
    const double f = static_cast<double>(t - tj) / static_cast<double>(tk - tj);
    out = s.values[uj] + f * (s.values[uj + 1] - s.values[uj]);
    return std::isfinite(out);
}

bool consistent(const UniformSignal& s) noexcept {
    return s.valid.empty() || s.valid.size() == s.values.size();
}

} // namespace

Result<AlternateListChoice>
CorrectedSpeed::alternateList(std::span<const std::optional<UniformSignal>> candidates,
                              const SpeedClamp& clamp) {
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (const std::optional<UniformSignal>& c = candidates[i]) {
            AlternateListChoice out;
            out.speedMps = *c;
            out.index = i;
            applyClamp(out.speedMps, clamp);
            return out;
        }
    }
    Error e{ErrorCode::NotFound, "no usable speed channel in the alternate list", {}};
    e.context.detail = "E-CSD-NO-SPEED";
    return Failure{std::move(e)};
}

Result<UniformSignal> CorrectedSpeed::gCorrection(const UniformSignal& wheelSpeedMps,
                                                  const UniformSignal& longAccelMps2,
                                                  const GCorrectionSettings& settings,
                                                  const SpeedClamp& clamp) {
    if (!wheelSpeedMps.rate.valid() || !longAccelMps2.rate.valid()) {
        return Failure{Error{ErrorCode::Invalid, "gCorrection: a signal has no rate", {}}};
    }
    if (!consistent(wheelSpeedMps) || !consistent(longAccelMps2)) {
        return Failure{Error{ErrorCode::Invalid, "gCorrection: validity and values differ in length", {}}};
    }
    const double dt =
        static_cast<double>(wheelSpeedMps.rate.den) / static_cast<double>(wheelSpeedMps.rate.num);
    const double threshold = std::abs(settings.lockThresholdMps);
    const double recover = settings.recoverSeconds;
    UniformSignal out;
    out.rate = wheelSpeedMps.rate;
    out.t0 = wheelSpeedMps.t0;
    const std::size_t n = wheelSpeedMps.size();
    out.values.assign(n, kNaN);
    out.valid.assign(n, 0);

    enum class State : std::uint8_t { Follow, Predict, Recover };
    State state = State::Follow;
    bool have = false;      // the previous output is valid
    bool anchored = false;  // Recover after a time-out: the per-sample test starts from the wheel
    double prev = 0.0;      // v_corr[i-1]
    double prevWheel = 0.0; // v_wheel[i-1]
    double ref = 0.0;       // the slip reference (D-203 (a)); the output itself while predicting
    double bias = 0.0;      // the estimated accelerometer offset, m/s^2 (D-203 (ai))
    const double pull = std::min(1.0, dt / kReferenceSeconds);
    // The offset's gain per unit of reference error; capped at 1 / dt, which keeps the estimate stable at
    // rates below 1 / kReferenceSeconds (where the reference restarts on the wheel every sample).
    const double learn = std::min(dt / (4.0 * kReferenceSeconds * kReferenceSeconds), 1.0 / dt);
    double offset = 0.0; // Recover: v_corr - v_wheel when the blend started
    Micros blendStart = 0;
    Micros predictStart = 0; // the first sample of the current prediction
    double predictGap = 0.0; // v_corr - v_wheel there
    for (std::size_t i = 0; i < n; ++i) {
        const double wheel = wheelSpeedMps.values[i];
        if (!wheelSpeedMps.validAt(i) || !std::isfinite(wheel)) {
            have = false; // an invalid wheel sample: invalid output, the next valid one starts afresh
            state = State::Follow;
            continue;
        }
        const Micros t = wheelSpeedMps.rate.timeAt(static_cast<SampleIdx>(i), wheelSpeedMps.t0);
        double accel = 0.0;
        double v = wheel;
        if (!have || !linearAt(longAccelMps2, t, accel)) {
            state = State::Follow; // nothing to predict from: follow the wheel
            ref = wheel;
        } else {
            const double a = accel - bias;
            const double predicted = (state == State::Recover && anchored ? prevWheel : prev) + a * dt;
            const double referenced = ref + a * dt;
            // 03 section 9.1 item 3 compares the wheel with the previous output plus a dt, a per-sample test
            // that a slip building over several samples never fails; the reference catches that too.
            const bool sudden = wheel < predicted - threshold || wheel > predicted + threshold;
            const bool progressive =
                state != State::Predict && (wheel < referenced - threshold || wheel > referenced + threshold);
            if (state == State::Predict && sudden && t - predictStart >= kMaxPredictMicros) {
                // No slip lasts this long: the wheel is right, and what the prediction gained on it since it
                // started is the accelerometer's offset. Back to the wheel; the reference restarts on it.
                bias += (predicted - wheel - predictGap) / (static_cast<double>(t - predictStart) / 1e6);
                ref = wheel;
                anchored = true;
                if (recover > 0.0) {
                    offset = predicted - wheel;
                    blendStart = t;
                    v = predicted;
                    state = State::Recover;
                } else {
                    state = State::Follow;
                }
            } else if (sudden || progressive) {
                // Lock (below) or spin (above): follow the prediction, from the reference when only it saw
                // the slip (the previous output already followed the wheel away).
                v = state != State::Predict && !sudden ? referenced : predicted;
                if (state != State::Predict) {
                    predictStart = t;
                    predictGap = v - wheel;
                }
                state = State::Predict;
                ref = v;
            } else if (state == State::Predict) {
                if (recover > 0.0) { // the wheel is back: blend from the prediction to the wheel
                    offset = predicted - wheel;
                    blendStart = t;
                    v = predicted;
                    state = State::Recover;
                    anchored = false;
                    ref = referenced + pull * (wheel - referenced);
                } else {
                    state = State::Follow;
                    ref = wheel;
                }
            } else if (state == State::Recover) {
                const double f = static_cast<double>(t - blendStart) / 1e6 / recover;
                if (f >= 1.0) { // back on the wheel: the reference restarts on it
                    state = State::Follow;
                    ref = wheel;
                } else {
                    v = wheel + offset * (1.0 - f);
                    ref = referenced + pull * (wheel - referenced);
                }
            } else { // Follow: the reference's error also estimates the offset
                const double error = referenced - wheel;
                ref = referenced - pull * error;
                bias += learn * error;
            }
        }
        out.values[i] = v;
        out.valid[i] = 1;
        prev = v;
        prevWheel = wheel;
        have = true;
    }
    if (std::all_of(out.valid.begin(), out.valid.end(), [](std::uint8_t f) { return f != 0; })) {
        out.valid.clear();
    }
    applyClamp(out, clamp);
    return out;
}

} // namespace esses::analysis
