# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# catch_discover_tests() for the vendored Catch2 v3 (08 section 6.2: every TEST_CASE is one ctest entry). After
# each link, cmake/catch_discover_tests.cmake lists the executable's tests through Catch2's JSON reporter and
# writes a ctest include file that registers one test per name. The Catch2 tags [unit] [property] [slow]
# [oracle] become ctest labels, so `ctest -L property` works. Any test name is supported, including names with
# ';', ',', '[', ']', '"', '\' or non-ASCII text; a name that a Catch2 test spec cannot select exactly (a
# leading or trailing '*', an "exclude:" prefix, a line break, or two names that differ only in case) fails the
# build with a message naming the test.
include_guard(GLOBAL)

set(ESSES_CATCH_DISCOVER_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/catch_discover_tests.cmake")
set(ESSES_CATCH_KNOWN_TAGS unit property slow oracle)

# esses_catch_discover(<target> [PREFIX <p>] [TIMEOUT <s>] [LABELS <l>...])
function(esses_catch_discover target)
  cmake_parse_arguments(A "" "PREFIX;TIMEOUT" "LABELS" ${ARGN})
  if(NOT A_PREFIX)
    set(A_PREFIX "${target}:")
  endif()
  if(NOT A_TIMEOUT)
    set(A_TIMEOUT 600)
  endif()
  set(ctest_file "${CMAKE_CURRENT_BINARY_DIR}/${target}_ctest.cmake")
  set(ctest_include "${CMAKE_CURRENT_BINARY_DIR}/${target}_ctest_include.cmake")
  string(REPLACE ";" "@" tags "${ESSES_CATCH_KNOWN_TAGS}")  # "@": no meaning to cmd.exe or sh
  string(REPLACE ";" "@" labels "${A_LABELS}")
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
            "-DTEST_EXECUTABLE=$<TARGET_FILE:${target}>"
            "-DTEST_WORKING_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            "-DTEST_PREFIX=${A_PREFIX}"
            "-DTEST_TIMEOUT=${A_TIMEOUT}"
            "-DTEST_TAGS=${tags}"
            "-DTEST_LABELS=${labels}"
            "-DCTEST_FILE=${ctest_file}"
            -P "${ESSES_CATCH_DISCOVER_SCRIPT}"
    BYPRODUCTS "${ctest_file}"
    COMMENT "Discovering Catch2 tests in ${target}"
    VERBATIM)
  file(WRITE "${ctest_include}"
       "if(EXISTS \"${ctest_file}\")\n"
       "  include(\"${ctest_file}\")\n"
       "else()\n"
       "  add_test(${target}_NOT_BUILT ${target}_NOT_BUILT)\n"
       "endif()\n")
  set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${ctest_include}")
endfunction()
