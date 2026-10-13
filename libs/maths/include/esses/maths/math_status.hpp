// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The status of a maths output (03 section 7.3, D-060). Written on the owning thread only (the GUI thread or
// the CLI main thread; 01 section 5.3 rule 10): workers post completions, MathGraph applies the transition.
// model::Channel::status is derived: Ok for Ok/Warning, Pending for Pending/Evaluating, Error for
// Error/Disabled.
#pragma once

#include <esses/maths/channel_key.hpp>
#include <esses/maths/diagnostics.hpp>
#include <esses/model/channel.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace esses::maths {

enum class MathState : std::uint8_t { Pending, Evaluating, Ok, Warning, Error, Disabled };

struct MathStatus {
    MathState state = MathState::Pending;
    Diagnostics diags;
    std::string summary;      // Channel Status text: "OK", "Calculating 37 %", the first message
    ChannelKey resolvedInput; // for Filter/ScaleOffset/self-maths; empty otherwise
    Version version;
    float progress = 0.0F; // sealed chunks / total while Evaluating
};

std::string_view mathStateName(MathState s) noexcept; // "Pending", "Evaluating", ...
model::ChannelStatus toChannelStatus(MathState s) noexcept;

} // namespace esses::maths
