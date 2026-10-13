// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// What the type checker learns about each node beyond its Type, for compile(): compile-time constants (folded
// literals, constants, numeric and text details), the scope entry of every reference, the function of every
// call, and how a window argument reads (points or seconds). Internal to esses_maths (src/lang).
#pragma once

#include <esses/maths/ast.hpp>
#include <esses/maths/diagnostics.hpp>
#include <esses/maths/intrinsics.hpp>
#include <esses/maths/math_function.hpp>
#include <esses/maths/scope.hpp>
#include <esses/maths/type_checker.hpp>
#include <esses/maths/types.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace esses::maths::lang {

struct NodeInfo {
    bool known = false; // a compile-time number: `value`, in the units of the node's type
    double value = 0.0;
    bool textKnown = false; // a compile-time text: `text`
    std::string text;
    bool literal = false;            // a bare numeric literal or an expression of literals only (no unit)
    bool unresolved = false;         // a reference the scope did not resolve
    bool keyword = false;            // rise / fall
    std::optional<ScopeEntry> entry; // ChannelRef: what the scope returned
    std::optional<IntrinsicId> intrinsic;       // Call: the intrinsic
    const FunctionManifest* manifest = nullptr; // Call: an external function's manifest
    // Window, Seconds and Hertz parameters: the canonical value (points, seconds or hertz) and its reading.
    bool param = false;
    bool paramIsSeconds = false; // Window: seconds (true) or points (false)
    bool paramInteger = false;   // points, filter order, counts: an IntLit for the kernel
    double paramValue = 0.0;
};

struct CheckResult {
    Type root;
    std::vector<NodeInfo> info; // one per node of the AST
    bool reachedRoot = false;
};

CheckResult checkAst(Ast& ast, const Scope& scope, const TypeChecker::Options& options, Diagnostics& diags);

// A value that varies per sample: Series, LapScalar, and a Bool or Invalid of per-sample operands (rate set).
inline bool perSample(const Type& t) noexcept {
    return t.kind == Kind::Series || t.kind == Kind::LapScalar ||
           ((t.kind == Kind::Bool || t.kind == Kind::Invalid) && t.rate.has_value());
}

inline bool numericKind(const Type& t) noexcept {
    return t.kind == Kind::Scalar || t.kind == Kind::Series || t.kind == Kind::LapScalar ||
           t.kind == Kind::Bool || t.kind == Kind::Invalid;
}

} // namespace esses::maths::lang
