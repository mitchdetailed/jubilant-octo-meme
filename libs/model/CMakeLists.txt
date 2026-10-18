# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# esses_model (layer 1, Qt-free): the data-source model (02_DATA_MODEL.md sections 4-5; D-013, D-014, D-015,
# D-130, D-132, D-140): channel identity and descriptors, layers, sources, details, markers and marker edits,
# ranges and range groups, the channel resolver, the sidecar-1 and markers-1 documents and the whole-outing
# range rule that esses-cli laps needs in M0. Links esses_base, esses_units and esses_store; nlohmann-json is a
# private dependency of the document code (no JSON type appears in a public header).
#
# M2: sources and tests are globbed (CONFIGURE_DEPENDS) so parallel work adds files without editing this file:
# src/**/*.cpp and src/**/*.hpp form the library; tests/**/*.cpp form one Catch2 executable (model.<test>).

file(GLOB_RECURSE _esses_model_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp")
esses_add_library(model LAYER 1 QT_FREE
  SOURCES ${_esses_model_sources}
  DEPS
    esses::base
    esses::units
    esses::store
  PRIVATE_DEPS
    esses::nlohmann_json)

# Unit tests of every type; the document tests read schemas/examples and validate Esses' own output against
# schemas/sidecar-1.schema.json and schemas/markers-1.schema.json with the vendored json-schema-validator.
file(GLOB_RECURSE _esses_model_tests CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp")
esses_add_test(model_tests QT_FREE
  SOURCES ${_esses_model_tests}
  DEPS
    esses::model
    esses::nlohmann_json
    esses::json_schema_validator)
if(TARGET ${ESSES_PREFIX}_model_tests)
  # Tests may reach the private document helpers (src/).
  target_include_directories(${ESSES_PREFIX}_model_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
