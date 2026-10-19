# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# esses_version_from_git(<describe> <head> <project version> <out version> <out commit>): the version and the commit
# that cmake/version.cmake stamps (08 section 3.10), from the output of `git describe --tags --dirty --always`
# (<describe>, empty when it failed) and of `git rev-parse --short HEAD` (<head>, empty when it failed).
#   - A trailing "-dirty" (a modified tree) is taken off first, so "v0.1.0-dirty" is the tag v0.1.0 modified, not
#     the pre-release "-dirty".
#   - vMAJOR.MINOR.PATCH[-pre][-N-gHASH] gives MAJOR.MINOR.PATCH[-pre][+N-gHASH], with ".dirty" for a modified tree;
#     the pre-release may hold hyphens ("v0.1.0-beta-1-2-gabc1234" is the tag v0.1.0-beta-1 and two commits).
#   - Anything else with a commit (no tag reachable, as in a shallow clone, where describe prints the hash alone; a
#     tag that is not a version) is a development build: <project version>+g<head>, ".dirty" when modified, so that
#     it never reads as the release (D-217). Without a commit (no git): <project version>.
#   - The commit is <head> in every case, also exactly at a tag where describe prints the tag alone, with "-dirty"
#     for a modified tree; "unknown" without a hash (no git, no commit). AC-M4-09, D-216 (a).
# No side effects: tests/check_version_stamp.cmake (ctest lint.version_stamp) runs it on hand-written cases.
#
# esses_git_describe(<source dir> <release tag> <out describe> <out head>): runs the read-only git queries above in
# <source dir>. With a <release tag> (ESSES_RELEASE_TAG) the describe is of that very tag when HEAD carries it
# (`git describe --tags --dirty --exact-match --match <tag>`), so that another v* tag on the same commit (the
# milestone's v0.1.0-mN, which plain describe may prefer as the newer one) cannot change a release build's version
# (D-217); when HEAD does not carry it, the plain describe stands and the packaging's check refuses the build.
# Empty outputs when git or the commit is missing. tests/check_version_git.cmake (ctest lint.version_git) runs it
# on a scratch repository.
include_guard(GLOBAL)

function(esses_version_from_git describe head project_version out_version out_commit)
  set(_dirty FALSE)
  if(describe MATCHES "^(.*)-dirty$")
    set(_dirty TRUE)
    set(describe "${CMAKE_MATCH_1}")
  endif()
  set(_tag "${describe}")
  set(_since "")
  if(describe MATCHES "^(.+)-([0-9]+)-g([0-9a-f]+)$")
    set(_tag "${CMAKE_MATCH_1}")
    set(_since "+${CMAKE_MATCH_2}-g${CMAKE_MATCH_3}")
  endif()
  set(_commit "unknown")
  if(head MATCHES "^[0-9a-f]+$")
    set(_commit "${head}")
    if(_dirty)
      string(APPEND _commit "-dirty")
    endif()
  endif()
  if(_tag MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)(-[0-9A-Za-z.-]+)?$")
    set(_version "${CMAKE_MATCH_1}${CMAKE_MATCH_2}${_since}")
    if(_dirty)
      string(APPEND _version ".dirty")
    endif()
  elseif(head MATCHES "^[0-9a-f]+$")
    set(_version "${project_version}+g${head}")
    if(_dirty)
      string(APPEND _version ".dirty")
    endif()
  else()
    set(_version "${project_version}")
  endif()
  set(${out_version} "${_version}" PARENT_SCOPE)
  set(${out_commit} "${_commit}" PARENT_SCOPE)
endfunction()

function(esses_git_describe source_dir release_tag out_describe out_head)
  set(_describe "")
  set(_head "")
  find_package(Git QUIET)
  if(GIT_FOUND AND EXISTS "${source_dir}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --dirty --always
                    WORKING_DIRECTORY "${source_dir}"
                    OUTPUT_VARIABLE _out RESULT_VARIABLE _rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_rc EQUAL 0)
      set(_describe "${_out}")
    endif()
    if(release_tag)
      execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --dirty --exact-match --match "${release_tag}"
                      WORKING_DIRECTORY "${source_dir}"
                      OUTPUT_VARIABLE _out RESULT_VARIABLE _rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
      if(_rc EQUAL 0)
        set(_describe "${_out}")
      endif()
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
                    WORKING_DIRECTORY "${source_dir}"
                    OUTPUT_VARIABLE _out RESULT_VARIABLE _rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_rc EQUAL 0)
      set(_head "${_out}")
    endif()
  endif()
  set(${out_describe} "${_describe}" PARENT_SCOPE)
  set(${out_head} "${_head}" PARENT_SCOPE)
endfunction()
