// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TypeChecker (03 section 3.2, D-021, D-085): infers every node's Type bottom-up against a Scope and reports
// dimension/unit warnings (W-DIM-MISMATCH, W-UNIT-SCALE, W-UNIT-MISSING, W-UNIT-FAMILY, W-RESULT-DIM, ...),
// unresolved names (E-UNKNOWN-CHANNEL with a data-backed scope, I-UNRESOLVED without data), arity and
// argument errors (E-ARITY, E-ARG-TYPE, E-ARG-VALUE, E-UNKNOWN-FUNCTION) and the one hard type error
// E-INTEGER-REQUIRED. Warnings never block evaluation.
//
// Readings (docs/maths-compat.md): a bare numeric literal, and an expression of literals only, takes the
// dimension of whatever it meets, so `'Speed'[km/h] > 100` or `'Temp'[degC] - 5` raise no W-DIM-MISMATCH (the
// reference tool adds the numbers as written; the literal carries no unit to compare). Function names resolve
// through the Scope's FunctionRegistry first; an intrinsic of 03 section 5 is known even to a registry
// without its manifest.
//
// The Convert node of 'Name'[unit] keeps the unit's spelling in Node::text ("" for []); check() resolves it
// into Node::unit when the conversion applies (same display family, 03 section 2.4 rule 5) and leaves
// Node::unit empty for [], for an unknown unit (E-UNIT-UNKNOWN) and for a family mismatch (W-UNIT-FAMILY:
// the numbers pass through unconverted).
#pragma once

#include <esses/maths/ast.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/maths/diagnostics.hpp>
#include <esses/maths/scope.hpp>
#include <esses/maths/types.hpp>
#include <esses/units/unit_registry.hpp>

#include <optional>

namespace esses::maths {

class TypeChecker {
public:
    // M2 contract addition: what the caller knows about the item (MathsEditor, compile()).
    struct Options {
        ChannelKey requester;                    // passed to Scope::lookup (self-maths, 03 section 4.3)
        std::optional<units::UnitId> resultUnit; // the item's declared unit: W-RESULT-DIM, I-RESULT-SCALE
        bool tableAxis = false; // resultUnit is a table2d axis unit (the wording of those two diagnostics)
    };

    // Fills Node::type of every node of `ast` and returns the root's type (Kind::Invalid after an Error).
    static Type check(Ast& ast, const Scope& scope, Diagnostics& diags);
    static Type check(Ast& ast, const Scope& scope, const Options& options, Diagnostics& diags);
};

} // namespace esses::maths
