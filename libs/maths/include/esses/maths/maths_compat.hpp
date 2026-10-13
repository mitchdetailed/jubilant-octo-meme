// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathsCompat: switches for behaviours that follow the reference tool rather than ordinary maths (03 section
// 2.3, D-020, D-080; documented in docs/maths-compat.md when the parser lands in M2). Part of every defHash.
#pragma once

namespace esses::maths {

struct MathsCompat {
    bool unaryMinusAbovePower = false; // project.json options.maths.compat.unaryMinusAbovePower
    friend bool operator==(const MathsCompat&, const MathsCompat&) = default;
};

} // namespace esses::maths
