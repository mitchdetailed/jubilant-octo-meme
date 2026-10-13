// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChangeMask (04 section 2.2, D-018, 01 section 5.5): which part of the view changed, and so which layers a
// component rebuilds.
//   Cursor, Datum                       -> Dynamic only (<= 4 ms GUI thread)
//   Zoom, Offsets, Scale, Size          -> Static + Dynamic (<= 16 ms wall: worker build + raster)
//   Domain, Selection, Data, Scheme     -> Static + Dynamic + Chrome (<= 50 ms)
// Hover feedback is a widget-local Dynamic repaint, not a Change bit.
// Added in M1: viewChanges (the ViewState part of a mask) and staticReusable (04 section 2.1 item 4: a Static
// layer built for an older view stays valid when only Cursor/Datum changed since).
#pragma once

#include <esses/plotcore/view_state.hpp>

#include <cstdint>

namespace esses::plot {

enum class Change : std::uint16_t {
    Cursor = 1,
    Datum = 2,
    Zoom = 4,
    Domain = 8,
    Selection = 16,
    Offsets = 32,
    Data = 64,
    Scale = 128,
    Size = 256,
    Scheme = 512
};
using ChangeMask = std::uint16_t; // OR of Change bits

constexpr ChangeMask operator|(Change a, Change b) noexcept {
    return static_cast<ChangeMask>(static_cast<ChangeMask>(a) | static_cast<ChangeMask>(b));
}
constexpr ChangeMask operator|(ChangeMask m, Change b) noexcept {
    return static_cast<ChangeMask>(m | static_cast<ChangeMask>(b));
}
constexpr bool has(ChangeMask m, Change b) noexcept {
    return (m & static_cast<ChangeMask>(b)) != 0;
}
constexpr ChangeMask mask(Change b) noexcept {
    return static_cast<ChangeMask>(b);
}

inline constexpr ChangeMask kDynamicOnly = Change::Cursor | Change::Datum;
inline constexpr ChangeMask kStaticRebuild = Change::Zoom | Change::Offsets | Change::Scale | Change::Size;
inline constexpr ChangeMask kFullRebuild = Change::Domain | Change::Selection | Change::Data | Change::Scheme;

// The bits that differ between two view states: Cursor, Datum, Zoom, Domain and Offsets (offsetAxis). The
// version is ignored.
constexpr ChangeMask viewChanges(const ViewState& from, const ViewState& to) noexcept {
    ChangeMask m = 0;
    if (from.cursorT != to.cursorT) {
        m = m | Change::Cursor;
    }
    if (from.datumT != to.datumT) {
        m = m | Change::Datum;
    }
    if (!(from.zoom == to.zoom)) {
        m = m | Change::Zoom;
    }
    if (from.domain != to.domain) {
        m = m | Change::Domain;
    }
    if (from.offsetAxis != to.offsetAxis) {
        m = m | Change::Offsets;
    }
    return m;
}

// A Static layer built before the changes in `m` can still be shown as final.
constexpr bool staticReusable(ChangeMask m) noexcept {
    return (m & static_cast<ChangeMask>(~kDynamicOnly)) == 0;
}

} // namespace esses::plot
