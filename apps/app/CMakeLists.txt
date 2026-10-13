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
  # Windows: esses is a GUI-subsystem executable, so app.version above only shows that it writes to the pipes
  # ctest gives it. Started from a terminal it gets no standard handles and has to attach to the terminal's
  # console (main.cpp, ParentConsole); the probe starts it that way inside a pseudo console (no window) and
  # checks that the version line arrives there.
  if(WIN32)
    add_executable(${ESSES_PREFIX}_console_probe tests/console_probe.cpp)
    esses_apply_warnings(${ESSES_PREFIX}_console_probe)
    # The pseudo console API needs Windows 10 1809 (NTDDI_WIN10_RS5); MinGW's headers default to Windows 7.
    target_compile_definitions(${ESSES_PREFIX}_console_probe PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000006)
    if(MINGW)
      target_link_options(${ESSES_PREFIX}_console_probe PRIVATE -municode) # wmain; MSVC picks it by itself
    endif()
    set_target_properties(${ESSES_PREFIX}_console_probe PROPERTIES FOLDER "apps" AUTOMOC OFF)
    add_test(NAME app.version_console
             COMMAND $<TARGET_FILE:${ESSES_PREFIX}_console_probe> $<TARGET_FILE:${ESSES_PREFIX}_app> --version)
    set_tests_properties(app.version_console PROPERTIES
                         ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                         ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
                         LABELS "qt" TIMEOUT 90)
  endif()
endif()
