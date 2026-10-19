# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# Version stamping (08 section 3.10). Runs the read-only `git describe --tags --dirty --always` and
# `git rev-parse --short HEAD` at configure time; esses_version_from_git (cmake/version_parse.cmake) turns them into
# ESSES_VERSION_STRING (vMAJOR.MINOR.PATCH[-pre][-N-gHASH] parsed, else the project() version) and ESSES_GIT_SHA
# (the commit in every case, exact tags included, "-dirty" for a modified tree; "unknown" without git; D-216 (a)).
# ESSES_BUILD_DATE honours SOURCE_DATE_EPOCH (string(TIMESTAMP) does) for reproducible builds.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/version_parse.cmake")

set(_esses_describe "")
set(_esses_head "")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --dirty --always
                  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                  OUTPUT_VARIABLE _esses_describe_out RESULT_VARIABLE _esses_describe_rc
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_esses_describe_rc EQUAL 0)
    set(_esses_describe "${_esses_describe_out}")
  endif()
  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
                  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                  OUTPUT_VARIABLE _esses_head_out RESULT_VARIABLE _esses_head_rc
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_esses_head_rc EQUAL 0)
    set(_esses_head "${_esses_head_out}")
  endif()
endif()
esses_version_from_git("${_esses_describe}" "${_esses_head}" "${PROJECT_VERSION}" ESSES_VERSION_STRING ESSES_GIT_SHA)
string(TIMESTAMP ESSES_BUILD_DATE "%Y-%m-%d" UTC)
set(ESSES_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(ESSES_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(ESSES_VERSION_PATCH ${PROJECT_VERSION_PATCH})
