# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# Configure-time Qt-free check (08 section 3.9, D-007). Runs at the end of configure over every target marked
# ESSES_QT_FREE and fails configure when (1) its transitive link closure contains a Qt or ADS target, or
# (2) one of its sources or headers includes a Qt header or uses a Qt type token. The definitive proof is the
# `core` CI job on runners without Qt. The negative tests (AC-M0-07, 01 section 11 item 2) are the ctest
# entries labelled `lint` in tests/CMakeLists.txt, driven by cmake/qt-free-negative-test.cmake.
#
# It also checks (M1, 01 section 2.2 rules 3-4):
#   (3) every target of ESSES_QT_FREE_REQUIRED that exists is marked QT_FREE, so a library cannot leave the Qt-free
#       set by dropping the flag;
#   (4) the Qt layers link only their allowed Qt modules: esses_qtmodel no Qt Gui/Widgets (Qt Core only),
#       esses_qtrender and esses-render no Qt Widgets, nothing but esses_widgets/esses_app the docking library,
#       and no target Qt Concurrent or Qt Sql (D-031, 01 section 2.1).
include_guard(GLOBAL)

# Short names (without ${ESSES_PREFIX}_) of the targets that must stay Qt-free whenever they are defined.
set(ESSES_QT_FREE_REQUIRED base units store model formats analysis maths project plotcore cli_core cli)
# <short name>|<regex of forbidden link-closure entries> (01 section 2.2 rule 4).
set(ESSES_QT_FORBIDDEN_LINKS
    "qtmodel|^(Qt[0-9]*::(Gui|Widgets|OpenGL|OpenGLWidgets|Svg|SvgWidgets|PrintSupport)|ads::.*|${ESSES_NAMESPACE}::ads)$"
    "qtrender|^(Qt[0-9]*::(Widgets|OpenGLWidgets|SvgWidgets|PrintSupport)|ads::.*|${ESSES_NAMESPACE}::ads)$"
    "render|^(Qt[0-9]*::(Widgets|OpenGLWidgets|SvgWidgets|PrintSupport)|ads::.*|${ESSES_NAMESPACE}::ads)$"
    "*|^Qt[0-9]*::(Concurrent|Sql)$")

function(esses_mark_qt_free target)
  set_property(TARGET ${target} PROPERTY ESSES_QT_FREE TRUE)
  set_property(GLOBAL APPEND PROPERTY ESSES_QT_FREE_TARGETS ${target})
endfunction()

function(_esses_links_of target out)
  set(libs "")
  get_target_property(type ${target} TYPE)
  if(NOT type STREQUAL "INTERFACE_LIBRARY")
    get_target_property(l ${target} LINK_LIBRARIES)
    if(l)
      list(APPEND libs ${l})
    endif()
  endif()
  get_target_property(il ${target} INTERFACE_LINK_LIBRARIES)
  if(il)
    list(APPEND libs ${il})
  endif()
  # Unwrap simple generator expressions such as $<LINK_ONLY:x> and $<BUILD_INTERFACE:x>.
  list(TRANSFORM libs REPLACE "^\\$<[A-Z_]+:(.*)>$" "\\1")
  set(${out} "${libs}" PARENT_SCOPE)
endfunction()

