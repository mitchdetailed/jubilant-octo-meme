# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# Packaging (M4, D-212; 07 section 7.1 "Packaging" and "Release", 08 sections 3.11 and 7; added by the top-level
# CMakeLists.txt with and without the Qt layers, D-216 (c): a Qt-free tree installs esses-cli, the data, the notices
# and the user guide, the release tree the lint job scans for vendor marks). Every install() rule of the tree is here.
#
# Two install components (cmake --install <build> --component <c> --prefix <dir>; tools/packaging/package.py
# drives them):
#   app  the application package: esses, esses-cli and esses-render, the docking library (ADS, shared), the
#        shipped data, NOTICES.md, LICENSE and the licence texts (licenses/), laid out where esses::Paths looks:
#          Windows  <prefix>/esses.exe ...  <prefix>/data/  <prefix>/NOTICES.md  <prefix>/licenses/
#          macOS    <prefix>/esses.app/Contents/{MacOS/esses..., Frameworks/<ADS>, Resources/{data, NOTICES.md,
#                   licenses, esses.icns}, Info.plist}
#          Linux    <prefix>/bin/esses...  <prefix>/lib/<ADS>  <prefix>/share/esses/{data, NOTICES.md, licenses}
#                   <prefix>/share/applications/esses.desktop  <prefix>/share/mime/packages/esses.xml
#                   <prefix>/share/icons/hicolor/<n>x<n>/apps/esses.png, scalable/apps/esses.svg
#        NOTICES.md and the user guide (docs/user/) sit beside the data folder on every OS (<data>/../NOTICES.md
#        and <data>/../docs/user/index.md: the About dialog and Help Contents find them there).
#        Qt itself is deployed by windeployqt, macdeployqt or linuxdeploy (package.py), never by these rules.
#   cli  the standalone command-line tool (D-034): <prefix>/esses-cli with data/ beside it, NOTICES.md, LICENSE
#        and the licence texts of what it links (no Qt, no ADS).
#
# A release build is configured with -DESSES_RELEASE_TAG=v<version> (the package workflow does it for a tag):
# the configure fails unless the tag is "v" + the stamped version (so the installer's version equals the tag,
# AC-M4-09), and the About dialog and `esses --version` give the tag's source URL (about_dialog.cpp, D-212).
# <build>/packaging/package_info.json tells package.py the version, the layout and the tools of this build.
# Tests (label packaging): the self-tests of the Python tools here, the icon check, the third-party manifest's
# consistency with NOTICES.md and third_party/, and the portable zip's association scripts (Windows; against a
# stand-in reg.exe, so the registry is never written).

set(ESSES_RELEASE_TAG "" CACHE STRING
    "Release build: the git tag being built (v<version>); empty for a development build (D-212)")
if(ESSES_RELEASE_TAG)
  if(NOT ESSES_RELEASE_TAG STREQUAL "v${ESSES_VERSION_STRING}")
    message(FATAL_ERROR "ESSES_RELEASE_TAG is ${ESSES_RELEASE_TAG}, but this checkout's version is "
                        "${ESSES_VERSION_STRING} (cmake/version.cmake, from git describe): a release is built from "
                        "its tag")
  endif()
  if(TARGET ${ESSES_PREFIX}_widgets)
    set_property(SOURCE "${CMAKE_SOURCE_DIR}/libs/widgets/src/shell/m4_about/about_dialog.cpp"
                 TARGET_DIRECTORY ${ESSES_PREFIX}_widgets
                 APPEND PROPERTY COMPILE_DEFINITIONS "ESSES_RELEASE_TAG=\"${ESSES_RELEASE_TAG}\"")
  endif()
  message(STATUS "packaging: release build of ${ESSES_RELEASE_TAG}")
endif()

# ---- layout ----------------------------------------------------------------------------------------------------
if(WIN32)
  set(_pkg_os windows)
  set(_pkg_bin ".")
  set(_pkg_lib ".")
  set(_pkg_share ".")
