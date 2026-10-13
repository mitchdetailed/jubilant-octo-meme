// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// compile() (03 sections 4.4, 6.1-6.5, 7.2; D-022, D-139): type-checks a private copy of the parsed AST and
// lowers it into a Plan.
//
//   * Maximal elementwise subtrees (operators, the elementwise intrinsics of 03 section 5.1, references and
//     their [unit] conversions, constants) become one EwKernel stage each; compile-time constants are folded
//     into immediates, so `'A'[km/h] * 3.6 / 2` costs one load and one multiply per sample.
//   * Every other intrinsic call becomes its own stage from makeIntrinsicKernel(). Its series arguments
//     (signatures.hpp roles Value, Channel, Cond, Reset, and derivative's per-sample y) are the stage's
//     inputs in argument order; every parameter (window, seconds, hertz, order, ripple, count) is folded into
//     a literal node of the AST handed to makeIntrinsicKernel: IntLit for points, orders and counts, DecLit
//     for seconds, hertz and ripple (in s, Hz and %), StrLit for range paths, Keyword for rise/fall.
//   * The last stage is the result. For an expression item the values are in the item's result unit and the
//     last stage converts them to the quantity's base unit (03 section 6.1); so for a scale_offset item,
//     whose synthesised expression MathGraph computes in the unit of its offset (D-186) and names in
//     resultUnit.
//   * Inputs are gathered at the result rate in the requested [unit] with the channel's interpolation, except
//     the channels of smooth's first argument, which are always resampled Linear (03 section 6.4 rule 6).
//   * integrate_dist reads the source's Corr Dist (base metres, resampled Linear) as one more stage input
//     after its arguments; a source without Corr Dist is E-NO-DISTANCE (03 section 5.4).
//   * A table2d item compiles its axis expression only; the host appends the Table stage after it
//     (MathGraph's plan builder, 03 sections 4.4 and 5.7). The table data is part of the defHash. The axis
//     expression is checked against the axis unit as a declared unit (W-RESULT-DIM, I-RESULT-SCALE): its
//     numbers are compared with the breakpoints as they are (D-185).
//   * Result rate (03 section 6.4): an explicit rate; else the highest rate of the plan's Series inputs (the
//     root's rate, or a channel read only by a statistic, whose Scalar result carries no rate); the host
//     takes the layer's highest rate only for a plan without Series inputs (D-185).
//   * Output dtype (03 section 6.5, D-185): I32 only for Bool results, enumerations and integer results that
//     provably fit (integer channels and constants through + - unary bitwise choose min max abs and the
//     integer roundings of integer values); every other integer result is F64 (exact to 2^53), and an
//     integer or Bool result converted from a declared non-base unit is real.

#include "checker_internal.hpp"
#include "codes.hpp"
#include "ew_kernel.hpp"
#include "ew_ops.hpp"
#include "lang_util.hpp"
#include "signatures.hpp"

#include <esses/base/hash.hpp>
#include <esses/maths/plan.hpp>
#include <esses/maths/type_checker.hpp>
#include <esses/units/unit_registry.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace esses::maths {

namespace {

using lang::EwInstr;
using lang::EwOperand;

// Part of every expression defHash (03 section 7.2 engineVersion): bump when a lowering rule changes.
constexpr std::string_view kEngineVersion = "esses_maths 0.1 whole-node M2 expression";

std::string literalText(double v) {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os.precision(17);
    os << v;
    return os.str();
}

std::string_view itemKindName(ItemKind k) {
    switch (k) {
    case ItemKind::Expression:
        return "expression";
    case ItemKind::Filter:
        return "filter";
    case ItemKind::ScaleOffset:
        return "scale_offset";
    case ItemKind::Table2D:
        return "table2d";
    case ItemKind::Table3D:
        return "table3d";
    case ItemKind::Builtin:
        return "builtin";
    case ItemKind::Plugin:
        return "plugin";
    case ItemKind::Regression:
        return "regression";
    }
    return "?";
}

// SSA values of one elementwise region before register allocation.
struct EwBuilder {
    std::vector<EwInstr> code; // dst = the instruction's own index until allocation
    std::vector<Stage::Ref> inputs;
    std::vector<std::pair<Stage::Ref, std::uint16_t>> loads; // input -> the value that loaded it

