// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/plotcore/hit_tester.hpp>

#include <algorithm>
#include <cstddef>

namespace esses::plot {

void HitTester::addRect(RectPx rect, HitResult result) {
    if (rect.w <= 0.0F || rect.h <= 0.0F) {
        return;
    }
    regions_.push_back(Region{rect, result});
}

HitResult HitTester::hit(float xPx, float yPx) const noexcept {
    // The last added region wins.
    const auto it = std::find_if(regions_.rbegin(), regions_.rend(), [&](const Region& region) {
        const RectPx& r = region.rect;
        return xPx >= r.x && xPx < r.x + r.w && yPx >= r.y && yPx < r.y + r.h;
    });
    return it != regions_.rend() ? it->result : HitResult{};
}

std::vector<HitResult> HitTester::hitAll(float xPx, float yPx) const {
    std::vector<HitResult> out;
    for (std::size_t i = regions_.size(); i-- > 0;) { // topmost (last added) first
        const RectPx& r = regions_[i].rect;
        if (xPx >= r.x && xPx < r.x + r.w && yPx >= r.y && yPx < r.y + r.h) {
            out.push_back(regions_[i].result);
        }
    }
    return out;
}

} // namespace esses::plot
