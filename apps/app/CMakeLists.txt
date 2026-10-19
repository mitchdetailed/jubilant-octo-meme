# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# esses (esses_app, layer 5, Qt Widgets; 01 section 2.1, 06 sections 18-19; 07 section 4.5 demo): main() and the
# start of an instance: QApplication, the AppRuntime (GUI thread marked ThreadRole::NoBlock), the command line
# (src/launch_request.*: -w <project>, -l <log>, --new-instance, --scheme, --keymap, --dump-commands, --version),
# single-instance forwarding (src/instance_channel.*, D-113 as built by D-212), what a request does to the window
# (src/launch.*), the widgets::MainWindow, the shutdown order of 01 section 5.3 rule 11. Built only with
# ESSES_BUILD_QT=ON. Sources are globbed (CONFIGURE_DEPENDS). Resources (D-212): the window icon
# (resources/esses.qrc), on Windows the icon and version resource (resources/esses.rc.in), on macOS the bundle's
# Info.plist (tools/packaging/macos/Info.plist.in) and icon; the icon files are tools/packaging/icons/. Install
# rules: tools/packaging/CMakeLists.txt.

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
file(GLOB_RECURSE _esses_app_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp")
set(_esses_app_icons "${CMAKE_SOURCE_DIR}/tools/packaging/icons")
set(_esses_app_resources "${CMAKE_CURRENT_SOURCE_DIR}/resources/esses.qrc")
# The fourth version number: the commits since the tag of a development build ("0.1.0+3-gabc1234"), 0 at a tag.
set(_esses_app_commits 0)
if(ESSES_VERSION_STRING MATCHES "\\+([0-9]+)-g")
  set(_esses_app_commits ${CMAKE_MATCH_1})
endif()
if(WIN32)
  set(ESSES_APP_RC_VERSION
      "${ESSES_VERSION_MAJOR},${ESSES_VERSION_MINOR},${ESSES_VERSION_PATCH},${_esses_app_commits}")
  set(ESSES_APP_ICON_ICO "${_esses_app_icons}/esses.ico")
  configure_file(resources/esses.rc.in "${CMAKE_CURRENT_BINARY_DIR}/esses.rc" @ONLY)
  list(APPEND _esses_app_resources "${CMAKE_CURRENT_BINARY_DIR}/esses.rc")
elseif(APPLE)
  set_source_files_properties("${_esses_app_icons}/esses.icns" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  list(APPEND _esses_app_resources "${_esses_app_icons}/esses.icns")
endif()

esses_add_executable(app LAYER 5 WIN32_GUI
  SOURCES ${_esses_app_sources} ${_esses_app_resources}
  DEPS
    esses::base
    esses::project
    esses::widgets
    esses::qtmodel
    Qt6::Widgets
  OUTPUT_NAME esses)

if(APPLE)
  # The bundle (esses_add_executable makes a WIN32_GUI executable a MACOSX_BUNDLE): identifier from the reversed
  # ESSES_ORG_DOMAIN of cmake/naming.cmake, the .ld document type (LSHandlerRank Alternate, 06 section 19 item 5).
  string(REPLACE "." ";" _esses_domain "${ESSES_ORG_DOMAIN}")
  list(REVERSE _esses_domain)
  string(JOIN "." _esses_bundle_prefix ${_esses_domain})
  set(_esses_numeric_version "${ESSES_VERSION_MAJOR}.${ESSES_VERSION_MINOR}.${ESSES_VERSION_PATCH}")
  # LSMinimumSystemVersion of the Info.plist: the deployment target the build was made for (package.yml sets
  # 14.0), so that Launch Services refuses an older macOS with a message instead of a dyld failure (D-217).
  if(CMAKE_OSX_DEPLOYMENT_TARGET)
    set(ESSES_MACOS_MINIMUM "${CMAKE_OSX_DEPLOYMENT_TARGET}")
  else()
    set(ESSES_MACOS_MINIMUM "14.0")
  endif()
  set_target_properties(${ESSES_PREFIX}_app PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/tools/packaging/macos/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "${ESSES_APP_NAME}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${_esses_bundle_prefix}.${ESSES_EXE_NAME}"
    MACOSX_BUNDLE_ICON_FILE "esses.icns"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${_esses_numeric_version}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${_esses_numeric_version}.${_esses_app_commits}"
    MACOSX_BUNDLE_LONG_VERSION_STRING "${ESSES_VERSION_STRING}"
    MACOSX_BUNDLE_COPYRIGHT "Copyright (C) 2026 the Esses contributors. MIT licence."
    INSTALL_RPATH "@executable_path/../Frameworks")
elseif(UNIX)
  set_target_properties(${ESSES_PREFIX}_app PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
endif()

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

  # D-212: the command line, the forwarded request and `esses --version` (AC-M4-09), the single-instance channel
  # (in process and with the built esses), --scheme/--keymap and a forwarded request in a window, the portable
  # layout; D-216: the command line of the demo of 07 section 7.5 (`esses -w <project> -l <log>`) in a real esses.
  # Each test compiles the application sources it exercises (the executable has no library of its own)
  # and finds the built esses through ESSES_APP_EXE (and its folder, with data/, through ESSES_TEST_BIN_DIR).
  set(_esses_app_test_sources
      src/launch_request.cpp src/launch_request.hpp src/instance_channel.cpp src/instance_channel.hpp
      src/launch.cpp src/launch.hpp tests/app_test_support.hpp)
  foreach(_esses_app_test tst_launch_request tst_instance_channel tst_launch_options tst_portable
                          tst_demo_command_line)
    esses_add_qt_test(app_${_esses_app_test} TEST_NAME app.${_esses_app_test} TIMEOUT 600
      SOURCES tests/${_esses_app_test}.cpp ${_esses_app_test_sources}
      DEPS esses::base esses::project esses::widgets esses::qtmodel Qt6::Widgets)
    set(_esses_app_test_target ${ESSES_PREFIX}_app_${_esses_app_test})
    if(TARGET ${_esses_app_test_target})
      target_include_directories(${_esses_app_test_target} PRIVATE
                                 "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/tests")
      target_compile_definitions(${_esses_app_test_target} PRIVATE
                                 "ESSES_APP_EXE=\"$<TARGET_FILE:${ESSES_PREFIX}_app>\""
                                 "ESSES_TEST_BIN_DIR=\"${CMAKE_RUNTIME_OUTPUT_DIRECTORY}\"")
      add_dependencies(${_esses_app_test_target} ${ESSES_PREFIX}_app)
    endif()
  endforeach()
endif()
