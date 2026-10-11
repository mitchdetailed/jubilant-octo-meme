// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/priority.hpp>

namespace esses {

std::string_view priorityName(Priority p) noexcept {
    switch (p) {
    case Priority::Interactive:
        return "Interactive";
    case Priority::Visible:
        return "Visible";
    case Priority::Background:
        return "Background";
    }
    return "Unknown";
}

TaskKey::Kind TaskKey::kind() const noexcept {
    if (text.empty()) {
        return Kind::None;
    }
    const std::string_view t(text);
    for (const std::string_view prefix : {"trace/", "geom/", "report/"}) {
        if (t.starts_with(prefix)) {
            return Kind::Replace;
        }
    }
    for (const std::string_view prefix : {"chunk/", "summary/"}) {
        if (t.starts_with(prefix)) {
            return Kind::ShareOwnedBySource;
        }
    }
    return Kind::Share;
}

} // namespace esses
