# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# M1 budget benchmarks (07 section 4.2 AC-M1-02 / AC-M1-03, bench/workloads.md "M1 budget rows"): plain Qt
# executables with a QElapsedTimer harness (Google Benchmark is not vendored yet, D-147; 08 section 9 names
# them as Google Benchmark targets). Built with the Qt layers, so they stay warning-free in the CI presets:
#   <build>/bin/esses-plot-bench --runs 5 --gate --json plot.json
#   <build>/bin/esses-open-bench --runs 5 --gate --json open.json
# Each writes its log with esses-gen-fixtures --bench into <build>/bench-data on first use.
# ctest:
#   ESSES_BUILD_BENCH=ON (the bench presets, .github/workflows/bench.yml): bench.plot_bench and
#     bench.open_bench, label `bench`, run serially with --gate (the absolute limits of 07 section 12 on the
#     reference machine, D-042); ESSES_BENCH_REPETITIONS sets the runs and ESSES_BENCH_OUT the directory of
#     their JSON (a Google-Benchmark-style "benchmarks" array plus the rows).
#   ESSES_BUILD_TESTS=ON: bench.<name>.smoke, label `bench-smoke`, the harness on small files (no budget).
#   ESSES_BENCH_GATE (default ON): the bench entries carry --gate. The hosted alarm's preset (linux-gcc-bench) sets
#     it OFF: a hosted runner is not the reference machine, so its rows are results for the +-30 % comparison of
#     bench.yml, not a verdict on the absolute limits (D-042, D-217).
option(ESSES_BENCH_GATE "Register the bench entries with --gate (the reference machine's absolute limits)" ON)
set(_esses_bench_gate "")
if(ESSES_BENCH_GATE)
  set(_esses_bench_gate --gate)
endif()

# M2 (AC-M2-13): the text engine on a 300 MB, 100-channel CSV (Qt-free; bench/formats_bench.cpp):
#   <build>/bin/esses-formats-bench --runs 5 --gate --json formats.json
esses_add_executable(formats_bench LAYER 3 QT_FREE
  SOURCES formats_bench.cpp
  DEPS
    esses::base
    esses::units
    esses::store
    esses::formats
    esses::nlohmann_json
  OUTPUT_NAME esses-formats-bench)
