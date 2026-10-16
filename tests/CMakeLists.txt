# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# Shared test infrastructure: smoke tests of the vendored libraries, the self-tests of the Catch2 v3 test
# discovery, and the configure-level checks run by the `lint` job (label `lint`). Catch2 itself (with its
# default main) is the esses::catch2 target of cmake/third_party.cmake, linked by esses_add_test().

# Vendored libraries link and compile warning-free under the project's flags (cmake/third_party.cmake).
esses_add_test(third_party_tests QT_FREE
  SOURCES third_party_tests.cpp
  DEPS esses::nlohmann_json esses::json_schema_validator esses::cli11 esses::fast_float esses::pugixml)

# The M0 JSON Schemas (schemas/*.schema.json) and their valid/invalid examples, through the validator that
# esses-cli validate uses; tools/check_schemas.py (registered below) checks the same examples independently.
esses_add_test(schema_examples_tests QT_FREE
  SOURCES schema_examples_tests.cpp
  DEPS esses::nlohmann_json esses::json_schema_validator)

# esses_add_test(... CUSTOM_MAIN) links esses::catch2_nomain.
esses_add_test(catch_custom_main_tests QT_FREE CUSTOM_MAIN SOURCES catch_custom_main_tests.cpp)

# Test discovery (cmake/catch_discover.cmake): names with ';', ',', brackets, quotes, backslashes, CMake syntax,
# shell characters and non-ASCII text become ctest entries that select exactly their test case ...
esses_add_test(catch_discovery_tests QT_FREE SOURCES catch_discovery_tests.cpp)
add_test(NAME lint.catch_discover.registered_names
         COMMAND "${CMAKE_COMMAND}"
                 "-DTEST_EXECUTABLE=$<TARGET_FILE:${ESSES_PREFIX}_catch_discovery_tests>"
                 "-DPREFIX=catch_discovery."
                 "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
                 "-DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/check_catch_registration.cmake")
set_tests_properties(lint.catch_discover.registered_names PROPERTIES LABELS "lint" TIMEOUT 120)

# ... and names that a Catch2 test spec cannot select exactly are refused with a message naming the test.
set(_esses_reject_cases
    "wildcard|wildcard in a test spec"
    "exclude|negation in a test spec"
    "case|is not unique once case"
    "linebreak|contains a line break")
foreach(_case IN LISTS _esses_reject_cases)
  string(REPLACE "|" ";" _parts "${_case}")
  list(GET _parts 0 _tag)
  list(GET _parts 1 _expect)
  add_test(NAME lint.catch_discover.reject_${_tag}
           COMMAND "${CMAKE_COMMAND}"
                   "-DTEST_EXECUTABLE=$<TARGET_FILE:${ESSES_PREFIX}_catch_discovery_tests>"
                   "-DTEST_SPEC=[reject-${_tag}]"
                   "-DCTEST_FILE=${CMAKE_CURRENT_BINARY_DIR}/catch-discover-checks/reject_${_tag}.cmake"
                   -P "${ESSES_CATCH_DISCOVER_SCRIPT}")
  set_tests_properties(lint.catch_discover.reject_${_tag} PROPERTIES
                       LABELS "lint" TIMEOUT 120 PASS_REGULAR_EXPRESSION "${_expect}")
endforeach()

# Qt-free check and layer check negative tests (AC-M0-07, 01 section 11 items 2 and 3), the ESSES_WITH_<LIB>
# switch message of the layer check, (M1) the required Qt-free set and the Qt module rules of the Qt layers (M2:
# Qt Gui allowed in esses_qtmodel for the undo framework, D-161), and who may link the docking library (D-026).
set(_esses_cfg_cases include_QString include_QtCore include_qglobal use_QString link_Qt6Core positive
                     layer_upward layer_forbidden_pair layer_downward layer_switched_off
                     required_qt_free qt_module_qtmodel_widgets qt_module_qtmodel_gui qt_module_concurrent
                     qt_module_qtmodel_core
                     ads_qtrender ads_other_library ads_widgets)
foreach(_case IN LISTS _esses_cfg_cases)
  add_test(NAME lint.configure_check.${_case}
           COMMAND "${CMAKE_COMMAND}"
                   "-DCASE=${_case}"
                   "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}"
                   "-DWORK_DIR=${CMAKE_BINARY_DIR}/configure-checks/${_case}"
                   "-DGENERATOR=${CMAKE_GENERATOR}"
                   "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
                   -P "${CMAKE_SOURCE_DIR}/cmake/qt-free-negative-test.cmake")
  set_tests_properties(lint.configure_check.${_case} PROPERTIES LABELS "lint" TIMEOUT 120)
endforeach()

# Self-tests of the stdlib-only lint scripts (01 section 11 items 3 and 11).
find_package(Python3 3.8 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME lint.check_vendor_marks.self_test
           COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/check_vendor_marks.py" --self-test)
  add_test(NAME lint.check_layering.self_test
           COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/check_layering.py" --self-test)
  add_test(NAME lint.check_schemas.self_test
           COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/check_schemas.py" --self-test)
  add_test(NAME lint.check_schemas
           COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/check_schemas.py"
                   --root "${CMAKE_SOURCE_DIR}")
  set_tests_properties(lint.check_vendor_marks.self_test lint.check_layering.self_test
                       lint.check_schemas.self_test lint.check_schemas
                       PROPERTIES LABELS "lint" TIMEOUT 120)
  # AC-M1-06 lint half: the rows of data/keymaps/default.json (status/milestone consistency, conflicts) and the
  # tests (every implemented row's command has a KEYMAP_TEST under tests/ui/keymap, ui.tst_keymap).
  add_test(NAME lint.check_keymap_coverage.self_test
           COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/check_keymap_coverage.py" --self-test)
  add_test(NAME lint.check_keymap_coverage.rows
           COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/check_keymap_coverage.py"
                   --root "${CMAKE_SOURCE_DIR}")
  set_tests_properties(lint.check_keymap_coverage.self_test lint.check_keymap_coverage.rows
                       PROPERTIES LABELS "lint" TIMEOUT 120)
endif()
