// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Helpers for the maths language tests (tests/lang): parse helpers, a fully parenthesised rendering of an AST
// (independent of print(), so precedence tests do not trust the code under test), diagnostic code lists and
// TestScope, a data-backed Scope over a fixed table of channels, constants and details.
#pragma once

#include "../maths_test_support.hpp"

#include <esses/base/casefold.hpp>
#include <esses/maths/ast.hpp>
#include <esses/maths/diagnostics.hpp>
#include <esses/maths/function_registry.hpp>
#include <esses/maths/maths_compat.hpp>
#include <esses/maths/parser.hpp>
#include <esses/maths/scope.hpp>
#include <esses/maths/type_checker.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::maths::test {

inline std::vector<std::string> codesOf(const Diagnostics& diags) {
    std::vector<std::string> out;
    out.reserve(diags.size());
    for (const Diagnostic& d : diags) {
        out.push_back(d.code);
    }
    return out;
}

inline bool hasCode(const Diagnostics& diags, std::string_view code) {
    return std::any_of(diags.begin(), diags.end(), [&](const Diagnostic& d) { return d.code == code; });
}

inline std::size_t countCode(const Diagnostics& diags, std::string_view code) {
    return static_cast<std::size_t>(
        std::count_if(diags.begin(), diags.end(), [&](const Diagnostic& d) { return d.code == code; }));
}

inline const Diagnostic* findCode(const Diagnostics& diags, std::string_view code) {
    const auto it =
        std::find_if(diags.begin(), diags.end(), [&](const Diagnostic& d) { return d.code == code; });
    return it == diags.end() ? nullptr : &*it;
}

inline Ast parseOk(std::string_view src, const MathsCompat& compat = {}) {
    Diagnostics diags;
    Result<Ast> ast = Parser::parse(src, compat, diags);
    INFO("source: " << src);
    if (!ast) {
        for (const Diagnostic& d : diags) {
            UNSCOPED_INFO(d.code << " [" << d.span.begin << "," << d.span.end << ") " << d.message);
        }
    }
    REQUIRE(ast.ok());
    return std::move(ast).value();
}

inline std::string_view opName(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add:
        return "+";
    case BinaryOp::Sub:
        return "-";
    case BinaryOp::Mul:
        return "*";
    case BinaryOp::Div:
        return "/";
    case BinaryOp::Pow:
        return "^";
    case BinaryOp::Lt:
        return "<";
    case BinaryOp::Le:
        return "<=";
    case BinaryOp::Gt:
        return ">";
    case BinaryOp::Ge:
        return ">=";
    case BinaryOp::Eq:
        return "==";
    case BinaryOp::Ne:
        return "!=";
    case BinaryOp::BitAnd:
        return "&";
    case BinaryOp::BitOr:
        return "|";
    case BinaryOp::And:
        return "&&";
    case BinaryOp::Or:
        return "||";
    }
    return "?";
}

inline std::string_view opName(UnaryOp op) {
    switch (op) {
    case UnaryOp::Neg:
        return "neg";
    case UnaryOp::Plus:
        return "plus";
    case UnaryOp::Not:
        return "not";
    case UnaryOp::BitNot:
        return "bnot";
    }
    return "?";
}

// (op a b) form of a subtree.
inline std::string sexpr(const Ast& ast, std::uint32_t i) {
    const Node& n = ast.nodes.at(i);
    switch (n.kind) {
    case NodeKind::IntLit:
    case NodeKind::DecLit:
        return n.text;
    case NodeKind::StrLit:
        return "\"" + n.text + "\"";
    case NodeKind::Keyword:
        return "#" + n.text;
    case NodeKind::ChannelRef:
        return "'" + n.text + "'";
    case NodeKind::Convert:
        return sexpr(ast, n.kids.at(0)) + "[" + n.text + "]";
    case NodeKind::Unary:
        return "(" + std::string(opName(n.uop)) + " " + sexpr(ast, n.kids.at(0)) + ")";
    case NodeKind::Binary:
        return "(" + std::string(opName(n.bop)) + " " + sexpr(ast, n.kids.at(0)) + " " +
               sexpr(ast, n.kids.at(1)) + ")";
    case NodeKind::Call: {
        std::string s = "(" + n.text;
        for (const std::uint32_t k : n.kids) {
            s += " " + sexpr(ast, k);
        }
        return s + ")";
    }
    }
    return "?";
}
inline std::string sexpr(const Ast& ast) {
    return sexpr(ast, ast.root);
}

inline std::string sexprOf(std::string_view src, const MathsCompat& compat = {}) {
    return sexpr(parseOk(src, compat));
}

// A data-backed scope over a fixed table (names compare case-folded).
class TestScope final : public Scope {
public:
    explicit TestScope(const FunctionRegistry& functions = FunctionRegistry::builtin())
        : Scope(functions, *shippedUnits()) {}

    std::optional<ScopeEntry> lookup(std::string_view nameCf,
                                     const ChannelKey& /*requester*/) const override {
        const auto it = entries_.find(std::string(nameCf));
        if (it == entries_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    ScopeEntry& channel(const std::string& name, std::string_view unit, Rate rate = Rate::fromHz(10),
                        store::Dtype dtype = store::Dtype::F32,
                        store::Interp interp = store::Interp::Linear) {
        ScopeEntry e;
        e.what = ScopeEntry::Channel;
        e.type.kind = Kind::Series;
        e.type.unit = unit.empty() ? units::kNoUnit : unitOf(unit);
        e.type.rate = rate;
        e.type.integer = dtype == store::Dtype::I32;
        e.key = ChannelKey::make(model::SourceId{}, "Normal", name);
        e.resolvedName = name;
        e.interp = interp;
        e.dtype = dtype;
        return entries_[caseFold(name)] = e;
    }
    ScopeEntry& constant(const std::string& name, double v, std::string_view unit = "",
                         ScopeEntry::What what = ScopeEntry::Constant) {
        ScopeEntry e;
        e.what = what;
        e.type.kind = Kind::Scalar;
        e.type.unit = unit.empty() ? units::kNoUnit : unitOf(unit);
        e.number = v;
        e.resolvedName = name;
        return entries_[caseFold(name)] = e;
    }
    ScopeEntry& text(const std::string& name, std::string value) {
        ScopeEntry e;
        e.what = ScopeEntry::DetailText;
        e.type.kind = Kind::Text;
        e.text = std::move(value);
        e.resolvedName = name;
        return entries_[caseFold(name)] = e;
    }

private:
    std::map<std::string, ScopeEntry> entries_;
};

// Parses and checks; the type of the root and every diagnostic of both steps.
struct Checked {
    Ast ast;
    Type type;
    Diagnostics diags;
    bool parsed = false;
};

inline Checked check(std::string_view src, const Scope& scope,
                     std::optional<std::string_view> resultUnit = {}, const MathsCompat& compat = {}) {
    Checked c;
    Result<Ast> ast = Parser::parse(src, compat, c.diags);
    if (!ast) {
        return c;
    }
    c.parsed = true;
    c.ast = std::move(ast).value();
    TypeChecker::Options options;
    if (resultUnit) {
        options.resultUnit = resultUnit->empty() ? units::kNoUnit : unitOf(*resultUnit);
    }
    c.type = TypeChecker::check(c.ast, scope, options, c.diags);
    return c;
}

} // namespace esses::maths::test
