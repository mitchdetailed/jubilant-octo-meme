# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# esses_version_from_git(<describe> <head> <project version> <out version> <out commit>): the version and the commit
# that cmake/version.cmake stamps (08 section 3.10), from the output of `git describe --tags --dirty --always`
# (<describe>, empty when it failed) and of `git rev-parse --short HEAD` (<head>, empty when it failed).
#   - A trailing "-dirty" (a modified tree) is taken off first, so "v0.1.0-dirty" is the tag v0.1.0 modified, not
#     the pre-release "-dirty".
#   - vMAJOR.MINOR.PATCH[-pre][-N-gHASH] gives MAJOR.MINOR.PATCH[-pre][+N-gHASH], with ".dirty" for a modified tree;
#     anything else (no tag yet: describe prints the hash alone) keeps <project version>.
#   - The commit is <head> in every case, also exactly at a tag where describe prints the tag alone, with "-dirty"
#     for a modified tree; "unknown" without a hash (no git, no commit). AC-M4-09, D-216 (a).
# No side effects: tests/check_version_stamp.cmake (ctest lint.version_stamp) runs it on hand-written cases.
include_guard(GLOBAL)

function(esses_version_from_git describe head project_version out_version out_commit)
  set(_dirty FALSE)
  if(describe MATCHES "^(.*)-dirty$")
    set(_dirty TRUE)
    set(describe "${CMAKE_MATCH_1}")
  endif()
  set(_version "${project_version}")
  if(describe MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)(-[0-9A-Za-z.]+)?(-([0-9]+)-g([0-9a-f]+))?$")
    set(_version "${CMAKE_MATCH_1}${CMAKE_MATCH_2}")
    if(CMAKE_MATCH_4)
      string(APPEND _version "+${CMAKE_MATCH_4}-g${CMAKE_MATCH_5}")
    endif()
    if(_dirty)
      string(APPEND _version ".dirty")
    endif()
  endif()
  set(_commit "unknown")
  if(head MATCHES "^[0-9a-f]+$")
    set(_commit "${head}")
    if(_dirty)
      string(APPEND _commit "-dirty")
    endif()
  endif()
  set(${out_version} "${_version}" PARENT_SCOPE)
  set(${out_commit} "${_commit}" PARENT_SCOPE)
endfunction()
