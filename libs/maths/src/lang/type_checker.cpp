// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TypeChecker (03 section 3.2, D-021, D-085). One iterative post-order walk from the root (no recursion, so
// the depth of the tree never reaches the stack): every node gets its Type, compile-time constants are folded
// with the same per-sample semantics the EwKernel uses (ew_ops.hpp), and every rule of 03 section 3.2 that
// fails becomes a diagnostic. Only E-* diagnostics block evaluation; dimension and unit findings are warnings
// or infos (D-021).

#include "checker_internal.hpp"
#include "codes.hpp"
#include "ew_ops.hpp"
#include "lang_util.hpp"
#include "signatures.hpp"

#include <esses/base/casefold.hpp>
#include <esses/maths/function_registry.hpp>
#include <esses/maths/type_checker.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace esses::maths {

namespace lang {

namespace {

constexpr std::uint32_t kNoParent = std::numeric_limits<std::uint32_t>::max();
constexpr double kScaleTolerance = 1e-9;

bool scalesDiffer(double a, double b) {
    if (!std::isfinite(a) || !std::isfinite(b)) {
        return false;
    }
    return std::fabs(a - b) > kScaleTolerance * std::max({1.0, std::fabs(a), std::fabs(b)});
}

std::string numberText(double v) {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os.precision(12);
    os << v;
    return os.str();
}

class Checker {
public:
    Checker(Ast& ast, const Scope& scope, const TypeChecker::Options& options, Diagnostics& diags)
        : ast_(ast), scope_(scope), units_(scope.units()), options_(options), diags_(diags) {}

    CheckResult run() {
        CheckResult result;
        const std::size_t n = ast_.nodes.size();
        info_.assign(n, NodeInfo{});
        parent_.assign(n, kNoParent);
        if (n == 0 || ast_.root >= n) {
            result.root.kind = Kind::Invalid;
            return result;
        }
        const std::size_t errorsBefore = errorCount();
        walk();
        checkRoot();
        result.root = ast_.nodes[ast_.root].type;
        result.reachedRoot = true;
        if (errorCount() > errorsBefore) {
            result.root.kind = Kind::Invalid;
        }
        result.info = std::move(info_);
        return result;
    }

private:
    // ---- walk
    // ------------------------------------------------------------------------------------------------

    void walk() {
        // Iterative post-order from the root; a node reached twice (a malformed DAG) is typed once.
        std::vector<std::uint8_t> state(ast_.nodes.size(), 0); // 0 new, 1 children pushed, 2 done
        std::vector<std::uint32_t> stack{ast_.root};
        while (!stack.empty()) {
            const std::uint32_t i = stack.back();
            if (state[i] == 2) {
                stack.pop_back();
                continue;
            }
            if (state[i] == 0) {
                state[i] = 1;
                for (const std::uint32_t k : ast_.nodes[i].kids) {
                    if (k < ast_.nodes.size() && state[k] == 0) {
                        parent_[k] = i;
                        stack.push_back(k);
                    }
                }
                continue;
            }
            stack.pop_back();
            state[i] = 2;
            visit(i);
        }
    }

    std::size_t errorCount() const {
        return static_cast<std::size_t>(std::count_if(
            diags_.begin(), diags_.end(), [](const Diagnostic& d) { return d.level == Diagnostic::Error; }));
    }

    void report(std::string_view code, Diagnostic::Level level, Span span, std::string message,
                std::string hint = {}) {
        diags_.push_back(makeDiag(code, level, span, std::move(message), std::move(hint)));
    }

    Node& node(std::uint32_t i) { return ast_.nodes[i]; }
    Type& type(std::uint32_t i) { return ast_.nodes[i].type; }

    // ---- type helpers
    // ----------------------------------------------------------------------------------------

    static Type scalar() {
        Type t;
        t.kind = Kind::Scalar;
        return t;
    }

    static Type unknownSeries() {
        Type t;
        t.kind = Kind::Series;
        t.dim = units::Dimension::unknown();
        t.scaleToBase = std::numeric_limits<double>::quiet_NaN();
        t.rate = Rate{};
        return t;
    }

    // Dimension and scale from the unit, when it has one.
    void normalise(Type& t) const {
        if (t.unit != units::kNoUnit && units_.contains(t.unit)) {
            const units::Unit& u = units_.unit(t.unit);
            t.dim = units_.quantity(u.qty).dim;
            t.scaleToBase = u.scalar;
        }
    }

    std::string unitLabel(units::UnitId u) const {
        if (u == units::kNoUnit || !units_.contains(u)) {
            return "no unit";
        }
        return std::string(units_.unit(u).label());
    }

    std::string dimText(const units::Dimension& d) const { return d.isDimensionless() ? "1" : d.toString(); }

    static std::optional<Rate> combineRates(const std::vector<const Type*>& types) {
        std::optional<Rate> out;
        for (const Type* t : types) {
            if (t == nullptr || !perSample(*t)) {
                continue;
            }
            const Rate r = t->rate.value_or(Rate{});
            if (!out.has_value() || (r.valid() && (!out->valid() || *out < r))) {
                out = r;
            }
        }
        return out;
    }

    // Kind of an elementwise combination (03 section 3.2 rule 3).
    static Kind combineKinds(const std::vector<const Type*>& types) {
        bool anyInvalid = false;
        bool anySeries = false;
        int laps = 0;
        bool anyPerSample = false;
        for (const Type* t : types) {
            if (t->kind == Kind::Invalid) {
                anyInvalid = true;
            }
            if (perSample(*t)) {
                anyPerSample = true;
                if (t->kind == Kind::LapScalar) {
                    ++laps;
                } else {
                    anySeries = true;
                }
            }
        }
        if (anyInvalid) {
            return Kind::Invalid;
        }
        if (!anyPerSample) {
            return Kind::Scalar;
        }
        if (laps == 1 && !anySeries) {
            return Kind::LapScalar; // LapScalar with scalars only
        }
        return Kind::Series;
    }

    bool isInteger(std::uint32_t i) const {
        const Type& t = ast_.nodes[i].type;
        return t.integer || t.kind == Kind::Bool || t.kind == Kind::Invalid || info_[i].unresolved;
    }

    bool isText(std::uint32_t i) const {
        const Type& t = ast_.nodes[i].type;
        return t.kind == Kind::Text || t.kind == Kind::RangeRef;
    }

    // Keywords outside edge_delay and text outside comparisons and range paths are E-ARG-TYPE.
    bool requireNumeric(std::uint32_t operand, std::string_view where) {
        if (info_[operand].keyword) {
            report(codes::kArgType, Diagnostic::Error, node(operand).span,
                   "'" + node(operand).text + "' is only an argument of edge_delay",
                   "edge_delay(x, t, rise) or edge_delay(x, t, fall)");
            return false;
        }
        if (isText(operand)) {
            report(codes::kArgType, Diagnostic::Error, node(operand).span,
                   "text cannot be used " + std::string(where),
                   "text can only be compared with text (== != < > ...)");
            return false;
        }
        return true;
    }

