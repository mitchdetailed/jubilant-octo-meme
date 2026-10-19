# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# Top-level build (08_REPO_AND_BUILD.md section 3). Layers 0-3 are Qt-free and build with
# ESSES_BUILD_QT=OFF; the Qt layers are added only when ESSES_BUILD_QT is ON (M1 onwards).

cmake_minimum_required(VERSION 3.28...4.4)   # presets v8 need 3.28; the developer has 4.4
project(esses VERSION 0.1.0 DESCRIPTION "Motorsport data analysis" LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(naming)          # the only place the codename appears as a prefix (D-003)

# ---- options (08 section 3.2) -------------------------------------------------------------------
option(ESSES_BUILD_QT "Build the Qt layers 4-5, esses-render and the GUI (M1 onwards)" OFF)
option(ESSES_BUILD_TESTS "Build the Catch2 test executables and enable ctest" ON)
option(ESSES_BUILD_BENCH "Build Google Benchmark targets (phase B: not vendored yet)" OFF)
option(ESSES_BUILD_FUZZ "Build libFuzzer targets (Clang only)" OFF)
option(ESSES_BUILD_PLUGINS "Build plugins/ and sdk/ (v1.0)" OFF)
option(ESSES_WARNINGS_AS_ERRORS "Treat warnings as errors (ON in the ci-* presets only)" OFF)
option(ESSES_ENABLE_CLANG_TIDY "Run clang-tidy on Qt-free targets" OFF)
option(ESSES_LTO "Interprocedural optimisation" OFF)
option(ESSES_FORCE_ASSERTS "Keep ESSES_ASSERT active in optimised builds (CI release-with-asserts)" OFF)
set(ESSES_SANITIZE "" CACHE STRING "Sanitizers for Clang/GCC: address,undefined or thread")
# DockHost backend of the Qt build (D-026, D-153): "ads" builds the Qt Advanced Docking System from third_party/qtads
# as a shared library (esses::qtads, cmake/qtads.cmake) and makes it the default, the QDockWidget backend staying
# selectable at run time (environment ESSES_DOCK_BACKEND=qdockwidget); "qdockwidget" builds without ADS, for an OS
# on which ADS is unusable (D-026's fallback rule).
set(ESSES_DOCK_BACKEND "ads" CACHE STRING "Default DockHost backend: ads or qdockwidget (the fallback; ADS not built)")
set_property(CACHE ESSES_DOCK_BACKEND PROPERTY STRINGS ads qdockwidget)
# Per-component switches (default ON): a configure can leave out a library or application that is still being
# written, so work on its siblings is not blocked. Honoured by the guarded add_subdirectory() calls below and in
# libs/CMakeLists.txt; a component that DEPS on a switched-off one fails the layer check naming the switch.
option(ESSES_WITH_UNITS "Build libs/units (esses_units)" ON)
option(ESSES_WITH_MODEL "Build libs/model (esses_model)" ON)
option(ESSES_WITH_FORMATS "Build libs/formats (esses_formats)" ON)
option(ESSES_WITH_CLI "Build the command-line application esses-cli (apps/cli)" ON)
option(ESSES_WITH_ANALYSIS "Build libs/analysis (esses_analysis)" ON)
option(ESSES_WITH_MATHS "Build libs/maths (esses_maths)" ON)
option(ESSES_WITH_PROJECT "Build libs/project (esses_project)" ON)
option(ESSES_WITH_PLOTCORE "Build libs/plotcore (esses_plotcore)" ON)
# The Qt layers (built only with ESSES_BUILD_QT=ON).
option(ESSES_WITH_QTMODEL "Build libs/qtmodel (esses_qtmodel, Qt Core)" ON)
option(ESSES_WITH_QTRENDER "Build libs/qtrender (esses_qtrender, Qt Gui)" ON)
option(ESSES_WITH_WIDGETS "Build libs/widgets (esses_widgets, Qt Widgets)" ON)
option(ESSES_WITH_RENDER "Build the offscreen renderer esses-render (apps/render)" ON)
option(ESSES_WITH_APP "Build the GUI application esses (apps/app)" ON)

# ---- global settings (08 section 3.1) -----------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)      # no C++20 modules; skips the per-file dependency scan
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")   # /MD, matches Qt kits
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")   # every exe, test exe and DLL in one directory
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(MSVC)
  add_compile_options(/utf-8 /bigobj
                      "$<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus;/Zc:preprocessor;/permissive-;/EHsc>")
endif()
if(ESSES_FORCE_ASSERTS)
  add_compile_definitions(ESSES_FORCE_ASSERTS=1)
endif()
if(ESSES_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _esses_ipo_ok OUTPUT _esses_ipo_msg LANGUAGES CXX)
  if(_esses_ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  else()
    message(WARNING "ESSES_LTO requested but not supported: ${_esses_ipo_msg}")
  endif()
endif()

include(sanitizers)
include(fuzzing)         # ESSES_BUILD_FUZZ: coverage instrumentation for the libFuzzer targets
include(version)
include(third_party)
include(esses_targets)    # esses_add_library / esses_add_executable / esses_add_test, layer check, warnings
include(qt-free-check)    # deferred configure-time check over every QT_FREE target (08 section 3.9)

if(ESSES_BUILD_QT)
  # Qt is found only here; with ESSES_BUILD_QT=OFF there is no find_package(Qt6) at all (D-007).
  find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui Widgets Svg PrintSupport Test)
  message(STATUS "Qt ${Qt6_VERSION} from ${Qt6_DIR}")
  include(qtads)          # esses::qtads, the docking library (shared, LGPL), when ESSES_DOCK_BACKEND is "ads"
endif()

if(ESSES_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)   # vendored-library smoke tests, test-discovery self-tests, configure-level checks
endif()

add_subdirectory(libs)

# Applications: 08 section 1 places the CLI in apps/cli; libs/cli is accepted too while the tree settles.
esses_component_enabled(cli _esses_cli_enabled)
foreach(_esses_app_dir apps/cli libs/cli)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_esses_app_dir}/CMakeLists.txt")
    if(_esses_cli_enabled)
      add_subdirectory(${_esses_app_dir})
    else()
      message(STATUS "${_esses_app_dir}: switched off (ESSES_WITH_CLI=OFF)")
    endif()
    break()
  endif()
