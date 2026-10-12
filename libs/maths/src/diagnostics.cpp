// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/maths/diagnostics.hpp>
#include <esses/maths/math_status.hpp>

#include <algorithm>

namespace esses::maths {

bool hasError(const Diagnostics& diags) noexcept {
    return std::any_of(diags.begin(), diags.end(),
                       [](const Diagnostic& d) { return d.level == Diagnostic::Error; });
}

std::string_view mathStateName(MathState s) noexcept {
    switch (s) {
    case MathState::Pending:
        return "Pending";
    case MathState::Evaluating:
        return "Evaluating";
    case MathState::Ok:
        return "Ok";
    case MathState::Warning:
        return "Warning";
    case MathState::Error:
        return "Error";
    case MathState::Disabled:
        return "Disabled";
    }
    return "Pending";
}

model::ChannelStatus toChannelStatus(MathState s) noexcept {
    switch (s) {
    case MathState::Ok:
    case MathState::Warning:
        return model::ChannelStatus::Ok;
    case MathState::Pending:
    case MathState::Evaluating:
        return model::ChannelStatus::Pending;
    case MathState::Error:
    case MathState::Disabled:
        return model::ChannelStatus::Error;
    }
    return model::ChannelStatus::Pending;
}

} // namespace esses::maths
