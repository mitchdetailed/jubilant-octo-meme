# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# The Saturday soak (07 section 7.1 "Soak", AC-M4-03; 08 sections 4.1 and 6.10; D-213): tst_saturday.cpp drives
# the real shell (MainWindow on a private configuration directory and a project folder, the shipped workbook)
# with ten files open (eight .ld + .ldx and two CSV, written by the test at realistic sizes, never committed)
# through the day's workflow in a loop for ESSES_SOAK_SECONDS (default 60 s), and fails on a crash, a Dispatcher
# thread violation (every ESSES_ASSERT and Qt's thread-affinity warnings are fatal here), pending work that never
# drains and sustained growth of the heap, the resident set, the chunk cache or the shell's objects. The nightly
# and pre-tag runs (30 min, 2 h) are the soak job of .github/workflows/bench.yml: the same executable under ASan +
# UBSan with LeakSanitizer, lsan.supp for the Qt, fontconfig and glib stacks and lsan_check.py deciding which
# leaks count (an esses frame in the stack). Added from the top-level CMakeLists.txt with ESSES_BUILD_QT and
# ESSES_BUILD_TESTS; README.md has the details.

if(NOT ESSES_BUILD_TESTS OR NOT ESSES_BUILD_QT)
  return()
endif()

# Two ctest entries of one executable (D-216 (b), amending D-213 (f)):
# - soak.tst_saturday (labels qt;soak): the soak function alone, a short run (ESSES_SOAK_SECONDS from the caller's
#   environment, unset: 60 s, at least two cycles). Not part of the normal runs: ci.yml's qt-app and the full local
#   runs leave it out with `ctest -LE soak` (about 5.5 minutes in MSVC Debug); `ctest -L soak` runs it, longer runs
#   call the executable (the soak job of bench.yml does), since ctest's TIMEOUT below would stop a run longer than
#   about 25 minutes.
# - soak.guard (label qt): the guard's and the growth rule's own tests (seconds), in every normal run.
esses_add_qt_test(soak_tst_saturday TEST_NAME soak.tst_saturday
  ARGS saturday
  SOURCES
    tst_saturday.cpp
    soak_data.cpp
    soak_data.hpp
    soak_driver.cpp
    soak_driver.hpp
    soak_guard.cpp
    soak_guard.hpp
    soak_metrics.cpp
    soak_metrics.hpp
    "${CMAKE_SOURCE_DIR}/tests/ui/ui_test_support.cpp"
    "${CMAKE_SOURCE_DIR}/tests/ui/ui_test_support.hpp"
  DEPS
    esses::widgets
    esses::qtmodel
    esses::qtrender
  LABELS soak
  TIMEOUT 1800)
if(TARGET ${ESSES_PREFIX}_soak_tst_saturday)
  target_include_directories(${ESSES_PREFIX}_soak_tst_saturday PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                                                                        "${CMAKE_SOURCE_DIR}/tests/ui")
endif()
esses_add_qt_test_entry(soak_tst_saturday TEST_NAME soak.guard
  ARGS growthRule threadViolationMessages guardRecordsDispatcherViolations violationsAreFatal
  TIMEOUT 600)

# The leak classifier and the suppression file's rules (lsan_check.py --self-test: hand-written LeakSanitizer
# reports, every line of lsan.supp checked against the rules of its header). Runs wherever Python is found, in the
# normal runs (label lint, not soak); the soak job runs it before the soak too.
find_package(Python3 3.8 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME soak.lsan_check.self_test
           COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/lsan_check.py" --self-test
                   --suppressions "${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")
  set_tests_properties(soak.lsan_check.self_test PROPERTIES LABELS "lint" TIMEOUT 120)
endif()
