# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# esses_model (layer 1, Qt-free): the data-source model (02_DATA_MODEL.md sections 4-5; D-013, D-014, D-015,
# D-130, D-132, D-140): channel identity and descriptors, layers, sources, details, markers and marker edits,
# ranges and range groups, the channel resolver, the sidecar-1 and markers-1 documents and the whole-outing
# range rule that esses-cli laps needs in M0. Links esses_base, esses_units and esses_store; nlohmann-json is a
# private dependency of the document code (no JSON type appears in a public header).

esses_add_library(model LAYER 1 QT_FREE
  SOURCES
    src/channel.cpp
    src/detail.cpp
    src/layer.cpp
    src/marker.cpp
    src/marker_json.cpp
    src/markers_document.cpp
    src/notes.cpp
    src/range.cpp
    src/resolver.cpp
    src/sidecar.cpp
    src/sidecar_json.cpp
    src/source.cpp
    src/whole_outing.cpp
  DEPS
    esses::base
    esses::units
    esses::store
  PRIVATE_DEPS
    esses::nlohmann_json)

# Unit tests of every type; the document tests read schemas/examples and validate Esses' own output against
# schemas/sidecar-1.schema.json and schemas/markers-1.schema.json with the vendored json-schema-validator.
esses_add_test(model_tests QT_FREE
  SOURCES
    tests/channel_tests.cpp
    tests/detail_tests.cpp
    tests/ids_tests.cpp
    tests/layer_tests.cpp
    tests/marker_tests.cpp
    tests/markers_document_tests.cpp
    tests/opaque_json_tests.cpp
    tests/range_tests.cpp
    tests/resolver_tests.cpp
    tests/sidecar_tests.cpp
    tests/source_tests.cpp
    tests/whole_outing_tests.cpp
  DEPS
    esses::model
    esses::nlohmann_json
    esses::json_schema_validator)
if(TARGET ${ESSES_PREFIX}_model_tests)
  # Tests may reach the private document helpers (src/).
  target_include_directories(${ESSES_PREFIX}_model_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