endforeach()
# C++ developer tools (tools/gen_ld_fixtures.cpp -> esses-gen-fixtures) and the fixture corpus tests; they share
# the CLI's command library, so they follow it.
if(TARGET ${ESSES_PREFIX}_cli_core AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/CMakeLists.txt")
  add_subdirectory(tools)
endif()
if(ESSES_BUILD_QT)
  # apps/render -> esses_render (switch ESSES_WITH_RENDER), apps/app -> esses_app (switch ESSES_WITH_APP).
  foreach(_esses_app render app)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/${_esses_app}/CMakeLists.txt")
      esses_component_enabled(${_esses_app} _esses_app_enabled)
      if(_esses_app_enabled)
        add_subdirectory(apps/${_esses_app})
      else()
        string(TOUPPER "${_esses_app}" _esses_app_upper)
        message(STATUS "apps/${_esses_app}: switched off (ESSES_WITH_${_esses_app_upper}=OFF)")
      endif()
    endif()
  endforeach()
  # Qt Test suites that span the Qt layers (07 section 4.2: tests/ui walkthrough, F9, render parity; tests/golden
  # pixel goldens; M4: tests/soak, the Saturday soak of 07 section 7.1, added when the soak package creates it,
  # D-206). Added after every library and application so their targets exist.
  if(ESSES_BUILD_TESTS)
    foreach(_esses_qt_test_dir tests/ui tests/golden tests/soak)
      if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_esses_qt_test_dir}/CMakeLists.txt")
        add_subdirectory(${_esses_qt_test_dir})
      endif()
    endforeach()
  endif()
endif()
# M4 (D-212): tools/packaging/ (every install rule, windeployqt, the Inno Setup installer, the portable zip, the
# AppImage and DMG scripts, the third-party manifest; 07 section 7.1 "Packaging", 08 sections 2 and 7). Added with
# and without the Qt layers (D-216 (c)): a Qt-free tree installs the command-line tool, the data, the notices and
# the user guide, which the lint job stages and scans for vendor marks (AC-M4-08); the application's rules apply
# when its targets exist.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/packaging/CMakeLists.txt")
  add_subdirectory(tools/packaging)
endif()

# bench/: the budget benchmarks (07 section 12): the Qt ones of M1 (plot_bench, open_bench, AC-M1-02/03) with the
# Qt layers, the Qt-free ones (M2: formats_bench, AC-M2-13) with the core. With ESSES_BUILD_BENCH they are ctest
# entries labelled `bench` (the bench presets build without tests).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bench/CMakeLists.txt" AND TARGET ${ESSES_PREFIX}_formats AND NOT ESSES_BUILD_FUZZ)
  if(ESSES_BUILD_BENCH AND NOT ESSES_BUILD_TESTS)
    enable_testing()
  endif()
  add_subdirectory(bench)
endif()
if(ESSES_BUILD_FUZZ AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/CMakeLists.txt")
  add_subdirectory(tests/fuzz)
endif()

message(STATUS "${ESSES_APP_NAME} ${ESSES_VERSION_STRING} (git ${ESSES_GIT_SHA}); Qt layers: ${ESSES_BUILD_QT}; "
               "tests: ${ESSES_BUILD_TESTS}; warnings as errors: ${ESSES_WARNINGS_AS_ERRORS}")
