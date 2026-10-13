// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Whole-extent signal reads for the 0.1 whole-node path (03 section 6.0): a built-in module output reads its
// logged inputs and MathGraph reads a node's upstream outputs over the whole extent into
// analysis::UniformSignal buffers (values as double, one validity flag per sample). Worker threads only:
// the column is pinned through Column::require, which materialises absent chunks inline (01 section 5.1
// Rule W).
#pragma once

#include <esses/analysis/corrected_speed.hpp>
#include <esses/base/cancel.hpp>
#include <esses/base/priority.hpp>
#include <esses/base/result.hpp>
#include <esses/store/series.hpp>
#include <esses/units/converter.hpp>

namespace esses::maths {

// Reads a UniformSeries over its whole extent and applies `toBase` to every valid sample (identity by
// default). Invalid samples (validity bitmap, NaN, AllInvalid chunks) get valid[i] == 0 and value NaN;
// `valid` stays empty when every sample is valid. Unsupported for a TimestampedSeries (M2, with
// ResampleView); Cancelled when `cancel` fires.
Result<analysis::UniformSignal> readWholeSignal(const store::Series& series, const CancelToken& cancel,
                                                const units::Converter& toBase = {},
                                                Priority priority = Priority::Background);

} // namespace esses::maths
