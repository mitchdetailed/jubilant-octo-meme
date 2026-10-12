# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# esses_analysis (layer 2, Qt-free; 01 section 2.1, 07 section 4.1 M1 subset): the range rules (IRangeRule,
# RuleRunner, BeaconPairRule, WholeOutingRule), the fastest-lap candidate rule, RangeTables, DistanceIndex and
# the CorrectedSpeed / CorrectedDistance algorithms behind esses.corrected_speed_distance. Links esses_base,
# esses_units, esses_store and esses_model; nlohmann-json is private (no JSON type in a public header).
# Floating-point text is parsed with fast_float (private), never std::from_chars: Apple Clang 15's libc++ (the
# macos-14 runner) has no floating-point from_chars (as in esses_formats).
#
# Sources and tests are globbed (CONFIGURE_DEPENDS) so parallel M1 work adds files without editing this file:
# src/**/*.cpp and src/**/*.hpp form the library; tests/**/*.cpp form one Catch2 executable (analysis.<test>).

file(GLOB_RECURSE _esses_analysis_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp")
esses_add_library(analysis LAYER 2 QT_FREE
  SOURCES ${_esses_analysis_sources}
  DEPS
    esses::base
    esses::units
    esses::store
    esses::model
  PRIVATE_DEPS
    esses::nlohmann_json
    esses::fast_float)

file(GLOB_RECURSE _esses_analysis_tests CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp")
esses_add_test(analysis_tests QT_FREE
  SOURCES ${_esses_analysis_tests}
  DEPS
    esses::analysis
    esses::nlohmann_json)
if(TARGET ${ESSES_PREFIX}_analysis_tests)
  # Fixtures (fixtures/motec.ld/...) and shipped rules (data/rules/...) are read from the source tree.
  target_compile_definitions(${ESSES_PREFIX}_analysis_tests PRIVATE "ESSES_TEST_REPO_ROOT=\"${CMAKE_SOURCE_DIR}\"")
  target_include_directories(${ESSES_PREFIX}_analysis_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
