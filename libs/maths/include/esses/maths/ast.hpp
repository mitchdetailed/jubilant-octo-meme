// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The expression AST (03 section 3.1, D-020): a flat node array with child indices, the original source and
// its normalised hash. Parser::parse builds it; TypeChecker::check fills Node::type; compile() lowers it into
// a Plan. print() is the canonical form: print(parse(x)) is idempotent and parse(print(parse(x))) is
// structurally equal (03 section 14.2 property 1).
#pragma once

#include <esses/base/hash.hpp>
#include <esses/maths/diagnostics.hpp>
#include <esses/maths/types.hpp>
#include <esses/units/unit_registry.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace esses::maths {

enum class NodeKind : std::uint8_t {
    IntLit,
    DecLit,
    StrLit,
    Keyword, // rise / fall (edge_delay only)
    ChannelRef,
    Unary,
    Binary,
    Call,
    Convert // [unit] after a reference
};

enum class UnaryOp : std::uint8_t { Neg, Plus, Not, BitNot };
enum class BinaryOp : std::uint8_t {
    Add,
    Sub,
    Mul,
    Div,
    Pow,
    Lt,
    Le,
    Gt,
    Ge,
    Eq,
    Ne,
    BitAnd,
    BitOr,
    And,
    Or
};

struct Node {
    NodeKind kind = NodeKind::IntLit;
    Span span;
    // Literal text, channel name (original spelling), function name (canonical lower case) or keyword.
    std::string text;
    double number = 0.0; // IntLit / DecLit value
    UnaryOp uop{};
    BinaryOp bop{};
    std::vector<std::uint32_t> kids;   // child indices in Ast::nodes
    std::optional<units::UnitId> unit; // Convert: requested unit (nullopt = "[]")
    Type type;                         // filled by TypeChecker
};

struct Ast {
    std::vector<Node> nodes;
    std::uint32_t root = 0;
    std::string source;
    // xxh3 of the normalised source (comments stripped, whitespace collapsed, names case-folded), so cosmetic
    // edits do not invalidate caches (03 section 7.2 defHash).
    Hash128 sourceHash;
};

// Canonical form of the expression (03 section 3.1).
std::string print(const Ast& ast);

} // namespace esses::maths