set_target_properties(${ESSES_PREFIX}_formats_bench PROPERTIES FOLDER "bench")
target_compile_definitions(${ESSES_PREFIX}_formats_bench PRIVATE "ESSES_BENCH_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
if(ESSES_BUILD_BENCH)
  add_test(NAME bench.formats_bench COMMAND ${ESSES_PREFIX}_formats_bench ${_esses_bench_gate})
  set_tests_properties(bench.formats_bench PROPERTIES LABELS "bench" TIMEOUT 3600 RUN_SERIAL TRUE)
endif()
if(ESSES_BUILD_TESTS)
  add_test(NAME bench.formats_bench.smoke COMMAND ${ESSES_PREFIX}_formats_bench --smoke)
  set_tests_properties(bench.formats_bench.smoke PROPERTIES LABELS "bench-smoke" TIMEOUT 600)
endif()

# M2 (AC-M2-14): the maths language on one core, elementwise 2.16 M samples <= 10 ms (Qt-free; bench/maths_bench.cpp):
#   <build>/bin/esses-maths-bench --runs 7 --gate --json maths.json
# AC-M2-05: the 60-lap marker rebuild (< 5 ms) is a row of the same Release bench (analysis.marker_rebuild_60lap).
esses_add_executable(maths_bench LAYER 3 QT_FREE
  SOURCES maths_bench.cpp
  DEPS
    esses::base
    esses::units
    esses::store
    esses::model
    esses::analysis
    esses::maths
    esses::nlohmann_json
  OUTPUT_NAME esses-maths-bench)
set_target_properties(${ESSES_PREFIX}_maths_bench PROPERTIES FOLDER "bench")
target_compile_definitions(${ESSES_PREFIX}_maths_bench PRIVATE "ESSES_BENCH_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
if(ESSES_BUILD_BENCH)
  add_test(NAME bench.maths_bench COMMAND ${ESSES_PREFIX}_maths_bench ${_esses_bench_gate})
  set_tests_properties(bench.maths_bench PROPERTIES LABELS "bench" TIMEOUT 1800 RUN_SERIAL TRUE)
endif()
if(ESSES_BUILD_TESTS)
  add_test(NAME bench.maths_bench.smoke COMMAND ${ESSES_PREFIX}_maths_bench --smoke)
  set_tests_properties(bench.maths_bench.smoke PROPERTIES LABELS "bench-smoke" TIMEOUT 600)
endif()

# M3 (AC-M3-03): the channel report, 50 laps x 30 channels x 8 statistics cold <= 300 ms (Qt-free;
# bench/report_bench.cpp):
#   <build>/bin/esses-report-bench --runs 7 --gate --json report.json
esses_add_executable(report_bench LAYER 3 QT_FREE
  SOURCES report_bench.cpp
  DEPS
    esses::base
    esses::units
    esses::store
    esses::model
    esses::analysis
    esses::nlohmann_json
  OUTPUT_NAME esses-report-bench)
set_target_properties(${ESSES_PREFIX}_report_bench PROPERTIES FOLDER "bench")
if(ESSES_BUILD_BENCH)
  add_test(NAME bench.report_bench COMMAND ${ESSES_PREFIX}_report_bench ${_esses_bench_gate})
  set_tests_properties(bench.report_bench PROPERTIES LABELS "bench" TIMEOUT 1800 RUN_SERIAL TRUE)
endif()
if(ESSES_BUILD_TESTS)
  add_test(NAME bench.report_bench.smoke COMMAND ${ESSES_PREFIX}_report_bench --smoke)
  set_tests_properties(bench.report_bench.smoke PROPERTIES LABELS "bench-smoke" TIMEOUT 600)
endif()

# The M1 Qt benches need the widgets layer.
if(NOT TARGET ${ESSES_PREFIX}_widgets)
  return()
endif()

foreach(_bench plot_bench open_bench)
  string(REPLACE "_" "-" _bench_exe "${_bench}")
  esses_add_executable(${_bench} LAYER 5
    SOURCES ${_bench}.cpp bench_support.cpp bench_support.hpp
    DEPS
      esses::base
      esses::units
      esses::store
      esses::model
      esses::analysis
      esses::project
      esses::plotcore
      esses::qtmodel
      esses::qtrender
      esses::widgets
    OUTPUT_NAME esses-${_bench_exe})
  set_target_properties(${ESSES_PREFIX}_${_bench} PROPERTIES FOLDER "bench")
  if(TARGET ${ESSES_PREFIX}_gen_fixtures)
    add_dependencies(${ESSES_PREFIX}_${_bench} ${ESSES_PREFIX}_gen_fixtures)
  endif()
  # open_bench's startup row launches the application (07 section 12 "Startup to empty workspace"; D-217).
  if(_bench STREQUAL "open_bench" AND TARGET ${ESSES_PREFIX}_app)
    add_dependencies(${ESSES_PREFIX}_${_bench} ${ESSES_PREFIX}_app)
    target_compile_definitions(${ESSES_PREFIX}_${_bench} PRIVATE
                               "ESSES_APP_EXE=\"$<TARGET_FILE:${ESSES_PREFIX}_app>\"")
  endif()
  set(_bench_env ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                 ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
  if(ESSES_BUILD_BENCH)
    add_test(NAME bench.${_bench} COMMAND ${ESSES_PREFIX}_${_bench} ${_esses_bench_gate})
    set_tests_properties(bench.${_bench} PROPERTIES LABELS "bench" TIMEOUT 1800 RUN_SERIAL TRUE ${_bench_env})
  endif()
  if(ESSES_BUILD_TESTS)
    add_test(NAME bench.${_bench}.smoke COMMAND ${ESSES_PREFIX}_${_bench} --smoke)
    set_tests_properties(bench.${_bench}.smoke PROPERTIES LABELS "bench-smoke;qt" TIMEOUT 600 ${_bench_env})
  endif()
endforeach()
