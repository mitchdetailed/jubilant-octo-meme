// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/json_depth.hpp>

#include <algorithm>

namespace esses {

std::size_t jsonNestingDepth(std::string_view text, std::size_t stopAbove) noexcept {
    std::size_t depth = 0;
    std::size_t deepest = 0;
    bool inString = false;
    bool escaped = false;
    for (const char c : text) {
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        switch (c) {
        case '"':
            inString = true;
            break;
        case '[':
        case '{':
            ++depth;
            deepest = std::max(deepest, depth);
            if (deepest > stopAbove) {
                return deepest;
            }
            break;
        case ']':
        case '}':
            depth = depth == 0 ? 0 : depth - 1;
            break;
        default:
            break;
        }
    }
    return deepest;
}

} // namespace esses
