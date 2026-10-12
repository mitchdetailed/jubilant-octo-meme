// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Maths diagnostics (03 section 3.1 and 2.5): stable codes (E-..., W-..., I-...), a level, the source span
// and our own message text. Built-in modules report through the same type ("W-CSD-FALLBACK",
// "E-CSD-NO-SPEED", 03 section 9.1).
#pragma once

#include <esses/base/result.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace esses::maths {

using Span = TextSpan;

struct Diagnostic {
    enum Level : std::uint8_t { Error, Warning, Info, Style };
    std::string code; // "E-CSD-NO-SPEED"
    Level level = Error;
    Span span;
    std::string message;
    std::string hint;
};
using Diagnostics = std::vector<Diagnostic>;

// True when any diagnostic has level Error.
bool hasError(const Diagnostics& diags) noexcept;

} // namespace esses::maths
