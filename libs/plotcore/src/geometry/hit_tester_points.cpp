// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// HitTester point sets (04 section 10.1 item 7: the nearest Main point over a 64 x 64 bucket grid; D-196
// tags). A set buckets its finite points over its bounds; points outside the bounds fall into the edge
// buckets, so clamping the query's bucket range the same way never misses one. A query visits only the
// buckets that intersect the square of side 2 * maxDistance around the pixel.

#include <esses/plotcore/hit_tester.hpp>

#include <cmath>
#include <cstddef>
#include <limits>

namespace esses::plot {

namespace {

constexpr int kGrid = HitTester::kPointGrid;
constexpr std::size_t kBuckets = static_cast<std::size_t>(kGrid) * static_cast<std::size_t>(kGrid);

// The bucket coordinate of `v` along one axis of [origin, origin + extent), clamped to [0, kGrid).
int bucketOf(float v, float origin, float extent) noexcept {
    if (!(extent > 0.0F)) {
        return 0;
    }
    const double f = (static_cast<double>(v) - static_cast<double>(origin)) / static_cast<double>(extent) *
                     static_cast<double>(kGrid);
    if (!(f >= 0.0)) { // negative or NaN
        return 0;
    }
    if (f >= static_cast<double>(kGrid - 1)) {
        return kGrid - 1;
    }
    return static_cast<int>(f);
}

bool finitePoint(const PointPx& p) noexcept {
    return std::isfinite(p.x) && std::isfinite(p.y);
}

} // namespace

void HitTester::addPoints(RectPx bounds, std::span<const PointPx> points, HitResult base,
                          std::span<const std::int64_t> tags) {
    PointSet set;
    set.bounds = bounds;
    set.base = base;
    if (set.base.kind == HitKind::None) {
        set.base.kind = HitKind::Point;
    }
    set.points.assign(points.begin(), points.end());
    if (tags.size() == points.size()) {
        set.tags.assign(tags.begin(), tags.end());
    }
    std::vector<std::uint32_t> bucket(set.points.size(), static_cast<std::uint32_t>(kBuckets));
    set.bucketStart.assign(kBuckets + 1, 0);
    for (std::size_t i = 0; i < set.points.size(); ++i) {
        const PointPx& p = set.points[i];
        if (!finitePoint(p)) {
            continue; // never stored, never found
        }
        const int bx = bucketOf(p.x, bounds.x, bounds.w);
        const int by = bucketOf(p.y, bounds.y, bounds.h);
        bucket[i] = static_cast<std::uint32_t>(by * kGrid + bx);
        ++set.bucketStart[bucket[i] + 1];
    }
    for (std::size_t b = 0; b < kBuckets; ++b) {
        set.bucketStart[b + 1] += set.bucketStart[b];
    }
    set.order.resize(set.bucketStart[kBuckets]);
    std::vector<std::uint32_t> next(set.bucketStart.begin(), set.bucketStart.end() - 1);
    for (std::size_t i = 0; i < set.points.size(); ++i) { // ascending index within a bucket
        if (bucket[i] < kBuckets) {
            set.order[next[bucket[i]]++] = static_cast<std::uint32_t>(i);
        }
    }
    pointSets_.push_back(std::move(set));
}

HitResult HitTester::nearestPoint(float xPx, float yPx, float maxDistancePx, std::int64_t* tag) const {
    if (tag != nullptr) {
        *tag = 0;
    }
    HitResult best;
    if (!std::isfinite(xPx) || !std::isfinite(yPx) || !(maxDistancePx >= 0.0F) ||
        !std::isfinite(maxDistancePx)) {
        return best;
    }
    const double maxD2 = static_cast<double>(maxDistancePx) * static_cast<double>(maxDistancePx);
    double bestD2 = std::numeric_limits<double>::infinity();
    for (const PointSet& set : pointSets_) {
        const int x0 = bucketOf(xPx - maxDistancePx, set.bounds.x, set.bounds.w);
        const int x1 = bucketOf(xPx + maxDistancePx, set.bounds.x, set.bounds.w);
        const int y0 = bucketOf(yPx - maxDistancePx, set.bounds.y, set.bounds.h);
        const int y1 = bucketOf(yPx + maxDistancePx, set.bounds.y, set.bounds.h);
        double setD2 = std::numeric_limits<double>::infinity();
        std::uint32_t setIndex = 0;
        bool found = false;
        for (int by = y0; by <= y1; ++by) {
            for (int bx = x0; bx <= x1; ++bx) {
                const std::size_t b = static_cast<std::size_t>(by) * static_cast<std::size_t>(kGrid) +
                                      static_cast<std::size_t>(bx);
                for (std::uint32_t k = set.bucketStart[b]; k < set.bucketStart[b + 1]; ++k) {
                    const std::uint32_t i = set.order[k];
                    const PointPx& p = set.points[i];
                    const double dx = static_cast<double>(p.x) - static_cast<double>(xPx);
                    const double dy = static_cast<double>(p.y) - static_cast<double>(yPx);
                    const double d2 = dx * dx + dy * dy;
                    if (d2 > maxD2) {
                        continue;
                    }
                    if (!found || d2 < setD2 || (d2 == setD2 && i < setIndex)) {
                        setD2 = d2;
                        setIndex = i;
                        found = true;
                    }
                }
            }
        }
        if (found && setD2 <= bestD2) { // a later set wins a tie
            bestD2 = setD2;
            best = set.base;
            best.point = static_cast<int>(setIndex);
            if (tag != nullptr) {
                *tag = setIndex < set.tags.size() ? set.tags[setIndex] : 0;
            }
        }
    }
    return best;
}

} // namespace esses::plot
