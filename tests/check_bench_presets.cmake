# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# ctest lint.bench_presets (D-042, D-217): the hosted alarm's configure preset (linux-gcc-bench, bench.yml's
# hosted-alarm job) registers the bench entries without --gate (ESSES_BENCH_GATE OFF): a hosted runner is not the
# reference machine, so the absolute limits of 07 section 12 are not its verdict and its job must not fail on
# them. The reference machine's preset (win-msvc-bench) keeps the gate (the option's default, ON), and every bench
# entry of bench/CMakeLists.txt takes its --gate from the option. cmake -DSOURCE_ROOT=<repo> -P
file(READ "${SOURCE_ROOT}/CMakePresets.json" _presets)
string(JSON _n LENGTH "${_presets}" configurePresets)
math(EXPR _last "${_n} - 1")
set(_hosted "")
set(_reference "unset")
foreach(_i RANGE ${_last})
  string(JSON _name GET "${_presets}" configurePresets ${_i} name)
  string(JSON _gate ERROR_VARIABLE _missing GET "${_presets}" configurePresets ${_i} cacheVariables ESSES_BENCH_GATE)
  if(_missing)
    set(_gate "unset")
  endif()
  if(_name STREQUAL "linux-gcc-bench")
    set(_hosted "${_gate}")
  elseif(_name STREQUAL "win-msvc-bench")
    set(_reference "${_gate}")
  endif()
endforeach()
set(_failures "")
if(NOT _hosted STREQUAL "OFF")
  list(APPEND _failures "linux-gcc-bench sets ESSES_BENCH_GATE to '${_hosted}', not OFF")
endif()
if(_reference STREQUAL "OFF")
  list(APPEND _failures "win-msvc-bench turns the reference gate off")
endif()
file(READ "${SOURCE_ROOT}/bench/CMakeLists.txt" _bench)
string(REGEX MATCHALL "add_test\\(NAME bench\\.[^ ]+ COMMAND [^)]*--gate\\)" _hardwired "${_bench}")
if(_hardwired)
  list(APPEND _failures "bench entries with a fixed --gate: ${_hardwired}")
endif()
if(_failures)
  message(FATAL_ERROR "bench presets: ${_failures}")
endif()
message(STATUS "bench presets: the hosted alarm runs without the reference gates")
