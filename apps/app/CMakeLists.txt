# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# esses (esses_app, layer 5, Qt Widgets; 01 section 2.1, 06 sections 18-19; 07 section 4.5 demo): main() only:
# QApplication, the AppRuntime (GUI thread marked ThreadRole::NoBlock), argument parsing (-w <project>,
# -l <log>, --dump-commands), the widgets::MainWindow, the shutdown order of 01 section 5.3 rule 11. Built only
# with ESSES_BUILD_QT=ON. Sources are globbed (CONFIGURE_DEPENDS).

set(CMAKE_AUTOMOC ON)
file(GLOB_RECURSE _esses_app_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp")
esses_add_executable(app LAYER 5 WIN32_GUI
  SOURCES ${_esses_app_sources}
  DEPS
    esses::base
    esses::widgets
    esses::qtmodel
    Qt6::Widgets
  OUTPUT_NAME esses)

if(ESSES_BUILD_TESTS)
  # The executable starts, assembles the shell offscreen and exits (06 section 19 --version).
  add_test(NAME app.version COMMAND $<TARGET_FILE:${ESSES_PREFIX}_app> --version)
  set_tests_properties(app.version PROPERTIES
                       ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                       ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
                       LABELS "qt" TIMEOUT 60)
endif()
