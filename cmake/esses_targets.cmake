# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# Target helpers (08 sections 2, 3.4, 3.8):
#   esses_add_library(<name> LAYER <n> [QT_FREE] [HEADER_ONLY] SOURCES ... DEPS ... PRIVATE_DEPS ...)
#   esses_add_executable(<name> LAYER <n> [QT_FREE] SOURCES ... DEPS ... [OUTPUT_NAME <exe>])
#   esses_add_test(<name> [QT_FREE] [CUSTOM_MAIN] SOURCES ... DEPS ... [LABELS ...] [TIMEOUT <s>] [PREFIX <p>])
#   esses_add_qt_test(<name> SOURCES ... DEPS ... [TEST_NAME <ctest name>] [ARGS ...] [LABELS ...] [TIMEOUT <s>])
#   esses_add_qt_test_entry(<name> TEST_NAME <ctest name> [ARGS ...] [LABELS ...] [TIMEOUT <s>])
#   esses_add_qt_test_dir(<prefix> DIR <dir> DEPS ... [LABELS ...] [TIMEOUT <s>])
#   esses_component_enabled(<component> <out-var>)
# Library targets are ${ESSES_PREFIX}_<name> with the alias ${ESSES_NAMESPACE}::<name>. The layer check
# (01 section 2.3) fails configure on an upward esses:: dependency or a forbidden pair of 01 section 2.2, and
# names the ESSES_WITH_<LIB> switch when a dependency was left out with it.
# Every registered target is written to ${CMAKE_BINARY_DIR}/esses_targets.json for tools/check_layering.py.
include_guard(GLOBAL)
include(warnings)
include(catch_discover)
include(qt-free-check)

