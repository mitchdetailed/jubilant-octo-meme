# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# Script mode: checks that every test case of a Catch2 executable is registered with ctest under its exact name
# (the regression test for names containing ';' and other special characters; see cmake/catch_discover.cmake).
#   cmake -DTEST_EXECUTABLE=<exe> -DPREFIX=<ctest prefix> -DBUILD_DIR=<build dir> -DCTEST_COMMAND=<ctest>
#         -P tests/check_catch_registration.cmake
cmake_minimum_required(VERSION 3.28)

foreach(_v TEST_EXECUTABLE PREFIX BUILD_DIR CTEST_COMMAND)
  if(NOT DEFINED ${_v})
    message(FATAL_ERROR "check_catch_registration: -D${_v}=... is required")
  endif()
endforeach()

execute_process(COMMAND "${TEST_EXECUTABLE}" --list-tests --reporter json ENCODING UTF-8
                OUTPUT_VARIABLE listing ERROR_VARIABLE err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "check_catch_registration: listing ${TEST_EXECUTABLE} failed (${rc}): ${err}")
endif()
string(JSON catch_tests GET "${listing}" listings tests)
string(JSON catch_count LENGTH "${catch_tests}")
if(catch_count EQUAL 0)
  message(FATAL_ERROR "check_catch_registration: ${TEST_EXECUTABLE} lists no test cases")
endif()

execute_process(COMMAND "${CTEST_COMMAND}" --test-dir "${BUILD_DIR}" --show-only=json-v1 ENCODING UTF-8
                OUTPUT_VARIABLE shown ERROR_VARIABLE err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "check_catch_registration: ctest --show-only failed (${rc}): ${err}")
endif()
string(JSON ctest_tests GET "${shown}" tests)
string(JSON ctest_count LENGTH "${ctest_tests}")

# Registered names with the prefix, keyed by MD5 so that no name ever passes through a CMake list.
set(registered 0)
if(ctest_count GREATER 0)
  math(EXPR last "${ctest_count} - 1")
  foreach(i RANGE ${last})
    string(JSON name GET "${ctest_tests}" ${i} name)
    string(FIND "${name}" "${PREFIX}" pos)
    if(pos EQUAL 0)
      string(MD5 key "${name}")
      set(seen_${key} TRUE)
      math(EXPR registered "${registered} + 1")
    endif()
  endforeach()
endif()

set(missing "")
if(catch_count GREATER 0)
  math(EXPR last "${catch_count} - 1")
  foreach(i RANGE ${last})
    string(JSON name GET "${catch_tests}" ${i} name)
    string(MD5 key "${PREFIX}${name}")
    if(NOT seen_${key})
      string(APPEND missing "  \"${PREFIX}${name}\"\n")
    endif()
  endforeach()
endif()

if(NOT missing STREQUAL "" OR NOT registered EQUAL catch_count)
  message(FATAL_ERROR "check_catch_registration: ${catch_count} test cases in ${TEST_EXECUTABLE}, ${registered} "
                      "ctest entries with the prefix '${PREFIX}'; not registered:\n${missing}")
endif()
message(STATUS "check_catch_registration: all ${catch_count} test cases are registered under their exact names")
