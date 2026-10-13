// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Umbrella header of esses_maths (layer 2, Qt-free; 03 section 1). M1 surface per 07 section 4.1 and D-044;
// the M2 contract headers (07 section 5.1) are listed in libs/maths/README.md.
#pragma once

#include <esses/maths/ast.hpp>
#include <esses/maths/builtin_module.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/maths/corrected_speed_distance.hpp>
#include <esses/maths/diagnostics.hpp>
#include <esses/maths/eval_context.hpp>
#include <esses/maths/function_registry.hpp>
#include <esses/maths/gps_corrections.hpp>
#include <esses/maths/intrinsics.hpp>
#include <esses/maths/kernel.hpp>
#include <esses/maths/lexer.hpp>
#include <esses/maths/math_function.hpp>
#include <esses/maths/math_graph.hpp>
#include <esses/maths/math_input_gatherer.hpp>
#include <esses/maths/math_item.hpp>
#include <esses/maths/math_module.hpp>
#include <esses/maths/math_module_io.hpp>
#include <esses/maths/math_selection.hpp>
#include <esses/maths/math_status.hpp>
#include <esses/maths/maths_compat.hpp>
#include <esses/maths/maths_scheduler.hpp>
#include <esses/maths/parser.hpp>
#include <esses/maths/plan.hpp>
#include <esses/maths/plan_provider.hpp>
#include <esses/maths/scope.hpp>
#include <esses/maths/type_checker.hpp>
#include <esses/maths/types.hpp>
#include <esses/maths/whole_signal.hpp>
