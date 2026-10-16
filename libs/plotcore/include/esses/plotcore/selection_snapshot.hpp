// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SelectionSnapshot (04 section 2.3, D-017; 01 section 12 D-3: the POD lives here, esses_qtmodel's
// DataSelectionModel owns the live instance). Overlays are numbered in selection order and renumbered
// compactly on removal; overlays[i] is "Overlay i+1". Draw order is overlays n..1, Overlay 0, Main (Main on
// top).
//
// traceColour (D-017, D-100): Main -> scheme.channelPalette[idx], or roles.mainSingleColour when
// singleColourMain; Overlay 0 -> roles.overlay0; Overlay n -> scheme.overlayStyle applied to the channel
// colour (default: plain channel colours): colourSource "overlayPalette" takes overlayPalette[n-1], lightness
// mixes toward white (> 0) or black (< 0); dash and widthScale come from traceDash / traceWidthScale.
#pragma once

#include <esses/model/range.hpp>
#include <esses/plotcore/draw_list.hpp>
#include <esses/project/rgba.hpp>
#include <esses/project/scheme.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace esses::plot {

using Rgba = project::Rgba;

enum class SlotKind : std::uint8_t { Main, Overlay0, Overlay };

struct Slot {
    SlotKind kind = SlotKind::Main;
    int number = 0; // 0 for Main and Overlay 0, 1..n otherwise
    model::RangeRef range;
};

struct SelectionSnapshot {
    model::RangeRef main;
    std::optional<model::RangeRef> overlay0;
    std::vector<model::RangeRef> overlays; // overlays[i] is "Overlay i+1"
    bool overlaysHidden = false;           // F4
    bool singleColourMain = false;         // J
    std::uint64_t version = 0;

    // overlay0, else overlays.front(), else nullopt; nullopt while overlaysHidden.
    std::optional<model::RangeRef> referenceOverlay() const;
    // Main first, then Overlay 0, then Overlays 1..n; overlays omitted while hidden.
    std::vector<Slot> visibleSlots() const;
};

Rgba traceColour(const project::Scheme& scheme, const SelectionSnapshot& selection, const Slot& slot,
                 int channelColourIndex);

// The key colour of a range (06 section 5.3 item 3: the range band, Track Position dots, variance traces, the
// offset sub-axes, scatter / histogram overlays, a gauge's reference value): Main -> roles.cursorMain,
// Overlay 0 -> roles.overlay0, Overlay k -> overlayPalette[(k - 1) mod 9]. Traces themselves take
// traceColour.
Rgba rangeKeyColour(const project::Scheme& scheme, const Slot& slot);

// The overlayStyle dash and width scale of a slot (D-100): Overlay n takes scheme.overlayStyle ("solid",
// "dash", "dot", "dashDot"; widthScale > 0), Main and Overlay 0 are solid at scale 1.
DashId traceDash(const project::Scheme& scheme, const Slot& slot);
float traceWidthScale(const project::Scheme& scheme, const Slot& slot);
// overlayStyle.lightness applied to a colour: > 0 mixes toward white, < 0 toward black (clamped to [-1, 1]);
// alpha is kept.
Rgba adjustLightness(Rgba colour, double lightness) noexcept;

} // namespace esses::plot