    static EwOperand imm(double v) {
        EwOperand o;
        o.imm = true;
        o.value = v;
        return o;
    }
    static EwOperand val(std::size_t index) {
        EwOperand o;
        o.imm = false;
        o.reg = static_cast<std::uint16_t>(index);
        return o;
    }
    EwOperand push(EwInstr ins) {
        ins.dst = static_cast<std::uint16_t>(code.size());
        code.push_back(ins);
        return val(code.size() - 1);
    }
    EwOperand load(const Stage::Ref& ref) {
        for (const auto& [r, v] : loads) {
            if (r.from == ref.from && r.index == ref.index) {
                return val(v);
            }
        }
        std::uint16_t slot = 0;
        while (slot < inputs.size() && !(inputs[slot].from == ref.from && inputs[slot].index == ref.index)) {
            ++slot;
        }
        if (slot == inputs.size()) {
            inputs.push_back(ref);
        }
        EwInstr ins;
        ins.op = EwInstr::Op::Load;
        ins.input = slot;
        const EwOperand o = push(ins);
        loads.emplace_back(ref, o.reg);
        return o;
    }
};

// Linear-scan register allocation over the SSA values: a register is free again after the last instruction
// that reads it, so the destination of an instruction may reuse one of its operands.
std::uint16_t allocate(std::vector<EwInstr>& code, std::uint16_t result, std::uint16_t& regs) {
    const std::size_t n = code.size();
    std::vector<std::size_t> lastUse(n, 0);
    const auto use = [&](const EwOperand& o, std::size_t at) {
        if (!o.imm && o.reg < n) {
            lastUse[o.reg] = std::max(lastUse[o.reg], at);
        }
    };
    for (std::size_t i = 0; i < n; ++i) {
        use(code[i].a, i);
        use(code[i].b, i);
        use(code[i].c, i);
    }
    lastUse[result] = n; // live to the end
    std::vector<std::uint16_t> phys(n, 0);
    std::vector<std::uint16_t> freeRegs;
    std::uint16_t count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        EwInstr& ins = code[i];
        for (EwOperand* o : {&ins.a, &ins.b, &ins.c}) {
            if (!o->imm && o->reg < n) {
                const std::uint16_t v = o->reg;
                o->reg = phys[v];
                if (lastUse[v] == i) {
                    freeRegs.push_back(phys[v]);
                    lastUse[v] = std::numeric_limits<std::size_t>::max(); // freed once
                }
            }
        }
        if (!freeRegs.empty()) {
            phys[i] = freeRegs.back();
            freeRegs.pop_back();
        } else {
            phys[i] = count++;
        }
        ins.dst = phys[i];
        if (lastUse[i] == 0 && i != result) {
            freeRegs.push_back(phys[i]); // never read
        }
    }
    regs = count;
    return phys[result];
}

class Compiler {
public:
    Compiler(Ast& ast, lang::CheckResult& checked, const Scope& scope, const MathItem& item,
             const MathsCompat& compat, Diagnostics& diags)
        : ast_(ast), info_(checked.info), scope_(scope), units_(scope.units()), item_(item), compat_(compat),
          diags_(diags) {}

    Result<Plan> run() {
        foldParameters();
        plan_.defHash = definitionHash();
        seed_ = plan_.defHash.lo ^ ew::splitmix64(plan_.defHash.hi);
        const std::uint32_t root = ast_.root;

        units::Converter toBase;
        if ((item_.kind == ItemKind::Expression || item_.kind == ItemKind::ScaleOffset) &&
            item_.resultUnit != units::kNoUnit && units_.contains(item_.resultUnit)) {
            // An expression's declared unit, or the unit a scale_offset item is computed in (D-186).
            toBase = units_.toBase(item_.resultUnit);
        }
        Result<std::uint32_t> last =
            (isStageCall(root) && toBase.identity()) ? stageFor(root) : ewStage(root, false, toBase);
        if (!last) {
            return Failure{last.error()};
        }

        const Type& out = ast_.nodes[root].type;
        plan_.out = out;
        if (!toBase.identity() && !item_.enumeration.has_value()) {
            // The values were converted from the declared unit to its base unit: no longer whole numbers or
            // 0/1 (a Bool 'X' > 1 declared in % stores 0.01), so the result is real (D-185).
            plan_.out.integer = false;
            if (plan_.out.kind == Kind::Bool) {
                plan_.out.kind = lang::perSample(out) ? Kind::Series : Kind::Scalar;
            }
        }
        if (!item_.rate.automatic) {
            plan_.out.rate = item_.rate.rate;
        } else {
            // 03 section 6.4 rule 2: the highest rate among the Series inputs, compared exactly. A Series
            // read only through a statistic (a Scalar) still counts; rule 3 (the layer's highest logged rate,
            // taken by the host) is for a plan without Series inputs.
            std::optional<Rate> best;
            if (out.rate && out.rate->valid()) {
                best = out.rate;
            }
            if (inputRate_ && (!best || *best < *inputRate_)) {
                best = inputRate_;
            }
            if (best) {
                plan_.out.rate = best;
            } else {
                plan_.out.rate.reset(); // no series input: the host takes the layer's highest logged rate
            }
        }
        plan_.extent = ExtentMode::Whole;
        plan_.outDtype = outputDtype();
        plan_.halo = haloOf(static_cast<std::uint32_t>(plan_.stages.size() - 1));
        std::sort(plan_.rangeGroups.begin(), plan_.rangeGroups.end());
        plan_.rangeGroups.erase(std::unique(plan_.rangeGroups.begin(), plan_.rangeGroups.end()),
                                plan_.rangeGroups.end());
        return std::move(plan_);
    }

private:
    const Node& node(std::uint32_t i) const { return ast_.nodes[i]; }

