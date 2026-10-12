// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ViewState (04 section 2.1, D-018 as amended by D-133): the value type copied to workers. `zoom`, `cursorT`
// and `datumT` are workspace-shared (WorkspaceViewModel); `domain` and `offsetAxis` are per worksheet
// (WorksheetViewModel composes both). Cursor and datum are Main-lap time: microseconds since Main.start
// (negative or >= |Main| in another lap of the outing). The zoom span is measured from the Main range start
// in domain units (microseconds or metres) and may extend over the whole outing; alignment offsets are not
// folded into it.
#pragma once

#include <esses/base/time.hpp>

#include <cstdint>
#include <optional>

namespace esses::plot {

enum class Domain : std::uint8_t { Time, Distance };

struct XSpan { // half-open [a, b) in domain units
    double a = 0.0;
    double b = 0.0;
    double length() const noexcept { return b - a; }
    friend bool operator==(const XSpan&, const XSpan&) = default;
};

struct ViewState {
    Domain domain = Domain::Time; // per worksheet (F9)
    XSpan zoom;                   // workspace-shared; W sets {0, |Main|}, F2 the outing extent
    Micros cursorT = 0;           // workspace-shared; Main-lap time
    std::optional<Micros> datumT; // workspace-shared; same base as cursorT
    bool offsetAxis = false;      // per worksheet (O)
    std::uint64_t version = 0;    // bumped on every change
    friend bool operator==(const ViewState&, const ViewState&) = default;
};

} // namespace esses::plot
