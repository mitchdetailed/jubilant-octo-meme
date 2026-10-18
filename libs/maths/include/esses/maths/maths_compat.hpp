// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathsCompat: switches for behaviours that follow the reference tool rather than ordinary maths (03 section
// 2.3, D-020, D-080; documented in docs/maths-compat.md). Part of every defHash.
#pragma once

#include <map>
#include <string>

namespace esses::maths {

struct MathsCompat {
    bool unaryMinusAbovePower = false; // project.json options.maths.compat.unaryMinusAbovePower
    // Alternative function spellings discovered later (running min/max, bit_decode; D-080), case-folded
    // spelling -> canonical name. Applied by the parser before the registry lookup.
    std::map<std::string, std::string> functionAliases;
    friend bool operator==(const MathsCompat&, const MathsCompat&) = default;
};

} // namespace esses::maths
