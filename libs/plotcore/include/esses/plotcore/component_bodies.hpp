// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Types shared by the component bodies of 04 sections 8.14-17 (M3 contract, 07 section 6.1) and the one
// dispatch that lists a component's channels by type.
//   ChannelUse           a channel a component shows: name, persisted display unit and decimals
//   componentChannelUses the channels of any MVP component in the order its builders receive them in
//                        SlotData::channels (esses_qtmodel's componentChannels and
//                        RenderService::prepareInputs resolve them per slot): time_distance and outing
//                        traces, scatter x then every y then the colour and gating channels, histogram traces
//                        then colour and gating, gauges their one channel, values_list rows, channel_report
//                        the report channels; time_report and track_position none. Each type's body parser
//                        supplies its list, so a new body field never touches this file.
//   ScaleSpec            {mode: channel | auto | manual, min, max} of scatter x/y/z, histogram y and
//                        gauge_bar
//   ColourBand           04 section 7.3 "ColourBands" items: {min, max, open, colour, visible}; ALT+n
//                        toggles band n (bands beyond the list are no-ops)
// componentChannelUses (src/geometry/component_channels.cpp) is final; the JSON helpers are in
// src/geometry/body_common.cpp: readers are strict about types (Invalid with a JSON pointer relative to the
// value) and tolerant about absence; writers emit every member except absent limits. A colour band is
// [min, max), an open band extends to -inf (below) or +inf (above).
#pragma once

#include <esses/analysis/band_axis.hpp>
#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/plotcore/draw_list.hpp>
#include <esses/plotcore/time_distance_builder.hpp>
#include <esses/project/component_spec.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace esses::plot {

struct ChannelUse {
    std::string name;
    std::string displayUnit; // "" = the channel's
    std::optional<int> dps;
    friend bool operator==(const ChannelUse&, const ChannelUse&) = default;
};

// The channels of a component in builder order (see above); empty for an unknown type or a body that does not
// parse.
std::vector<ChannelUse> componentChannelUses(const project::ComponentSpec& spec);

struct ScaleSpec {
    ScaleMode mode = ScaleMode::Channel;    // JSON "channel" | "auto" | "manual" (time_distance_builder.hpp)
    std::optional<analysis::UnitValue> min; // manual limits
    std::optional<analysis::UnitValue> max;
    friend bool operator==(const ScaleSpec&, const ScaleSpec&) = default;
};
// Invalid with context.detail (a JSON pointer relative to the object) on a wrong type or mode.
Result<ScaleSpec> parseScaleSpec(const JsonDoc& json);
JsonDoc scaleSpecToJson(const ScaleSpec& scale);

enum class BandOpen : std::uint8_t { None, Below, Above }; // JSON "none" | "below" | "above"

struct ColourBand {
    analysis::UnitValue min;
    analysis::UnitValue max;
    BandOpen open = BandOpen::None;
    Rgba colour = 0xFFB000FF;
    bool visible = true;
    friend bool operator==(const ColourBand&, const ColourBand&) = default;
};
Result<std::vector<ColourBand>> parseColourBands(const JsonDoc& array);
JsonDoc colourBandsToJson(std::span<const ColourBand> bands);
// The first visible band containing `value` (given in each band's unit; the caller converts); open bands
// extend below min / above max.
std::optional<std::size_t> colourBandOf(std::span<const ColourBand> bands, double value) noexcept;

} // namespace esses::plot
