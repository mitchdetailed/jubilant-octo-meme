# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# Qt Test suites across the Qt layers (07 section 4.2): tst_walkthrough_m1 (AC-M1-01), tst_f9_preservation
# (AC-M1-09), tst_corr_dist_prefetch (AC-M1-10), tst_render_parity (AC-M1-05), the keymap rows (AC-M1-06); M2:
# tst_project_recovery (AC-M2-06), tst_lap_editor (AC-M2-05) and tst_mapping_panel (AC-M2-11) in the shell; M3:
# tst_offset_axis (AC-M3-09) and tst_demo_m3 (the demo script of 07 section 6.5), each over a log it writes; M4:
# tst_budget_warning (AC-M4-02), tst_reload_source (AC-M4-10), tst_dpr_switch (AC-M4-07) and
# tst_walkthrough_saturday (AC-M4-01 items 1-10 and the offscreen demo of 07 section 7.5).
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

# The Saturday walkthrough (AC-M4-01, D-214): eleven functions in the real shell, 140 to 190 s in MSVC Debug on the
# reference machine against the directory's 600 s; CI's MSVC Debug runner can be several times slower (its shell
# tests are), so it gets 1,500 s (D-216 (b)). ci.yml runs it as a step of its own in qt-app.
if(TEST ui.tst_walkthrough_saturday)
  set_tests_properties(ui.tst_walkthrough_saturday PROPERTIES TIMEOUT 1500)
endif()
