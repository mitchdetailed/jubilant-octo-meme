# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# esses_project (layer 2, Qt-free; 01 section 2.1, 02 section 6). M1 (07 section 4.1): ProjectFile,
# WorkbookFile, ComponentSpec, Scheme, Keymap and Profile read/write paths plus the shipped documents
# (data/workbooks/circuit-default.workbook.json, data/schemes/{light,dark}.scheme.json, data/keymaps/default.json,
# data/profiles/circuit.profile.json, data/rules/*.rule.json). M2: the channel documents, SchemaRegistry,
# Migrations and Journal (src/persistence/).
# Links esses_base, esses_units, esses_model, esses_analysis and esses_maths (same-layer edges, D-122);
# nlohmann-json and json-schema-validator are private (no JSON library type in a public header).
#
# Sources and tests are globbed (CONFIGURE_DEPENDS): src/**/*.cpp and src/**/*.hpp form the library;
# tests/**/*.cpp form one Catch2 executable (project.<test>).

file(GLOB_RECURSE _esses_project_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp")
esses_add_library(project LAYER 2 QT_FREE
  SOURCES ${_esses_project_sources}
  DEPS
    esses::base
    esses::units
    esses::model
    esses::analysis
    esses::maths
  PRIVATE_DEPS
    esses::nlohmann_json
    esses::json_schema_validator)
# SchemaRegistry::findSchemasDir() falls back to the source tree's schemas/ (developer builds, tests), after
# <dataDir>/schemas and <exe dir>/data/schemas.
target_compile_definitions(${ESSES_PREFIX}_project PRIVATE "ESSES_PROJECT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")

file(GLOB_RECURSE _esses_project_tests CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp")
esses_add_test(project_tests QT_FREE
  SOURCES ${_esses_project_tests}
  DEPS
    esses::project
    esses::nlohmann_json
    esses::json_schema_validator)
if(TARGET ${ESSES_PREFIX}_project_tests)
  target_compile_definitions(${ESSES_PREFIX}_project_tests PRIVATE "ESSES_TEST_REPO_ROOT=\"${CMAKE_SOURCE_DIR}\"")
  target_include_directories(${ESSES_PREFIX}_project_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()

# The shipped documents beside the executables (<exe dir>/data, esses_base Paths::dataDir()): every JSON file
# under data/workbooks, data/schemes, data/keymaps, data/profiles and data/rules. configure_file makes each a
# configure dependency and the glob is CONFIGURE_DEPENDS, so a new or edited document reaches the build tree.
# (data/units.json and schemas/ are copied by apps/cli.)
foreach(_esses_data_dir workbooks schemes keymaps profiles rules)
  file(GLOB_RECURSE _esses_data_files CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/data/${_esses_data_dir}/*.json")
  foreach(_file IN LISTS _esses_data_files)
    file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}/data" "${_file}")
    configure_file("${_file}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/data/${_rel}" COPYONLY)
  endforeach()
endforeach()
