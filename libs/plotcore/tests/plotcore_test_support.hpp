// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Shared helpers of the esses_plotcore tests: in-memory columns on a private ChunkCache, a known-data
// ILapDistance (the stand-in for analysis::DistanceIndex, which these tests do not depend on), chains built
// from speed samples, and the property-test seed.
#pragma once

#include <esses/base/env.hpp>
#include <esses/plotcore/plotcore.hpp>
#include <esses/store/store.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace esses::plot::test {

// Property-test seed (08 section 6.2): ESSES_TEST_SEED when set, else a fixed default.
inline std::uint64_t testSeed() {
    if (const auto v = getEnv(envName("TEST_SEED")); v && !v->empty()) {
        return std::stoull(*v, nullptr, 0);
    }
    return 0x5EED'0000'2026'1011ULL;
}

// Owns the ChunkCache every test column lives in.
class Store {
public:
    Store() : cache_(std::make_unique<store::ChunkCache>(std::size_t{1} << 31)) {}
    store::ChunkCache& cache() noexcept { return *cache_; }

    store::Column column(std::shared_ptr<store::IChunkProvider> provider) const {
        store::ColumnOptions options;
        options.cache = cache_.get();
        return store::Column(std::move(provider), options);
    }
    store::Column f32(const std::vector<float>& v, std::vector<bool> valid = {}) const {
        return column(store::InMemoryProvider::fromF32(v, std::move(valid)));
    }
    store::Column f64(const std::vector<double>& v, std::vector<bool> valid = {}) const {
        return column(store::InMemoryProvider::fromF64(v, std::move(valid)));
    }
    store::Column i32(const std::vector<std::int32_t>& v, std::vector<bool> valid = {}) const {
        return column(store::InMemoryProvider::fromI32(v, std::move(valid)));
    }
    store::Column i64(const std::vector<std::int64_t>& v) const {
        return column(store::InMemoryProvider::fromI64(v));
    }

private:
    std::unique_ptr<store::ChunkCache> cache_;
};

// A lap distance table from samples: absolute times (increasing) and cumulative metres (non-decreasing).
// distAt interpolates linearly; timeAt returns the FIRST time a distance is reached (plateaus = stopped car),
// in seconds since lapStart, like analysis::DistanceIndex.
class FakeLapDistance final : public ILapDistance {
public:
    FakeLapDistance(Micros lapStart, std::vector<Micros> timesUs, std::vector<double> distM)
        : lapStart_(lapStart), t_(std::move(timesUs)), d_(std::move(distM)) {}

    // A constant speed over [start, end).
    static std::shared_ptr<const FakeLapDistance> constant(Micros start, Micros end, double metresPerSecond) {
        return std::make_shared<const FakeLapDistance>(
            start, std::vector<Micros>{start, end},
            std::vector<double>{0.0, metresPerSecond * static_cast<double>(end - start) / 1e6});
    }

    double distAt(Micros t) const noexcept override {
        if (t_.empty()) {
            return 0.0;
        }
        if (t <= t_.front()) {
            return 0.0;
        }
        if (t >= t_.back()) {
            return d_.back() - d_.front();
        }
        const auto it = std::upper_bound(t_.begin(), t_.end(), t);
        const auto i = static_cast<std::size_t>(it - t_.begin());
        const double f = static_cast<double>(t - t_[i - 1]) / static_cast<double>(t_[i] - t_[i - 1]);
        return d_[i - 1] + f * (d_[i] - d_[i - 1]) - d_.front();
    }
    double timeAt(double d) const noexcept override {
        if (t_.empty()) {
            return 0.0;
        }
        const double target = std::clamp(d, 0.0, lengthM()) + d_.front();
        const auto it = std::lower_bound(d_.begin(), d_.end(), target);
        const auto i = static_cast<std::size_t>(it - d_.begin());
        double t = 0.0;
        if (i == 0) {
            t = static_cast<double>(t_.front());
        } else if (i >= d_.size()) {
            t = static_cast<double>(t_.back());
        } else {
            const double f = (target - d_[i - 1]) / (d_[i] - d_[i - 1]);
            t = static_cast<double>(t_[i - 1]) + f * static_cast<double>(t_[i] - t_[i - 1]);
        }
        return (t - static_cast<double>(lapStart_)) / 1e6;
    }
    double lengthM() const noexcept override { return t_.empty() ? 0.0 : d_.back() - d_.front(); }

private:
    Micros lapStart_;
    std::vector<Micros> t_;
    std::vector<double> d_;
};

// Laps [bounds[i], bounds[i+1]) of a speed signal sampled at `times` (m/s); the trapezoid integral per lap.
// `indexed[i] == false` leaves lap i without an index; `stretch[i]` (default 1) scales it.
inline std::vector<LapPiece> lapsFromSpeed(std::span<const Micros> times, std::span<const double> speed,
                                           std::span<const Micros> bounds, std::vector<bool> indexed = {},
                                           std::vector<double> stretch = {}) {
    std::vector<LapPiece> pieces;
    for (std::size_t lap = 0; lap + 1 < bounds.size(); ++lap) {
        LapPiece piece;
        piece.start = bounds[lap];
        piece.end = bounds[lap + 1];
        piece.stretch = lap < stretch.size() ? stretch[lap] : 1.0;
        if (lap < indexed.size() && !indexed[lap]) {
            pieces.push_back(piece);
            continue;
        }
        std::vector<Micros> t;
        std::vector<double> d;
        double acc = 0.0;
        for (std::size_t i = 0; i < times.size(); ++i) {
            if (times[i] < piece.start || times[i] > piece.end) {
                continue;
            }
            if (!t.empty()) {
                const double dt = static_cast<double>(times[i] - t.back()) / 1e6;
                acc += 0.5 * (speed[i] + speed[i - 1]) * dt;
            }
            t.push_back(times[i]);
            d.push_back(acc);
        }
        piece.lookup = std::make_shared<const FakeLapDistance>(piece.start, std::move(t), std::move(d));
        pieces.push_back(std::move(piece));
    }
    return pieces;
}

} // namespace esses::plot::test