elseif(APPLE)
  set(_pkg_os macos)
  set(_pkg_bundle "esses.app") # the bundle of the esses target (OUTPUT_NAME esses); package.py names it Esses.app
  set(_pkg_bin "${_pkg_bundle}/Contents/MacOS")
  set(_pkg_lib "${_pkg_bundle}/Contents/Frameworks")
  set(_pkg_share "${_pkg_bundle}/Contents/Resources")
else()
  set(_pkg_os linux)
  set(_pkg_bin "bin")
  set(_pkg_lib "lib")
  set(_pkg_share "share/${ESSES_DIR_NAME}")
endif()

# ---- the application package (component app) --------------------------------------------------------------------
set(_pkg_targets "")
foreach(_t app cli render)
  if(TARGET ${ESSES_PREFIX}_${_t})
    list(APPEND _pkg_targets ${ESSES_PREFIX}_${_t})
  endif()
endforeach()
if(APPLE AND TARGET ${ESSES_PREFIX}_app)
  install(TARGETS ${ESSES_PREFIX}_app BUNDLE DESTINATION "." COMPONENT app)
  list(REMOVE_ITEM _pkg_targets ${ESSES_PREFIX}_app)
endif()
if(_pkg_targets)
  install(TARGETS ${_pkg_targets} RUNTIME DESTINATION "${_pkg_bin}" COMPONENT app)
