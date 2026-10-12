# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# esses_maths (layer 2, Qt-free; 01 section 2.1, 03 section 1). M1 is the minimal subset of 07 section 4.1 and
# D-044: a basic MathGraph (define/require/version), PlanProvider into the ChunkCache on the 0.1 whole-node path
# (03 section 6.0), EvalContext, and the built-in module esses.corrected_speed_distance with its settings schema.
# The language (Lexer, Parser, TypeChecker, kernels) and esses-cli eval arrive in M2. Links esses_base,
# esses_units, esses_store, esses_model and esses_analysis (same-layer edge, 01 section 2.2); never
# esses_formats or esses_project. Iir1 is not vendored yet (M2, filters). nlohmann-json and json-schema-validator
# are private (settings validation; no JSON library type in a public header).
#
# Sources and tests are globbed (CONFIGURE_DEPENDS): src/**/*.cpp and src/**/*.hpp form the library;
# tests/**/*.cpp form one Catch2 executable (maths.<test>).

file(GLOB_RECURSE _esses_maths_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp")
esses_add_library(maths LAYER 2 QT_FREE
  SOURCES ${_esses_maths_sources}
  DEPS
    esses::base
    esses::units
    esses::store
    esses::model
    esses::analysis
  PRIVATE_DEPS
    esses::nlohmann_json
    esses::json_schema_validator)
# The settings schema of the built-in module (schemas/builtin-esses.corrected_speed_distance-1.schema.json) is
# found next to the executables (<exe dir>/data/schemas, copied by apps/cli) or in the source tree.
target_compile_definitions(${ESSES_PREFIX}_maths PRIVATE "ESSES_MATHS_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")

file(GLOB_RECURSE _esses_maths_tests CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp")
esses_add_test(maths_tests QT_FREE
  SOURCES ${_esses_maths_tests}
  DEPS
    esses::maths
    esses::nlohmann_json
    esses::json_schema_validator)
if(TARGET ${ESSES_PREFIX}_maths_tests)
  target_compile_definitions(${ESSES_PREFIX}_maths_tests PRIVATE "ESSES_TEST_REPO_ROOT=\"${CMAKE_SOURCE_DIR}\"")
  target_include_directories(${ESSES_PREFIX}_maths_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
