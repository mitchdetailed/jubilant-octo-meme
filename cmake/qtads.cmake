# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# The Qt Advanced Docking System (ADS 5.1.1, LGPL-2.1-or-later; D-026, D-119, D-153), the default DockHost
# backend. Included by the top-level CMakeLists.txt after find_package(Qt6), so only with ESSES_BUILD_QT=ON.
#
#   esses::qtads   alias of ADS's own target qtadvanceddocking-qt6, a SHARED library (LGPL: users can replace it)
#
# Unlike the libraries of cmake/third_party.cmake, ADS is built by its own, unmodified CMake project
# (third_party/qtads/CMakeLists.txt, 08 section 3.7.3; D-147 keeps ADS's own route), configured only through the
# options it declares: BUILD_STATIC OFF, BUILD_EXAMPLES OFF and QT_VERSION_MAJOR from our find_package(Qt6). The
# variables are set inside a function, so they reach ADS's directory scope without leaking into ours. After
# add_subdirectory():
#   - the output directories ADS sets for itself (<build>/x64/bin, <build>/x64/lib) are replaced by ours, so the
#     DLL (Windows) or .so/.dylib lands in <build>/bin next to every executable and test (08 section 3.7.5);
#   - the directory is SYSTEM, so ADS's headers are system headers for esses_widgets (/external:I, -isystem), and
#     esses_silence_third_party() builds ADS without warnings and never with warnings as errors;
#   - ADS's own install() rules stay as upstream wrote them (DLL to bin/, .so/.dylib to lib/); 08 section 7's
#     packages ship only the shared library and the licence (NOTICES.md).
# Linux: ADS links Qt6::GuiPrivate (Qt 6.9 and later; the private headers are part of the aqtinstall kit) and the
# system xcb library (libxcb1-dev). Layering: only esses_widgets and the esses app may link esses::qtads
# (cmake/qt-free-check.cmake rule 4, tools/check_layering.py rule 5).
#
# ESSES_DOCK_BACKEND (top-level CMakeLists.txt): "ads" (default) builds it; "qdockwidget" leaves ADS out and
# DockHost runs on its QDockWidget backend only.
include_guard(GLOBAL)
include(warnings)

if(NOT ESSES_DOCK_BACKEND MATCHES "^(ads|qdockwidget)$")
  message(FATAL_ERROR "ESSES_DOCK_BACKEND must be \"ads\" or \"qdockwidget\", not \"${ESSES_DOCK_BACKEND}\"")
endif()

function(_esses_add_qtads)
  set(dir "${ESSES_THIRD_PARTY_DIR}/qtads")
  foreach(f CMakeLists.txt src/CMakeLists.txt src/DockManager.h LICENSE)
    if(NOT EXISTS "${dir}/${f}")
      message(FATAL_ERROR "qtads: ${dir}/${f} not found (third_party/qtads, D-153); or configure with "
                          "-DESSES_DOCK_BACKEND=qdockwidget")
    endif()
  endforeach()
  # ADS's options (option() honours normal variables, policy CMP0077).
  set(BUILD_STATIC OFF)
  set(BUILD_EXAMPLES OFF)
  set(QT_VERSION_MAJOR ${Qt6_VERSION_MAJOR})
  add_subdirectory("${dir}" "${CMAKE_BINARY_DIR}/third_party/qtads" SYSTEM)
  set(t qtadvanceddocking-qt${QT_VERSION_MAJOR})
  if(NOT TARGET ${t})
    message(FATAL_ERROR "qtads: ADS did not define the target ${t}")
  endif()
  get_target_property(type ${t} TYPE)
  if(NOT type STREQUAL "SHARED_LIBRARY")
    message(FATAL_ERROR "qtads: ${t} is ${type}; ADS must be a shared library (LGPL, D-026)")
  endif()
  set_target_properties(${t} PROPERTIES
                        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
                        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
                        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}")
  esses_silence_third_party(${t})
  add_library(${ESSES_NAMESPACE}::qtads ALIAS ${t})
  get_target_property(version ${t} VERSION)
  message(STATUS "qtads: Qt Advanced Docking System ${version} (${t}, shared), the default DockHost backend")
endfunction()

if(ESSES_DOCK_BACKEND STREQUAL "ads")
  _esses_add_qtads()
else()
  message(STATUS "qtads: not built (ESSES_DOCK_BACKEND=${ESSES_DOCK_BACKEND}); DockHost uses its QDockWidget backend")
endif()
