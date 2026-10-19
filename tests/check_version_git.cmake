# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# ctest lint.version_git: esses_git_describe and esses_version_from_git (cmake/version_parse.cmake) on a scratch
# repository of their own under WORK_DIR (never the source checkout), as a release build of cmake/version.cmake
# sees it (AC-M4-09, D-217): the release tag v0.1.0 and the newer milestone tag v0.1.0-m4 on one commit (plain
# `git describe` prints the newer one), the release built from v0.1.0 still stamps 0.1.0; a modified tree or a
# later commit does not; a repository without a version tag stamps a development version.
#   cmake -DSOURCE_ROOT=<repo> -DWORK_DIR=<scratch dir> [-DGIT_EXECUTABLE=<git>] -P check_version_git.cmake
# tests/CMakeLists.txt registers it only where git is found.
include("${SOURCE_ROOT}/cmake/version_parse.cmake")

find_package(Git QUIET)
if(NOT GIT_FOUND)
  message(FATAL_ERROR "version git: no git")
endif()
set(_repo "${WORK_DIR}/repo")
file(REMOVE_RECURSE "${_repo}")
file(MAKE_DIRECTORY "${_repo}")

# git with a fixed identity and fixed dates, in the scratch repository only.
function(scratch_git date)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env "GIT_AUTHOR_DATE=${date}" "GIT_COMMITTER_DATE=${date}"
                          "GIT_CONFIG_NOSYSTEM=1" "GIT_CONFIG_GLOBAL=${WORK_DIR}/none.gitconfig"
                          "${GIT_EXECUTABLE}" -c user.name=esses -c user.email=esses@example.invalid
                          -c commit.gpgsign=false -c tag.gpgsign=false -c init.defaultBranch=main ${ARGN}
                  WORKING_DIRECTORY "${_repo}" RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "git ${ARGN}: ${_err}")
  endif()
endfunction()

set(_failures 0)
function(expect release_tag want_version what)
  esses_git_describe("${_repo}" "${release_tag}" _describe _head)
  esses_version_from_git("${_describe}" "${_head}" "9.9.9" _version _commit)
  if(NOT _version MATCHES "${want_version}")
    message(SEND_ERROR "${what}: release tag '${release_tag}', describe '${_describe}': version '${_version}', "
                       "want ${want_version}")
    math(EXPR _n "${_failures} + 1")
    set(_failures ${_n} PARENT_SCOPE)
  endif()
endfunction()

file(WRITE "${WORK_DIR}/none.gitconfig" "")
scratch_git("2026-10-19T09:00:00Z" init -q)
file(WRITE "${_repo}/a.txt" "a\n")
scratch_git("2026-10-19T09:00:00Z" add a.txt)
scratch_git("2026-10-19T09:00:00Z" commit -q -m one)
expect("" "^9\\.9\\.9\\+g[0-9a-f]+$" "no version tag: a development version")
scratch_git("2026-10-19T10:00:00Z" tag -a v0.1.0 -m release)
scratch_git("2026-10-19T10:05:00Z" tag -a v0.1.0-m4 -m milestone)
expect("v0.1.0" "^0\\.1\\.0$" "the release tag with a newer milestone tag on the same commit")
expect("v0.1.0-m4" "^0\\.1\\.0-m4$" "the milestone tag built as a release")
expect("v0.2.0" "^0\\.1\\.0" "a release tag HEAD does not carry: the plain describe (refused by the packaging)")
file(WRITE "${_repo}/a.txt" "b\n")
expect("v0.1.0" "^0\\.1\\.0\\.dirty$" "the release tag, modified")
scratch_git("2026-10-19T11:00:00Z" commit -q -a -m two)
expect("v0.1.0" "\\+1-g[0-9a-f]+$" "a commit after the release tag")
file(REMOVE_RECURSE "${_repo}")
if(_failures GREATER 0)
  message(FATAL_ERROR "version git: ${_failures} case(s) failed")
endif()
message(STATUS "version git: 6 cases passed")
