// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Nesting depth of JSON text, checked before a document is parsed. nlohmann-json parses and destroys
// iteratively, but every writer, validator and walker that recurses over a parsed value would overflow the
// stack on a document nested 100,000 levels deep (a shared sidecar, a probe or an options string from a
// file). Every JSON entry point that accepts outside text therefore refuses a document deeper than
// kMaxJsonDepth before parsing it.
#pragma once

#include <cstddef>
#include <string_view>

namespace esses {

// Arrays and objects nested more deeply than this are refused (no Esses schema comes near 32).
inline constexpr std::size_t kMaxJsonDepth = 256;

// The deepest nesting of '[' and '{' outside string literals (escapes honoured), scanning stopping as soon
// as the depth exceeds `stopAbove`. Not a validator: unbalanced or malformed text is left to the parser.
std::size_t jsonNestingDepth(std::string_view text, std::size_t stopAbove = kMaxJsonDepth) noexcept;

} // namespace esses
