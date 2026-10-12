// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// HitTester (04 section 5.5; ARCHITECTURE_OUTLINE section 4 esses_plotcore): pixel -> {kind, group, trace,
// slot, band, marker, handle} for clicks, hover and drag. Built alongside the Static DrawList on the worker
// and kept with the layer; queried on the GUI thread. M1 basics: panels, key labels and traces by rectangle;
// band/marker/handle hits arrive with the bands (M3).
#pragma once

#include <esses/model/ids.hpp>
#include <esses/plotcore/draw_list.hpp>

#include <cstdint>
#include <vector>

namespace esses::plot {

enum class HitKind : std::uint8_t { None, Panel, Trace, KeyLabel, KeyValue, Axis, Band, Marker, Handle };

struct HitResult {
    HitKind kind = HitKind::None;
    int group = -1;
    int trace = -1;
    int slot = -1;
    int band = -1;
    model::MarkerId markerId;
    int handle = -1;
};

class HitTester {
public:
    // Later additions win over earlier ones (paint order).
    void addRect(RectPx rect, HitResult result);
    HitResult hit(float xPx, float yPx) const noexcept;
    void clear() noexcept { regions_.clear(); }
    bool empty() const noexcept { return regions_.empty(); }

private:
    struct Region {
        RectPx rect;
        HitResult result;
    };
    std::vector<Region> regions_;
};

} // namespace esses::plot
