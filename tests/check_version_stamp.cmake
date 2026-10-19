# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# ctest lint.version_stamp: esses_version_from_git (cmake/version_parse.cmake) on hand-written outputs of
# `git describe --tags --dirty --always` and `git rev-parse --short HEAD` (08 section 3.10, AC-M4-09, D-216 (a)).
# The expectations are worked from the stamp's rules, not taken from the function. cmake -DSOURCE_ROOT=<repo> -P
include("${SOURCE_ROOT}/cmake/version_parse.cmake")

set(_failures 0)
# <describe>|<head>|<expected version>|<expected commit>; "-" stands for an empty field (a failed git call).
set(_cases
    # exactly at the release tag: the commit is HEAD's, never "unknown"
    "v0.1.0|56d8065|0.1.0|56d8065"
    # the tag, modified: not the pre-release "-dirty"
    "v0.1.0-dirty|56d8065|0.1.0.dirty|56d8065-dirty"
    # after the tag
    "v0.1.0-3-g56d8065|56d8065|0.1.0+3-g56d8065|56d8065"
    "v0.1.0-3-g56d8065-dirty|56d8065|0.1.0+3-g56d8065.dirty|56d8065-dirty"
    # pre-release tags, as this repository's milestone tags
    "v0.1.0-m3|59df0f8|0.1.0-m3|59df0f8"
    "v0.1.0-m3-1-g56d8065|56d8065|0.1.0-m3+1-g56d8065|56d8065"
    "v0.1.0-m3-1-g56d8065-dirty|56d8065|0.1.0-m3+1-g56d8065.dirty|56d8065-dirty"
    "v1.2.3-rc.1|abcdef0123|1.2.3-rc.1|abcdef0123"
    "v1.2.3-rc.1-dirty|abcdef0123|1.2.3-rc.1.dirty|abcdef0123-dirty"
    # no tag yet: describe prints the hash alone, the version is the project's
    "56d8065|56d8065|9.8.7|56d8065"
    "56d8065-dirty|56d8065|9.8.7|56d8065-dirty"
    # a tag that is not a version, and a version without its v
    "release-1|56d8065|9.8.7|56d8065"
    "0.1.0|56d8065|9.8.7|56d8065"
    # the dots of the version are dots
    "v0x1x0|56d8065|9.8.7|56d8065"
    # git missing or no commit
    "-|-|9.8.7|unknown"
    "v0.1.0|-|0.1.0|unknown"
    "-|56d8065|9.8.7|56d8065")
foreach(_case IN LISTS _cases)
  string(REPLACE "|" ";" _f "${_case}")
  list(GET _f 0 _describe)
  list(GET _f 1 _head)
  list(GET _f 2 _want_version)
  list(GET _f 3 _want_commit)
  foreach(_v _describe _head)
    if(${_v} STREQUAL "-")
      set(${_v} "")
    endif()
  endforeach()
  esses_version_from_git("${_describe}" "${_head}" "9.8.7" _version _commit)
  if(NOT _version STREQUAL _want_version OR NOT _commit STREQUAL _want_commit)
    message(SEND_ERROR "describe '${_describe}', head '${_head}': got version '${_version}' commit '${_commit}', "
                       "want '${_want_version}' '${_want_commit}'")
    math(EXPR _failures "${_failures} + 1")
  endif()
endforeach()
list(LENGTH _cases _n)
if(_failures GREATER 0)
  message(FATAL_ERROR "version stamp: ${_failures} of ${_n} cases failed")
endif()
message(STATUS "version stamp: ${_n} cases passed")