    // Two operands that should have the same dimension (+, -, comparisons, min, max, choose, hypot, atan2):
    // W-DIM-MISMATCH when the dimensions differ, W-UNIT-SCALE (Info) when only the scale does. A bare literal
    // takes the other side's dimension. Returns the operand whose dimension and unit the result takes.
    std::uint32_t unify(std::uint32_t a, std::uint32_t b, Span span, std::string_view what) {
        const Type& ta = type(a);
        const Type& tb = type(b);
        const bool la = info_[a].literal || ta.kind == Kind::Invalid;
        const bool lb = info_[b].literal || tb.kind == Kind::Invalid;
        if (la && !lb) {
            return b;
        }
        if (la || lb) {
            return a;
        }
        if (ta.dim.isKnown() && tb.dim.isKnown()) {
            if (ta.dim != tb.dim) {
                report(codes::kDimMismatch, Diagnostic::Warning, span,
                       std::string(what) + " values of different dimensions: " + dimText(ta.dim) + " (" +
                           unitLabel(ta.unit) + ") and " + dimText(tb.dim) + " (" + unitLabel(tb.unit) + ")",
                       "the numbers are used as they are; check the [unit] of each reference");
            } else if (scalesDiffer(ta.scaleToBase, tb.scaleToBase)) {
                report(codes::kUnitScale, Diagnostic::Info, span,
                       std::string(what) + " the same quantity in different units: " + unitLabel(ta.unit) +
                           " and " + unitLabel(tb.unit),
                       "no conversion is inserted; request the same [unit] on both sides");
            }
        }
        return a;
    }

    // ---- nodes
    // -----------------------------------------------------------------------------------------------

    void visit(std::uint32_t i) {
        switch (node(i).kind) {
        case NodeKind::IntLit:
        case NodeKind::DecLit: {
            Type t = scalar();
            t.integer = node(i).kind == NodeKind::IntLit;
            type(i) = t;
            info_[i].known = true;
            info_[i].value = node(i).number;
            info_[i].literal = true;
            return;
        }
        case NodeKind::StrLit:
            type(i) = Type{};
            type(i).kind = Kind::Text;
            info_[i].textKnown = true;
            info_[i].text = node(i).text;
            return;
        case NodeKind::Keyword:
            type(i) = Type{};
            type(i).kind = Kind::Text;
            info_[i].keyword = true;
            return;
        case NodeKind::ChannelRef:
            visitReference(i);
            return;
        case NodeKind::Convert:
            visitConvert(i);
            return;
        case NodeKind::Unary:
            visitUnary(i);
            return;
        case NodeKind::Binary:
            visitBinary(i);
            return;
        case NodeKind::Call:
            visitCall(i);
            return;
        }
    }

    void visitReference(std::uint32_t i) {
        Node& n = node(i);
        const std::optional<ScopeEntry> entry = scope_.lookup(caseFold(n.text), options_.requester);
        if (!entry) {
            info_[i].unresolved = true;
            n.type = unknownSeries();
            if (scope_.dataBacked()) {
                report(codes::kUnknownChannel, Diagnostic::Error, n.span,
                       "no channel, constant or detail is named '" + n.text + "'",
                       "check the spelling; names are not case-sensitive");
            } else {
                report(codes::kUnresolved, Diagnostic::Info, n.span,
                       "'" + n.text + "' is not resolved: no data is loaded");
            }
            return;
        }
        for (Diagnostic d : entry->diags) {
            if (d.span.begin == 0 && d.span.end == 0) {
                d.span = n.span;
            }
            diags_.push_back(std::move(d));
        }
        Type t = entry->type;
        normalise(t);
        switch (entry->what) {
        case ScopeEntry::Channel:
            t.kind = Kind::Series;
            if (!t.rate) {
                t.rate = Rate{};
            }
            break;
        case ScopeEntry::Constant:
        case ScopeEntry::DetailNumber:
        case ScopeEntry::Setup:
            t.kind = Kind::Scalar;
            t.rate.reset();
            if (entry->number) {
                info_[i].known = true;
                info_[i].value = *entry->number;
            }
            break;
        case ScopeEntry::DetailText:
            t.kind = Kind::Text;
            t.rate.reset();
            info_[i].textKnown = true;
            info_[i].text = entry->text;
            break;
        }
        n.type = t;
        info_[i].entry = entry;

        // W-UNIT-MISSING (03 section 2.4 rule 5): a dimensioned quantity read without [unit] is in base
        // units.
        const std::uint32_t p = parent_[i];
        const bool hasUnit = p != kNoParent && node(p).kind == NodeKind::Convert && !node(p).text.empty();
        if (!hasUnit && entry->what != ScopeEntry::DetailText && t.unit != units::kNoUnit &&
            units_.contains(t.unit) && units_.unit(t.unit).qty != units::kUnitless) {
            const std::string sym = units_.unit(t.unit).symbol;
            report(codes::kUnitMissing, Diagnostic::Warning, n.span,
                   "'" + n.text + "' has no [unit]: its values are read in " + unitLabel(t.unit),
                   "write '" + n.text + "'[" + sym + "] or the unit you mean");
        }
    }