function(esses_qt_free_check)
  get_property(targets GLOBAL PROPERTY ESSES_QT_FREE_TARGETS)
  foreach(t IN LISTS targets)
    # (1) the transitive link closure must contain no Qt target
    set(todo ${t})
    set(seen "")
    while(todo)
      list(POP_FRONT todo cur)
      if(cur IN_LIST seen)
        continue()
      endif()
      list(APPEND seen ${cur})
      if(cur MATCHES "^(Qt[0-9]*::|Qt[0-9]*[A-Z]|ads::|${ESSES_NAMESPACE}::ads$|qt[0-9]*advanceddocking)")
        message(FATAL_ERROR "qt-free-check: ${t} links ${cur} (via ${seen})")
      endif()
      if(TARGET ${cur})
        _esses_links_of(${cur} more)
        list(APPEND todo ${more})
      endif()
    endwhile()
    # (2) no source or header may include a Qt header or use a Qt type
    get_target_property(srcs ${t} SOURCES)
    get_target_property(dir ${t} SOURCE_DIR)
    if(NOT srcs)
      set(srcs "")
    endif()
    file(GLOB_RECURSE hdrs "${dir}/include/*.hpp" "${dir}/include/*.h" "${dir}/src/*.hpp" "${dir}/src/*.h")
    foreach(s IN LISTS srcs hdrs)
      if(s MATCHES "^\\$<")
        continue()
      endif()
      if(NOT IS_ABSOLUTE "${s}")
        set(s "${dir}/${s}")
      endif()
      if(NOT EXISTS "${s}")
        continue()
      endif()
      # CMake regex has no \b (it matches a literal 'b'), so word boundaries are spelled out as character classes.
      # Include: <QString>, "QString", <QtCore>, <QtCore/QString>, and lower-case <qglobal.h>.
      file(STRINGS "${s}" hits REGEX "^[ \t]*#[ \t]*include[ \t]*([<\"]Q[A-Za-z]|<q[a-z0-9_]+\\.h>)")
      if(hits)
        message(FATAL_ERROR "qt-free-check: ${s} includes Qt: ${hits}")
      endif()
      file(STRINGS "${s}" qs REGEX "(^|[^A-Za-z0-9_])Q(String|Object|Image|Painter|Variant|Byte[A-Z][A-Za-z]*|List|Map|Hash)([^A-Za-z0-9_]|$)")
      if(qs)
        message(FATAL_ERROR "qt-free-check: ${s} uses a Qt type: ${qs}")
      endif()
    endforeach()
  endforeach()
  # (3) the required Qt-free set
  foreach(short IN LISTS ESSES_QT_FREE_REQUIRED)
    set(t ${ESSES_PREFIX}_${short})
    if(TARGET ${t})
      get_target_property(qf ${t} ESSES_QT_FREE)
      if(NOT qf)
        message(FATAL_ERROR "qt-free-check: ${t} must be declared QT_FREE (01 section 2.2 rule 3)")
      endif()
    endif()
  endforeach()
  # (4) Qt module allowance of the Qt layers
  get_property(all GLOBAL PROPERTY ESSES_ALL_TARGETS)
  foreach(t IN LISTS all)
    string(REGEX REPLACE "^${ESSES_PREFIX}_" "" short "${t}")
    set(rules "")
    foreach(entry IN LISTS ESSES_QT_FORBIDDEN_LINKS)
      string(FIND "${entry}" "|" bar)
      string(SUBSTRING "${entry}" 0 ${bar} who)
      math(EXPR rbegin "${bar} + 1")
      string(SUBSTRING "${entry}" ${rbegin} -1 regex)
      if(who STREQUAL "*" OR who STREQUAL short)
        list(APPEND rules "${regex}")
      endif()
    endforeach()
    if(NOT rules)
      continue()
    endif()
    set(todo ${t})
    set(seen "")
    while(todo)
      list(POP_FRONT todo cur)
      if(cur IN_LIST seen)
        continue()
      endif()
      list(APPEND seen ${cur})
      foreach(regex IN LISTS rules)
        if(cur MATCHES "${regex}")
          message(FATAL_ERROR "qt-free-check: ${t} must not link ${cur} (01 section 2.2 rule 4; via ${seen})")
        endif()
      endforeach()
      if(TARGET ${cur})
        _esses_links_of(${cur} more)
        list(APPEND todo ${more})
      endif()
    endwhile()
  endforeach()
  list(LENGTH targets n)
  message(STATUS "qt-free-check: ${n} Qt-free targets checked: ${targets}")
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL esses_qt_free_check)
