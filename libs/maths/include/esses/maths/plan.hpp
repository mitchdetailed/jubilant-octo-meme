// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Plan and compile (03 sections 4.4, 6.1-6.5, D-022): compile() lowers a type-checked AST (an expression
// item, or the synthesised expression of a filter/scale_offset item, or a table's axis expression) into
// stages of kernels: maximal elementwise subtrees fuse into one EwKernel, every other intrinsic becomes its
// own stage. The plan records its inputs (chain-head keys with Interp and unit Converter), the result type
// and dtype, whether it is SelectionDependent, the range groups it reads and its defHash (03 section 7.2).
//
// 0.1 evaluation (03 section 6.0, D-139): evaluateWhole() runs the plan over the whole layer extent of `ctx`
// (one task per node, parallel across nodes only), reading inputs through EvalContext::getChannel.
#pragma once

#include <esses/analysis/corrected_speed.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/result.hpp>
#include <esses/maths/ast.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/maths/diagnostics.hpp>
#include <esses/maths/kernel.hpp>
#include <esses/maths/math_item.hpp>
#include <esses/maths/maths_compat.hpp>
#include <esses/maths/scope.hpp>
#include <esses/maths/types.hpp>
#include <esses/store/types.hpp>
#include <esses/units/converter.hpp>
#include <esses/units/unit_registry.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace esses::maths {

struct EvalContext;

struct PlanInput {
    ChannelKey key; // the chain head the reference reads (03 section 4.3)
    store::Interp interp = store::Interp::Linear;
    units::Converter conv;               // base -> the [unit] the expression asked for
    units::UnitId unit = units::kNoUnit; // the unit `conv` converts into (EvalContext::getChannel)
    bool f64 = false;                    // gather in F64 (03 section 6.5)
};

struct Stage {
    struct Ref { // one input of the stage: the output of an earlier stage or a plan input
        enum class From : std::uint8_t { Stage, Input };
        From from = From::Input;
        std::uint32_t index = 0;
    };
    std::unique_ptr<Kernel> kernel;
    std::vector<Ref> inputs;
    std::uint32_t outTemp = 0;
};

struct Plan {
    std::vector<Stage> stages; // topological order; the last stage is the result
    Type out;
    store::Dtype outDtype = store::Dtype::F32;
    ExtentMode extent = ExtentMode::Whole; // every 0.1 plan is Whole (03 section 6.0)
    std::uint32_t halo = 0;                // longest-path sum of stage halos (v1.0 chunk path)
    std::vector<PlanInput> inputs;
    bool selectionDependent = false;
    std::vector<std::string> rangeGroups; // range group paths read (rangeVersion in the node version)
    Hash128 defHash;                      // 03 section 7.2
};

Result<Plan> compile(const Ast& ast, const Scope& scope, const MathItem& item, const MathsCompat& compat,
                     Diagnostics& diags);

// The 0.1 whole-node evaluation (03 section 6.0): ctx.resultCount samples from ctx.resultFirst at
// ctx.resultRate. Worker only; honours ctx.cancel.
Result<analysis::UniformSignal> evaluateWhole(Plan& plan, const EvalContext& ctx);

} // namespace esses::maths
