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
    src/maths_cli.cpp
    src/export.cpp
  DEPS
    esses::base
    esses::units
    esses::store
    esses::model
    esses::formats
    esses::analysis         # M1: esses-cli laps runs the profile's rules (RuleRunner, AC-M1-07)
    esses::project          # M2: validate runs the registered migrations before validating (02 section 6.6)
    esses::maths            # M2: eval, maths check, maths functions (03 section 13)
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
# (M3) The component body schemas, schemas/components/<type>-<version>.schema.json (04 sections 8.14-17).
file(GLOB _esses_cli_component_schemas CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/schemas/components/*.schema.json")
foreach(_schema IN LISTS _esses_cli_component_schemas)
  get_filename_component(_name "${_schema}" NAME)
  configure_file("${_schema}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/data/schemas/components/${_name}" COPYONLY)
endforeach()

if(ESSES_BUILD_TESTS)
  # Unit tests of the helpers (SHA-256, the expected-channel CSVs and their comparison, the file-name encoding,
  # Windows argument quoting, schema ids) and of the commands on synthetic files.
  esses_add_test(cli_tests QT_FREE TIMEOUT 300
    SOURCES tests/cli_tests.cpp tests/document_roundtrip_tests.cpp tests/validate_tests.cpp tests/maths_cli_tests.cpp
    DEPS esses::cli_core esses::formats esses::project esses::maths esses::nlohmann_json)
  if(TARGET ${ESSES_PREFIX}_cli_tests)
    target_compile_definitions(${ESSES_PREFIX}_cli_tests PRIVATE "ESSES_TEST_REPO_ROOT=\"${CMAKE_SOURCE_DIR}\"")
  endif()

  set(_cli "$<TARGET_FILE:${ESSES_PREFIX}_cli>")
  # validate: exit 0 on the valid schema examples, 1 on an invalid one, 2 on a missing file; every JSON document
  # shipped under data/ and schemas/ validates (08 section 5).
  add_test(NAME cli.validate.examples_valid
           COMMAND ${_cli} validate "${CMAKE_SOURCE_DIR}/schemas/examples/fixture-1/valid"
                   "${CMAKE_SOURCE_DIR}/schemas/examples/laps-1/valid"
                   "${CMAKE_SOURCE_DIR}/schemas/examples/probe-1/valid"
                   "${CMAKE_SOURCE_DIR}/schemas/examples/project-1/valid"
                   "${CMAKE_SOURCE_DIR}/schemas/examples/overrides-1/valid"
                   "${CMAKE_SOURCE_DIR}/schemas/examples/aliases-1/valid"
                   "${CMAKE_SOURCE_DIR}/schemas/examples/mappings-1/valid"
                   "${CMAKE_SOURCE_DIR}/schemas/examples/maths-1/valid"
                   "${CMAKE_SOURCE_DIR}/schemas/examples/journal-1/valid")
  add_test(NAME cli.validate.example_invalid
           COMMAND ${_cli} validate "${CMAKE_SOURCE_DIR}/schemas/examples/laps-1/invalid/unknown-range-kind.json")
  set_tests_properties(cli.validate.example_invalid PROPERTIES WILL_FAIL TRUE)
  add_test(NAME cli.validate.missing_file COMMAND ${_cli} validate "${CMAKE_BINARY_DIR}/no-such-file.json")
  set_tests_properties(cli.validate.missing_file PROPERTIES WILL_FAIL TRUE)
  add_test(NAME cli.validate.data_and_schemas
           COMMAND ${_cli} validate "${CMAKE_SOURCE_DIR}/data" ${_esses_cli_schemas})
  # AC-M2-08: every fixture expectation validates (the private checkout is not part of the public corpus) ...
  add_test(NAME cli.validate.fixtures
           COMMAND ${_cli} validate "${CMAKE_SOURCE_DIR}/fixtures" --exclude "${CMAKE_SOURCE_DIR}/fixtures/private")
  # ... and a deliberately broken file fails (exit 1) with the JSON pointer of the error.
  set(_esses_broken "${CMAKE_SOURCE_DIR}/schemas/examples/overrides-1/invalid/dps-out-of-range.json")
  add_test(NAME cli.validate.broken_fails COMMAND ${_cli} validate "${_esses_broken}")
  add_test(NAME cli.validate.broken_pointer COMMAND ${_cli} validate "${_esses_broken}")
  set_tests_properties(cli.validate.broken_fails PROPERTIES WILL_FAIL TRUE)
  set_tests_properties(cli.validate.broken_pointer PROPERTIES
                       PASS_REGULAR_EXPRESSION "dps-out-of-range.json  .overrides-1..*at '/channels/Engine RPM/dps': ")
  # Steps 1-2 of 02 section 6.6 rule 2: a version-1 project.json is migrated before it is validated, and a newer
  # major is refused with the pointer of "$schema".
  add_test(NAME cli.validate.migrates COMMAND ${_cli} validate "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/project-v1.json")
  set_tests_properties(cli.validate.migrates PROPERTIES
                       PASS_REGULAR_EXPRESSION "project-1: migrated schemaVersion 1 -> 2.*1 valid, 0 invalid")
  add_test(NAME cli.validate.newer_major
           COMMAND ${_cli} validate "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/project-major-2.json")
  set_tests_properties(cli.validate.newer_major PROPERTIES
                       PASS_REGULAR_EXPRESSION "at '/\\$schema': project-2 is a newer major than this build reads")
  set_tests_properties(cli.validate.examples_valid cli.validate.example_invalid cli.validate.missing_file
                       cli.validate.data_and_schemas cli.validate.fixtures cli.validate.broken_fails
                       cli.validate.broken_pointer cli.validate.migrates cli.validate.newer_major
                       PROPERTIES LABELS "cli" TIMEOUT 120)

  # eval (AC-M2-14) over the generated synthetic-acc-dialect input: an integer channel against the oracle's
  # values (expected.channels, D-041), and --json with the rate, unit and values of one lap of the 1 kHz speed.
  set(_acc_input "${CMAKE_BINARY_DIR}/fixture-inputs/motec.ld/synthetic-acc-dialect/input.ld")
  set(_acc_expected "${CMAKE_SOURCE_DIR}/fixtures/motec.ld/synthetic-acc-dialect/expected.channels")
  add_test(NAME cli.eval.golden_gear
           COMMAND "${CMAKE_COMMAND}" "-DCLI=${_cli}" "-DINPUT=${_acc_input}" "-DMODE=linear"
                   "-DEXPR='GEAR' * 2 + 1" "-DEXPECTED=${_acc_expected}/003_GEAR.csv" -DSCALE=2 -DOFFSET=1
                   "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/eval-golden-gear"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/eval_golden.cmake")
  add_test(NAME cli.eval.json_speed_lap
           COMMAND "${CMAKE_COMMAND}" "-DCLI=${_cli}" "-DINPUT=${_acc_input}" "-DMODE=json"
                   "-DEXPR='SPEED'[m/s] * 2" "-DARGS=--range Outings:Laps/1" -DRATE_NUM=1000 -DRATE_DEN=1
                   -DUNIT=m/s -DDTYPE=F32 "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/eval-json-speed"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/eval_golden.cmake")
  # --rate is exact (D-008): "20.5/1" evaluates at 41/2 Hz, not 20 Hz.
  add_test(NAME cli.eval.json_rate_exact
           COMMAND "${CMAKE_COMMAND}" "-DCLI=${_cli}" "-DINPUT=${_acc_input}" "-DMODE=json"
                   "-DEXPR='SPEED'[m/s] * 2" "-DARGS=--range Outings:Laps/1 --rate 20.5/1" -DRATE_NUM=41
                   -DRATE_DEN=2 -DUNIT=m/s -DDTYPE=F32 "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/eval-json-rate"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/eval_golden.cmake")
  set_tests_properties(cli.eval.golden_gear cli.eval.json_speed_lap cli.eval.json_rate_exact PROPERTIES
                       FIXTURES_REQUIRED esses_fixture_inputs LABELS "cli;fixtures" TIMEOUT 300)
  # export (07 section 5.1, project-aware CSV with maths) over the generated synthetic-acc-dialect input with the
  # project of tests/data/export-project: mappings at open, the project's maths (a selection-dependent stat over
  # the exported lap), override display units, the --rate grid; the relations are checked by export_check.py.
  find_package(Python3 3.8 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME cli.export.project
             COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/tests/export_check.py"
                     --cli ${_cli} --project "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/export-project"
                     --input "${_acc_input}" --work "${CMAKE_CURRENT_BINARY_DIR}/export-project")
    set_tests_properties(cli.export.project PROPERTIES
                         FIXTURES_REQUIRED esses_fixture_inputs LABELS "cli;fixtures" TIMEOUT 300)
    # tools/maths_corpus_check.py (AC-M2-01) with --cli relative to the working directory, as M2.md and the
    # lint job write it, over a public module directory.
    add_test(NAME cli.maths_corpus_check.relative_cli
             COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/maths_corpus_check.py"
                     --cli "bin/$<TARGET_FILE_NAME:${ESSES_PREFIX}_cli>"
                     --corpus "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/export-project/maths"
                     --compat-doc "${CMAKE_SOURCE_DIR}/docs/maths-compat.md"
             WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
    set_tests_properties(cli.maths_corpus_check.relative_cli PROPERTIES LABELS "cli" TIMEOUT 120
                         PASS_REGULAR_EXPRESSION "5 items \\(5 enabled\\).*PASS")
  endif()
  # fixture diff (05 section 9.2) explains an altered copy of a committed CSV fixture.
  add_test(NAME cli.fixture_diff.explains
           COMMAND "${CMAKE_COMMAND}" "-DCLI=${_cli}" "-DFIXTURE=${CMAKE_SOURCE_DIR}/fixtures/generic.csv/sparse-gps"
                   "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixture-diff"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixture_diff.cmake")
  set_tests_properties(cli.fixture_diff.explains PROPERTIES LABELS "cli;fixtures" TIMEOUT 120)
  # A syntax error is exit status 1 with the diagnostic code on stderr (no log is opened).
  add_test(NAME cli.eval.syntax_error COMMAND ${_cli} eval "1 +" "${CMAKE_BINARY_DIR}/no-such-log.ld")
  set_tests_properties(cli.eval.syntax_error PROPERTIES LABELS "cli" TIMEOUT 60 WILL_FAIL TRUE)
  add_test(NAME cli.eval.syntax_error_code COMMAND ${_cli} eval "max(1 2)" "${CMAKE_BINARY_DIR}/no-such-log.ld")
  set_tests_properties(cli.eval.syntax_error_code PROPERTIES LABELS "cli" TIMEOUT 60
                       PASS_REGULAR_EXPRESSION "E-COMMA-MISSING error 5-6")
endif()
