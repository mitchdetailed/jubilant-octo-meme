# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# Version stamping (08 section 3.10). Runs the read-only `git describe --tags --dirty --always` and
# `git rev-parse --short HEAD` at configure time (esses_git_describe; for a release build, ESSES_RELEASE_TAG, the
# describe of that very tag when HEAD carries it, D-217); esses_version_from_git (cmake/version_parse.cmake) turns
# them into ESSES_VERSION_STRING (vMAJOR.MINOR.PATCH[-pre][-N-gHASH] parsed; else <project version>+g<hash> for a
# development build without a version tag, D-217; the project() version without git) and ESSES_GIT_SHA (the commit
# in every case, exact tags included, "-dirty" for a modified tree; "unknown" without git; D-216 (a)).
# ESSES_BUILD_DATE honours SOURCE_DATE_EPOCH (string(TIMESTAMP) does) for reproducible builds.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/version_parse.cmake")

esses_git_describe("${CMAKE_SOURCE_DIR}" "${ESSES_RELEASE_TAG}" _esses_describe _esses_head)
esses_version_from_git("${_esses_describe}" "${_esses_head}" "${PROJECT_VERSION}" ESSES_VERSION_STRING ESSES_GIT_SHA)
string(TIMESTAMP ESSES_BUILD_DATE "%Y-%m-%d" UTC)
set(ESSES_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(ESSES_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(ESSES_VERSION_PATCH ${PROJECT_VERSION_PATCH})
