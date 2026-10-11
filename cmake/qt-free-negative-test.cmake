# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# Negative (and positive) tests of the configure-time checks (AC-M0-07; 01 section 11 items 2 and 3).
# Script mode:  cmake -DCASE=<case> -DSOURCE_ROOT=<repo> -DWORK_DIR=<dir> [-DGENERATOR=<g>]
#                     [-DMAKE_PROGRAM=<p>] -P cmake/qt-free-negative-test.cmake
# Each case writes a scratch project that includes the real cmake/ helpers and a scratch copy of
# libs/base/src/rate.cpp with one injected line, configures it (no compiler is needed: LANGUAGES NONE)
# and checks that configure fails with the expected diagnostic, or succeeds for the positive cases.
cmake_minimum_required(VERSION 3.28)

foreach(v CASE SOURCE_ROOT WORK_DIR)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "qt-free-negative-test: -D${v}=... is required")
  endif()
endforeach()

set(expect_fail TRUE)
set(expect_text "qt-free-check")
set(inject "")
set(extra_cmake "")
set(lib_args "LAYER 0 QT_FREE HEADER_ONLY SOURCES src/rate.cpp")
if(CASE STREQUAL "include_QString")
  set(inject "#include <QString>")
elseif(CASE STREQUAL "include_QtCore")
  set(inject "#include <QtCore>")
elseif(CASE STREQUAL "include_qglobal")
  set(inject "#include <qglobal.h>")
elseif(CASE STREQUAL "use_QString")
  set(inject "QString s;")
elseif(CASE STREQUAL "link_Qt6Core")
  set(extra_cmake "add_library(Qt6::Core INTERFACE IMPORTED)\n")
  set(lib_args "${lib_args} DEPS Qt6::Core")
elseif(CASE STREQUAL "positive")
  set(inject "Quantity q; QuantityMap m;")
  set(expect_fail FALSE)
elseif(CASE STREQUAL "layer_upward")
  set(expect_text "layer check")
  set(extra_cmake "esses_add_library(high LAYER 2 HEADER_ONLY)\n")
  set(lib_args "LAYER 1 QT_FREE HEADER_ONLY SOURCES src/rate.cpp DEPS esses::high")
elseif(CASE STREQUAL "layer_forbidden_pair")
  set(expect_text "layer check")
  set(extra_cmake "esses_add_library(maths LAYER 2 HEADER_ONLY)\n")
  set(lib_args "LAYER 2 QT_FREE HEADER_ONLY SOURCES src/rate.cpp DEPS esses::maths")
  set(probe_name formats)
elseif(CASE STREQUAL "layer_switched_off")
  # A dependency left out with ESSES_WITH_<LIB>=OFF: the layer check names the switch.
  set(expect_text "ESSES_WITH_MODEL=OFF")
  set(extra_cmake "set(ESSES_WITH_MODEL OFF)\n")
  set(lib_args "LAYER 2 QT_FREE HEADER_ONLY SOURCES src/rate.cpp DEPS esses::model")
elseif(CASE STREQUAL "layer_downward")
  set(expect_fail FALSE)
  set(extra_cmake "esses_add_library(low LAYER 0 HEADER_ONLY)\n")
  set(lib_args "LAYER 1 QT_FREE HEADER_ONLY SOURCES src/rate.cpp DEPS esses::low")
else()
  message(FATAL_ERROR "qt-free-negative-test: unknown CASE '${CASE}'")
endif()
if(NOT probe_name)
  set(probe_name probe)
endif()

set(src_rate "${SOURCE_ROOT}/libs/base/src/rate.cpp")
if(NOT EXISTS "${src_rate}")
  message(FATAL_ERROR "qt-free-negative-test: ${src_rate} not found")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/src/libs/${probe_name}/src")
file(READ "${src_rate}" rate_text)
file(WRITE "${WORK_DIR}/src/libs/${probe_name}/src/rate.cpp" "${inject}\n${rate_text}")
file(WRITE "${WORK_DIR}/src/libs/${probe_name}/CMakeLists.txt" "esses_add_library(${probe_name} ${lib_args})\n")
file(WRITE "${WORK_DIR}/src/CMakeLists.txt"
     "cmake_minimum_required(VERSION 3.28)\n"
     "project(qt_free_probe LANGUAGES NONE)\n"
     "list(APPEND CMAKE_MODULE_PATH \"${SOURCE_ROOT}/cmake\")\n"
     "set(ESSES_BUILD_TESTS OFF)\n"
     "include(naming)\n"
     "include(esses_targets)\n"
     "include(qt-free-check)\n"
     "${extra_cmake}"
     "add_subdirectory(libs/${probe_name})\n")

set(gen_args "")
if(GENERATOR)
  list(APPEND gen_args -G "${GENERATOR}")
endif()
if(MAKE_PROGRAM)
  list(APPEND gen_args "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" ${gen_args} -S "${WORK_DIR}/src" -B "${WORK_DIR}/build"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)

if(expect_fail)
  if(rc EQUAL 0)
    message(FATAL_ERROR "qt-free-negative-test(${CASE}): configure succeeded but must fail\n${out}\n${err}")
  endif()
  string(FIND "${out}${err}" "${expect_text}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "qt-free-negative-test(${CASE}): configure failed without '${expect_text}':\n${out}\n${err}")
  endif()
  message(STATUS "qt-free-negative-test(${CASE}): configure failed as expected")
else()
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "qt-free-negative-test(${CASE}): configure must succeed:\n${out}\n${err}")
  endif()
  message(STATUS "qt-free-negative-test(${CASE}): configure succeeded as expected")
endif()