    Failure error(std::string_view code, Span span, const std::string& message, std::string hint = {}) {
        diags_.push_back(lang::makeDiag(code, Diagnostic::Error, span, message, std::move(hint)));
        ErrorContext ctx;
        ctx.detail = std::string(code);
        ctx.span = span;
        return Failure{Error{ErrorCode::Invalid, std::string(code) + ": " + message, ctx}};
    }

    // ---- parameters and hashes
    // -------------------------------------------------------------------------------

    // The AST makeIntrinsicKernel reads: parameter arguments become literal nodes in canonical units.
    void foldParameters() {
        folded_ = ast_;
        for (std::size_t i = 0; i < folded_.nodes.size(); ++i) {
            const lang::NodeInfo& in = info_[i];
            if (!in.param) {
                continue;
            }
            Node& n = folded_.nodes[i];
            n.kind = in.paramInteger ? NodeKind::IntLit : NodeKind::DecLit;
            n.number = in.paramValue;
            n.text = literalText(in.paramValue);
            n.kids.clear();
            n.type = Type{};
            n.type.integer = in.paramInteger;
        }
    }

    Hash128 definitionHash() const {
        Hash128Stream h;
        h.updatePart(kEngineVersion);
        h.updatePart(ast_.sourceHash.hex());
        h.updatePart(itemKindName(item_.kind));
        h.updatePart(item_.rate.automatic ? std::string("auto") : item_.rate.rate.toString());
        h.updatePart(units_.contains(item_.resultUnit) ? units_.unit(item_.resultUnit).symbol
                                                       : std::string());
        h.updatePart(store::interpName(item_.interp));
        h.updatePart(item_.precision.value_or(""));
        h.updatePart(item_.enumeration.value_or(""));
        // MathsCompat::unaryMinusAbovePower is not hashed: it only changes how an expression with both a sign
        // and '^' parses, and the canonical text of that AST (sourceHash, above) already differs between the
        // two settings, so flipping it re-versions exactly those expressions (03 section 2.3).
        if (item_.kind == ItemKind::Table2D) { // 03 section 7.2: the table data is part of the definition
            const TableSpec& t = item_.table;
            h.updatePart(units_.contains(t.x.unit) ? units_.unit(t.x.unit).symbol : std::string());
            for (const double b : t.x.breakpoints) {
                h.updatePart(literalText(b));
            }
            h.updatePart(units_.contains(t.valuesUnit) ? units_.unit(t.valuesUnit).symbol : std::string());
            for (const double v : t.values) {
                h.updatePart(literalText(v));
            }
            h.updatePart(t.interpolation == TableSpec::Interp::Linear ? "linear"
                         : t.interpolation == TableSpec::Interp::Next ? "next"
                                                                      : "previous");
            h.updatePart(t.outOfRange == TableSpec::OutOfRange::Clamp ? "clamp" : "invalid");
        }
        for (const auto& [from, to] : compat_.functionAliases) {
            h.updatePart(from);
            h.updatePart(to);
        }
        return h.digest();
    }

