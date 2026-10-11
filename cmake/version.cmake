# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# Version stamping (08 section 3.10). Runs the read-only `git describe --tags --dirty --always` at configure time
# and parses vMAJOR.MINOR.PATCH[-pre][-N-gHASH]. Without tags or commits it falls back to the project() version.
# ESSES_BUILD_DATE honours SOURCE_DATE_EPOCH (string(TIMESTAMP) does) for reproducible builds.
include_guard(GLOBAL)

set(ESSES_VERSION_STRING "${PROJECT_VERSION}")
set(ESSES_GIT_SHA "unknown")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --dirty --always
                  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                  OUTPUT_VARIABLE _esses_describe RESULT_VARIABLE _esses_describe_rc
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_esses_describe_rc EQUAL 0 AND _esses_describe)
    if(_esses_describe MATCHES "^v([0-9]+\.[0-9]+\.[0-9]+)(-[0-9A-Za-z.]+)?(-([0-9]+)-g([0-9a-f]+))?(-dirty)?$")
      set(ESSES_VERSION_STRING "${CMAKE_MATCH_1}${CMAKE_MATCH_2}")
      if(CMAKE_MATCH_4)
        string(APPEND ESSES_VERSION_STRING "+${CMAKE_MATCH_4}-g${CMAKE_MATCH_5}")
      endif()
      if(CMAKE_MATCH_6)
        string(APPEND ESSES_VERSION_STRING ".dirty")
      endif()
      if(CMAKE_MATCH_5)
        set(ESSES_GIT_SHA "${CMAKE_MATCH_5}")
      endif()
    elseif(_esses_describe MATCHES "^([0-9a-f]+)(-dirty)?$")
      set(ESSES_GIT_SHA "${CMAKE_MATCH_1}${CMAKE_MATCH_2}")   # no tag yet: commit hash only
    endif()
  endif()
endif()
string(TIMESTAMP ESSES_BUILD_DATE "%Y-%m-%d" UTC)
set(ESSES_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(ESSES_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(ESSES_VERSION_PATCH ${PROJECT_VERSION_PATCH})
