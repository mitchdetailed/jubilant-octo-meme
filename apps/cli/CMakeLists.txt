# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# esses-cli (05 section 9.2, 08 section 3.5: apps/cli, layer 3, Qt-free) and its command implementation as
# the internal library esses_cli_core, which esses-gen-fixtures (tools/) and the CLI's unit tests share.

esses_add_library(cli_core LAYER 3 QT_FREE
  SOURCES
    src/cli_common.cpp
    src/sha256.cpp
    src/process.cpp
    src/app_context.cpp
    src/inspect.cpp
    src/validate.cpp
    src/expect_channels.cpp
    src/fixture.cpp
  DEPS
    esses::base
    esses::units
    esses::store
    esses::model
    esses::formats
    esses::nlohmann_json
  PRIVATE_DEPS
    esses::json_schema_validator
    esses::fast_float)
# The headers live in src/ (quoted includes; no public esses/cli/ namespace yet).
target_include_directories(${ESSES_PREFIX}_cli_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# Developer builds find data/, schemas/ and tools/ld_ref_decode.py in the source tree when <exe>/data lacks them.
target_compile_definitions(${ESSES_PREFIX}_cli_core PRIVATE "ESSES_CLI_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")

esses_add_executable(cli LAYER 3 QT_FREE
  SOURCES src/main.cpp
  DEPS esses::cli_core esses::cli11
  OUTPUT_NAME esses-cli)

# The shipped data beside the executable (esses_base Paths: <exe dir>/data): the unit table and the schemas.
# configure_file makes each a configure dependency, so an edited schema reaches the build directory.
configure_file("${CMAKE_SOURCE_DIR}/data/units.json" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/data/units.json" COPYONLY)
file(GLOB _esses_cli_schemas CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/schemas/*.schema.json")
foreach(_schema IN LISTS _esses_cli_schemas)
  get_filename_component(_name "${_schema}" NAME)
  configure_file("${_schema}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/data/schemas/${_name}" COPYONLY)
endforeach()

if(ESSES_BUILD_TESTS)
  # Unit tests of the helpers (SHA-256, the expected-channel CSVs and their comparison, the file-name encoding,
  # Windows argument quoting, schema ids) and of the commands on synthetic files.
  esses_add_test(cli_tests QT_FREE TIMEOUT 300
    SOURCES tests/cli_tests.cpp
    DEPS esses::cli_core esses::formats esses::nlohmann_json)
  if(TARGET ${ESSES_PREFIX}_cli_tests)
    target_compile_definitions(${ESSES_PREFIX}_cli_tests PRIVATE "ESSES_TEST_REPO_ROOT=\"${CMAKE_SOURCE_DIR}\"")
  endif()

  set(_cli "$<TARGET_FILE:${ESSES_PREFIX}_cli>")
  # validate: exit 0 on the valid schema examples, 1 on an invalid one, 2 on a missing file; every JSON document
  # shipped under data/ and schemas/ validates (08 section 5).
  add_test(NAME cli.validate.examples_valid
           COMMAND ${_cli} validate "${CMAKE_SOURCE_DIR}/schemas/examples/fixture-1/valid"
                   "${CMAKE_SOURCE_DIR}/schemas/examples/laps-1/valid"
                   "${CMAKE_SOURCE_DIR}/schemas/examples/probe-1/valid")
  add_test(NAME cli.validate.example_invalid
           COMMAND ${_cli} validate "${CMAKE_SOURCE_DIR}/schemas/examples/laps-1/invalid/unknown-range-kind.json")
  set_tests_properties(cli.validate.example_invalid PROPERTIES WILL_FAIL TRUE)
  add_test(NAME cli.validate.missing_file COMMAND ${_cli} validate "${CMAKE_BINARY_DIR}/no-such-file.json")
  set_tests_properties(cli.validate.missing_file PROPERTIES WILL_FAIL TRUE)
  add_test(NAME cli.validate.data_and_schemas
           COMMAND ${_cli} validate "${CMAKE_SOURCE_DIR}/data" ${_esses_cli_schemas})
  set_tests_properties(cli.validate.examples_valid cli.validate.example_invalid cli.validate.missing_file
                       cli.validate.data_and_schemas PROPERTIES LABELS "cli" TIMEOUT 120)
endif()