    void visitConvert(std::uint32_t i) {
        Node& n = node(i);
        if (n.kids.empty()) {
            n.type = unknownSeries();
            return;
        }
        const std::uint32_t ref = n.kids[0];
        const Type t = type(ref);
        info_[i] = info_[ref];
        info_[i].entry.reset();
        n.unit.reset();
        n.type = t;
        if (n.text.empty()) {
            return; // [] = as stored
        }
        if (isText(ref)) {
            report(codes::kArgType, Diagnostic::Error, n.span, "a unit cannot follow a text value");
            return;
        }
        const bool hasQuantity = t.unit != units::kNoUnit && units_.contains(t.unit);
        std::optional<units::UnitId> u;
        if (hasQuantity) {
            u = units_.find(n.text, units_.quantityOf(t.unit));
        }
        if (!u) {
            u = units_.find(n.text);
        }
        if (!u) {
            std::vector<std::string> symbols;
            for (const units::Unit& unit : units_.units()) {
                if (!unit.isCompat()) {
                    symbols.push_back(unit.symbol);
                }
            }
            const std::string near = nearestName(n.text, symbols);
            report(codes::kUnitUnknown, Diagnostic::Error, n.span, "unknown unit [" + n.text + "]",
                   near.empty() ? "Tools > Units lists the unit spellings" : "did you mean [" + near + "]?");
            return;
        }
        if (info_[ref].unresolved) {
            n.unit = u; // without data the requested unit is all there is to type the reference with
            n.type.unit = *u;
            normalise(n.type);
            return;
        }
        const units::Quantity& want = units_.quantity(units_.quantityOf(*u));
        const units::Quantity& have =
            units_.quantity(units_.quantityOf(hasQuantity ? t.unit : units::kNoUnit));
        if (!hasQuantity && want.dim == units::Dimension::none() && *u == want.base) {
            // A unitless channel read in the unit of factor one of a dimensionless quantity ([ratio], [rad]):
            // the numbers are the same either way, so they are read as stored and typed in that unit, without
            // W-UNIT-FAMILY (lang.unit-family in docs/maths-compat.md). n.unit stays empty: no conversion.
            n.type.unit = *u;
            normalise(n.type);
            return;
        }
        if (want.family != have.family) {
            report(codes::kUnitFamily, Diagnostic::Warning, n.span,
                   "'" + node(ref).text + "' is " + (hasQuantity ? have.name : std::string("unitless")) +
                       "; [" + n.text + "] is " + want.name + ": the values are not converted",
                   "request a unit of " + (hasQuantity ? have.name : std::string("the channel's quantity")));
            return;
        }
        n.unit = u;
        n.type.unit = *u;
        normalise(n.type);
        const Result<units::Converter> conv = units_.converter(t.unit, *u);
        if (conv && !conv->identity()) {
            n.type.integer = false;
            if (info_[i].known) {
                info_[i].value = (*conv)(info_[i].value);
            }
        }
    }

    void visitUnary(std::uint32_t i) {
        Node& n = node(i);
        const std::uint32_t k = n.kids.empty() ? i : n.kids[0];
        if (k == i) {
            n.type = unknownSeries();
            return;
        }
        if (!requireNumeric(k, "with a sign or a logical operator")) {
            n.type = unknownSeries();
            return;
        }
        Type t = type(k);
        info_[i].literal = info_[k].literal;
        switch (n.uop) {
        case UnaryOp::Neg:
        case UnaryOp::Plus:
            if (t.kind == Kind::Bool) {
                t.kind = perSample(t) ? Kind::Series : Kind::Scalar;
            }
            break;
        case UnaryOp::Not:
            t = boolOf({&type(k)});
            break;
        case UnaryOp::BitNot:
            if (!isInteger(k)) {
                report(codes::kIntegerRequired, Diagnostic::Error, n.span, "'~' needs an integer operand",
                       "use int(x) or an integer channel");
            }
            if (t.kind == Kind::Bool) {
                t.kind = perSample(t) ? Kind::Series : Kind::Scalar;
            }
            t.integer = true;
            break;
        }
        n.type = t;
        if (info_[k].known) {
            info_[i].known = true;
            info_[i].value = ew::unary(n.uop, info_[k].value);
        }
    }

    static Type boolOf(const std::vector<const Type*>& operands) {
        Type t;
        t.kind = Kind::Bool;
        t.integer = true;
        t.rate = combineRates(operands);
        return t;
    }

    void visitBinary(std::uint32_t i) {
        Node& n = node(i);
        if (n.kids.size() != 2) {
            n.type = unknownSeries();
            return;
        }
        const std::uint32_t a = n.kids[0];
        const std::uint32_t b = n.kids[1];
        if (info_[a].keyword || info_[b].keyword) {
            requireNumeric(info_[a].keyword ? a : b, "here");
            n.type = unknownSeries();
            return;
        }
        const Type& ta = type(a);
        const Type& tb = type(b);
        switch (n.bop) {
        case BinaryOp::Lt:
        case BinaryOp::Le:
        case BinaryOp::Gt:
        case BinaryOp::Ge:
        case BinaryOp::Eq:
        case BinaryOp::Ne:
            visitComparison(i, a, b);
            return;
        default:
            break;
        }
        const char* where =
            n.bop == BinaryOp::And || n.bop == BinaryOp::Or ? "with && or ||" : "in arithmetic";
        const bool okA = requireNumeric(a, where);
        const bool okB = requireNumeric(b, where);
        if (!okA || !okB) {
            n.type = unknownSeries();
            return;
        }
        info_[i].literal = info_[a].literal && info_[b].literal;
        Type t;
        t.kind = combineKinds({&ta, &tb});
        t.rate = combineRates({&ta, &tb});
        const bool la = info_[a].literal;
        const bool lb = info_[b].literal;
        switch (n.bop) {
        case BinaryOp::Add:
        case BinaryOp::Sub: {
            const std::uint32_t rep = unify(a, b, n.span, n.bop == BinaryOp::Add ? "adds" : "subtracts");
            t.dim = type(rep).dim;
            t.unit = type(rep).unit;
            t.scaleToBase = type(rep).scaleToBase;
            if (!la && !lb && ta.unit != tb.unit) {
                t.unit = ta.dim == tb.dim && !scalesDiffer(ta.scaleToBase, tb.scaleToBase) ? ta.unit
                                                                                           : units::kNone;
            }
            t.integer = isInteger(a) && isInteger(b) && ta.kind != Kind::Invalid && tb.kind != Kind::Invalid;
            break;
        }
        case BinaryOp::Mul:
        case BinaryOp::Div: {
            const bool mul = n.bop == BinaryOp::Mul;
            t.dim = mul ? ta.dim * tb.dim : ta.dim / tb.dim;
            t.scaleToBase = mul ? ta.scaleToBase * tb.scaleToBase : ta.scaleToBase / tb.scaleToBase;
            // A numeric literal factor other than 1 is how units are converted by hand ('Speed'[km/h] / 3.6,
            // derivative(...) / 9.81): the dimension stays and the scale becomes unknown, so no
            // I-RESULT-SCALE is inferred through it (D-185; W-RESULT-DIM still checks the dimension).
            const auto handFactor = [&](std::uint32_t k, const Type& other) {
                return info_[k].literal && info_[k].known && std::fabs(info_[k].value) != 1.0 &&
                       other.dim.isKnown() && !other.dim.isDimensionless();
            };
            if (handFactor(a, tb) || handFactor(b, ta)) {
                t.scaleToBase = std::numeric_limits<double>::quiet_NaN();
            }
            // A plain number keeps the other operand's unit; anything else is a composite.
            const bool plainA =
                la || (ta.unit == units::kNoUnit && ta.dim.isDimensionless() && ta.scaleToBase == 1.0);
            const bool plainB =
                lb || (tb.unit == units::kNoUnit && tb.dim.isDimensionless() && tb.scaleToBase == 1.0);
            if (plainB) {
                t.unit = ta.unit;
            } else if (plainA && mul) {
                t.unit = tb.unit;
            } else {
                t.unit = units::kNone;
            }
            t.integer =
                mul && isInteger(a) && isInteger(b) && ta.kind != Kind::Invalid && tb.kind != Kind::Invalid;
            break;
        }
        case BinaryOp::Pow:
            powerType(t, a, b);
            break;
        case BinaryOp::And:
        case BinaryOp::Or:
            t = boolOf({&ta, &tb});
            break;
        case BinaryOp::BitAnd:
        case BinaryOp::BitOr:
            for (const std::uint32_t k : {a, b}) {
                if (!isInteger(k)) {
                    report(codes::kIntegerRequired, Diagnostic::Error, node(k).span,
                           std::string("'") + (n.bop == BinaryOp::BitAnd ? "&" : "|") +
                               "' needs integer operands",
                           "use int(x), an integer channel, or && / || for a logical test");
                }
            }
            t.dim = ta.dim;
            t.unit = ta.unit;
            t.scaleToBase = ta.scaleToBase;
            t.integer = true;
            break;
        default:
            break;
        }
        n.type = t;
        if (info_[a].known && info_[b].known) {
            info_[i].known = true;
            info_[i].value = ew::binary(n.bop, info_[a].value, info_[b].value);
        }
    }

