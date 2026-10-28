# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# esses_qtrender (layer 4, Qt Gui; 01 section 2.1, 04 section 6; D-025, D-033, D-137): RasterBackend
# (plot::IRenderBackend on QPainter into QImage layers), LayerCache (one cached Static image, Dynamic and Chrome
# lists painted directly), OffscreenRenderer (goldens, print, clipboard, esses-render), SchemeFonts and
# QtTextMetrics (thread-safe ITextMetrics). Links esses_plotcore (and what it includes from below) and Qt Gui;
# never esses_qtmodel (01 section 2.2 rule 1) and never Qt Widgets (rule 4). Built only with ESSES_BUILD_QT=ON.
# Qt Gui's private API (Qt6::GuiPrivate, privately) for src/glyph_bins.cpp only (D-220).
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
    Qt6::Gui
  PRIVATE_DEPS
    Qt6::GuiPrivate)

# Qt keywords off for every Qt target (use Q_SIGNALS, Q_SLOTS, Q_EMIT): Qt-free headers use the plain names, e.g.
# 04 section 4.2's TraceBuilder::emit and ComponentBuildInputs::slots. PUBLIC, so consumers and tests inherit it.
target_compile_definitions(${ESSES_PREFIX}_qtrender PUBLIC QT_NO_KEYWORDS)

esses_add_qt_test_dir(qtrender DIR tests DEPS esses::qtrender)

# Diagnostic probe (ESSES_BUILD_PROBES, D-220): probes/probe_text_engines.cpp reports which font engine
# draws the plot text on each thread, its subpixel position count after each DPR history, and whether pinning
# the count (src/glyph_bins.hpp) makes the pixels independent of that history. ctest label probe (a suite run
# with the option on can leave it out with -LE probe). More entries run it on other font engines: on macOS the
# cocoa platform (CoreText as the application uses it) and the minimal platform (meant as a FreeType control; on
# macos-14 with Qt 6.12.0 it finds no font and draws box engines, ci 40413826613); on Windows the windows platform's
# DirectWrite engines, also at QT_SCALE_FACTOR=1.5 (a scaled monitor, where DirectWrite's default hinting is subpixel).
if(ESSES_BUILD_PROBES)
  esses_add_qt_test(qtrender_probe_text_engines TEST_NAME qtrender.probe_text_engines
    SOURCES probes/probe_text_engines.cpp tests/qtrender_test_support.cpp tests/qtrender_test_support.hpp
    DEPS esses::qtrender Qt6::GuiPrivate LABELS probe TIMEOUT 900)
  if(TARGET ${ESSES_PREFIX}_qtrender_probe_text_engines)
    target_include_directories(${ESSES_PREFIX}_qtrender_probe_text_engines PRIVATE
                               "${CMAKE_CURRENT_SOURCE_DIR}/tests" "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(APPLE)
      esses_add_qt_test_entry(qtrender_probe_text_engines TEST_NAME qtrender.probe_text_engines.cocoa
                              ARGS -platform cocoa LABELS probe TIMEOUT 900)
      esses_add_qt_test_entry(qtrender_probe_text_engines TEST_NAME qtrender.probe_text_engines.freetype
                              ARGS -platform minimal:enable_fonts,freetype LABELS probe TIMEOUT 900)
    elseif(WIN32)
      esses_add_qt_test_entry(qtrender_probe_text_engines TEST_NAME qtrender.probe_text_engines.directwrite
                              ARGS -platform windows LABELS probe TIMEOUT 900)
      set(_esses_probe_scaled qtrender.probe_text_engines.directwrite_scaled)
      esses_add_qt_test_entry(qtrender_probe_text_engines TEST_NAME ${_esses_probe_scaled}
                              ARGS -platform windows LABELS probe TIMEOUT 900)
      set_property(TEST ${_esses_probe_scaled} APPEND PROPERTY ENVIRONMENT "QT_SCALE_FACTOR=1.5")
    endif()
  endif()
endif()
