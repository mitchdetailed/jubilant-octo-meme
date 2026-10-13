# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# Qt Test suites across the Qt layers (07 section 4.2): tst_walkthrough_m1 (AC-M1-01), tst_f9_preservation
# (AC-M1-09), tst_corr_dist_prefetch (AC-M1-10), tst_render_parity (AC-M1-05), the keymap rows (AC-M1-06).
# Every tst_<name>.cpp here is one executable (ctest ui.tst_<name>, offscreen); other .cpp files are shared
# helpers. Added from the top-level CMakeLists.txt with ESSES_BUILD_QT and ESSES_BUILD_TESTS.

esses_add_qt_test_dir(ui DIR "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 600
  DEPS
    esses::widgets
    esses::qtmodel
    esses::qtrender)

# The suites that open the public ACC stand-in read it from <build>/fixture-inputs, which the ctest fixture
# fixtures.generate_inputs (tools/CMakeLists.txt) writes first; tst_render_parity runs esses-render from the
# same bin directory.
foreach(_suite tst_walkthrough_m1 tst_f9_preservation tst_corr_dist_prefetch tst_render_parity)
  if(TEST ui.${_suite})
    set_tests_properties(ui.${_suite} PROPERTIES FIXTURES_REQUIRED esses_fixture_inputs)
  endif()
endforeach()
if(TARGET ${ESSES_PREFIX}_ui_tst_render_parity AND TARGET ${ESSES_PREFIX}_render)
  add_dependencies(${ESSES_PREFIX}_ui_tst_render_parity ${ESSES_PREFIX}_render)
endif()