    // ^ and power(x, y) (03 section 3.2 rule 5): the exponent should be unitless; an integer constant
    // exponent multiplies the dimension, anything else leaves a dimensioned base's dimension unknown.
    void powerType(Type& t, std::uint32_t base, std::uint32_t exponent) {
        const Type& tb = type(base);
        const Type& te = type(exponent);
        if (!info_[exponent].literal && te.dim.isKnown() && !te.dim.isDimensionless()) {
            report(codes::kDimMismatch, Diagnostic::Warning, node(exponent).span,
                   "the exponent has the dimension " + dimText(te.dim) + "; an exponent should be unitless");
        }
        const bool intExponent = info_[exponent].known &&
                                 std::trunc(info_[exponent].value) == info_[exponent].value &&
                                 std::fabs(info_[exponent].value) <= 64.0;
        if (intExponent) {
            const int e = static_cast<int>(info_[exponent].value);
            t.dim = tb.dim.pow(e);
            t.scaleToBase = std::pow(tb.scaleToBase, static_cast<double>(e));
            t.unit = e == 1 ? tb.unit : units::kNone;
        } else if (tb.dim.isKnown() && tb.dim.isDimensionless()) {
            t.dim = units::Dimension::none();
            t.scaleToBase = tb.scaleToBase == 1.0 ? 1.0 : std::numeric_limits<double>::quiet_NaN();
            t.unit = units::kNone;
        } else {
            t.dim = info_[base].literal ? units::Dimension::none() : units::Dimension::unknown();
            t.scaleToBase = info_[base].literal ? 1.0 : std::numeric_limits<double>::quiet_NaN();
            t.unit = units::kNone;
        }
        t.integer = false;
    }

    void visitComparison(std::uint32_t i, std::uint32_t a, std::uint32_t b) {
        Node& n = node(i);
        const bool textA = isText(a);
        const bool textB = isText(b);
        if ((textA && info_[b].unresolved) || (textB && info_[a].unresolved)) {
            n.type = boolOf({}); // text against a name the data-less scope cannot resolve
            return;
        }
        if (textA || textB) {
            if (!(textA && textB)) {
                report(codes::kArgType, Diagnostic::Error, n.span, "text can only be compared with text",
                       "compare a text detail with \"text\"");
                n.type = unknownSeries();
                return;
            }
            n.type = boolOf({});
            if (info_[a].textKnown && info_[b].textKnown) {
                const int c = info_[a].text.compare(info_[b].text); // UTF-8 code units, case-sensitive
                bool r = false;
                switch (n.bop) {
                case BinaryOp::Lt:
                    r = c < 0;
                    break;
                case BinaryOp::Le:
                    r = c <= 0;
                    break;
                case BinaryOp::Gt:
                    r = c > 0;
                    break;
                case BinaryOp::Ge:
                    r = c >= 0;
                    break;
                case BinaryOp::Eq:
                    r = c == 0;
                    break;
                default:
                    r = c != 0;
                    break;
                }
                info_[i].known = true;
                info_[i].value = r ? 1.0 : 0.0;
            }
            return;
        }
        (void)unify(a, b, n.span, "compares");
        n.type = boolOf({&type(a), &type(b)});
        info_[i].literal = info_[a].literal && info_[b].literal;
        if (info_[a].known && info_[b].known) {
            info_[i].known = true;
            info_[i].value = ew::binary(n.bop, info_[a].value, info_[b].value);
        }
    }

    // ---- calls
    // -----------------------------------------------------------------------------------------------

    void visitCall(std::uint32_t i) {
        Node& n = node(i);
        const FunctionRegistry& reg = scope_.functions();
        const FunctionManifest* manifest = reg.find(n.text);
        std::optional<IntrinsicId> id = reg.intrinsicId(n.text);
        if (manifest == nullptr && !id) {
            id = intrinsicByName(n.text);
        }
        if (id) {
            info_[i].intrinsic = id;
            visitIntrinsic(i, *id);
            return;
        }
        if (manifest != nullptr) {
            info_[i].manifest = manifest;
            visitExternal(i, *manifest);
            return;
        }
        std::vector<std::string> names;
        for (const FunctionManifest* m : reg.all()) {
            names.push_back(m->name);
        }
        for (std::size_t k = 0; k < static_cast<std::size_t>(IntrinsicId::Count); ++k) {
            names.emplace_back(intrinsicName(static_cast<IntrinsicId>(k)));
        }
        const std::string near = nearestName(n.text, names);
        report(codes::kUnknownFunction, Diagnostic::Error, n.span, "unknown function " + n.text + "()",
               near.empty() ? "the Maths Functions list shows every function"
                            : "did you mean " + near + "()?");
        n.type = unknownSeries();
    }

    void argError(std::uint32_t arg, std::string_view fn, std::size_t k, std::string_view what) {
        report(codes::kArgType, Diagnostic::Error, node(arg).span,
               std::string(fn) + "(): argument " + std::to_string(k + 1) + " " + std::string(what));
    }

    // A compile-time number in a parameter slot, or an E-ARG-TYPE. `seconds` / `hertz` normalise a time or a
    // frequency to s / Hz.
    bool constantParam(std::uint32_t arg, std::string_view fn, std::size_t k, std::string_view expect) {
        if (info_[arg].keyword || isText(arg)) {
            argError(arg, fn, k, "must be " + std::string(expect));
            return false;
        }
        if (perSample(type(arg))) {
            argError(arg, fn, k, "must be " + std::string(expect) + ", not a channel");
            return false;
        }
        if (!info_[arg].known) {
            if (!info_[arg].unresolved) {
                argError(arg, fn, k, "must be a constant (" + std::string(expect) + ")");
            }
            return false;
        }
        return true;
    }

