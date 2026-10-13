// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The kernel factories of src/kernels, one per kernel class of 03 section 5 (makeIntrinsicKernel dispatches
// to them), and the Table kernel of a table2d item (03 sections 4.4 and 5.7; the host appends it after the
// axis expression's plan). Every factory reads the call's parameters from the literal nodes compile() folded
// into `ast` (IntLit for points, orders and counts, DecLit for seconds, hertz and ripple, StrLit for range
// paths, Keyword for rise/fall); the call's series arguments are the stage inputs in argument order.
// Internal to esses_maths.
#pragma once

#include <esses/base/result.hpp>
#include <esses/maths/ast.hpp>
#include <esses/maths/intrinsics.hpp>
#include <esses/maths/kernel.hpp>
#include <esses/maths/math_item.hpp>
#include <esses/units/unit_registry.hpp>

#include <cstdint>
#include <memory>

namespace esses::maths::kern {

using KernelResult = Result<std::unique_ptr<Kernel>>;

// smooth, derivative
KernelResult makeWindowed(IntrinsicId id, const Ast& ast, std::uint32_t call);
// filter_lp, filter_hp, filter_cheby_lp/hp/bp/bs
KernelResult makeIir(IntrinsicId id, const Ast& ast, std::uint32_t call);
// integrate, integrate_dist, stat_*, running_*, flip_flop, edge_delay, time_valid, time_shift, pulse_width,
// previous_sample
KernelResult makeStateful(IntrinsicId id, const Ast& ast, std::uint32_t call);
// range_is, range_change
KernelResult makeRange(IntrinsicId id, const Ast& ast, std::uint32_t call);
// variance_dist, variance_time
KernelResult makeVariance(IntrinsicId id, const Ast& ast, std::uint32_t call);

// The Table kernel of a table2d item (03 section 4.4): one input, the axis values in the axis unit; the
// output is in the base unit of the values' quantity (the values are converted once, here). Errors:
// E-TABLE-AXIS (fewer than 2 breakpoints, breakpoints not strictly ascending or not finite, a values count
// that differs from the breakpoint count, a value that is not finite), E-KIND-UNSUPPORTED for a table with a
// y axis.
KernelResult makeTableKernel(const TableSpec& table, const units::UnitRegistry& units);

} // namespace esses::maths::kern