    store::Dtype outputDtype() const {
        const Type& t = plan_.out;
        if (t.kind == Kind::Bool || item_.enumeration.has_value() || (t.integer && fitsI32(ast_.root))) {
            return store::Dtype::I32;
        }
        if (t.integer || item_.precision == "double" || integrator_) {
            return store::Dtype::F64; // an integer result that may leave the I32 range is exact in F64
        }
        for (const PlanInput& in : plan_.inputs) {
            if (in.f64) {
                return store::Dtype::F64;
            }
        }
        return store::Dtype::F32;
    }

    // Whether every value of an integer-typed node provably fits an I32 column: compile-time numbers within
    // the range, Bool values, integer channels stored in at most 32 bits (I32, or F32 whole numbers), and
    // those through operations that cannot leave the range of their operands (+ and - of two such values
    // are taken to stay in range, as the logger's own integer channels do). A product, a power, a rounding
    // of a real value or a bit field of unknown width is not provable.
    bool fitsI32(std::uint32_t i) const {
        constexpr double kMax = 2147483647.0;
        const Node& n = node(i);
        const lang::NodeInfo& in = info_[i];
        if (in.known) {
            return !(std::fabs(in.value) > kMax); // NaN (invalid) fits
        }
        if (n.type.kind == Kind::Bool) {
            return true;
        }
        const auto all = [&](std::size_t from) {
            for (std::size_t k = from; k < n.kids.size(); ++k) {
                if (!fitsI32(n.kids[k])) {
                    return false;
                }
            }
            return true;
        };
        switch (n.kind) {
        case NodeKind::IntLit:
        case NodeKind::DecLit:
            return !(std::fabs(n.number) > kMax);
        case NodeKind::ChannelRef: {
            const std::optional<ScopeEntry>& e = in.entry;
            return e.has_value() && e->what == ScopeEntry::Channel && e->type.integer &&
                   (e->dtype == store::Dtype::I32 || e->dtype == store::Dtype::F32);
        }
        case NodeKind::Convert:
            return n.type.integer && !n.kids.empty() && fitsI32(n.kids[0]);
        case NodeKind::Unary:
            return n.uop == UnaryOp::Not || n.uop == UnaryOp::BitNot || all(0);
        case NodeKind::Binary:
            switch (n.bop) {
            case BinaryOp::Add:
            case BinaryOp::Sub:
            case BinaryOp::BitAnd:
            case BinaryOp::BitOr:
                return all(0);
            default:
                return false; // * / ^ (comparisons and logic are Bool, above)
            }
        case NodeKind::Call:
            return in.intrinsic.has_value() && callFitsI32(n, *in.intrinsic, all);
        case NodeKind::StrLit:
        case NodeKind::Keyword:
            return false;
        }
        return false;
    }

    template <class All> bool callFitsI32(const Node& n, IntrinsicId id, const All& all) const {
        switch (id) {
        case IntrinsicId::Abs:
        case IntrinsicId::Min:
        case IntrinsicId::Max:
        case IntrinsicId::BitAnd:
        case IntrinsicId::BitOr:
        case IntrinsicId::BitXor:
        case IntrinsicId::Int:
        case IntrinsicId::Round:
        case IntrinsicId::RoundDown:
        case IntrinsicId::RoundUp:
        case IntrinsicId::Remainder:
            return all(0);
        case IntrinsicId::Sgn:
        case IntrinsicId::BitNot:
        case IntrinsicId::RangeIs:
        case IntrinsicId::RangeChange:
            return true;
        case IntrinsicId::Choose:
            return all(1);
        case IntrinsicId::BitDecode: {
            // bit_decode(x, bit) is one bit; bit_decode(x, low, count) fits for a known count below 32.
            if (n.kids.size() < 3) {
                return true;
            }
            const lang::NodeInfo& count = info_[n.kids[2]];
            return count.known && count.value >= 0.0 && count.value < 32.0;
        }
        case IntrinsicId::TimeShift:
        case IntrinsicId::PreviousSample:
        case IntrinsicId::RunningMin:
        case IntrinsicId::RunningMax:
        case IntrinsicId::StatMin:
        case IntrinsicId::StatMax:
        case IntrinsicId::StatStart:
        case IntrinsicId::StatEnd:
            return !n.kids.empty() && fitsI32(n.kids[0]);
        default:
            return false;
        }
    }