    // The value of a time (or plain number) in seconds; nullopt and E-ARG-TYPE for another dimension.
    std::optional<double> asSeconds(std::uint32_t arg, std::string_view fn, std::size_t k) {
        const Type& t = type(arg);
        if (info_[arg].literal || t.dim.isDimensionless() || t.dim.isUnknown()) {
            return info_[arg].value;
        }
        if (t.dim == units::Dimension::basis(units::Dimension::Time)) {
            return info_[arg].value / t.scaleToBase;
        }
        argError(arg, fn, k, "must be a time, not " + dimText(t.dim));
        return std::nullopt;
    }

    void windowParam(std::uint32_t arg, std::string_view fn, std::size_t k) {
        if (!constantParam(arg, fn, k, "a number of points or seconds")) {
            return;
        }
        const Type& t = type(arg);
        NodeInfo& in = info_[arg];
        bool seconds = false;
        double v = in.value;
        if (in.literal) {
            // 03 section 2.4 rule 2: an integer literal counts points, a decimal one seconds (1e3 is
            // decimal).
            seconds = !t.integer;
        } else if (t.dim == units::Dimension::basis(units::Dimension::Time)) {
            seconds = true;
            v = in.value / t.scaleToBase;
        } else if (t.dim.isDimensionless() || t.dim.isUnknown()) {
            if (std::trunc(v) == v) {
                seconds = false;
            } else {
                seconds = true;
                report(codes::kWindowArg, Diagnostic::Warning, node(arg).span,
                       std::string(fn) + "(): the window " + numberText(v) +
                           " is not a whole number of points: read as seconds",
                       "write an integer for points or a decimal literal for seconds");
            }
        } else {
            argError(arg, fn, k, "must be a number of points or seconds, not " + dimText(t.dim));
            return;
        }
        if (!(v >= 0.0)) {
            report(codes::kArgValue, Diagnostic::Error, node(arg).span,
                   std::string(fn) + "(): the window must not be negative");
            return;
        }
        in.param = true;
        in.paramIsSeconds = seconds;
        in.paramInteger = !seconds;
        in.paramValue = v;
    }

    void secondsParam(std::uint32_t arg, IntrinsicId id, std::string_view fn, std::size_t k) {
        if (!constantParam(arg, fn, k, "a time in seconds")) {
            return;
        }
        const std::optional<double> s = asSeconds(arg, fn, k);
        if (!s) {
            return;
        }
        if (id != IntrinsicId::TimeShift && *s < 0.0) {
            report(codes::kArgValue, Diagnostic::Error, node(arg).span,
                   std::string(fn) + "(): the time must not be negative");
            return;
        }
        info_[arg].param = true;
        info_[arg].paramIsSeconds = true;
        info_[arg].paramValue = *s;
    }

    void hertzParam(std::uint32_t arg, std::string_view fn, std::size_t k) {
        if (!constantParam(arg, fn, k, "a frequency in Hz")) {
            return;
        }
        const Type& t = type(arg);
        double hz = info_[arg].value;
        if (!info_[arg].literal && t.dim.isKnown() && !t.dim.isDimensionless()) {
            if (t.dim != units::Dimension::basis(units::Dimension::Time, -1)) {
                argError(arg, fn, k, "must be a frequency, not " + dimText(t.dim));
                return;
            }
            hz = info_[arg].value / t.scaleToBase;
        }
        if (!(hz > 0.0)) {
            report(codes::kArgValue, Diagnostic::Error, node(arg).span,
                   std::string(fn) + "(): the frequency must be above 0 Hz");
            return;
        }
        info_[arg].param = true;
        info_[arg].paramValue = hz;
    }

    void orderParam(std::uint32_t arg, IntrinsicId id, std::string_view fn, std::size_t k) {
        if (!constantParam(arg, fn, k, "an integer filter order")) {
            return;
        }
        const double v = info_[arg].value;
        const bool rc = id == IntrinsicId::FilterLp || id == IntrinsicId::FilterHp;
        static constexpr std::array<int, 5> kRc{1, 2, 3, -1, -2};
        static constexpr std::array<int, 6> kCheby{2, 4, 6, -4, -8, -12};
        bool ok = std::trunc(v) == v;
        if (ok) {
            const int o = static_cast<int>(v);
            ok = rc ? std::find(kRc.begin(), kRc.end(), o) != kRc.end()
                    : std::find(kCheby.begin(), kCheby.end(), o) != kCheby.end();
        }
        if (!ok) {
            report(codes::kArgValue, Diagnostic::Error, node(arg).span,
                   std::string(fn) + "(): order " + numberText(v) + " is not supported",
                   rc ? "the order is 1, 2 or 3 (forward passes), -1 (one backward pass) or -2 (zero phase)"
                      : "the order is 2, 4 or 6 (forward) or -4, -8 or -12 (zero phase)");
            return;
        }
        info_[arg].param = true;
        info_[arg].paramInteger = true;
        info_[arg].paramValue = v;
    }

