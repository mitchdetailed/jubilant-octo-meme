# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# esses_widgets (layer 5, Qt Widgets; 01 section 2.1, 04 section 7, 06; D-026, D-027, D-030). M1 (07 section
# 4.1): DockHost (facade with an IDockBackend seam: a QDockWidget backend now, the Qt Advanced Docking System
# backend once ADS is vendored), MainWindow, WorksheetCanvas, ComponentWidget, TimeDistanceGraph, ValuesList,
# the DataWindow, ChannelsWindow, ValuesWindow and MessagesWindow docks, CommandActions and KeyEventRouter.
# Links esses_qtmodel, esses_qtrender and Qt Widgets. Built only with ESSES_BUILD_QT=ON.
#
# Three independent glob roots so two agents can work in this library without touching a shared file:
#   src/shell/**       include/esses/widgets/shell/**       MainWindow, DockHost, canvas, commands, keys
#   src/docks/**       include/esses/widgets/docks/**       the docks
#   src/components/**  include/esses/widgets/components/**  ComponentWidget and the components
# Every tests/**/tst_<name>.cpp is one Qt Test executable (ctest widgets.tst_<name>, run offscreen); other
# tests/**/*.cpp are helpers shared by them.

set(CMAKE_AUTOMOC ON)
set(_esses_widgets_sources "")
foreach(_root shell docks components)
  file(GLOB_RECURSE _files CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/src/${_root}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/${_root}/*.hpp"
       "${CMAKE_CURRENT_SOURCE_DIR}/include/esses/widgets/${_root}/*.hpp")
  list(APPEND _esses_widgets_sources ${_files})
endforeach()
esses_add_library(widgets LAYER 5
  SOURCES ${_esses_widgets_sources}
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
    esses::qtmodel
    esses::qtrender
    Qt6::Widgets)

# Qt keywords off for every Qt target (use Q_SIGNALS, Q_SLOTS, Q_EMIT): Qt-free headers use the plain names, e.g.
# 04 section 4.2's TraceBuilder::emit and ComponentBuildInputs::slots. PUBLIC, so consumers and tests inherit it.
target_compile_definitions(${ESSES_PREFIX}_widgets PUBLIC QT_NO_KEYWORDS)

esses_add_qt_test_dir(widgets DIR tests DEPS esses::widgets)
