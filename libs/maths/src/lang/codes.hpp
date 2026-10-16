// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The diagnostic codes of the maths language (03 sections 2.5, 3.2 and 6.4; D-020, D-021, D-085). The codes
// of 03 section 2.5 are reproduced as written there; the others are ours and listed in libs/maths/README.md
// ("Diagnostics of the language"). Codes are stable: tests, Channel Status and `esses-cli maths check --json`
// match on them. Internal to esses_maths (src/lang).
#pragma once

#include <string_view>

namespace esses::maths::codes {

// ---- lexer and parser (03 section 2.5)
// ---------------------------------------------------------------------
inline constexpr std::string_view kQuoteMissing = "E-QUOTE-MISSING";       // bare word, or unclosed ' / "
inline constexpr std::string_view kUnitBracket = "E-UNIT-BRACKET";         // 'X' km/h, stray [ ], [ unclosed
inline constexpr std::string_view kParenUnbalanced = "E-PAREN-UNBALANCED"; // ( without ), ) without (
inline constexpr std::string_view kCallUnclosed = "E-CALL-UNCLOSED";       // f(a, b  at the end
inline constexpr std::string_view kCommaMissing = "E-COMMA-MISSING";       // f(a b)
// ours
inline constexpr std::string_view kSyntax = "E-SYNTAX";        // any other unexpected token
inline constexpr std::string_view kExprEmpty = "E-EXPR-EMPTY"; // nothing but blanks/comments
inline constexpr std::string_view kUnexpectedChar =
    "E-UNEXPECTED-CHAR"; // # $ ; = and non-ASCII outside quotes
inline constexpr std::string_view kCommentUnclosed = "E-COMMENT-UNCLOSED"; // /* without */
inline constexpr std::string_view kNumberFormat = "E-NUMBER-FORMAT";       // 2x, 1e, 1e999
inline constexpr std::string_view kExprTooDeep = "E-EXPR-TOO-DEEP";        // nesting beyond the parser limit
inline constexpr std::string_view kNameEmpty = "E-NAME-EMPTY";             // ''
inline constexpr std::string_view kStyleCase = "W-STYLE-CASE"; // Abs(x): canonical spelling abs (Style)
inline constexpr std::string_view kCompatPow = "I-COMPAT-POW"; // -a^b depends on the compat switch

// ---- type checker (03 sections 2.4, 2.5, 3.2)
// --------------------------------------------------------------
inline constexpr std::string_view kArity = "E-ARITY";
inline constexpr std::string_view kArgType = "E-ARG-TYPE";
inline constexpr std::string_view kArgValue = "E-ARG-VALUE";
inline constexpr std::string_view kUnknownFunction = "E-UNKNOWN-FUNCTION";
inline constexpr std::string_view kUnknownChannel = "E-UNKNOWN-CHANNEL";
inline constexpr std::string_view kUnresolved = "I-UNRESOLVED";
inline constexpr std::string_view kUnitUnknown = "E-UNIT-UNKNOWN";
inline constexpr std::string_view kIntegerRequired = "E-INTEGER-REQUIRED";
inline constexpr std::string_view kDimMismatch = "W-DIM-MISMATCH";
inline constexpr std::string_view kUnitScale = "W-UNIT-SCALE"; // level Info (03 section 3.2 rule 4)
inline constexpr std::string_view kUnitMissing = "W-UNIT-MISSING";
inline constexpr std::string_view kUnitFamily = "W-UNIT-FAMILY";
inline constexpr std::string_view kResultDim = "W-RESULT-DIM";
inline constexpr std::string_view kResultScale = "I-RESULT-SCALE";
inline constexpr std::string_view kWindowArg = "W-WINDOW-ARG";

// ---- compile (03 sections 4.4, 6.4)
// ------------------------------------------------------------------------
inline constexpr std::string_view kRateZero = "E-RATE-ZERO";
inline constexpr std::string_view kKindUnsupported = "E-KIND-UNSUPPORTED";
inline constexpr std::string_view kPluginUnavailable = "E-PLUGIN-UNAVAILABLE";
inline constexpr std::string_view kKernelUnavailable = "E-KERNEL-UNAVAILABLE"; // ours: no kernel for a call

// ---- kernels (03 sections 2.4 rule 3, 4.4, 5.3, 5.4; src/kernels report them through Kernel::status or
// makeIntrinsicKernel, with the code in Error::context.detail)
// ----------------------------------------------------------------------------------------------------------
inline constexpr std::string_view kRangeUnknown = "E-RANGE-UNKNOWN"; // range path not in the source
inline constexpr std::string_view kNoDistance = "E-NO-DISTANCE";     // integrate_dist without Corr Dist
inline constexpr std::string_view kTableAxis = "E-TABLE-AXIS";       // breakpoints unusable
inline constexpr std::string_view kRefSource = "E-REF-SOURCE";       // variance vs another source's lap

} // namespace esses::maths::codes
