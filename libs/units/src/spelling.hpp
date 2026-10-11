// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Lookup keys and spelling rules of UnitRegistry (internal; the rules are documented in unit_registry.hpp).
#pragma once

#include <string>
#include <string_view>

namespace esses::units::spelling {

// Drops leading and trailing ASCII white space and collapses inner runs to one space (the exact key).
std::string collapse(std::string_view s);
// The case-folded key: collapse(caseFold(collapse(s))).
std::string folded(std::string_view s);
// The normalised key: degree glyphs and mojibake, degree words, micro, ohm, lambda, multiplication signs and
// '^' rewritten after case folding.
std::string normalised(std::string_view s);

// A unit symbol: printable ASCII, single inner spaces, no leading or trailing space; empty allowed.
bool isValidSymbol(std::string_view s) noexcept;
// An alias or display form: non-empty valid UTF-8 without control characters, tabs or line breaks, single
// inner spaces and no leading or trailing space.
bool isValidSpelling(std::string_view s) noexcept;
// A quantity or family name: non-empty valid UTF-8 without control characters and without leading or trailing
// white space.
bool isValidName(std::string_view s) noexcept;
// A compat tag: [a-z0-9][a-z0-9-]*.
bool isValidTag(std::string_view s) noexcept;

} // namespace esses::units::spelling
