# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# C++ developer tools (08 section 1): tools/gen_ld_fixtures.cpp built as esses-gen-fixtures, and the fixture
# corpus tests that use it (AC-M0-01..03). The Python tools beside this file are run with python -I.

esses_add_executable(gen_fixtures LAYER 3 QT_FREE
  SOURCES gen_ld_fixtures.cpp
  DEPS esses::cli_core esses::cli11
  OUTPUT_NAME esses-gen-fixtures)

if(NOT ESSES_BUILD_TESTS)
  return()
endif()

set(_cli "$<TARGET_FILE:${ESSES_PREFIX}_cli>")
set(_gen "$<TARGET_FILE:${ESSES_PREFIX}_gen_fixtures>")
set(_inputs "${CMAKE_BINARY_DIR}/fixture-inputs")

# The synthetic inputs are generated into the build tree (never into the source tree, so concurrent builds do
# not race) together with a manifest that points the committed fixture directories at them.
add_test(NAME fixtures.generate_inputs
         COMMAND ${_gen} --inputs-only --out "${_inputs}/motec.ld" --fixtures "${CMAKE_SOURCE_DIR}/fixtures/motec.ld"
                 --manifest "${_inputs}/manifest.json")
set_tests_properties(fixtures.generate_inputs PROPERTIES FIXTURES_SETUP esses_fixture_inputs LABELS "fixtures"
                     TIMEOUT 600)

# AC-M0-01/02/03: every public fixture (probe, markers, laps byte-identical; channels against the oracle; input
# sha256 = the generator's output). Private fixtures without a manifest report "skipped (private)".
add_test(NAME fixtures.verify_public
         COMMAND ${_cli} fixture verify "${CMAKE_SOURCE_DIR}/fixtures" --manifest "${_inputs}/manifest.json")
set_tests_properties(fixtures.verify_public PROPERTIES FIXTURES_REQUIRED esses_fixture_inputs LABELS "fixtures"
                     TIMEOUT 900)

# The user's real files (fixtures/private, never committed): built by tools/build_private_fixtures.py from
# fixtures/private/s1/corpus.json; skipped when absent (08 section 4.5).
add_test(NAME fixtures.verify_private
         COMMAND ${_cli} fixture verify --if-present --manifest "${CMAKE_SOURCE_DIR}/fixtures/private/motec.ld/manifest.json")
set_tests_properties(fixtures.verify_private PROPERTIES LABELS "fixtures;oracle" TIMEOUT 1800
                     SKIP_REGULAR_EXPRESSION "skipped \\(private\\): no manifest present")

# Every JSON document of the corpus satisfies the schema it names (08 section 5; AC-M0-10). The private fixture
# directory is checked when it exists (fixtures/private/s1 holds spike outputs that are not schema documents).
add_test(NAME fixtures.validate_json COMMAND ${_cli} validate "${CMAKE_SOURCE_DIR}/fixtures/motec.ld")
set_tests_properties(fixtures.validate_json PROPERTIES LABELS "fixtures;lint" TIMEOUT 300)
if(EXISTS "${CMAKE_SOURCE_DIR}/fixtures/private/motec.ld")
  add_test(NAME fixtures.validate_private_json
           COMMAND ${_cli} validate "${CMAKE_SOURCE_DIR}/fixtures/private/motec.ld")
  set_tests_properties(fixtures.validate_private_json PROPERTIES LABELS "fixtures;oracle" TIMEOUT 600)
endif()

# The generator is deterministic: the same family written twice is bit-identical.
add_test(NAME fixtures.generator_deterministic
         COMMAND "${CMAKE_COMMAND}" "-DGEN=${_gen}" "-DWORK=${CMAKE_BINARY_DIR}/fixture-determinism"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/fixtures_determinism.cmake")
set_tests_properties(fixtures.generator_deterministic PROPERTIES LABELS "fixtures" TIMEOUT 600)

find_package(Python3 3.8 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  # 07 section 3.6 demo script, synthetic part: info --json on a synthetic family, fixture add with the oracle
  # into a scratch directory, fixture verify of that directory.
  add_test(NAME cli.demo_script
           COMMAND "${CMAKE_COMMAND}" "-DCLI=${_cli}" "-DPYTHON=${Python3_EXECUTABLE}"
                   "-DSOURCE=${CMAKE_SOURCE_DIR}" "-DINPUTS=${_inputs}/motec.ld"
                   "-DWORK=${CMAKE_BINARY_DIR}/demo-script"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/demo_script.cmake")
  set_tests_properties(cli.demo_script PROPERTIES FIXTURES_REQUIRED esses_fixture_inputs LABELS "cli" TIMEOUT 600)

  # AC-M0-12: every Import Log code the reader emits on the corpus (public fixtures, and the user's files when
  # fixtures/private is present) is documented in docs/formats/ld.md.
  add_test(NAME lint.check_importlog_codes
           COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/check_importlog_codes.py"
                   --root "${CMAKE_SOURCE_DIR}" --cli "${_cli}")
  add_test(NAME lint.check_importlog_codes.self_test
           COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/check_importlog_codes.py" --self-test)
  set_tests_properties(lint.check_importlog_codes PROPERTIES LABELS "lint" TIMEOUT 1800)
  set_tests_properties(lint.check_importlog_codes.self_test PROPERTIES LABELS "lint" TIMEOUT 120)

  # AC-M0-12 timing (opt-in: writes a 500 MB file): median of 5 'esses-cli info' runs on the synthetic file.
  option(ESSES_PERF_TESTS "Register the opt-in timing tests (perf.*)" OFF)
  if(ESSES_PERF_TESTS)
    add_test(NAME perf.probe_500mb
             COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/probe_timing.py" --cli "${_cli}"
                     --gen "${_gen}" --file "${CMAKE_BINARY_DIR}/perf/synthetic-500mb.ld" --runs 5 --limit-ms 200)
    set_tests_properties(perf.probe_500mb PROPERTIES LABELS "perf" TIMEOUT 1800 RUN_SERIAL TRUE)
  endif()
endif()
