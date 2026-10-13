// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The MathInputGatherer constructor (final). compile(), evaluateWhole() and MathInputGatherer::fill() are
// implemented in src/lang (compile.cpp, evaluate.cpp), next to the EwKernel that compile() emits.

#include <esses/maths/math_input_gatherer.hpp>

#include <utility>

namespace esses::maths {

MathInputGatherer::MathInputGatherer(store::Series series, Rate out, Micros outT0, store::Interp interp,
                                     units::Converter conv, CancelToken cancel, Priority priority)
    : series_(std::move(series)), out_(out), outT0_(outT0), interp_(interp), conv_(conv),
      cancel_(std::move(cancel)), priority_(priority) {}

} // namespace esses::maths
