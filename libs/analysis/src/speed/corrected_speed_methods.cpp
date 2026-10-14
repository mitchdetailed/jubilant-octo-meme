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
    bool have = false;   // the previous output is valid
    double prev = 0.0;   // v_corr[i-1]
    double offset = 0.0; // Recover: v_corr - v_wheel when the blend started
    Micros blendStart = 0;
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
        } else {
            const double predicted = prev + accel * dt;
            if (wheel < predicted - threshold || wheel > predicted + threshold) {
                v = predicted; // lock (below) or spin (above): follow the prediction
                state = State::Predict;
            } else if (state == State::Predict) {
                if (recover > 0.0) { // the wheel is back: blend from the prediction to the wheel
                    offset = predicted - wheel;
                    blendStart = t;
                    v = predicted;
                    state = State::Recover;
                } else {
                    state = State::Follow;
                }
            } else if (state == State::Recover) {
                const double f = static_cast<double>(t - blendStart) / 1e6 / recover;
                if (f >= 1.0) {
                    state = State::Follow;
                } else {
                    v = wheel + offset * (1.0 - f);
                }
            }
        }
        out.values[i] = v;
        out.valid[i] = 1;
        prev = v;
        have = true;
    }
    if (std::all_of(out.valid.begin(), out.valid.end(), [](std::uint8_t f) { return f != 0; })) {
        out.valid.clear();
    }
    applyClamp(out, clamp);
    return out;
}

} // namespace esses::analysis
