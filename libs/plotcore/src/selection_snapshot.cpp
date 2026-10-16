// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/plotcore/selection_snapshot.hpp>

#include <algorithm>
#include <cmath>

namespace esses::plot {

namespace {

constexpr Rgba kOverlay0Fallback = 0xBDBDBDFFU;
constexpr Rgba kMainKeyFallback = 0xFF0000FFU;

std::uint8_t mixChannel(std::uint8_t c, double lightness) noexcept {
    const auto v = static_cast<double>(c);
    const double out = lightness >= 0.0 ? v + (255.0 - v) * lightness : v * (1.0 + lightness);
    return static_cast<std::uint8_t>(std::clamp(std::lround(out), 0L, 255L));
}

} // namespace

std::optional<model::RangeRef> SelectionSnapshot::referenceOverlay() const {
    if (overlaysHidden) {
        return std::nullopt;
    }
    if (overlay0) {
        return overlay0;
    }
    if (!overlays.empty()) {
        return overlays.front();
    }
    return std::nullopt;
}

std::vector<Slot> SelectionSnapshot::visibleSlots() const {
    std::vector<Slot> slots;
    slots.push_back(Slot{SlotKind::Main, 0, main});
    if (overlaysHidden) {
        return slots;
    }
    if (overlay0) {
        slots.push_back(Slot{SlotKind::Overlay0, 0, *overlay0});
    }
    int number = 1;
    for (const model::RangeRef& r : overlays) {
        slots.push_back(Slot{SlotKind::Overlay, number++, r});
    }
    return slots;
}

Rgba adjustLightness(Rgba colour, double lightness) noexcept {
    if (!std::isfinite(lightness) || lightness == 0.0) {
        return colour;
    }
    const double l = std::clamp(lightness, -1.0, 1.0);
    return project::rgba(mixChannel(project::red(colour), l), mixChannel(project::green(colour), l),
                         mixChannel(project::blue(colour), l), project::alpha(colour));
}

Rgba traceColour(const project::Scheme& scheme, const SelectionSnapshot& selection, const Slot& slot,
                 int channelColourIndex) {
    const Rgba channel = scheme.channelColour(channelColourIndex);
    switch (slot.kind) {
    case SlotKind::Main:
        return selection.singleColourMain ? scheme.role("mainSingleColour", channel) : channel;
    case SlotKind::Overlay0:
        return scheme.role("overlay0", kOverlay0Fallback);
    case SlotKind::Overlay:
        break;
    }
    const project::OverlayStyle& style = scheme.overlayStyle;
    Rgba base = channel;
    if (style.colourSource == "overlayPalette" && !scheme.overlayPalette.empty()) {
        const auto n = static_cast<std::size_t>(std::max(slot.number, 1) - 1);
        base = scheme.overlayPalette[n % scheme.overlayPalette.size()];
    }
    return adjustLightness(base, style.lightness);
}

Rgba rangeKeyColour(const project::Scheme& scheme, const Slot& slot) {
    switch (slot.kind) {
    case SlotKind::Main:
        return scheme.role("cursorMain", kMainKeyFallback);
    case SlotKind::Overlay0:
        return scheme.role("overlay0", kOverlay0Fallback);
    case SlotKind::Overlay:
        break;
    }
    return scheme.overlayColour(std::max(slot.number, 1), kOverlay0Fallback);
}

DashId traceDash(const project::Scheme& scheme, const Slot& slot) {
    if (slot.kind != SlotKind::Overlay) {
        return DashId::Solid;
    }
    const std::string& dash = scheme.overlayStyle.dash;
    if (dash == "dash") {
        return DashId::Dash;
    }
    if (dash == "dot") {
        return DashId::Dot;
    }
    if (dash == "dashDot") {
        return DashId::DashDot;
    }
    return DashId::Solid;
}

float traceWidthScale(const project::Scheme& scheme, const Slot& slot) {
    if (slot.kind != SlotKind::Overlay) {
        return 1.0F;
    }
    const double s = scheme.overlayStyle.widthScale;
    return std::isfinite(s) && s > 0.0 ? static_cast<float>(s) : 1.0F;
}

} // namespace esses::plot
