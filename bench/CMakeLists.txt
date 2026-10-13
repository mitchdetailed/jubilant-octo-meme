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
  set(_bench_env ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                 ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
  if(ESSES_BUILD_BENCH)
    add_test(NAME bench.${_bench} COMMAND ${ESSES_PREFIX}_${_bench} --gate)
    set_tests_properties(bench.${_bench} PROPERTIES LABELS "bench" TIMEOUT 1800 RUN_SERIAL TRUE ${_bench_env})
  endif()
  if(ESSES_BUILD_TESTS)
    add_test(NAME bench.${_bench}.smoke COMMAND ${ESSES_PREFIX}_${_bench} --smoke)
    set_tests_properties(bench.${_bench}.smoke PROPERTIES LABELS "bench-smoke;qt" TIMEOUT 600 ${_bench_env})
  endif()
endforeach()