    void visitIntrinsic(std::uint32_t i, IntrinsicId id) {
        Node& n = node(i);
        const Signature& sig = signatureOf(id);
        const std::string fn = n.text;
        const std::size_t argc = n.kids.size();
        if (!sig.accepts(argc)) {
            report(codes::kArity, Diagnostic::Error, n.span,
                   fn + "() takes " + sig.aritiesText() +
                       (sig.arities[sig.arityCount - 1] == 1 ? " argument" : " arguments") + ", not " +
                       std::to_string(argc),
                   "check the argument list of " + fn + "()");
            n.type = unknownSeries();
            return;
        }
        bool ok = true;
        bool allKnown = true;
        bool allLiteral = true;
        std::vector<const Type*> seriesArgs;
        for (std::size_t k = 0; k < argc; ++k) {
            const std::uint32_t arg = n.kids[k];
            const Role role = k < sig.paramCount ? sig.params[k].role : Role::Value;
            const bool argPerSample = perSample(type(arg));
            if (role != Role::Edge && info_[arg].keyword) {
                ok = requireNumeric(arg, "") && ok;
                continue;
            }
            switch (role) {
            case Role::Value:
            case Role::Cond:
            case Role::Reset:
                if (!requireNumeric(arg, "as an argument of " + fn + "()")) {
                    ok = false;
                }
                seriesArgs.push_back(&type(arg));
                break;
            case Role::Channel:
                if (!requireNumeric(arg, "as an argument of " + fn + "()")) {
                    ok = false;
                } else if (!argPerSample && !info_[arg].unresolved && type(arg).kind != Kind::Invalid) {
                    argError(arg, fn, k, "must be a channel or an expression of channels, not a constant");
                    ok = false;
                }
                seriesArgs.push_back(&type(arg));
                break;
            case Role::WindowOrChannel:
                if (argPerSample || info_[arg].unresolved) {
                    if (!requireNumeric(arg, "as an argument of " + fn + "()")) {
                        ok = false;
                    }
                    seriesArgs.push_back(&type(arg));
                } else {
                    windowParam(arg, fn, k);
                }
                break;
            case Role::Window:
                windowParam(arg, fn, k);
                break;
            case Role::Seconds:
                secondsParam(arg, id, fn, k);
                break;
            case Role::Hertz:
                hertzParam(arg, fn, k);
                break;
            case Role::Order:
                orderParam(arg, id, fn, k);
                break;
            case Role::Ripple:
                if (constantParam(arg, fn, k, "a ripple in percent")) {
                    if (!(info_[arg].value >= 0.0)) {
                        report(codes::kArgValue, Diagnostic::Error, node(arg).span,
                               fn + "(): the ripple must not be negative");
                    } else {
                        info_[arg].param = true;
                        info_[arg].paramValue = info_[arg].value;
                    }
                }
                break;
            case Role::Count:
                if (constantParam(arg, fn, k, "a whole number")) {
                    const double v = info_[arg].value;
                    const double min = id == IntrinsicId::PreviousSample ? 1.0 : 0.0;
                    if (std::trunc(v) != v || v < min) {
                        report(codes::kArgValue, Diagnostic::Error, node(arg).span,
                               fn + "(): " + numberText(v) + " is not a whole number of at least " +
                                   numberText(min));
                    } else {
                        info_[arg].param = true;
                        info_[arg].paramInteger = true;
                        info_[arg].paramValue = v;
                    }
                }
                break;
            case Role::RangePath:
                if (node(arg).kind != NodeKind::StrLit) {
                    argError(arg, fn, k, "must be a range path in double quotes, such as \"Outings:Laps\"");
                    ok = false;
                } else if (node(arg).text.empty()) {
                    report(codes::kArgValue, Diagnostic::Error, node(arg).span,
                           fn + "(): the range path is empty");
                    ok = false;
                } else {
                    type(arg).kind = Kind::RangeRef;
                }
                break;
            case Role::Edge:
                if (!info_[arg].keyword) {
                    argError(arg, fn, k, "must be rise or fall");
                    ok = false;
                }
                break;
            }
            if (!info_[arg].known) {
                allKnown = false;
            }
            if (!info_[arg].literal) {
                allLiteral = false;
            }
        }
        if (!ok) {
            n.type = unknownSeries();
            return;
        }
        resultType(i, id, sig, seriesArgs);
        const bool elementwise = intrinsicClass(id) == KernelClass::Elementwise;
        if (elementwise && id != IntrinsicId::RandVal && id != IntrinsicId::RandChan &&
            id != IntrinsicId::Invalid) {
            info_[i].literal = allLiteral && argc > 0;
            if (allKnown && argc > 0) {
                const double a = info_[n.kids[0]].value;
                const double b = argc > 1 ? info_[n.kids[1]].value : 0.0;
                const double c =
                    argc > 2 ? info_[n.kids[2]].value : (id == IntrinsicId::BitDecode ? 1.0 : 0.0);
                info_[i].known = true;
                info_[i].value = ew::call(id, a, b, c);
            }
        }
    }