if(ESSES_ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
endif()

# The environment of every Qt Test executable under ctest. QT_FORCE_STDERR_LOGGING=1: on Windows Qt Test's plain
# logger writes to OutputDebugString instead of stdout whenever the process has no console window (ctest from CI,
# an IDE or a service), so the ctest log would hold no line of a failing Qt test; forced, it writes to the pipe.
set(ESSES_QT_TEST_ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1")

# 01 section 2.2 rule 1 and 08 section 2 rule 3: pairs that are forbidden regardless of layer numbers.
# analysis>project closes the same-layer cycle project -> analysis; plotcore>maths is D-048 as amended by D-122
# (maths outputs reach the plot as Columns through the ChunkCache, never through a link).
set(ESSES_FORBIDDEN_EDGES
    "formats>maths" "formats>analysis" "formats>project"
    "analysis>maths" "analysis>project" "analysis>formats"
    "maths>project" "maths>formats"
    "plotcore>maths"
    "qtrender>qtmodel")

function(_esses_short_name target out)
  string(REGEX REPLACE "^${ESSES_PREFIX}_" "" short "${target}")
  set(${out} "${short}" PARENT_SCOPE)
endfunction()

function(_esses_layer_check target layer)
  _esses_short_name(${target} self)
  foreach(d IN LISTS ARGN)
    if(NOT d MATCHES "^${ESSES_NAMESPACE}::(.+)$")
      continue()
    endif()
    set(dshort "${CMAKE_MATCH_1}")
    if(NOT TARGET ${d})
      string(TOUPPER "${dshort}" dupper)
      if(DEFINED ESSES_WITH_${dupper} AND NOT ESSES_WITH_${dupper})
        string(TOUPPER "${self}" supper)
        if(DEFINED ESSES_WITH_${supper})
          set(remedy "configure with -DESSES_WITH_${supper}=OFF as well")
        else()
          set(remedy "leave ${target} out as well")
        endif()
        message(FATAL_ERROR "layer check: ${target} depends on ${d}, which is switched off "
                            "(ESSES_WITH_${dupper}=OFF); ${remedy}, or turn ESSES_WITH_${dupper} on")
      endif()
      message(FATAL_ERROR "layer check: ${target} depends on ${d}, which is not defined yet "
                          "(libraries are added in layer order in libs/CMakeLists.txt)")
    endif()
    get_target_property(real ${d} ALIASED_TARGET)
    if(NOT real)
      set(real ${d})
    endif()
    get_target_property(dl ${real} ESSES_LAYER)        # NOTFOUND for vendored third-party targets
    if(NOT dl STREQUAL "dl-NOTFOUND" AND dl GREATER layer)
      message(FATAL_ERROR "layer check: ${target} (layer ${layer}) depends on ${d} (layer ${dl})")
    endif()
    if("${self}>${dshort}" IN_LIST ESSES_FORBIDDEN_EDGES)
      message(FATAL_ERROR "layer check: ${target} must never depend on ${d} (01 section 2.2 rule 1)")
    endif()
  endforeach()
endfunction()

# ESSES_WITH_<COMPONENT> (top-level CMakeLists.txt) lets a configure leave out a library or application that is
# still being written. A component without such a switch is always enabled.
function(esses_component_enabled component out)
  string(TOUPPER "${component}" upper)
  if(DEFINED ESSES_WITH_${upper} AND NOT ESSES_WITH_${upper})
    set(${out} FALSE PARENT_SCOPE)
  else()
    set(${out} TRUE PARENT_SCOPE)
  endif()
endfunction()

function(_esses_register target kind layer)
  set_property(GLOBAL APPEND PROPERTY ESSES_ALL_TARGETS ${target})
  set_target_properties(${target} PROPERTIES ESSES_KIND "${kind}" ESSES_LAYER_REG "${layer}")
endfunction()

function(_esses_apply_tidy target)
  if(ESSES_ENABLE_CLANG_TIDY)
    set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE};--warnings-as-errors=*")
  endif()
endfunction()

function(esses_add_library name)
  cmake_parse_arguments(A "QT_FREE;HEADER_ONLY" "LAYER" "SOURCES;DEPS;PRIVATE_DEPS" ${ARGN})
  if(NOT DEFINED A_LAYER)
    message(FATAL_ERROR "esses_add_library(${name}): LAYER is required")
  endif()
  set(t ${ESSES_PREFIX}_${name})
  if(A_HEADER_ONLY)
    add_library(${t} INTERFACE ${A_SOURCES})
    target_include_directories(${t} INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                              $<INSTALL_INTERFACE:include>)
    target_link_libraries(${t} INTERFACE ${A_DEPS} ${A_PRIVATE_DEPS})
    if(CMAKE_CXX_COMPILER_LOADED)
      target_compile_features(${t} INTERFACE cxx_std_20)
    endif()
  else()
    add_library(${t} STATIC ${A_SOURCES})
    target_include_directories(${t} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                           $<INSTALL_INTERFACE:include>)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src")
      target_include_directories(${t} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    endif()
    target_link_libraries(${t} PUBLIC ${A_DEPS} PRIVATE ${A_PRIVATE_DEPS})
    target_compile_features(${t} PUBLIC cxx_std_20)
    esses_apply_warnings(${t})
    set_target_properties(${t} PROPERTIES FOLDER "libs/${name}")
    if(A_QT_FREE)
      _esses_apply_tidy(${t})
    endif()
  endif()
  add_library(${ESSES_NAMESPACE}::${name} ALIAS ${t})
  set_target_properties(${t} PROPERTIES ESSES_LAYER ${A_LAYER}
                                        ESSES_DEPS "${A_DEPS}" ESSES_PRIVATE_DEPS "${A_PRIVATE_DEPS}")
  _esses_layer_check(${t} ${A_LAYER} ${A_DEPS} ${A_PRIVATE_DEPS})
  if(A_QT_FREE)
    esses_mark_qt_free(${t})
  endif()
  _esses_register(${t} library ${A_LAYER})
endfunction()

function(esses_add_executable name)
  cmake_parse_arguments(A "QT_FREE;WIN32_GUI" "LAYER;OUTPUT_NAME" "SOURCES;DEPS" ${ARGN})
  if(NOT DEFINED A_LAYER)
    message(FATAL_ERROR "esses_add_executable(${name}): LAYER is required")
  endif()
  set(t ${ESSES_PREFIX}_${name})
  if(A_WIN32_GUI)
    add_executable(${t} WIN32 MACOSX_BUNDLE ${A_SOURCES})
  else()
    add_executable(${t} ${A_SOURCES})
  endif()
  target_link_libraries(${t} PRIVATE ${A_DEPS})
  esses_apply_warnings(${t})
  if(A_OUTPUT_NAME)
    set_target_properties(${t} PROPERTIES OUTPUT_NAME "${A_OUTPUT_NAME}")
  endif()
  set_target_properties(${t} PROPERTIES FOLDER "apps" ESSES_LAYER ${A_LAYER} ESSES_DEPS "${A_DEPS}")
  _esses_layer_check(${t} ${A_LAYER} ${A_DEPS})
  if(A_QT_FREE)
    esses_mark_qt_free(${t})
    _esses_apply_tidy(${t})
  endif()
  _esses_register(${t} executable ${A_LAYER})
endfunction()

function(esses_add_test name)
  if(NOT ESSES_BUILD_TESTS)
    return()
  endif()
  cmake_parse_arguments(A "QT_FREE;CUSTOM_MAIN" "TIMEOUT;PREFIX" "SOURCES;DEPS;LABELS" ${ARGN})
  if(name MATCHES "^${ESSES_PREFIX}_")
    set(t ${name})
  else()
    set(t ${ESSES_PREFIX}_${name})
  endif()
  _esses_short_name(${t} short)
  string(REGEX REPLACE "_tests$" "" short "${short}")
  if(NOT A_PREFIX)
    set(A_PREFIX "${short}.")
  endif()
  add_executable(${t} ${A_SOURCES})
  # Catch2 v3 with its default main(); CUSTOM_MAIN links the variant without main() for a test that brings one.
  if(A_CUSTOM_MAIN)
    target_link_libraries(${t} PRIVATE ${A_DEPS} ${ESSES_NAMESPACE}::catch2_nomain)
  else()
    target_link_libraries(${t} PRIVATE ${A_DEPS} ${ESSES_NAMESPACE}::catch2)
  endif()
  target_compile_definitions(${t} PRIVATE "ESSES_TEST_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\""
                                          "ESSES_TEST_BINARY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
  esses_apply_warnings(${t})
  set_target_properties(${t} PROPERTIES FOLDER "tests" ESSES_DEPS "${A_DEPS}")
  if(A_QT_FREE)
    esses_mark_qt_free(${t})
  endif()
  set(discover_args PREFIX "${A_PREFIX}")
  if(A_TIMEOUT)
    list(APPEND discover_args TIMEOUT ${A_TIMEOUT})
  endif()
  if(A_LABELS)
    list(APPEND discover_args LABELS ${A_LABELS})
  endif()
  esses_catch_discover(${t} ${discover_args})
  _esses_register(${t} test "")
endfunction()

# ---- Qt Test executables (08 section 6.2: Qt Test for the Qt layers; run offscreen) ------------------------
# esses_add_qt_test(<name> SOURCES ... DEPS ... [TEST_NAME <ctest name>] [ARGS <arg>...] [LABELS ...]
#                   [TIMEOUT <s>])
# One executable ${ESSES_PREFIX}_<name> (AUTOMOC on, linked with Qt6::Test) = one ctest entry (default name
# <name> with the first '_' turned into '.'), run with ARGS (the Qt Test functions to run, for example; default
# none: every function). The test runs with ESSES_QT_TEST_ENVIRONMENT (offscreen, Qt Test output on stdout on
# every OS) and the Qt kit's bin directory prepended to PATH, so it needs no deployment; esses_add_qt_test_entry
# adds another entry of the same executable with arguments of its own. Only with ESSES_BUILD_QT and
# ESSES_BUILD_TESTS.
function(esses_add_qt_test name)
  if(NOT ESSES_BUILD_TESTS OR NOT ESSES_BUILD_QT)
    return()
  endif()
  cmake_parse_arguments(A "" "TIMEOUT;TEST_NAME" "SOURCES;DEPS;LABELS;ARGS" ${ARGN})
  set(t ${ESSES_PREFIX}_${name})
  if(NOT A_TEST_NAME)
    string(REGEX REPLACE "^([^_]+)_" "\\1." A_TEST_NAME "${name}")   # qtmodel_dispatcher -> qtmodel.dispatcher
  endif()
  if(NOT A_TIMEOUT)
    set(A_TIMEOUT 300)
  endif()
  add_executable(${t} ${A_SOURCES})
  target_link_libraries(${t} PRIVATE ${A_DEPS} Qt6::Test)
  target_compile_definitions(${t} PRIVATE "ESSES_TEST_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\""
                                          "ESSES_TEST_BINARY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\""
                                          "ESSES_TEST_REPO_ROOT=\"${CMAKE_SOURCE_DIR}\"")
  esses_apply_warnings(${t})
  set_target_properties(${t} PROPERTIES AUTOMOC ON FOLDER "tests" ESSES_DEPS "${A_DEPS}")
  esses_add_qt_test_entry(${name} TEST_NAME ${A_TEST_NAME} ARGS ${A_ARGS} LABELS ${A_LABELS} TIMEOUT ${A_TIMEOUT})
  _esses_register(${t} test "")
endfunction()

# esses_add_qt_test_entry(<name> TEST_NAME <ctest name> [ARGS <arg>...] [LABELS ...] [TIMEOUT <s>])
# One ctest entry (labels qt and LABELS; default TIMEOUT 300) of the Qt Test executable ${ESSES_PREFIX}_<name> that
# esses_add_qt_test made, run with ARGS in the Qt test environment. M4: the soak's own function and the soak guard's
# tests are two entries of one executable (tests/soak, D-216 (b)).
function(esses_add_qt_test_entry name)
  if(NOT ESSES_BUILD_TESTS OR NOT ESSES_BUILD_QT)
    return()
  endif()
  cmake_parse_arguments(A "" "TIMEOUT;TEST_NAME" "ARGS;LABELS" ${ARGN})
  if(NOT A_TIMEOUT)
    set(A_TIMEOUT 300)
  endif()
  add_test(NAME ${A_TEST_NAME} COMMAND ${ESSES_PREFIX}_${name} ${A_ARGS})
  set_tests_properties(${A_TEST_NAME} PROPERTIES
                       ENVIRONMENT "${ESSES_QT_TEST_ENVIRONMENT}"
                       ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
                       LABELS "qt;${A_LABELS}" TIMEOUT ${A_TIMEOUT})
endfunction()

# esses_add_qt_test_dir(<prefix> DIR <dir> DEPS ... [LABELS ...] [TIMEOUT <s>])
# Every <dir>/**/tst_<name>.cpp becomes its own Qt Test executable ${ESSES_PREFIX}_<prefix>_tst_<name> (ctest
# <prefix>.tst_<name>); every other .cpp under <dir> is a shared helper compiled into each of them. The glob is
# CONFIGURE_DEPENDS, so adding a test file needs no CMakeLists edit (parallel M1 work, 07 section 4).
function(esses_add_qt_test_dir prefix)
  if(NOT ESSES_BUILD_TESTS OR NOT ESSES_BUILD_QT)
    return()
  endif()
  cmake_parse_arguments(A "" "DIR;TIMEOUT" "DEPS;LABELS" ${ARGN})
  if(NOT IS_ABSOLUTE "${A_DIR}")
    set(A_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${A_DIR}")
  endif()
  file(GLOB_RECURSE all_cpp CONFIGURE_DEPENDS "${A_DIR}/*.cpp")
  file(GLOB_RECURSE all_hpp CONFIGURE_DEPENDS "${A_DIR}/*.hpp" "${A_DIR}/*.h")
  set(tests "")
  set(helpers "")
  foreach(f IN LISTS all_cpp)
    get_filename_component(stem "${f}" NAME_WE)
    if(stem MATCHES "^tst_")
      list(APPEND tests "${f}")
    else()
      list(APPEND helpers "${f}")
    endif()
  endforeach()
  set(extra "")
  if(A_TIMEOUT)
    set(extra TIMEOUT ${A_TIMEOUT})
  endif()
  foreach(f IN LISTS tests)
    get_filename_component(stem "${f}" NAME_WE)
    esses_add_qt_test(${prefix}_${stem} TEST_NAME "${prefix}.${stem}" SOURCES "${f}" ${helpers} ${all_hpp}
                      DEPS ${A_DEPS} LABELS ${A_LABELS} ${extra})
    if(TARGET ${ESSES_PREFIX}_${prefix}_${stem})
      target_include_directories(${ESSES_PREFIX}_${prefix}_${stem} PRIVATE "${A_DIR}")
    endif()
  endforeach()
endfunction()

# ---- esses_targets.json (consumed by tools/check_layering.py) ------------------------------------------
function(_esses_json_string out value)
  string(REPLACE "\\" "\\\\" value "${value}")
  string(REPLACE "\"" "\\\"" value "${value}")
  set(${out} "\"${value}\"" PARENT_SCOPE)
endfunction()

function(_esses_json_array out)
  set(items "")
  foreach(v IN LISTS ARGN)
    _esses_json_string(q "${v}")
    list(APPEND items "${q}")
  endforeach()
  string(JOIN ", " joined ${items})
  set(${out} "[${joined}]" PARENT_SCOPE)
endfunction()

function(_esses_write_targets_manifest)
  get_property(targets GLOBAL PROPERTY ESSES_ALL_TARGETS)
  set(entries "")
  foreach(t IN LISTS targets)
    get_target_property(kind ${t} ESSES_KIND)
    get_target_property(layer ${t} ESSES_LAYER_REG)
    get_target_property(dir ${t} SOURCE_DIR)
    get_target_property(deps ${t} ESSES_DEPS)
    get_target_property(pdeps ${t} ESSES_PRIVATE_DEPS)
    get_target_property(qtfree ${t} ESSES_QT_FREE)
    foreach(v deps pdeps)
      if(NOT ${v} OR ${v} MATCHES "-NOTFOUND$")
        set(${v} "")
      endif()
    endforeach()
    _esses_json_string(jname "${t}")
    _esses_json_string(jkind "${kind}")
    _esses_json_string(jdir "${dir}")
    _esses_json_array(jdeps ${deps})
    _esses_json_array(jpdeps ${pdeps})
    if(layer STREQUAL "")
      set(jlayer "null")
    else()
      set(jlayer "${layer}")
    endif()
    if(qtfree)
      set(jqt "true")
    else()
      set(jqt "false")
    endif()
    list(APPEND entries "    {\"name\": ${jname}, \"kind\": ${jkind}, \"layer\": ${jlayer}, \"sourceDir\": ${jdir}, \"deps\": ${jdeps}, \"privateDeps\": ${jpdeps}, \"qtFree\": ${jqt}}")
  endforeach()
  string(JOIN ",\n" body ${entries})
  _esses_json_string(jprefix "${ESSES_PREFIX}")
  _esses_json_string(jns "${ESSES_NAMESPACE}")
  _esses_json_array(jforbidden ${ESSES_FORBIDDEN_EDGES})
  # D-161: the Qt Gui header directories (QtGui, or the framework's Headers on macOS), so check_layering.py can
  # refuse every Qt Gui header in esses_qtmodel except the undo framework's.
  set(gui_dirs "")
  foreach(qt_gui Qt6::Gui Qt::Gui)
    if(TARGET ${qt_gui})
      get_target_property(dirs ${qt_gui} INTERFACE_INCLUDE_DIRECTORIES)
      foreach(d IN LISTS dirs)
        if(NOT d MATCHES "^\\$<" AND IS_DIRECTORY "${d}" AND d MATCHES "(QtGui|QtGui[.]framework/Headers)/?$")
          list(APPEND gui_dirs "${d}")
        endif()
      endforeach()
    endif()
  endforeach()
  list(REMOVE_DUPLICATES gui_dirs)
  _esses_json_array(jguidirs ${gui_dirs})
  file(WRITE "${CMAKE_BINARY_DIR}/esses_targets.json"
       "{\n  \"prefix\": ${jprefix},\n  \"namespace\": ${jns},\n  \"forbiddenEdges\": ${jforbidden},\n"
       "  \"qtGuiHeaderDirs\": ${jguidirs},\n"
       "  \"targets\": [\n${body}\n  ]\n}\n")
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _esses_write_targets_manifest)