    std::uint32_t haloOf(std::uint32_t stage) const {
        std::vector<std::uint32_t> halo(plan_.stages.size(), 0);
        for (std::size_t s = 0; s < plan_.stages.size(); ++s) {
            std::uint32_t upstream = 0;
            for (const Stage::Ref& r : plan_.stages[s].inputs) {
                if (r.from == Stage::Ref::From::Stage && r.index < s) {
                    upstream = std::max(upstream, halo[r.index]);
                }
            }
            halo[s] = upstream + (plan_.stages[s].kernel != nullptr ? plan_.stages[s].kernel->halo() : 0);
        }
        return stage < halo.size() ? halo[stage] : 0;
    }

    // ---- lowering
    // --------------------------------------------------------------------------------------------

    bool isStageCall(std::uint32_t i) const {
        const Node& n = node(i);
        if (n.kind != NodeKind::Call) {
            return false;
        }
        const std::optional<IntrinsicId>& id = info_[i].intrinsic;
        return !id || intrinsicClass(*id) != KernelClass::Elementwise;
    }

    // The reference node (ChannelRef, or the ChannelRef under a Convert) when `i` reads a channel.
    std::optional<std::uint32_t> channelRef(std::uint32_t i) const {
        const Node& n = node(i);
        std::uint32_t ref = i;
        if (n.kind == NodeKind::Convert && !n.kids.empty()) {
            ref = n.kids[0];
        }
        const std::optional<ScopeEntry>& entry = info_[ref].entry;
        if (node(ref).kind != NodeKind::ChannelRef || !entry.has_value() ||
            entry->what != ScopeEntry::Channel) {
            return std::nullopt;
        }
        return ref;
    }

    Result<std::uint32_t> inputFor(std::uint32_t i, bool linear) {
        const std::uint32_t ref = *channelRef(i);
        const ScopeEntry& entry = *info_[ref].entry;
        const units::UnitId stored = entry.type.unit;
        units::UnitId unit = stored;
        if (node(i).kind == NodeKind::Convert && node(i).unit) {
            unit = *node(i).unit;
        }
        units::Converter conv;
        if (unit != stored && units_.contains(unit) && units_.contains(stored)) {
            Result<units::Converter> c = units_.converter(stored, unit);
            if (!c) {
                return error(codes::kUnitFamily, node(i).span,
                             "cannot convert '" + node(ref).text + "' into [" + node(i).text + "]");
            }
            conv = *c;
        }
        const store::Interp interp = linear ? store::Interp::Linear : entry.interp;
        for (std::size_t k = 0; k < plan_.inputs.size(); ++k) {
            const PlanInput& p = plan_.inputs[k];
            if (p.key == entry.key && p.unit == unit && p.interp == interp) {
                return static_cast<std::uint32_t>(k);
            }
        }
        PlanInput in;
        in.key = entry.key;
        in.interp = interp;
        in.conv = conv;
        in.unit = unit;
        in.f64 = entry.dtype == store::Dtype::F64;
        plan_.inputs.push_back(std::move(in));
        if (entry.type.rate && entry.type.rate->valid() && (!inputRate_ || *inputRate_ < *entry.type.rate)) {
            inputRate_ = entry.type.rate; // 03 section 6.4 rule 2
        }
        return static_cast<std::uint32_t>(plan_.inputs.size() - 1);
    }

    // A per-sample value as a stage input: a plan input, a call's stage, or an elementwise stage.
    Result<Stage::Ref> series(std::uint32_t i, bool linear) {
        Stage::Ref ref;
        if (channelRef(i)) {
            Result<std::uint32_t> in = inputFor(i, linear);
            if (!in) {
                return Failure{in.error()};
            }
            ref.from = Stage::Ref::From::Input;
            ref.index = *in;
            return ref;
        }
        Result<std::uint32_t> s = isStageCall(i) ? stageFor(i) : ewStage(i, linear, units::Converter{});
        if (!s) {
            return Failure{s.error()};
        }
        ref.from = Stage::Ref::From::Stage;
        ref.index = *s;
        return ref;
    }

