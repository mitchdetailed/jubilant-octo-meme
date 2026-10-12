# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# esses_qtrender (layer 4, Qt Gui; 01 section 2.1, 04 section 6; D-025, D-033, D-137): RasterBackend
# (plot::IRenderBackend on QPainter into QImage layers), LayerCache (one cached Static image, Dynamic and Chrome
# lists painted directly), OffscreenRenderer (goldens, print, clipboard, esses-render), SchemeFonts and
# QtTextMetrics (thread-safe ITextMetrics). Links esses_plotcore (and what it includes from below) and Qt Gui;
# never esses_qtmodel (01 section 2.2 rule 1) and never Qt Widgets (rule 4). Built only with ESSES_BUILD_QT=ON.
#
# Sources and tests are globbed (CONFIGURE_DEPENDS): src/**/*.cpp, src/**/*.hpp and include/**/*.hpp form the
# library (AUTOMOC sees the headers); every tests/**/tst_<name>.cpp is one Qt Test executable
# (ctest qtrender.tst_<name>, run offscreen), other tests/**/*.cpp are helpers shared by them.

set(CMAKE_AUTOMOC ON)
file(GLOB_RECURSE _esses_qtrender_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp")
esses_add_library(qtrender LAYER 4
  SOURCES ${_esses_qtrender_sources}
  DEPS
    esses::base
    esses::units
    esses::store
    esses::model
    esses::project
    esses::plotcore
    Qt6::Gui)

# Qt keywords off for every Qt target (use Q_SIGNALS, Q_SLOTS, Q_EMIT): Qt-free headers use the plain names, e.g.
# 04 section 4.2's TraceBuilder::emit and ComponentBuildInputs::slots. PUBLIC, so consumers and tests inherit it.
target_compile_definitions(${ESSES_PREFIX}_qtrender PUBLIC QT_NO_KEYWORDS)

esses_add_qt_test_dir(qtrender DIR tests DEPS esses::qtrender)
