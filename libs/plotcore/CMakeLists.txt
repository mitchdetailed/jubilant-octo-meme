# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# esses_plotcore (layer 3, Qt-free; 01 section 2.1, 04 sections 2-5; D-012, D-018, D-025): ViewState,
# ChangeMask, SelectionSnapshot, DomainMapper/DistanceChain, TraceBuilder (exact M4), AxisLayout, KeyLayout,
# HitTester, the DrawList IR, IRenderBackend and ITextMetrics. No Qt and no text rasterisation: text leaves as
# UTF-8 runs with a style id (04 section 1). Links esses_base, esses_units, esses_store, esses_model,
# esses_analysis and esses_project; never esses_maths (D-048 as amended by D-122: maths outputs reach the plot
# as Columns through the shared ChunkCache).
#
# Sources and tests are globbed (CONFIGURE_DEPENDS): src/**/*.cpp and src/**/*.hpp form the library;
# tests/**/*.cpp form one Catch2 executable (plotcore.<test>), which also hosts the DrawList JSON goldens that
# run on every OS (AC-M1-04, 08 section 6.4) and the M4 property tests (04 section 4.9).

file(GLOB_RECURSE _esses_plotcore_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp")
esses_add_library(plotcore LAYER 3 QT_FREE
  SOURCES ${_esses_plotcore_sources}
  DEPS
    esses::base
    esses::units
    esses::store
    esses::model
    esses::analysis
    esses::project
  PRIVATE_DEPS
    esses::nlohmann_json)

# DrawList goldens compare geometry across compilers (AC-M1-04): no FMA contraction, so GCC/Clang on ARM64 and
# x86-64 round like MSVC (/fp:precise, which does not contract).
if(TARGET ${ESSES_PREFIX}_plotcore)
  target_compile_options(${ESSES_PREFIX}_plotcore PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-ffp-contract=off>)
endif()

file(GLOB_RECURSE _esses_plotcore_tests CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp")
esses_add_test(plotcore_tests QT_FREE
  SOURCES ${_esses_plotcore_tests}
  DEPS
    esses::plotcore
    esses::nlohmann_json)
if(TARGET ${ESSES_PREFIX}_plotcore_tests)
  target_compile_definitions(${ESSES_PREFIX}_plotcore_tests PRIVATE "ESSES_TEST_REPO_ROOT=\"${CMAKE_SOURCE_DIR}\"")
  target_include_directories(${ESSES_PREFIX}_plotcore_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_options(${ESSES_PREFIX}_plotcore_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-ffp-contract=off>)
endif()