    Result<std::uint32_t> stageFor(std::uint32_t call) {
        const Node& n = node(call);
        const std::optional<IntrinsicId> id = info_[call].intrinsic;
        if (!id) {
            return error(
                codes::kKernelUnavailable, n.span,
                n.text +
                    "() is an external function; external functions cannot be evaluated in this version");
        }
        const lang::Signature& sig = lang::signatureOf(*id);
        Stage stage;
        for (std::size_t k = 0; k < n.kids.size(); ++k) {
            const std::uint32_t arg = n.kids[k];
            const lang::Role role = k < sig.paramCount ? sig.params[k].role : lang::Role::Value;
            if (!lang::isSeriesRole(role, lang::perSample(node(arg).type))) {
                if (role == lang::Role::RangePath) {
                    plan_.rangeGroups.push_back(node(arg).text);
                }
                continue;
            }
            Result<Stage::Ref> in = series(arg, *id == IntrinsicId::Smooth && k == 0);
            if (!in) {
                return Failure{in.error()};
            }
            stage.inputs.push_back(*in);
        }
        if (*id == IntrinsicId::IntegrateDist) {
            Result<Stage::Ref> dist = corrDist(n.span);
            if (!dist) {
                return Failure{dist.error()};
            }
            stage.inputs.push_back(*dist);
        }
        if (*id == IntrinsicId::Integrate || *id == IntrinsicId::IntegrateDist) {
            integrator_ = true;
        }
        const bool ungatedStat = sig.rule == lang::Rule::Stat && n.kids.size() < 3;
        if (ungatedStat || *id == IntrinsicId::VarianceDist || *id == IntrinsicId::VarianceTime) {
            plan_.selectionDependent = true;
        }
        Result<std::unique_ptr<Kernel>> kernel = makeIntrinsicKernel(*id, folded_, call);
        if (!kernel) {
            const Error& e = kernel.error();
            const std::string detail = e.context.detail.value_or("");
            if (detail.starts_with("E-")) {
                return error(detail, n.span, n.text + "(): " + e.message);
            }
            return error(codes::kKernelUnavailable, n.span, n.text + "() cannot be evaluated: " + e.message);
        }
        stage.kernel = std::move(kernel).value();
        stage.outTemp = static_cast<std::uint32_t>(plan_.stages.size());
        plan_.stages.push_back(std::move(stage));
        return static_cast<std::uint32_t>(plan_.stages.size() - 1);
    }

    // The source's Corr Dist (the chain head of the corrected-distance module) in base metres, Linear.
    Result<Stage::Ref> corrDist(Span span) {
        static constexpr std::string_view kCorrDistCf = "corr dist";
        const std::optional<ScopeEntry> entry = scope_.lookup(kCorrDistCf, ChannelKey{});
        if (!entry || entry->what != ScopeEntry::Channel) {
            return error(
                codes::kNoDistance, span,
                "integrate_dist() needs the Corr Dist channel of the corrected speed and distance module",
                "enable Corrected Speed and Distance for this source");
        }
        PlanInput in;
        in.key = entry->key;
        in.interp = store::Interp::Linear;
        in.unit = entry->type.unit;
        in.f64 = true;
        for (std::size_t k = 0; k < plan_.inputs.size(); ++k) {
            const PlanInput& p = plan_.inputs[k];
            if (p.key == in.key && p.unit == in.unit && p.interp == in.interp) {
                return Stage::Ref{Stage::Ref::From::Input, static_cast<std::uint32_t>(k)};
            }
        }
        plan_.inputs.push_back(std::move(in));
        return Stage::Ref{Stage::Ref::From::Input, static_cast<std::uint32_t>(plan_.inputs.size() - 1)};
    }

    Result<std::uint32_t> ewStage(std::uint32_t root, bool linear, const units::Converter& toBase) {
        EwBuilder b;
        Result<EwOperand> value = emit(b, root, linear);
        if (!value) {
            return Failure{value.error()};
        }
        EwOperand v = *value;
        if (!toBase.identity()) {
            if (v.imm) {
                v = EwBuilder::imm(v.value * toBase.a + toBase.b);
            } else {
                EwInstr ins;
                ins.op = EwInstr::Op::Affine;
                ins.a = v;
                ins.k0 = toBase.a;
                ins.k1 = toBase.b;
                v = b.push(ins);
            }
        }
        if (v.imm) {
            EwInstr ins;
            ins.op = EwInstr::Op::Fill;
            ins.a = v;
            v = b.push(ins);
        }
        if (b.code.size() >= std::numeric_limits<std::uint16_t>::max()) {
            return error(codes::kExprTooDeep, node(root).span, "the expression is too large to evaluate");
        }
        std::uint16_t regs = 0;
        const std::uint16_t result = allocate(b.code, v.reg, regs);
        Stage stage;
        stage.kernel = std::make_unique<lang::EwKernel>(std::move(b.code), regs, result, seed_);
        stage.inputs = std::move(b.inputs);
        stage.outTemp = static_cast<std::uint32_t>(plan_.stages.size());
        plan_.stages.push_back(std::move(stage));
        return static_cast<std::uint32_t>(plan_.stages.size() - 1);
    }