    void resultType(std::uint32_t i, IntrinsicId id, const Signature& sig,
                    const std::vector<const Type*>& seriesArgs) {
        Node& n = node(i);
        const std::string& fn = n.text;
        const std::size_t argc = n.kids.size();
        const std::uint32_t x = argc > 0 ? n.kids[0] : i;
        const Type tx = argc > 0 ? type(x) : Type{};
        const bool elementwise = intrinsicClass(id) == KernelClass::Elementwise;
        Type t;
        if (elementwise) {
            std::vector<const Type*> all;
            all.reserve(n.kids.size());
            for (const std::uint32_t k : n.kids) {
                all.push_back(&type(k));
            }
            t.kind = combineKinds(all);
            t.rate = combineRates(all);
        } else {
            t.kind = Kind::Series;
            t.rate = combineRates(seriesArgs);
            if (!t.rate) {
                t.rate = Rate{};
            }
        }
        const auto sameAs = [&](const Type& s) {
            t.dim = s.dim;
            t.unit = s.unit;
            t.scaleToBase = s.scaleToBase;
        };
        const auto unitless = [&]() {
            t.dim = units::Dimension::none();
            t.unit = units::kNone;
            t.scaleToBase = 1.0;
        };
        const auto radians = [&]() {
            unitless();
            if (const std::optional<units::UnitId> rad = units_.find("rad")) {
                t.unit = *rad;
            }
        };
        switch (sig.rule) {
        case Rule::SameAs0:
            sameAs(tx);
            t.integer = tx.integer && (id == IntrinsicId::Abs || id == IntrinsicId::Remainder);
            break;
        case Rule::Unitless:
            unitless();
            break;
        case Rule::UnitlessFromDim:
            if (!info_[x].literal && tx.dim.isKnown() && !tx.dim.isDimensionless()) {
                report(codes::kDimMismatch, Diagnostic::Warning, node(x).span,
                       fn + "() expects a dimensionless argument, not " + dimText(tx.dim));
            }
            unitless();
            break;
        case Rule::AngleIn:
            if (!info_[x].literal && tx.dim.isKnown() && !tx.dim.isDimensionless()) {
                report(codes::kDimMismatch, Diagnostic::Warning, node(x).span,
                       fn + "() expects an angle, not " + dimText(tx.dim));
            } else if (!info_[x].literal && scalesDiffer(tx.scaleToBase, 1.0)) {
                report(codes::kUnitScale, Diagnostic::Info, node(x).span,
                       fn + "() takes radians; the argument is in " + unitLabel(tx.unit),
                       "write the reference with [rad]");
            }
            unitless();
            break;
        case Rule::AngleOut:
            radians();
            break;
        case Rule::Atan2:
            (void)unify(n.kids[0], n.kids[1], n.span, "atan2() takes");
            radians();
            break;
        case Rule::Sqrt0:
            t.dim = tx.dim.pow(units::Rational{1, 2});
            t.scaleToBase = std::sqrt(tx.scaleToBase);
            t.unit = tx.dim.isDimensionless() ? tx.unit : units::kNone;
            break;
        case Rule::Sqr0:
            t.dim = tx.dim.pow(2);
            t.scaleToBase = tx.scaleToBase * tx.scaleToBase;
            t.unit = tx.dim.isDimensionless() ? tx.unit : units::kNone;
            t.integer = tx.integer;
            break;
        case Rule::Pow01:
            powerType(t, n.kids[0], n.kids[1]);
            break;
        case Rule::Unify01: {
            const std::uint32_t rep = unify(n.kids[0], n.kids[1], n.span, fn + "() takes");
            sameAs(type(rep));
            t.integer = id != IntrinsicId::Hypot && isInteger(n.kids[0]) && isInteger(n.kids[1]);
            break;
        }
        case Rule::Integer0:
            sameAs(tx);
            t.integer = true;
            break;
        case Rule::Sign:
            unitless();
            t.integer = true;
            break;
        case Rule::Bitwise:
        case Rule::BitDecode:
            for (const std::uint32_t k : n.kids) {
                if (!isInteger(k)) {
                    report(codes::kIntegerRequired, Diagnostic::Error, node(k).span,
                           fn + "() needs integer arguments", "use int(x) or an integer channel");
                }
            }
            if (sig.rule == Rule::Bitwise) {
                sameAs(tx);
            } else {
                unitless();
            }
            t.integer = true;
            break;
        case Rule::Choose: {
            const std::uint32_t a = n.kids[1];
            const std::uint32_t b = n.kids[2];
            const std::uint32_t rep = unify(a, b, n.span, "choose() returns");
            sameAs(type(rep));
            const Type& ta = type(a);
            const Type& tb = type(b);
            // 03 section 3.2 rule 8: Invalid unifies with anything; Scalar < LapScalar < Series.
            if (ta.kind == Kind::Bool && tb.kind == Kind::Bool) {
                t.kind = Kind::Bool;
            } else if (ta.kind == Kind::Invalid && tb.kind == Kind::Invalid) {
                t.kind = Kind::Invalid;
            } else {
                std::vector<const Type*> branches;
                for (const Type* bt : std::array<const Type*, 3>{&ta, &tb, &type(n.kids[0])}) {
                    if (bt->kind != Kind::Invalid) {
                        branches.push_back(bt);
                    }
                }
                t.kind = combineKinds(branches);
                if (t.kind == Kind::Scalar && perSample(type(n.kids[0]))) {
                    t.kind = Kind::Series;
                }
            }
            t.integer = isInteger(a) && isInteger(b);
            break;
        }
        case Rule::Invalid:
            t = Type{};
            t.kind = Kind::Invalid;
            break;
        case Rule::RandomScalar:
            unitless();
            t.kind = Kind::Scalar;
            t.rate.reset();
            break;
        case Rule::RandomSeries:
            unitless();
            t.kind = Kind::Series;
            t.rate = Rate{};
            break;
        case Rule::Derivative:
            if (argc >= 2 && perSample(type(n.kids[1]))) {
                const Type& ty = type(n.kids[1]);
                t.dim = tx.dim / ty.dim;
                t.scaleToBase = tx.scaleToBase / ty.scaleToBase;
            } else {
                t.dim = tx.dim / units::Dimension::basis(units::Dimension::Time);
                t.scaleToBase = tx.scaleToBase;
            }
            t.unit = units::kNone;
            break;
        case Rule::TimesTime0:
            t.dim = tx.dim * units::Dimension::basis(units::Dimension::Time);
            t.scaleToBase = tx.scaleToBase;
            t.unit = units::kNone;
            break;
        case Rule::TimesLength0:
            t.dim = tx.dim * units::Dimension::basis(units::Dimension::Length);
            t.scaleToBase = tx.scaleToBase;
            t.unit = units::kNone;
            break;
        case Rule::Stat:
            sameAs(tx);
            t.integer = tx.integer && id != IntrinsicId::StatMean && id != IntrinsicId::StatStdDev;
            if (argc < 3) {
                t.kind = Kind::Scalar; // the math window: SelectionDependent (D-080)
                t.rate.reset();
            } else {
                t.kind = Kind::LapScalar;
                const Node& reset = node(n.kids[2]);
                if (reset.kind == NodeKind::Call && reset.text == "range_change" && reset.kids.size() == 1 &&
                    node(reset.kids[0]).kind == NodeKind::StrLit) {
                    t.lapGroup = node(reset.kids[0]).text;
                }
            }
            break;
        case Rule::Series0:
            sameAs(tx);
            t.integer = tx.integer && (id == IntrinsicId::TimeShift || id == IntrinsicId::PreviousSample ||
                                       id == IntrinsicId::RunningMin || id == IntrinsicId::RunningMax);
            break;
        case Rule::Bool:
            unitless();
            t.kind = Kind::Bool;
            t.integer = true;
            break;
        case Rule::Time:
            t.dim = units::Dimension::basis(units::Dimension::Time);
            t.scaleToBase = 1.0;
            t.unit = units_.find("s").value_or(units::kNone);
            break;
        case Rule::RangeCount:
            unitless();
            t.integer = true;
            break;
        }
        n.type = t;
    }

    // An external function (03 section 10.1): arity and argument kinds from its manifest; the unit rule of 03
    // section 3.2 rule 9 by argument name.
    void visitExternal(std::uint32_t i, const FunctionManifest& m) {
        Node& n = node(i);
        const std::size_t argc = n.kids.size();
        if (!m.arities.empty() &&
            std::find(m.arities.begin(), m.arities.end(),
                      static_cast<std::uint8_t>(std::min<std::size_t>(argc, 255))) == m.arities.end()) {
            std::string list;
            for (std::size_t k = 0; k < m.arities.size(); ++k) {
                list += (k == 0 ? "" : (k + 1 == m.arities.size() ? " or " : ", ")) +
                        std::to_string(m.arities[k]);
            }
            report(codes::kArity, Diagnostic::Error, n.span,
                   n.text + "() takes " + list + " arguments, not " + std::to_string(argc));
            n.type = unknownSeries();
            return;
        }
        std::vector<const Type*> series;
        bool ok = true;
        for (std::size_t k = 0; k < argc; ++k) {
            const std::uint32_t arg = n.kids[k];
            const ArgSpec::Kind kind = k < m.args.size() ? m.args[k].kind : ArgSpec::Kind::Series;
            switch (kind) {
            case ArgSpec::Kind::Scalar:
            case ArgSpec::Kind::Series:
                ok = requireNumeric(arg, "as an argument of " + n.text + "()") && ok;
                series.push_back(&type(arg));
                break;
            case ArgSpec::Kind::Text:
                if (!isText(arg)) {
                    argError(arg, n.text, k, "must be text");
                    ok = false;
                }
                break;
            case ArgSpec::Kind::RangeRef:
                if (node(arg).kind != NodeKind::StrLit) {
                    argError(arg, n.text, k, "must be a range path in double quotes");
                    ok = false;
                } else {
                    type(arg).kind = Kind::RangeRef;
                }
                break;
            case ArgSpec::Kind::Keyword:
                if (!info_[arg].keyword) {
                    argError(arg, n.text, k, "must be a keyword");
                    ok = false;
                }
                break;
            }
        }
        if (!ok) {
            n.type = unknownSeries();
            return;
        }
        Type t;
        t.kind = m.returns.kind == ArgSpec::Kind::Scalar && combineRates(series) == std::nullopt
                     ? Kind::Scalar
                     : Kind::Series;
        if (t.kind == Kind::Series) {
            t.rate = combineRates(series).value_or(Rate{});
        }
        applyUnitRule(t, n, m);
        n.type = t;
    }