endif()
# Installed beside esses, the other programs find the libraries it ships the same way (apps/app sets esses's own).
foreach(_t cli render)
  if(APPLE AND TARGET ${ESSES_PREFIX}_${_t})
    set_target_properties(${ESSES_PREFIX}_${_t} PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks")
  elseif(UNIX AND TARGET ${ESSES_PREFIX}_${_t})
    set_target_properties(${ESSES_PREFIX}_${_t} PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
  endif()
endforeach()
# The docking library (LGPL, shared; D-026, D-153): only the runtime file and its SONAME link, never the headers,
# import library or CMake package that ADS's own install rules (component Unspecified) would add.
if(TARGET ${ESSES_NAMESPACE}::qtads)
  get_target_property(_pkg_ads ${ESSES_NAMESPACE}::qtads ALIASED_TARGET)
  install(TARGETS ${_pkg_ads}
          RUNTIME DESTINATION "${_pkg_bin}" COMPONENT app
          LIBRARY DESTINATION "${_pkg_lib}" COMPONENT app NAMELINK_SKIP
          ARCHIVE DESTINATION "lib" COMPONENT sdk)
endif()

# The shipped data as the build tree has it beside the executables (libs/project and apps/cli copy the same files
# into <build>/bin/data): data/{workbooks,schemes,keymaps,profiles,rules}/*.json, data/units.json, the schemas.
set(_pkg_data_dirs workbooks schemes keymaps profiles rules)
file(GLOB _pkg_schemas CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/schemas/*.schema.json")
file(GLOB _pkg_component_schemas CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/schemas/components/*.schema.json")
function(_esses_install_data destination component)
  foreach(_d IN LISTS _pkg_data_dirs)
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/data/${_d}" DESTINATION "${destination}" COMPONENT ${component}
            FILES_MATCHING PATTERN "*.json")
  endforeach()
  install(FILES "${CMAKE_SOURCE_DIR}/data/units.json" DESTINATION "${destination}" COMPONENT ${component})
  install(FILES ${_pkg_schemas} DESTINATION "${destination}/schemas" COMPONENT ${component})
  install(FILES ${_pkg_component_schemas} DESTINATION "${destination}/schemas/components" COMPONENT ${component})
endfunction()
_esses_install_data("${_pkg_share}/data" app)

# Notices and licence texts (08 sections 7.4 and 9; NOTICES.md lists what each one covers).
# THIRD_PARTY_LICENSES.md is written beside NOTICES.md by package.py from the deployed tree.
set(_pkg_licences_common MIT BSD-2-Clause BSD-3-Clause BSL-1.0 Apache-2.0)
function(_esses_install_notices share component)
  install(FILES "${CMAKE_SOURCE_DIR}/NOTICES.md" "${CMAKE_SOURCE_DIR}/LICENSE" DESTINATION "${share}"
          COMPONENT ${component})
  foreach(_l IN LISTS ARGN)
    install(FILES "${CMAKE_SOURCE_DIR}/LICENSES/${_l}.txt" DESTINATION "${share}/licenses" COMPONENT ${component})
  endforeach()
endfunction()
_esses_install_notices("${_pkg_share}" app ${_pkg_licences_common} LGPL-2.1-or-later)
# The user guide (docs/user/, D-215) beside the data folder, where Help Contents looks for docs/user/index.md.
install(DIRECTORY "${CMAKE_SOURCE_DIR}/docs/user" DESTINATION "${_pkg_share}/docs" COMPONENT app)
# Qt's licence (LGPL-3.0-only) and the GPL-3.0 it builds on (08 section 9 item 2), named as the About dialog says.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/licenses/LICENSE.LGPL-3.0-only.txt" DESTINATION "${_pkg_share}/licenses"
        RENAME "LGPL-3.0-only.txt" COMPONENT app)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/licenses/LICENSE.GPL-3.0-only.txt" DESTINATION "${_pkg_share}/licenses"
        RENAME "GPL-3.0-only.txt" COMPONENT app)

if(_pkg_os STREQUAL "linux")
  # The desktop entry, the MIME type of .ld (a low-priority glob, 06 section 19 item 5) and the icon theme.
  install(FILES linux/esses.desktop DESTINATION "share/applications" COMPONENT app)
  install(FILES linux/esses-mime.xml DESTINATION "share/mime/packages" RENAME "${ESSES_DIR_NAME}.xml" COMPONENT app)
  foreach(_n 16 32 48 64 128 256 512)
    install(FILES "icons/esses-${_n}.png" DESTINATION "share/icons/hicolor/${_n}x${_n}/apps" RENAME "esses.png"
            COMPONENT app)
  endforeach()
  install(FILES icons/esses.svg DESTINATION "share/icons/hicolor/scalable/apps" COMPONENT app)
endif()

# ---- the standalone command-line tool (component cli, D-034) -----------------------------------------------------
if(TARGET ${ESSES_PREFIX}_cli)
  install(TARGETS ${ESSES_PREFIX}_cli RUNTIME DESTINATION "." COMPONENT cli)
  _esses_install_data("data" cli)
  _esses_install_notices("." cli ${_pkg_licences_common})
endif()

# ---- what package.py needs to know ---------------------------------------------------------------------------
set(_pkg_qt_prefix "${QT6_INSTALL_PREFIX}")
if(NOT _pkg_qt_prefix AND Qt6_DIR)
  get_filename_component(_pkg_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
endif()
set(_pkg_tool_windeployqt "")
set(_pkg_tool_macdeployqt "")
set(_pkg_tool_qmake "")
if(TARGET Qt6::windeployqt)
  set(_pkg_tool_windeployqt "$<TARGET_FILE:Qt6::windeployqt>")
endif()
if(TARGET Qt6::macdeployqt)
  set(_pkg_tool_macdeployqt "$<TARGET_FILE:Qt6::macdeployqt>")
endif()
if(TARGET Qt6::qmake)
  set(_pkg_tool_qmake "$<TARGET_FILE:Qt6::qmake>")
endif()
set(_pkg_ads_file "")
if(TARGET ${ESSES_NAMESPACE}::qtads)
  set(_pkg_ads_file "$<TARGET_FILE_NAME:${ESSES_NAMESPACE}::qtads>")
endif()
# @...@ filled now (string(CONFIGURE)), the generator expressions at generation (file(GENERATE)).
string(CONFIGURE [=[{
  "schema": "esses-package-info-1",
  "os": "@_pkg_os@",
  "appName": "@ESSES_APP_NAME@",
  "exeName": "@ESSES_EXE_NAME@",
  "dirName": "@ESSES_DIR_NAME@",
  "version": "@ESSES_VERSION_STRING@",
  "versionMajor": @ESSES_VERSION_MAJOR@,
  "versionMinor": @ESSES_VERSION_MINOR@,
  "versionPatch": @ESSES_VERSION_PATCH@,
  "commit": "@ESSES_GIT_SHA@",
  "buildDate": "@ESSES_BUILD_DATE@",
  "releaseTag": "@ESSES_RELEASE_TAG@",
  "config": "$<CONFIG>",
  "qtVersion": "@Qt6_VERSION@",
  "qtPrefix": "@_pkg_qt_prefix@",
  "qtPlugins": "@_pkg_qt_prefix@/@QT6_INSTALL_PLUGINS@",
  "qtBin": "@_pkg_qt_prefix@/@QT6_INSTALL_BINS@",
  "qtLib": "@_pkg_qt_prefix@/@QT6_INSTALL_LIBS@",
  "windeployqt": "@_pkg_tool_windeployqt@",
  "macdeployqt": "@_pkg_tool_macdeployqt@",
  "qmake": "@_pkg_tool_qmake@",
  "adsFile": "@_pkg_ads_file@",
  "dockBackend": "@ESSES_DOCK_BACKEND@",
  "binDir": "@CMAKE_RUNTIME_OUTPUT_DIRECTORY@",
  "layout": {"bin": "@_pkg_bin@", "lib": "@_pkg_lib@", "share": "@_pkg_share@"}
}
]=] _pkg_info @ONLY)
# A multi-config generator (Visual Studio, Ninja Multi-Config) gets one file per configuration.
get_property(_pkg_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_pkg_multi)
  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/packaging/package_info-$<CONFIG>.json" CONTENT "${_pkg_info}")
else()
  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/packaging/package_info.json" CONTENT "${_pkg_info}")
endif()

# ---- tests ---------------------------------------------------------------------------------------------------------
if(ESSES_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    foreach(_tool package gen_third_party smoke)
      add_test(NAME packaging.${_tool}_self_test
               COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/${_tool}.py" --self-test)
      set_tests_properties(packaging.${_tool}_self_test PROPERTIES LABELS "packaging;lint" TIMEOUT 120)
    endforeach()
    add_test(NAME packaging.icons
             COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/icons/make_icons.py" --check)
    set_tests_properties(packaging.icons PROPERTIES LABELS "packaging;lint" TIMEOUT 300)
    add_test(NAME packaging.third_party_consistent
             COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/gen_third_party.py" --check
                     --root "${CMAKE_SOURCE_DIR}")
    set_tests_properties(packaging.third_party_consistent PROPERTIES LABELS "packaging;lint" TIMEOUT 120)
    # The portable zip's associate-ld.cmd and unassociate-ld.cmd under cmd (D-218 (b)): from awkward folder names,
    # with decoys of reg, find and the like in the current folder and first on PATH, against a stand-in reg.exe
    # (windows/reg_stub.cpp) in a temporary SystemRoot, so nothing reaches the registry. Windows only; elsewhere the
    # entry reports a skip.
    set(_pkg_script_test_args "")
    if(WIN32)
      add_executable(${ESSES_PREFIX}_reg_stub windows/reg_stub.cpp)
      esses_apply_warnings(${ESSES_PREFIX}_reg_stub)
      if(MINGW)
        target_link_options(${ESSES_PREFIX}_reg_stub PRIVATE -municode) # wmain; MSVC picks it by itself
      endif()
      set_target_properties(${ESSES_PREFIX}_reg_stub PROPERTIES FOLDER "tests")
      set(_pkg_script_test_args --reg-stub "$<TARGET_FILE:${ESSES_PREFIX}_reg_stub>")
    endif()
    add_test(NAME packaging.association_scripts
             COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/smoke.py" --script-test
                     ${_pkg_script_test_args})
    set_tests_properties(packaging.association_scripts PROPERTIES LABELS "packaging;lint" TIMEOUT 300
                         SKIP_RETURN_CODE 77)
  else()
    message(STATUS "packaging: no Python 3, the packaging self-tests are not registered")
  endif()
endif()