    Result<EwOperand> emit(EwBuilder& b, std::uint32_t i, bool linear) {
        const Node& n = node(i);
        const lang::NodeInfo& in = info_[i];
        if (in.known) {
            return EwBuilder::imm(in.value);
        }
        if (n.type.kind == Kind::Invalid && !lang::perSample(n.type)) {
            return EwBuilder::imm(ew::kInvalid);
        }
        if (channelRef(i)) {
            Result<std::uint32_t> input = inputFor(i, linear);
            if (!input) {
                return Failure{input.error()};
            }
            Stage::Ref ref;
            ref.from = Stage::Ref::From::Input;
            ref.index = *input;
            return b.load(ref);
        }
        switch (n.kind) {
        case NodeKind::ChannelRef:
        case NodeKind::Convert: {
            const std::uint32_t ref = n.kind == NodeKind::Convert && !n.kids.empty() ? n.kids[0] : i;
            if (info_[ref].unresolved || !info_[ref].entry) {
                return error(codes::kUnknownChannel, n.span, "'" + node(ref).text + "' is not resolved");
            }
            return error(codes::kArgType, n.span, "'" + node(ref).text + "' has no value to evaluate");
        }
        case NodeKind::Unary: {
            Result<EwOperand> a = emit(b, n.kids[0], linear);
            if (!a) {
                return a;
            }
            if (a->imm) {
                return EwBuilder::imm(ew::unary(n.uop, a->value));
            }
            EwInstr ins;
            ins.op = EwInstr::Op::Unary;
            ins.uop = n.uop;
            ins.a = *a;
            return b.push(ins);
        }
        case NodeKind::Binary: {
            Result<EwOperand> a = emit(b, n.kids[0], linear);
            if (!a) {
                return a;
            }
            Result<EwOperand> c = emit(b, n.kids[1], linear);
            if (!c) {
                return c;
            }
            if (a->imm && c->imm) {
                return EwBuilder::imm(ew::binary(n.bop, a->value, c->value));
            }
            EwInstr ins;
            ins.op = EwInstr::Op::Binary;
            ins.bop = n.bop;
            ins.a = *a;
            ins.b = *c;
            return b.push(ins);
        }
        case NodeKind::Call: {
            if (isStageCall(i)) {
                Result<std::uint32_t> s = stageFor(i);
                if (!s) {
                    return Failure{s.error()};
                }
                Stage::Ref ref;
                ref.from = Stage::Ref::From::Stage;
                ref.index = *s;
                return b.load(ref);
            }
            const IntrinsicId id = *in.intrinsic;
            EwInstr ins;
            if (id == IntrinsicId::RandVal || id == IntrinsicId::RandChan) {
                ins.op = id == IntrinsicId::RandVal ? EwInstr::Op::RandVal : EwInstr::Op::RandChan;
                return b.push(ins);
            }
            if (id == IntrinsicId::Invalid) {
                return EwBuilder::imm(ew::kInvalid);
            }
            ins.op = EwInstr::Op::Call;
            ins.fn = id;
            ins.a = EwBuilder::imm(0.0);
            ins.b = EwBuilder::imm(0.0);
            ins.c = EwBuilder::imm(id == IntrinsicId::BitDecode ? 1.0 : 0.0); // bit_decode(x, bit): one bit
            EwOperand* slots[3] = {&ins.a, &ins.b, &ins.c};
            bool allImm = true;
            for (std::size_t k = 0; k < n.kids.size() && k < 3; ++k) {
                Result<EwOperand> a = emit(b, n.kids[k], linear);
                if (!a) {
                    return a;
                }
                *slots[k] = *a;
                allImm = allImm && a->imm;
            }
            if (allImm) {
                return EwBuilder::imm(ew::call(id, ins.a.value, ins.b.value, ins.c.value));
            }
            return b.push(ins);
        }
        case NodeKind::StrLit:
        case NodeKind::Keyword:
            return error(codes::kArgType, n.span, "text has no numeric value here");
        case NodeKind::IntLit:
        case NodeKind::DecLit:
            return EwBuilder::imm(n.number);
        }
        return error(codes::kSyntax, n.span, "cannot evaluate this part of the expression");
    }

