# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# esses_qtmodel (layer 4, Qt Core only; 01 section 2.1, 06 section 1; D-017, D-018, D-030, D-031): the
# observable application model. M1 (07 section 4.1): Dispatcher, AppRuntime, DataSelectionModel (all D-017
# slots), WorkspaceViewModel + WorksheetViewModel (setDomain with both F9 rules, zoom/pan/cursor), DistanceService,
# RenderService, ProjectSession (no undo yet), SourceLoader, minimal MathsService, CommandRegistry, KeymapBinder,
# SettingsStore, MessagesModel and the item models of the M1 docks (DataWindowModel, ChannelsModel, ValuesModel).
# Links every Qt-free library and Qt Core; never Qt Gui or Qt Widgets (01 section 2.2 rule 4, checked by
# cmake/qt-free-check.cmake and tools/check_layering.py). Built only with ESSES_BUILD_QT=ON.
#
# Sources and tests are globbed (CONFIGURE_DEPENDS): src/**/*.cpp, src/**/*.hpp and include/**/*.hpp form the
# library (AUTOMOC sees the headers); every tests/**/tst_<name>.cpp is one Qt Test executable
# (ctest qtmodel.tst_<name>, run offscreen), other tests/**/*.cpp are helpers shared by them.

set(CMAKE_AUTOMOC ON)
file(GLOB_RECURSE _esses_qtmodel_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp")
esses_add_library(qtmodel LAYER 4
  SOURCES ${_esses_qtmodel_sources}
  DEPS
    esses::base
    esses::units
    esses::store
    esses::model
    esses::formats
    esses::analysis
    esses::maths
    esses::project
    esses::plotcore
    Qt6::Core)

# Qt keywords off for every Qt target (use Q_SIGNALS, Q_SLOTS, Q_EMIT): Qt-free headers use the plain names, e.g.
# 04 section 4.2's TraceBuilder::emit and ComponentBuildInputs::slots. PUBLIC, so consumers and tests inherit it.
target_compile_definitions(${ESSES_PREFIX}_qtmodel PUBLIC QT_NO_KEYWORDS)

esses_add_qt_test_dir(qtmodel DIR tests DEPS esses::qtmodel)
# tst_source_loader opens the generated public ACC stand-in (<build>/fixture-inputs): run it after the ctest
# fixture fixtures.generate_inputs (tools/CMakeLists.txt) has written it, never while it is being rewritten.
if(TEST qtmodel.tst_source_loader)
  set_tests_properties(qtmodel.tst_source_loader PROPERTIES FIXTURES_REQUIRED esses_fixture_inputs)
endif()
