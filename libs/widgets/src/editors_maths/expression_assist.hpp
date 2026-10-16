// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Text helpers of the Maths Editor's expression field (03 section 11 items 5-7; private to
// src/editors_maths): the mapping between QString positions and the UTF-8 byte offsets of maths::Span, the
// "[unit] after a channel reference" context of the unit popup, the token under the mouse for the hover help,
// and the help texts (the operator precedence of 03 section 2.3 with the unaryMinusAbovePower setting, a
// function's signature).
#pragma once

#include <esses/maths/function_registry.hpp>
#include <esses/maths/lexer.hpp>
#include <esses/maths/maths_compat.hpp>

#include <QString>
#include <cstdint>
#include <optional>

namespace esses::widgets::assist {

// The UTF-8 byte offset of QString position `pos` (clamped to the text).
std::uint32_t byteOffset(const QString& text, int pos);
// The QString position of a UTF-8 byte offset (clamped; an offset inside a code point maps to its start).
int charPosition(const QString& text, std::uint32_t byte);

// The caret inside the brackets of a unit annotation after a channel reference: 'Name'[km|] or 'Name' [|].
struct UnitBracket {
    QString channel; // the referenced name, without the quotes
    int open = -1;   // position of '['
    int close = -1;  // position of the matching ']' after the caret, or -1 while it is not typed yet
    QString typed;   // the text between '[' and the caret
};
std::optional<UnitBracket> unitBracketAt(const QString& text, int position);

// The text with `symbol` written between the brackets of `bracket` (a missing ']' is added) and the caret
// position after the closing bracket.
struct Edit {
    QString text;
    int caret = 0;
};
Edit insertUnit(const QString& text, const UnitBracket& bracket, const QString& symbol);

// The token of `text` that contains character position `pos` (End when none).
maths::Token tokenAt(const QString& text, int pos);

// Hover help: the operator precedence table (highest first) and what the compat switch does to -2^2.
QString precedenceHelp(const maths::MathsCompat& compat);
// Hover help of a function: signature, accepted argument counts, description.
QString functionHelp(const maths::FunctionManifest& manifest);

} // namespace esses::widgets::assist