    Ast& ast_;
    std::vector<lang::NodeInfo>& info_;
    const Scope& scope_;
    const units::UnitRegistry& units_;
    const MathItem& item_;
    const MathsCompat& compat_;
    Diagnostics& diags_;
    Ast folded_;
    Plan plan_;
    std::uint64_t seed_ = 0;
    bool integrator_ = false;
    std::optional<Rate> inputRate_; // the highest rate of a Series input the expression names
};

} // namespace

Result<Plan> compile(const Ast& ast, const Scope& scope, const MathItem& item, const MathsCompat& compat,
                     Diagnostics& diags) {
    switch (item.kind) {
    case ItemKind::Expression:
    case ItemKind::Filter:
    case ItemKind::ScaleOffset:
    case ItemKind::Table2D:
        break;
    case ItemKind::Plugin: {
        diags.push_back(lang::makeDiag(codes::kPluginUnavailable, Diagnostic::Error, {},
                                       "plugin items cannot be evaluated in this version"));
        ErrorContext ctx;
        ctx.detail = std::string(codes::kPluginUnavailable);
        return fail(ErrorCode::Unsupported, "E-PLUGIN-UNAVAILABLE: plugin item", std::move(ctx));
    }
    default: {
        diags.push_back(lang::makeDiag(codes::kKindUnsupported, Diagnostic::Error, {},
                                       "items of kind " + std::string(itemKindName(item.kind)) +
                                           " are not compiled from an expression"));
        ErrorContext ctx;
        ctx.detail = std::string(codes::kKindUnsupported);
        return fail(ErrorCode::Unsupported, "E-KIND-UNSUPPORTED: " + std::string(itemKindName(item.kind)),
                    std::move(ctx));
    }
    }
    if (!item.rate.automatic && !item.rate.rate.valid()) {
        diags.push_back(lang::makeDiag(codes::kRateZero, Diagnostic::Error, {},
                                       "the sample rate is 0; use Auto or a rate above 0 Hz"));
        ErrorContext ctx;
        ctx.detail = std::string(codes::kRateZero);
        return fail(ErrorCode::Invalid, "E-RATE-ZERO: sample rate 0", std::move(ctx));
    }

    Ast checked = ast;
    TypeChecker::Options options;
    // A declared result unit is checked (W-RESULT-DIM, I-RESULT-SCALE); an item that leaves both its unit and
    // its quantity at the defaults (unitless) declares nothing.
    if (item.kind == ItemKind::Expression &&
        (item.resultUnit != units::kNoUnit || item.quantity != units::kUnitless)) {
        options.resultUnit = item.resultUnit;
    }
    // A table axis is compared with the breakpoints as it is, so its expression is checked against the axis
    // unit like a declared result unit (D-185).
    if (item.kind == ItemKind::Table2D && item.table.x.unit != units::kNoUnit) {
        options.resultUnit = item.table.x.unit;
        options.tableAxis = true;
    }
    const std::size_t before = diags.size();
    lang::CheckResult result = lang::checkAst(checked, scope, options, diags);
    const auto firstError = [&]() -> const Diagnostic* {
        for (std::size_t k = before; k < diags.size(); ++k) {
            if (diags[k].level == Diagnostic::Error) {
                return &diags[k];
            }
        }
        return nullptr;
    };
    if (!result.reachedRoot) {
        diags.push_back(lang::makeDiag(codes::kExprEmpty, Diagnostic::Error, {}, "the expression is empty"));
    }
    // A reference the data-less scope could not resolve cannot be evaluated.
    for (std::size_t i = 0; i < checked.nodes.size() && i < result.info.size(); ++i) {
        if (result.info[i].unresolved && checked.nodes[i].kind == NodeKind::ChannelRef &&
            firstError() == nullptr) {
            diags.push_back(lang::makeDiag(codes::kUnknownChannel, Diagnostic::Error, checked.nodes[i].span,
                                           "'" + checked.nodes[i].text + "' is not resolved"));
        }
    }
    if (const Diagnostic* e = firstError()) {
        ErrorContext ctx;
        ctx.detail = e->code;
        ctx.span = e->span;
        return fail(ErrorCode::Invalid, e->code + ": " + e->message, std::move(ctx));
    }
    return Compiler(checked, result, scope, item, compat, diags).run();
}

} // namespace esses::maths
