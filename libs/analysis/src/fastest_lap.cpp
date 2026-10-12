// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/analysis/fastest_lap.hpp>

#include <algorithm>

namespace esses::analysis {

namespace {

bool isFlying(const model::Range& r) noexcept {
    return r.lapClass == model::LapClass::Flying;
}

} // namespace

std::vector<const model::Range*> fastestLapCandidates(std::span<const model::Range> laps) {
    const bool anyTrustedFlying = std::any_of(laps.begin(), laps.end(), [](const model::Range& r) {
        return r.enabled && r.trusted && isFlying(r);
    });
    std::vector<const model::Range*> out;
    for (const model::Range& r : laps) {
        if (!r.enabled) {
            continue;
        }
        const bool candidate = anyTrustedFlying ? (r.trusted && isFlying(r))
                                                : (isFlying(r) || r.lapClass == model::LapClass::Partial);
        if (candidate) {
            out.push_back(&r);
        }
    }
    return out;
}

const model::Range* fastestLap(std::span<const model::Range> laps) {
    const model::Range* best = nullptr;
    for (const model::Range* r : fastestLapCandidates(laps)) {
        if (best == nullptr || r->duration() < best->duration() ||
            (r->duration() == best->duration() &&
             (r->number < best->number || (r->number == best->number && r->start < best->start)))) {
            best = r;
        }
    }
    return best;
}

std::vector<const model::Range*> fastLapsWithin(std::span<const model::Range> laps, double tolerancePct) {
    const model::Range* best = fastestLap(laps);
    if (best == nullptr) {
        return {};
    }
    const double limit = static_cast<double>(best->duration()) * (1.0 + std::max(0.0, tolerancePct) / 100.0);
    std::vector<const model::Range*> out;
    for (const model::Range* r : fastestLapCandidates(laps)) {
        if (static_cast<double>(r->duration()) <= limit) {
            out.push_back(r);
        }
    }
    return out;
}

} // namespace esses::analysis