    void applyUnitRule(Type& t, const Node& n, const FunctionManifest& m) {
        const std::string& rule = m.returns.unitRule;
        const std::size_t colon = rule.find(':');
        const std::string head = rule.substr(0, colon);
        std::vector<std::string> names;
        if (colon != std::string::npos) {
            std::string rest = rule.substr(colon + 1);
            std::size_t start = 0;
            while (start <= rest.size()) {
                const std::size_t comma = rest.find(',', start);
                names.push_back(
                    rest.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
                if (comma == std::string::npos) {
                    break;
                }
                start = comma + 1;
            }
        }
        const auto argType = [&](std::size_t which) -> const Type* {
            if (which >= names.size()) {
                return nullptr;
            }
            for (std::size_t k = 0; k < m.args.size() && k < n.kids.size(); ++k) {
                if (m.args[k].name == names[which]) {
                    return &type(n.kids[k]);
                }
            }
            return nullptr;
        };
        t.dim = units::Dimension::unknown();
        t.scaleToBase = std::numeric_limits<double>::quiet_NaN();
        t.unit = units::kNone;
        const Type* a = argType(0);
        if (head == "unitless" || head == "angle") {
            t.dim = units::Dimension::none();
            t.scaleToBase = 1.0;
        } else if ((head == "same_as" || head == "unify") && a != nullptr) {
            t.dim = a->dim;
            t.unit = a->unit;
            t.scaleToBase = a->scaleToBase;
        } else if (head == "sqrt" && a != nullptr) {
            t.dim = a->dim.pow(units::Rational{1, 2});
            t.scaleToBase = std::sqrt(a->scaleToBase);
        } else if (head == "per_time" && a != nullptr) {
            t.dim = a->dim / units::Dimension::basis(units::Dimension::Time);
            t.scaleToBase = a->scaleToBase;
        } else if (head == "times_time" && a != nullptr) {
            t.dim = a->dim * units::Dimension::basis(units::Dimension::Time);
            t.scaleToBase = a->scaleToBase;
        } else if (head == "times_length" && a != nullptr) {
            t.dim = a->dim * units::Dimension::basis(units::Dimension::Length);
            t.scaleToBase = a->scaleToBase;
        }
    }

    // ---- root
    // ------------------------------------------------------------------------------------------------

    void checkRoot() {
        const std::uint32_t r = ast_.root;
        Node& n = node(r);
        if (info_[r].keyword) {
            requireNumeric(r, "");
            return;
        }
        if (isText(r)) {
            report(codes::kArgType, Diagnostic::Error, n.span, "the expression gives text, not numbers",
                   "compare the text, e.g. 'Driver' == \"Joe\"");
            return;
        }
        if (!options_.resultUnit || !numericKind(n.type) || info_[r].literal || info_[r].unresolved) {
            return;
        }
        const units::UnitId declared = *options_.resultUnit;
        if (!units_.contains(declared)) {
            return;
        }
        const units::Unit& du = units_.unit(declared);
        const units::Dimension want = units_.quantity(du.qty).dim;
        const Type& t = n.type;
        if (t.kind == Kind::Invalid || t.dim.isUnknown()) {
            return;
        }
        const std::string what = options_.tableAxis ? "the axis unit" : "the declared unit";
        if (t.dim != want) {
            report(codes::kResultDim, Diagnostic::Warning, n.span,
                   "the expression gives " + dimText(t.dim) + ", " + what + " " + unitLabel(declared) +
                       " is " + dimText(want),
                   options_.tableAxis ? "check the [unit] of the references or the table's axis unit"
                                      : "check the [unit] of the references or the item's result unit");
            return;
        }
        if (scalesDiffer(t.scaleToBase, du.scalar)) {
            // The suggestion: the unit the numbers are in (its symbol is what the item editor takes).
            std::optional<units::UnitId> suggestion;
            if (t.unit != units::kNoUnit && units_.contains(t.unit) && units_.unit(t.unit).qty == du.qty) {
                suggestion = t.unit;
            } else {
                for (const units::UnitId u : units_.unitsOf(du.qty)) {
                    const units::Unit& cand = units_.unit(u);
                    if (!scalesDiffer(cand.scalar, t.scaleToBase) && cand.offset == du.offset) {
                        suggestion = u;
                        break;
                    }
                }
            }
            const std::string unitWord = options_.tableAxis ? "axis unit " : "result unit ";
            report(codes::kResultScale, Diagnostic::Info, n.span,
                   "the values are " +
                       (suggestion ? "in " + unitLabel(*suggestion) : std::string("in another unit")) + ", " +
                       what + " is " + unitLabel(declared),
                   suggestion
                       ? (options_.tableAxis ? "request [" + units_.unit(declared).symbol +
                                                   "] in the axis expression or set the axis unit " +
                                                   units_.unit(*suggestion).symbol
                                             : "declare the " + unitWord + units_.unit(*suggestion).symbol)
                       : "check the " + unitWord.substr(0, unitWord.size() - 1));
        }
    }

    Ast& ast_;
    const Scope& scope_;
    const units::UnitRegistry& units_;
    const TypeChecker::Options& options_;
    Diagnostics& diags_;
    std::vector<NodeInfo> info_;
    std::vector<std::uint32_t> parent_;
};

} // namespace

CheckResult checkAst(Ast& ast, const Scope& scope, const TypeChecker::Options& options, Diagnostics& diags) {
    return Checker(ast, scope, options, diags).run();
}

} // namespace lang

Type TypeChecker::check(Ast& ast, const Scope& scope, Diagnostics& diags) {
    return check(ast, scope, Options{}, diags);
}

Type TypeChecker::check(Ast& ast, const Scope& scope, const Options& options, Diagnostics& diags) {
    return lang::checkAst(ast, scope, options, diags).root;
}

} // namespace esses::maths
