# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# Libraries in layer order (01 section 2.1, 08 section 2). The layer check in esses_add_library() requires every
# esses:: dependency to exist already, so the order below is the layer order. Each directory is added only when
# it exists and its ESSES_WITH_<LIB> switch (top-level CMakeLists.txt; every library except base and store) is
# not OFF, so libraries can be dropped in one at a time and an unfinished one can be left out of a configure.

set(_esses_qt_free_libs
    base units          # layer 0
    store model         # layer 1
    formats analysis    # layer 2
    maths project       # layer 2 (same-layer edges maths -> analysis, project -> analysis, project -> maths)
    plotcore)           # layer 3
foreach(_lib IN LISTS _esses_qt_free_libs)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_lib}/CMakeLists.txt")
    esses_component_enabled(${_lib} _enabled)
    if(_enabled)
      add_subdirectory(${_lib})
    else()
      string(TOUPPER "${_lib}" _upper)
      message(STATUS "libs/${_lib}: switched off (ESSES_WITH_${_upper}=OFF)")
    endif()
  endif()
endforeach()

if(ESSES_BUILD_QT)
  foreach(_lib qtmodel qtrender widgets)   # layers 4-5 (switches ESSES_WITH_QTMODEL, _QTRENDER, _WIDGETS)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_lib}/CMakeLists.txt")
      esses_component_enabled(${_lib} _enabled)
      if(_enabled)
        add_subdirectory(${_lib})
      else()
        string(TOUPPER "${_lib}" _upper)
        message(STATUS "libs/${_lib}: switched off (ESSES_WITH_${_upper}=OFF)")
      endif()
    endif()
  endforeach()
endif()
