// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Parser (03 sections 2.2-2.5, D-020): Pratt parser over the Lexer's tokens with the binding powers of 03
// section 2.3 (-2^2 = -4 unless MathsCompat::unaryMinusAbovePower; I-COMPAT-POW once per affected
// expression). Syntax errors are diagnostics with the 03 section 2.5 codes; parse() fails (ErrorCode::Parse,
// context.span) when any Error was reported and returns the AST otherwise.
#pragma once

#include <esses/base/result.hpp>
#include <esses/maths/ast.hpp>
#include <esses/maths/diagnostics.hpp>
#include <esses/maths/maths_compat.hpp>

#include <string_view>

namespace esses::maths {

class Parser {
public:
    static Result<Ast> parse(std::string_view src, const MathsCompat& compat, Diagnostics& diags);
};

} // namespace esses::maths
