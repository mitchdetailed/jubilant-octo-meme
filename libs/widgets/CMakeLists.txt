# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# esses_widgets (layer 5, Qt Widgets; 01 section 2.1, 04 section 7, 06; D-026, D-027, D-030). M1 (07 section
# 4.1): DockHost (facade with an IDockBackend seam: the Qt Advanced Docking System backend, the default, and the
# QDockWidget fallback; D-153), MainWindow, WorksheetCanvas, ComponentWidget, TimeDistanceGraph, ValuesList,
# the DataWindow, ChannelsWindow, ValuesWindow and MessagesWindow docks, CommandActions and KeyEventRouter.
# Links esses_qtmodel, esses_qtrender, Qt Widgets and, privately, esses::qtads (the ADS shared library, only
# when ESSES_DOCK_BACKEND=ads; its types stay inside src/shell/ads_backend.cpp). Built only with ESSES_BUILD_QT=ON.
#
# Independent glob roots so parallel agents can work in this library without touching a shared file:
#   src/shell/**            include/esses/widgets/shell/**            MainWindow, DockHost, canvas, commands, keys
#   src/docks/**            include/esses/widgets/docks/**            the docks
#   src/components/**       include/esses/widgets/components/**       ComponentWidget and the components
#   src/editors_maths/**    include/esses/widgets/editors_maths/**    (M2) MathsEditor, ChannelStatusWindow
#   src/editors_data/**     include/esses/widgets/editors_data/**     (M2) ChannelEditor, MappingPanel, DetailsEditor,
#                                                                     LapEditor
#   src/editors_options/**  include/esses/widgets/editors_options/**  (M2) OptionsDialog, SchemeSelector
#   src/properties_shell/** include/esses/widgets/properties_shell/** (M3) PropertiesDialog, FieldBinder,
#                                                                     ZoomSummaryBar, CorrectedSpeedDialog,
#                                                                     LayoutEditor, CommandPalette,
#                                                                     AnimationController, ComponentClipboard
# The M3 components split src/components/ further by owner (time_distance/, plots/, reports_track/; headers
# stay flat in include/esses/widgets/components/); the components glob is recursive, so no edit is needed here.
# Every tests/**/tst_<name>.cpp is one Qt Test executable (ctest widgets.tst_<name>, run offscreen); other
# tests/**/*.cpp are helpers shared by them.

set(CMAKE_AUTOMOC ON)
set(_esses_widgets_sources "")
foreach(_root shell docks components editors_maths editors_data editors_options properties_shell)
  file(GLOB_RECURSE _files CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/src/${_root}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/${_root}/*.hpp"
       "${CMAKE_CURRENT_SOURCE_DIR}/include/esses/widgets/${_root}/*.hpp")
  list(APPEND _esses_widgets_sources ${_files})
endforeach()
# The ADS backend is compiled only when the docking library is built (cmake/qtads.cmake).
set(_esses_widgets_ads "")
if(TARGET ${ESSES_NAMESPACE}::qtads)
  set(_esses_widgets_ads ${ESSES_NAMESPACE}::qtads)
else()
  list(FILTER _esses_widgets_sources EXCLUDE REGEX "/src/shell/ads_backend[.]cpp$")
endif()
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
    Qt6::Widgets
  PRIVATE_DEPS
    ${_esses_widgets_ads})

# Qt keywords off for every Qt target (use Q_SIGNALS, Q_SLOTS, Q_EMIT): Qt-free headers use the plain names, e.g.
# 04 section 4.2's TraceBuilder::emit and ComponentBuildInputs::slots. PUBLIC, so consumers and tests inherit it.
target_compile_definitions(${ESSES_PREFIX}_widgets PUBLIC QT_NO_KEYWORDS)
# ESSES_HAVE_ADS (PUBLIC: the dock tests run on every backend the build has) and the build's default backend
# (DockHost::makeDefaultBackend; the environment variable ESSES_DOCK_BACKEND overrides it at run time).
if(_esses_widgets_ads)
  target_compile_definitions(${ESSES_PREFIX}_widgets PUBLIC ESSES_HAVE_ADS=1
                             PRIVATE ESSES_DEFAULT_DOCK_BACKEND="ads")
else()
  target_compile_definitions(${ESSES_PREFIX}_widgets PUBLIC ESSES_HAVE_ADS=0
                             PRIVATE ESSES_DEFAULT_DOCK_BACKEND="qdockwidget")
endif()

# The dock tests drive ADS through its own API (the dock's float and close buttons, a drag to another side), so
# the test executables see its headers; tests may link what their library links (tools/check_layering.py).
esses_add_qt_test_dir(widgets DIR tests DEPS esses::widgets ${_esses_widgets_ads})
